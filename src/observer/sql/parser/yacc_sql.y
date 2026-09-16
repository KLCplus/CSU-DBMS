
/**
 * @file yacc_sql.y
 * @brief Bison LALR(1) 语法定义：把 Flex 的 token 流归约为 ParsedSqlNode AST
 * @ingroup SQLParser
 * @details 本文件是编译器流水线的核心语法分析环节，整体链路为：
 *   SQL 文本 --(lex_sql.l/Flex)--> token 流 --(本文件/Bison LALR)-->
 *   ParsedSqlNode AST --(ParseStage)--> ResolveStage --> Stmt。
 * 核心实现原则：
 *   1. 每个 SQL 语句对应一条 grammar rule，其语义动作构造 parse_defs.h 中对应的
 *      SqlNode 结构体，并通过 ParsedSqlResult 收集；
 *   2. 表达式（算术/比较/布尔/聚合）在语法阶段构造未绑定的 Expression 树，具体
 *      表/字段/类型解析留给 ResolveStage 的 ExpressionBinder；
 *   3. 错误处理使用 %define parse.error custom，由 yyreport_syntax_error 生成
 *      带行列位置与期望终结符集合的结构化错误消息；
 *   4. 自动补全复用同一 parser：在输入末尾追加非法哨兵触发错误，再用
 *      yypcontext_expected_tokens 取出光标处合法的终结符集合。
 */

%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log/log.h"
#include "common/lang/string.h"
#include "sql/parser/parse_defs.h"
#include "sql/parser/yacc_sql.hpp"
#include "sql/parser/lex_sql.h"
#include "sql/expr/expression.h"

using namespace std;

/**
 * @brief 依据位置区间从原始 SQL 文本中截取对应的词素文本
 * @param sql_string 原始 SQL 字符串
 * @param llocp      Bison 位置对象（含 first_column/last_column）
 * @return 对应的子串（用作表达式的显示名 name）
 * @details 实现原理：first_column 为 1-based，故直接以 first_column 作为偏移，
 * 长度为 last-first+1，本实现如此以匹配 lex_sql.l 的列号约定。
 */
string token_name(const char *sql_string, YYLTYPE *llocp)
{
  return string(sql_string + llocp->first_column, llocp->last_column - llocp->first_column + 1);
}

/**
 * @brief 传统 yyerror 入口：生成带行列定位的语法错误节点
 * @param llocp      错误位置
 * @param sql_string 原始 SQL 文本（本实现未直接使用）
 * @param sql_result 输出：追加一个 SCF_ERROR 节点
 * @param scanner    词法扫描器（未使用）
 * @param msg        bison 生成的错误消息（verbose 模式下含 unexpected/expecting 文本）
 * @return 恒为 0，表示错误已被处理
 * @details 实现原理：解析 bison 的 verbose 文案，拆出“实际遇到的符号”和“期望集合”，
 * 并统一改写为 “SyntaxError at line x, column y / unexpected token / expected” 的结构化
 * 消息写入 ErrorSqlNode。注意：%define parse.error custom 时实际由
 * yyreport_syntax_error 负责报错，本函数保留以兼容/兜底。
 */
int yyerror(YYLTYPE *llocp, const char *sql_string, ParsedSqlResult *sql_result, yyscan_t scanner, const char *msg)
{
  unique_ptr<ParsedSqlNode> error_sql_node = make_unique<ParsedSqlNode>(SCF_ERROR);

  // bison 在 %define parse.error verbose 下给出的消息形如：
  //   syntax error, unexpected ';', expecting IDENTIFIER or CONST or '(' or NOT
  // 这里把它整理为需求要求的结构：错误位置 + 实际符号 + 期望集合
  std::string detail(msg ? msg : "");
  std::string unexpected_token;
  std::string expected_set;

  const std::string unexpected_marker = "unexpected ";
  const std::string expecting_marker  = ", expecting ";

  auto upos = detail.find(unexpected_marker);
  if (upos != std::string::npos) {
    auto tstart = upos + unexpected_marker.size();
    auto tcomma = detail.find(',', tstart);
    if (tcomma == std::string::npos) {
      unexpected_token = detail.substr(tstart);
    } else {
      unexpected_token = detail.substr(tstart, tcomma - tstart);
    }
  }

  auto epos = detail.find(expecting_marker);
  if (epos != std::string::npos) {
    expected_set = detail.substr(epos + expecting_marker.size());
    // 把 bison 的 "A or B or C" 改写为 "A | B | C"
    const std::string or_word = " or ";
    std::string::size_type pos = 0;
    while ((pos = expected_set.find(or_word, pos)) != std::string::npos) {
      expected_set.replace(pos, or_word.size(), " | ");
      pos += 3;
    }
  }

  std::string message;
  message += "SyntaxError at line " + std::to_string(llocp->first_line) + ", column " +
             std::to_string(llocp->first_column) + "\n";
  if (!unexpected_token.empty()) {
    message += "unexpected token: " + unexpected_token + "\n";
  }
  if (!expected_set.empty()) {
    message += "expected: " + expected_set;
  } else if (unexpected_token.empty()) {
    message += detail;
  }

  error_sql_node->error.error_msg = message;
  error_sql_node->error.line      = llocp->first_line;
  error_sql_node->error.column    = llocp->first_column;
  sql_result->add_sql_node(std::move(error_sql_node));
  return 0;
}

/**
 * @brief 构造一个未绑定的算术表达式节点
 * @param type      算术类型（ADD/SUB/MUL/DIV/NEGATIVE）
 * @param left      左操作数（一元负号时为空）
 * @param right     右操作数（一元负号时为 nullptr）
 * @param sql_string 原始 SQL，用于设置表达式显示名
 * @param llocp     整个表达式的位置
 * @param op_locp   运算符本身的位置（用于精确的语义错误定位）
 * @return 新建的 ArithmeticExpr（所有权交给 bison 语义值）
 * @details 实现原理：new 出节点后设置 name 与 <行,列>；单独记录运算符位置是因为
 * 类型错误提示需要指向运算符而非整个表达式。
 */
ArithmeticExpr *create_arithmetic_expression(ArithmeticExpr::Type type,
                                             Expression *left,
                                             Expression *right,
                                             const char *sql_string,
                                             YYLTYPE *llocp,
                                             YYLTYPE *op_locp)
{
  ArithmeticExpr *expr = new ArithmeticExpr(type, left, right);
  expr->set_name(token_name(sql_string, llocp));
  // 记录运算符本身的位置，便于语义错误定位（如 "operator '+' cannot be applied..."）
  expr->set_location(op_locp->first_line, op_locp->first_column);
  return expr;
}

/**
 * @brief 构造一个未绑定的聚合表达式节点
 * @param aggregate_name 聚合函数名（SUM/AVG/COUNT/MAX/MIN 等，大小写不敏感）
 * @param child          聚合的子表达式（如 COUNT(*) 时为 StarExpr）
 * @param sql_string     原始 SQL，用于设置显示名
 * @param llocp          表达式位置
 * @return 新建的 UnboundAggregateExpr
 * @details 实现原理：此处只记录名字与子表达式，具体聚合类型解析与合法性校验
 * （见 ExpressionBinder::bind_aggregate_expression）留到语义阶段完成。
 */
UnboundAggregateExpr *create_aggregate_expression(const char *aggregate_name,
                                           Expression *child,
                                           const char *sql_string,
                                           YYLTYPE *llocp)
{
  UnboundAggregateExpr *expr = new UnboundAggregateExpr(aggregate_name, child);
  expr->set_name(token_name(sql_string, llocp));
  expr->set_location(llocp->first_line, llocp->first_column);
  return expr;
}

/**
 * @brief 为表达式同时记录显示名与 <行,列> 位置
 * @param expr       目标表达式（允许为 nullptr）
 * @param sql_string 原始 SQL，用于截取词素作为名字
 * @param llocp      表达式位置
 * @return 传入的 expr（便于在语义动作里链式使用）
 * @details 实现原理：空指针直接返回；否则用 token_name 取词素设 name，并保存位置，
 * 供后续 ExpressionBinder 生成带定位的语义错误。
 */
// 为表达式同时记录名字与位置（行列号）
static Expression *set_expr_name_and_location(Expression *expr, const char *sql_string, YYLTYPE *llocp)
{
  if (expr != nullptr) {
    expr->set_name(token_name(sql_string, llocp));
    expr->set_location(llocp->first_line, llocp->first_column);
  }
  return expr;
}

/**
 * @brief 将带引号的字符串常量还原为原始内容并处理转义
 * @param quoted 含首尾引号的原始字符串（来自 lex_sql.l 的 SSS）
 * @param len    quoted 的长度
 * @return 新分配的、已反转义的字符串（调用方负责 free）
 * @details 实现原理：跳过首尾引号逐字符扫描；`\x` 还原为 `x`；`''`/`""` 这类
 * 连续相同引号还原为单个引号；其余原样复制。长度不足 2 时直接拷贝。
 * 处理规则：单引号串 '' -> '，双引号串 "" -> "，通用 \\ -> \、\x -> x。
 */
// 将带引号的字符串常量还原为原始内容，并处理转义：
//   单引号串：'' -> '，双引号串："" -> "，通用：\\ -> \，\x -> x
char *unescape_quoted_string(char *quoted, int len)
{
  if (len < 2) {
    return common::substr(quoted, 0, len);
  }
  const char quote = quoted[0];
  char      *out   = (char *)malloc(len + 1);
  int        o     = 0;
  for (int i = 1; i < len - 1; i++) {
    if (quoted[i] == '\\' && i + 1 < len - 1) {
      out[o++] = quoted[i + 1];
      i++;
    } else if (quoted[i] == quote && i + 1 < len - 1 && quoted[i + 1] == quote) {
      out[o++] = quote;
      i++;
    } else {
      out[o++] = quoted[i];
    }
  }
  out[o] = '\0';
  return out;
}

/**
 * @brief 向 JOIN 子句列表追加一个 JOIN 节点
 * @param join_list     已有的 JOIN 列表；为 nullptr 时新建
 * @param relation_name 本次 JOIN 的表名（所有权转移给节点）
 * @param condition     ON 条件表达式（所有权转移给节点）
 * @return 追加后的列表指针（可能为新对象）
 * @details 实现原理：列表为空则 new 一个 vector，随后 emplace_back 得到新节点，
 * 转移 relation_name 与 condition（unique_ptr::reset）。支持普通 JOIN 与 INNER JOIN。
 */
// 向 join_clause 累积列表追加一个 JOIN 子句（支持 JOIN / INNER JOIN）
vector<JoinSqlNode> *append_join_node(vector<JoinSqlNode> *join_list, char *relation_name, Expression *condition)
{
  if (join_list == nullptr) {
    join_list = new vector<JoinSqlNode>;
  }
  join_list->emplace_back();
  JoinSqlNode &node = join_list->back();
  node.relation_name = relation_name;
  node.condition.reset(condition);
  return join_list;
}

%}

/* 生成可重入（pure）解析器，状态与参数显式传递，不使用全局变量 */
%define api.pure full
/*
 * %define parse.error custom：不使用 bison 默认的 "syntax error" 文案，而是要求
 * 我们实现 yyreport_syntax_error()。它的关键价值是回调参数 yypcontext_t 提供了
 * yypcontext_expected_tokens()，能直接拿到“当前状态期望的终结符集合”，
 * 既用于生成更友好的错误提示，也被自动补全复用（见文件末尾 collect_expected_tokens）。
 */
%define parse.error custom
/** 启用位置标识 **/
%locations
%lex-param { yyscan_t scanner }
/** 这些定义了在yyparse函数中的参数 **/
%parse-param { const char * sql_string }
%parse-param { ParsedSqlResult * sql_result }
%parse-param { void * scanner }

//标识tokens
%token  SEMICOLON
        BY
        CREATE
        DROP
        GROUP
        ORDER
        TABLE
        TABLES
        INDEX
        CALC
        SELECT
        ASC
        DESC
        SHOW
        SYNC
        INSERT
        DELETE
        UPDATE
        LBRACE
        RBRACE
        COMMA
        TRX_BEGIN
        TRX_COMMIT
        TRX_ROLLBACK
        INT_T
        STRING_T
        FLOAT_T
        VECTOR_T
        DATE
        HELP
        EXIT
        DOT //QUOTE
        INTO
        VALUES
        FROM
        WHERE
        AND
        SET
        ON
        JOIN
        INNER
        LOAD
        DATA
        INFILE
        EXPLAIN
        STORAGE
        FORMAT
        PRIMARY
        KEY
        ANALYZE
        FIELDS
        TERMINATED
        ENCLOSED
        EQ
        LT
        GT
        LE
        GE
        NE
        IS
        NULL_T

/**
 * %union 定义非终结符/终结符可携带的语义值类型，生成代码中即为 C 联合体。
 * 因此成员只能是 POD（或指针）类型，vector/unique_ptr 等只能以指针形式出现，
 * 所有权在语义动作中手动管理。
 */
/** union 中定义各种数据类型，真实生成的代码也是union类型，所以不能有非POD类型的数据 **/
%union {
  ParsedSqlNode *                            sql_node;
  ConditionSqlNode *                         condition;
  Value *                                    value;
  enum CompOp                                comp;
  RelAttrSqlNode *                           rel_attr;
  vector<AttrInfoSqlNode> *                  attr_infos;
  AttrInfoSqlNode *                          attr_info;
  Expression *                               expression;
  vector<unique_ptr<Expression>> *           expression_list;
  OrderByUnit *                              order_by_unit;
  vector<OrderByUnit> *                      order_by_list;
  vector<Value> *                            value_list;
  vector<ConditionSqlNode> *                 condition_list;
  vector<RelAttrSqlNode> *                   rel_attr_list;
  vector<string> *                           relation_list;
  vector<JoinSqlNode> *                      join_list;
  vector<string> *                           key_list;
  char *                                     cstring;
  int                                        number;
  float                                      floats;
}

/*
 * %destructor：当语法错误发生、bison 丢弃已入栈的符号时，对指针型语义值释放内存，
 * 避免解析失败路径上的内存泄漏。以下为各类指针语义值注册回收动作。
 */
%destructor { delete $$; } <condition>
%destructor { delete $$; } <value>
%destructor { delete $$; } <rel_attr>
%destructor { delete $$; } <attr_infos>
%destructor { delete $$; } <expression>
%destructor { delete $$; } <expression_list>
%destructor { delete $$; } <order_by_unit>
%destructor { delete $$; } <order_by_list>
%destructor { delete $$; } <value_list>
%destructor { delete $$; } <condition_list>
// %destructor { delete $$; } <rel_attr_list>
%destructor { delete $$; } <relation_list>
%destructor { delete $$; } <join_list>
%destructor { delete $$; } <key_list>

/* 带语义值的终结符：数字/浮点常量携带数值，标识符/字符串携带 cstring 指针 */
%token <number> NUMBER
%token <floats> FLOAT
%token <cstring> ID
%token <cstring> SSS
//非终结符

/** type 定义了各种解析后的结果输出的是什么类型。类型对应了 union 中的定义的成员变量名称 **/
%type <number>              type
%type <condition>           condition
%type <value>               value
%type <number>              number
%type <cstring>             relation
%type <comp>                comp_op
%type <rel_attr>            rel_attr
%type <attr_infos>          attr_def_list
%type <attr_info>           attr_def
%type <number>              null_def
%type <value_list>          value_list
%type <condition_list>      where
%type <condition_list>      condition_list
%type <cstring>             storage_format
%type <key_list>            primary_key
%type <key_list>            attr_list
%type <relation_list>       rel_list
%type <join_list>           join_clause
%type <expression>          expression
%type <expression>          boolean_expr
%type <expression>          comparison_predicate
%type <expression>          aggregate_expression
%type <expression>          select_where
%type <expression_list>     expression_list
%type <expression_list>     group_by
%type <order_by_unit>       order_by_unit
%type <order_by_list>       order_by_list
%type <order_by_list>       order_by
%type <cstring>             fields_terminated_by
%type <cstring>             enclosed_by
%type <sql_node>            calc_stmt
%type <sql_node>            select_stmt
%type <sql_node>            insert_stmt
%type <sql_node>            update_stmt
%type <sql_node>            delete_stmt
%type <sql_node>            create_table_stmt
%type <sql_node>            drop_table_stmt
%type <sql_node>            analyze_table_stmt
%type <sql_node>            show_tables_stmt
%type <sql_node>            desc_table_stmt
%type <sql_node>            create_index_stmt
%type <sql_node>            drop_index_stmt
%type <sql_node>            sync_stmt
%type <sql_node>            begin_stmt
%type <sql_node>            commit_stmt
%type <sql_node>            rollback_stmt
%type <sql_node>            load_data_stmt
%type <sql_node>            explain_stmt
%type <sql_node>            set_variable_stmt
%type <sql_node>            help_stmt
%type <sql_node>            exit_stmt
%type <sql_node>            command_wrapper
%type <sql_node>            commands

/*
 * 算术优先级（越靠后越紧）：加减 < 乘除 < 一元负号。
 * 这些声明同时解决 `a - b - c` 等表达式的二义性（默认左结合）。
 */
%left '+' '-'
%left '*' '/'
%right UMINUS
/* 布尔逻辑优先级（越靠后越紧）：OR < AND < 比较 < NOT */
%left OR
%left AND
%left LT GT LE GE EQ NE
%precedence NOT
%%

/*
 * 顶层规则：一条或多条语句。
 * 空产生式允许空输入（避免仅空白/注释时报错）；每归约一条命令就把它追加到
 * ParsedSqlResult，从而天然支持以分号分隔的多语句输入。
 */
commands: /* empty */                   { /* 允许空输入，便于多语句解析 */ }
    | commands command_wrapper opt_semicolon
  {
    unique_ptr<ParsedSqlNode> sql_node = unique_ptr<ParsedSqlNode>($2);
    sql_result->add_sql_node(std::move(sql_node));
  }
  ;

/*
 * 所有语句类型的统一入口，按 token 首关键字分派到具体语句规则；
 * 各规则构造对应 ParsedSqlNode（见 parse_defs.h）。
 */
command_wrapper:
    calc_stmt
  | select_stmt
  | insert_stmt
  | update_stmt
  | delete_stmt
  | create_table_stmt
  | drop_table_stmt
  | analyze_table_stmt
  | show_tables_stmt
  | desc_table_stmt
  | create_index_stmt
  | drop_index_stmt
  | sync_stmt
  | begin_stmt
  | commit_stmt
  | rollback_stmt
  | load_data_stmt
  | explain_stmt
  | set_variable_stmt
  | help_stmt
  | exit_stmt
    ;

/* EXIT：退出客户端，构建仅含 flag 的 SCF_EXIT 节点 */
exit_stmt:      
    EXIT {
      (void)yynerrs;  // 这么写为了消除yynerrs未使用的告警。如果你有更好的方法欢迎提PR
      $$ = new ParsedSqlNode(SCF_EXIT);
    };

/* HELP：打印帮助，构建 SCF_HELP 节点（无附加字段） */
help_stmt:
    HELP {
      $$ = new ParsedSqlNode(SCF_HELP);
    };

/* SYNC：触发一次落盘同步，构建 SCF_SYNC 节点 */
sync_stmt:
    SYNC {
      $$ = new ParsedSqlNode(SCF_SYNC);
    }
    ;

/* BEGIN：开启事务，构建 SCF_BEGIN 节点（事务语义在执行层实现） */
begin_stmt:
    TRX_BEGIN  {
      $$ = new ParsedSqlNode(SCF_BEGIN);
    }
    ;

/* COMMIT：提交事务，构建 SCF_COMMIT 节点 */
commit_stmt:
    TRX_COMMIT {
      $$ = new ParsedSqlNode(SCF_COMMIT);
    }
    ;

/* ROLLBACK：回滚事务，构建 SCF_ROLLBACK 节点 */
rollback_stmt:
    TRX_ROLLBACK  {
      $$ = new ParsedSqlNode(SCF_ROLLBACK);
    }
    ;

/* DROP TABLE 表名：构建 SCF_DROP_TABLE 与 DropTableSqlNode */
drop_table_stmt:    /*drop table 语句的语法解析树*/
    DROP TABLE ID {
      $$ = new ParsedSqlNode(SCF_DROP_TABLE);
      $$->drop_table.relation_name = $3;
    };

/* ANALYZE TABLE 表名：构建 SCF_ANALYZE_TABLE 与 AnalyzeTableSqlNode */
analyze_table_stmt:  /* analyze table 语法的语法解析树*/
    ANALYZE TABLE ID {
      $$ = new ParsedSqlNode(SCF_ANALYZE_TABLE);
      $$->analyze_table.relation_name = $3;
    }
    ;

/* SHOW TABLES：列出当前库所有表，构建 SCF_SHOW_TABLES 节点 */
show_tables_stmt:
    SHOW TABLES {
      $$ = new ParsedSqlNode(SCF_SHOW_TABLES);
    }
    ;

/* DESC 表名：查看表结构，构建 SCF_DESC_TABLE 与 DescTableSqlNode */
desc_table_stmt:
    DESC ID  {
      $$ = new ParsedSqlNode(SCF_DESC_TABLE);
      $$->desc_table.relation_name = $2;
    }
    ;

/*
 * CREATE INDEX 索引名 ON 表名 ( 字段名 )：
 * 构建 SCF_CREATE_INDEX 与 CreateIndexSqlNode{index_name, relation_name, attribute_name}
 */
create_index_stmt:    /*create index 语句的语法解析树*/
    CREATE INDEX ID ON ID LBRACE ID RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $3;
      create_index.relation_name = $5;
      create_index.attribute_name = $7;
    }
    ;

/* DROP INDEX 索引名 ON 表名：构建 SCF_DROP_INDEX 与 DropIndexSqlNode */
drop_index_stmt:      /*drop index 语句的语法解析树*/
    DROP INDEX ID ON ID
    {
      $$ = new ParsedSqlNode(SCF_DROP_INDEX);
      $$->drop_index.index_name = $3;
      $$->drop_index.relation_name = $5;
    }
    ;
/*
 * CREATE TABLE 表名 ( 字段定义列表 [主键] ) [storage format=...]：
 * 构建 SCF_CREATE_TABLE 与 CreateTableSqlNode{relation_name, attr_infos,
 * primary_keys, storage_format}。$5 属性列表/$6 主键列表以 swap 接管后删除临时容器。
 */
create_table_stmt:    /*create table 语句的语法解析树*/
    CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE storage_format
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      //free($3);

      create_table.attr_infos.swap(*$5);
      delete $5;

      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      if ($8 != nullptr) {
        create_table.storage_format = $8;
      }
    }
    ;
    
/* 字段定义列表：一个或多个 attr_def，逗号分隔，累积为 vector<AttrInfoSqlNode> */
attr_def_list:
    attr_def
    {
      $$ = new vector<AttrInfoSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | attr_def_list COMMA attr_def
    {
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
    
/*
 * 单个字段定义：ID 类型 (长度) [NULL|NOT NULL]。
 * 带长度时使用指定长度；省略长度时默认 4。构建 AttrInfoSqlNode。
 */
attr_def:
    ID type LBRACE number RBRACE null_def
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = $4;
      $$->nullable = ($6 != 0);
    }
    | ID type null_def
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = 4;
      $$->nullable = ($3 != 0);
    }
    ;
/* NULL 约束：空/NULL 都表示允许为 NULL（1），NOT NULL 不允许（0） */
null_def:
    /* empty */
    {
      $$ = 1;  // 默认允许 NULL
    }
    | NULL_T
    {
      $$ = 1;
    }
    | NOT NULL_T
    {
      $$ = 0;
    }
    ;
/* 字段长度：仅接受正整数常量 */
number:
    NUMBER {$$ = $1;}
    ;
/* 字段类型：把类型 token 映射为 AttrType 的整型值传入 %union.number */
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    | DATE     { $$ = static_cast<int>(AttrType::DATES); }
    ;
/* 主键子句：[ , PRIMARY KEY (列...) ]；缺省为空（nullptr），否则取 attr_list */
primary_key:
    /* empty */
    {
      $$ = nullptr;
    }
    | COMMA PRIMARY KEY LBRACE attr_list RBRACE
    {
      $$ = $5;
    }
    ;

/* 主键列名列表：ID 序列，逗号分隔，构建 vector<string>（保持书写顺序） */
attr_list:
    ID {
      $$ = new vector<string>();
      $$->push_back($1);
    }
    | ID COMMA attr_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }

      $$->insert($$->begin(), $1);
    }
    ;

/*
 * INSERT 语句两种写法：
 *   1) INSERT INTO 表 VALUES (值列表)：按表字段顺序插入，columns 为空；
 *   2) INSERT INTO 表 (列列表) VALUES (值列表)：指定目标列。
 * 均构建 SCF_INSERT 与 InsertSqlNode{relation_name, columns, values}。
 */
insert_stmt:        /*insert   语句的语法解析树*/
    INSERT INTO ID VALUES LBRACE value_list RBRACE 
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      $$->insertion.values.swap(*$6);
      delete $6;
    }
    | INSERT INTO ID LBRACE attr_list RBRACE VALUES LBRACE value_list RBRACE
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      $$->insertion.columns.swap(*$5);
      delete $5;
      $$->insertion.values.swap(*$9);
      delete $9;
    }
    ;

/* 值列表：逗号分隔的 value 序列，累积为 vector<Value> */
value_list:
    value
    {
      $$ = new vector<Value>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | value_list COMMA value { 
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
/*
 * 单个值：整数 -> Value(int)，浮点 -> Value(float)，字符串 -> 反转义后的 Value(char*)，
 * NULL -> 空 Value 并 set_null()。@$ = @1 用于把词素位置传给值。
 */
value:
    NUMBER {
      $$ = new Value((int)$1);
      @$ = @1;
    }
    |FLOAT {
      $$ = new Value((float)$1);
      @$ = @1;
    }
    |SSS {
      char *tmp = unescape_quoted_string($1, strlen($1));
      $$ = new Value(tmp);
      free(tmp);
    }
    | NULL_T {
      $$ = new Value();
      $$->set_null();
      @$ = @1;
    }
    ;
/* 建表时的存储格式扩展：[ STORAGE FORMAT = ID ]；缺省为 nullptr */
storage_format:
    /* empty */
    {
      $$ = nullptr;
    }
    | STORAGE FORMAT EQ ID
    {
      $$ = $4;
    }
    ;
    
/* DELETE FROM 表 [WHERE 条件]：构建 SCF_DELETE 与 DeleteSqlNode{relation_name, conditions} */
delete_stmt:    /*  delete 语句的语法解析树*/
    DELETE FROM ID where 
    {
      $$ = new ParsedSqlNode(SCF_DELETE);
      $$->deletion.relation_name = $3;
      if ($4 != nullptr) {
        $$->deletion.conditions.swap(*$4);
        delete $4;
      }
    }
    ;
/*
 * UPDATE 表 SET 字段 = 值 [WHERE 条件]：
 * 构建 SCF_UPDATE 与 UpdateSqlNode{relation_name, attribute_name, value, conditions}，
 * 注意 value 采用拷贝赋值（*$6）。
 */
update_stmt:      /*  update 语句的语法解析树*/
    UPDATE ID SET ID EQ value where 
    {
      $$ = new ParsedSqlNode(SCF_UPDATE);
      $$->update.relation_name = $2;
      $$->update.attribute_name = $4;
      $$->update.value = *$6;
      if ($7 != nullptr) {
        $$->update.conditions.swap(*$7);
        delete $7;
      }
    }
    ;
/*
 * SELECT 查询：SELECT 表达式列表 FROM 表列表 [JOIN...] [WHERE 布尔表达式]
 * [GROUP BY ...] [ORDER BY ...]。
 * 构建 SCF_SELECT 与 SelectSqlNode{expressions, relations, joins,
 * where_expression, group_by, order_by}。各可选子句返回 nullptr 时跳过，
 * 非空则用 swap/reset 接管临时容器后再删除。
 */
select_stmt:        /*  select 语句的语法解析树*/
    SELECT expression_list FROM rel_list join_clause select_where group_by order_by
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.swap(*$4);
        delete $4;
      }

      if ($5 != nullptr) {
        $$->selection.joins.swap(*$5);
        delete $5;
      }

      if ($6 != nullptr) {
        $$->selection.where_expression.reset($6);
      }

      if ($7 != nullptr) {
        $$->selection.group_by.swap(*$7);
        delete $7;
      }

      if ($8 != nullptr) {
        $$->selection.order_by.swap(*$8);
        delete $8;
      }
    }
    ;
/* CALC 表达式列表：不查表、直接计算并返回结果，构建 SCF_CALC 与 CalcSqlNode */
calc_stmt:
    CALC expression_list
    {
      $$ = new ParsedSqlNode(SCF_CALC);
      $$->calc.expressions.swap(*$2);
      delete $2;
    }
    ;

/* 表达式列表：逗号分隔，构建 vector<unique_ptr<Expression>>（递归时头插保持顺序） */
expression_list:
    expression
    {
      $$ = new vector<unique_ptr<Expression>>;
      $$->emplace_back($1);
    }
    | expression COMMA expression_list
    {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<unique_ptr<Expression>>;
      }
      $$->emplace($$->begin(), $1);
    }
    ;
/*
 * 算术/基础表达式：双目 + - * /（按 %left/%right 声明的优先级与结合性归约）、
 * 括号、一元负号、`*` 通配、常量、字段引用与聚合函数。
 * 依赖 create_arithmetic_expression/set_expr_name_and_location 构造未绑定表达式树。
 */
expression:
    expression '+' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::ADD, $1, $3, sql_string, &@$, &@2);
    }
    | expression '-' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::SUB, $1, $3, sql_string, &@$, &@2);
    }
    | expression '*' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::MUL, $1, $3, sql_string, &@$, &@2);
    }
    | expression '/' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::DIV, $1, $3, sql_string, &@$, &@2);
    }
    | LBRACE expression RBRACE {
      $$ = $2;
      $$->set_name(token_name(sql_string, &@$));
    }
    | '-' expression %prec UMINUS {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::NEGATIVE, $2, nullptr, sql_string, &@$, &@1);
    }
    | '*' {
      $$ = new StarExpr();
    }
    | value {
      $$ = set_expr_name_and_location(new ValueExpr(*$1), sql_string, &@$);
      delete $1;
    }
    | rel_attr {
      RelAttrSqlNode *node = $1;
      $$ = set_expr_name_and_location(new UnboundFieldExpr(node->relation_name, node->attribute_name), sql_string, &@$);
      delete $1;
    }
    | aggregate_expression {
      $$ = $1;
    }
    ;

/* 聚合函数调用：函数名 ( 表达式 )，构建未绑定的 UnboundAggregateExpr */
aggregate_expression:
    ID LBRACE expression RBRACE {
      $$ = create_aggregate_expression($1, $3, sql_string, &@$);
    }
    ;

/* 字段引用：`列名` 或 `表名.列名`，构建 RelAttrSqlNode */
rel_attr:
    ID {
      $$ = new RelAttrSqlNode;
      $$->attribute_name = $1;
    }
    | ID DOT ID {
      $$ = new RelAttrSqlNode;
      $$->relation_name  = $1;
      $$->attribute_name = $3;
    }
    ;

/* 单个表名：直接把 ID 字符串作为语义值 */
relation:
    ID {
      $$ = $1;
    }
    ;
/* 表名列表：逗号分隔，构建 vector<string>（递归尾接后头插，保持书写顺序） */
rel_list:
    relation {
      $$ = new vector<string>();
      $$->push_back($1);
    }
    | relation COMMA rel_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }

      $$->insert($$->begin(), $1);
    }
    ;

/*
 * JOIN 子句（可空，可连续出现多个）：
 *   [JOIN|INNER JOIN 表 ON 布尔条件]...
 * 每遇到一个 JOIN 就用 append_join_node 追加 JoinSqlNode{relation_name, condition}。
 */
join_clause:
    /* empty */
    {
      $$ = nullptr;
    }
    | join_clause JOIN relation ON boolean_expr
    {
      $$ = append_join_node($1, $3, $5);
    }
    | join_clause INNER JOIN relation ON boolean_expr
    {
      $$ = append_join_node($1, $4, $6);
    }
    ;

/*
 * DELETE/UPDATE 使用的传统 WHERE：由 condition_list（AND 连接的简单比较）承载，
 * 产物是 vector<ConditionSqlNode>，缺省为 nullptr。
 */
where:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE condition_list {
      $$ = $2;  
    }
    ;

/* SELECT 使用的 WHERE：直接构建可执行的布尔表达式（支持 AND/OR/NOT/括号/算术） */
select_where:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE boolean_expr {
      $$ = $2;
      $$->set_name(token_name(sql_string, &@$));
    }
    ;

/*
 * 布尔表达式：支持 OR / AND（优先级由声明保证）、NOT、括号与比较谓词。
 * 归约结果都是已可执行的 Expression 子树（比较用 ComparisonExpr，逻辑用
 * ConjunctionExpr），字段/常量在绑定阶段再解析。
 */
boolean_expr:
    /* 逻辑或：合并为 ConjunctionExpr(OR) */
    boolean_expr OR boolean_expr {
      vector<unique_ptr<Expression>> children;
      children.emplace_back($1);
      children.emplace_back($3);
      $$ = set_expr_name_and_location(new ConjunctionExpr(ConjunctionExpr::Type::OR, children), sql_string, &@$);
    }
    | boolean_expr AND boolean_expr {
      vector<unique_ptr<Expression>> children;
      children.emplace_back($1);
      children.emplace_back($3);
      $$ = set_expr_name_and_location(new ConjunctionExpr(ConjunctionExpr::Type::AND, children), sql_string, &@$);
    }
    | NOT boolean_expr {
      // NOT expr 表示为 (expr == FALSE)，expr 为布尔表达式，结果为 0/1
      ValueExpr *const_false = new ValueExpr(Value(false));
      $$ = set_expr_name_and_location(new ComparisonExpr(EQUAL_TO, unique_ptr<Expression>($2), unique_ptr<Expression>(const_false)), sql_string, &@$);
    }
    | LBRACE boolean_expr RBRACE {
      $$ = set_expr_name_and_location($2, sql_string, &@$);
    }
    | comparison_predicate {
      $$ = $1;
    }
    ;

/*
 * 比较谓词：表达式 比较符 表达式，或 表达式 IS [NOT] NULL。
 * 统一构建 ComparisonExpr，IS NULL 系列用空值常量作为右操作数占位。
 */
comparison_predicate:
    expression comp_op expression {
      $$ = set_expr_name_and_location(new ComparisonExpr($2, unique_ptr<Expression>($1), unique_ptr<Expression>($3)), sql_string, &@$);
    }
    | expression IS NULL_T {
      $$ = set_expr_name_and_location(new ComparisonExpr(IS_NULL, unique_ptr<Expression>($1), unique_ptr<Expression>(new ValueExpr(Value::null_value()))), sql_string, &@$);
    }
    | expression IS NOT NULL_T {
      $$ = set_expr_name_and_location(new ComparisonExpr(IS_NOT_NULL, unique_ptr<Expression>($1), unique_ptr<Expression>(new ValueExpr(Value::null_value()))), sql_string, &@$);
    }
    ;
/*
 * 传统条件列表：AND 连接的 condition（用于 DELETE/UPDATE 的 where），
 * 构建 vector<ConditionSqlNode>。
 */
condition_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | condition {
      $$ = new vector<ConditionSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | condition AND condition_list {
      $$ = $3;
      $$->emplace_back(*$1);
      delete $1;
    }
    ;
/*
 * 单个条件：左/右操作数均可为字段(rel_attr)或常量(value)，四种子组合分别设置
 * left_is_attr / right_is_attr，构建 ConditionSqlNode{comp, 左右属性与值}。
 */
condition:
    rel_attr comp_op value
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | value comp_op value 
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_value = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | rel_attr comp_op rel_attr
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 1;
      $$->right_attr = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | value comp_op rel_attr
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_value = *$1;
      $$->right_is_attr = 1;
      $$->right_attr = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    ;

/* 比较运算符 token 到 CompOp 枚举的映射（IS NULL/IS NOT NULL 在谓词规则中直接指定） */
comp_op:
      EQ { $$ = EQUAL_TO; }
    | LT { $$ = LESS_THAN; }
    | GT { $$ = GREAT_THAN; }
    | LE { $$ = LESS_EQUAL; }
    | GE { $$ = GREAT_EQUAL; }
    | NE { $$ = NOT_EQUAL; }
    ;

// your code here
/* GROUP BY 子句（可空）：GROUP BY 表达式列表，直接复用 expression_list */
group_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | GROUP BY expression_list
    {
      // group by 的表达式范围与select查询值的表达式范围是不同的，比如group by不支持 *
      // 但是这里没有处理。
      $$ = $3;
    }
    ;
/* ORDER BY 子句（可空）：ORDER BY 排序项列表 */
order_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ORDER BY order_by_list
    {
      $$ = $3;
    }
    ;
/*
 * 单个排序项：表达式 [ASC|DESC]。每个排序项独立携带方向，以正确表达多键混合升降序。
 */
order_by_unit:
    expression
    {
      // 未显式指定方向时遵循 SQL 默认语义：按 ASC 排序。
      $$ = new OrderByUnit;
      $$->expression.reset($1);
      $$->direction = OrderDirection::ASC;
    }
    | expression ASC
    {
      $$ = new OrderByUnit;
      $$->expression.reset($1);
      $$->direction = OrderDirection::ASC;
    }
    | expression DESC
    {
      $$ = new OrderByUnit;
      $$->expression.reset($1);
      $$->direction = OrderDirection::DESC;
    }
    ;
/* 排序项列表：逗号分隔，构建 vector<OrderByUnit>（用 move 转移表达式所有权） */
order_by_list:
    order_by_unit
    {
      $$ = new vector<OrderByUnit>;
      $$->emplace_back(std::move(*$1));
      delete $1;
    }
    | order_by_list COMMA order_by_unit
    {
      $$ = $1;
      $$->emplace_back(std::move(*$3));
      delete $3;
    }
    ;
/*
 * LOAD DATA INFILE '文件' INTO TABLE 表 [FIELDS TERMINATED BY s] [ENCLOSED BY s]：
 * 构建 SCF_LOAD_DATA 与 LoadDataSqlNode{relation_name, file_name, terminated, enclosed}。
 * 文件名的首尾引号用 substr 去掉。
 */
load_data_stmt:
    LOAD DATA INFILE SSS INTO TABLE ID fields_terminated_by enclosed_by
    {
      char *tmp_file_name = common::substr($4, 1, strlen($4) - 2);
      
      $$ = new ParsedSqlNode(SCF_LOAD_DATA);
      $$->load_data.relation_name = $7;
      $$->load_data.file_name = tmp_file_name;
      if ($8 != nullptr) {
        char *tmp = common::substr($8,1,strlen($8)-2);
        $$->load_data.terminated = $8;
        free(tmp);
      }
      if ($9 != nullptr) {
        char *tmp = common::substr($9,1,strlen($9)-2);
        $$->load_data.enclosed = $9;
        free(tmp);
      }
      free(tmp_file_name);
    }
    ;

/* LOAD DATA 的字段分隔符：[FIELDS TERMINATED BY '字符串']，缺省 nullptr */
fields_terminated_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | FIELDS TERMINATED BY SSS
    {
      $$ = $4;
    };

/* LOAD DATA 的字段包围符：[ENCLOSED BY '字符串']，缺省 nullptr */
enclosed_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ENCLOSED BY SSS
    {
      $$ = $3;
    };

/*
 * EXPLAIN 语句：在任意可执行语句前加 EXPLAIN，得到 SCF_EXPLAIN，
 * 其内嵌 sql_node 为被解释的语句（用于输出执行计划）。
 */
explain_stmt:
    EXPLAIN command_wrapper
    {
      $$ = new ParsedSqlNode(SCF_EXPLAIN);
      $$->explain.sql_node = unique_ptr<ParsedSqlNode>($2);
    }
    ;

/* SET 变量 = 值：构建 SCF_SET_VARIABLE 与 SetVariableSqlNode{name, value} */
set_variable_stmt:
    SET ID EQ value
    {
      $$ = new ParsedSqlNode(SCF_SET_VARIABLE);
      $$->set_variable.name  = $2;
      $$->set_variable.value = *$4;
      delete $4;
    }
    ;

/* 可选的语句结束分号：作为 command_wrapper 之后的独立可选产生式 */
opt_semicolon: /*empty*/
    | SEMICOLON
    ;
%%
//_____________________________________________________________________
// 声明在 lex_sql.l 中实现、用于把字符串设为扫描输入的辅助函数
extern void scan_string(const char *str, yyscan_t scanner);

#include "sql/parser/expected_tokens.h"

/*
 * 自动补全与错误报告共用同一套线程本地状态。
 * 当 g_expected_collecting 为 true 时表示当前处于“收集期望 token”模式：
 *   - 由 collect_expected_tokens 在 SQL 前缀末尾追加非法哨兵字符触发一次语法错误；
 *   - yyreport_syntax_error 被 bison 调用后，仅通过 yypcontext_expected_tokens
 *     回填光标处合法的终结符集合，而不再生成错误节点；
 *   - 只认第一次（哨兵处）的错误，忽略 bison 后续的错误恢复。
 * 正常报错路径下 g_expected_collecting 为 false，则生成结构化 SyntaxError 节点。
 * 这些变量用 thread_local，保证多会话并发解析时互不干扰。
 */
// %define parse.error custom 时由 bison 调用，可拿到完整的期望符号集合
namespace {
// 自动补全：以线程本地槽位收集光标处的期望符号，避免与正常报错路径相互影响
thread_local bool                       g_expected_collecting = false;
thread_local bool                       g_expected_reported   = false;
thread_local std::vector<std::string>  *g_expected_tokens     = nullptr;
thread_local int                        g_expected_line       = 0;
thread_local int                        g_expected_column     = 0;
}  // namespace

/**
 * @brief %define parse.error custom 指定的语法错误回调
 * @param ctx        bison 提供的错误上下文，可用 yypcontext_token /
 *                   yypcontext_expected_tokens / yypcontext_location 查询
 * @param sql_string 原始 SQL（用于把无法识别的字符还原成可读文本）
 * @param sql_result 正常报错路径下追加 SCF_ERROR 节点
 * @param scanner    词法扫描器（未使用）
 * @return 恒为 0
 * @details 实现原理：
 *   1. 若处于自动补全收集模式，只记录首个错误的位置并通过
 *      yypcontext_expected_tokens 把期望终结符名写入线程本地列表，然后返回；
 *   2. 否则进入正常报错：检查实际遇到的符号；当 bison 给出 "invalid token"
 *      时，用位置从原串还原该字符（未闭合引号则提示 unterminated string literal）；
 *   3. 同样通过 yypcontext_expected_tokens 收集期望集合，拼成
 *      “SyntaxError at line/column + unexpected token + expected: A | B | C”；
 *   4. 追加一个 SCF_ERROR 节点，供 ParseStage 转成用户可见的语法错误。
 * 该函数是自动补全与诊断共享语法信息的核心，无需第二套 SQL parser。
 */
int yyreport_syntax_error(
    const yypcontext_t *ctx, const char *sql_string, ParsedSqlResult *sql_result, yyscan_t scanner)
{
  (void)scanner;

  const YYLTYPE *loc = yypcontext_location(ctx);

  // 自动补全模式：只回填期望符号集合，不生成错误节点
  if (g_expected_collecting) {
    // 只取第一次（光标哨兵处）的错误，忽略后续错误恢复
    if (g_expected_reported) {
      return 0;
    }
    g_expected_reported = true;
    g_expected_line     = loc->first_line;
    g_expected_column   = loc->first_column;
    if (g_expected_tokens != nullptr) {
      const int       token_max = 128;
      yysymbol_kind_t expected[token_max];
      int             n = yypcontext_expected_tokens(ctx, expected, token_max);
      if (n >= 0) {
        for (int i = 0; i < n; i++) {
          g_expected_tokens->emplace_back(yysymbol_name(expected[i]));
        }
      }
    }
    return 0;
  }

  std::string    unexpected_token;
  yysymbol_kind_t unexpected = yypcontext_token(ctx);
  if (unexpected != YYSYMBOL_YYEMPTY) {
    unexpected_token = yysymbol_name(unexpected);
    // 词法无法识别的字符不在语法符号表内，bison 会给出 "invalid token"，
    // 这里回填原始字符，并专门识别未闭合字符串，给出更清晰的原因。
    if (unexpected_token == "invalid token" && sql_string != nullptr && loc->first_column > 0) {
      size_t index = static_cast<size_t>(loc->first_column - 1);
      if (index < strlen(sql_string)) {
        char ch = sql_string[index];
        if (ch == '\'' || ch == '"') {
          unexpected_token = "unterminated string literal";
        } else {
          unexpected_token = std::string("'") + ch + "'";
        }
      }
    }
  }

  std::string expected_set;
  {
    const int       token_max = 64;
    yysymbol_kind_t expected[token_max];
    int             n = yypcontext_expected_tokens(ctx, expected, token_max);
    if (n >= 0) {
      for (int i = 0; i < n; i++) {
        if (i != 0) {
          expected_set += " | ";
        }
        expected_set += yysymbol_name(expected[i]);
      }
    }
  }

  std::string message = "SyntaxError at line " + std::to_string(loc->first_line) + ", column " +
                        std::to_string(loc->first_column) + "\n";
  if (!unexpected_token.empty()) {
    message += "unexpected token: " + unexpected_token + "\n";
  }
  if (!expected_set.empty()) {
    message += "expected: " + expected_set;
  }

  unique_ptr<ParsedSqlNode> error_sql_node = make_unique<ParsedSqlNode>(SCF_ERROR);
  error_sql_node->error.error_msg = message;
  error_sql_node->error.line      = loc->first_line;
  error_sql_node->error.column    = loc->first_column;
  sql_result->add_sql_node(std::move(error_sql_node));
  return 0;
}

/**
 * @brief 驱动一次完整的词法+语法分析
 * @param s          待解析的 SQL 字符串
 * @param sql_result 输出：解析得到的语句节点或错误节点
 * @return yyparse 的返回值（0 表示成功归约）
 * @details 实现原理：
 *   1. yylex_init_extra 创建可重入扫描器，并把一个 vector<char*> 作为 yyextra
 *      传入，用于登记 lex_sql.l 中 strdup 出来的标识符/字符串；
 *   2. scan_string 把输入切到扫描器缓冲区，yyparse 反复调用 yylex 完成 LALR 归约；
 *   3. 解析结束后统一 free yyextra 中登记的字符串，再销毁扫描器，避免内存泄漏。
 */
int sql_parse(const char *s, ParsedSqlResult *sql_result) {
  yyscan_t scanner;
  std::vector<char *> allocated_strings;
  yylex_init_extra(static_cast<void*>(&allocated_strings),&scanner);
  scan_string(s, scanner);
  int result = yyparse(s, sql_result, scanner);

  for (char *ptr : allocated_strings) {
    free(ptr);
  }
  allocated_strings.clear();

  yylex_destroy(scanner);
  return result;
}

/**
 * @brief 收集给定 SQL 前缀末尾处合法的终结符集合（用于自动补全）
 * @param sql    待分析的 SQL 前缀
 * @param tokens 输出：期望的终结符符号名列表
 * @param line   输出：语法错误（哨兵）所在行
 * @param column 输出：语法错误（哨兵）所在列
 * @return 期望符号个数；sql 为 nullptr 时返回 -1
 * @details 实现原理（哨兵技巧）：不新建 parser，而是直接复用 sql_parse 解析该前缀。
 * 由于前缀本身在末尾缺少后续 token，bison 会走到一个“期望更多输入”的状态；此时
 * 通过线程本地的 g_expected_collecting 置位，让 yyreport_syntax_error 只回填
 * yypcontext_expected_tokens 的结果，而不产生错误节点。收集完成后恢复线程本地状态，
 * 最后返回期望集合的大小。这样补全信息与语法诊断始终来自同一份 LALR 状态机。
 */
int collect_expected_tokens(const char *sql, std::vector<std::string> &tokens, int &line, int &column)
{
  tokens.clear();
  line   = 0;
  column = 0;
  if (sql == nullptr) {
    return -1;
  }

  ParsedSqlResult result;
  // 进入“仅收集期望 token”模式，并重置首次错误标记
  g_expected_tokens     = &tokens;
  g_expected_collecting = true;
  g_expected_reported   = false;
  sql_parse(sql, &result);
  // 收集完毕，恢复线程本地状态，避免影响后续正常解析
  g_expected_collecting = false;
  g_expected_reported   = false;
  g_expected_tokens     = nullptr;

  line   = g_expected_line;
  column = g_expected_column;
  return static_cast<int>(tokens.size());
}
