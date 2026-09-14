
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

string token_name(const char *sql_string, YYLTYPE *llocp)
{
  return string(sql_string + llocp->first_column, llocp->last_column - llocp->first_column + 1);
}

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

// 为表达式同时记录名字与位置（行列号）
static Expression *set_expr_name_and_location(Expression *expr, const char *sql_string, YYLTYPE *llocp)
{
  if (expr != nullptr) {
    expr->set_name(token_name(sql_string, llocp));
    expr->set_location(llocp->first_line, llocp->first_column);
  }
  return expr;
}

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

%define api.pure full
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

%left '+' '-'
%left '*' '/'
%right UMINUS
/* 布尔逻辑优先级（越靠后越紧）：OR < AND < 比较 < NOT */
%left OR
%left AND
%left LT GT LE GE EQ NE
%precedence NOT
%%

commands: /* empty */                   { /* 允许空输入，便于多语句解析 */ }
    | commands command_wrapper opt_semicolon
  {
    unique_ptr<ParsedSqlNode> sql_node = unique_ptr<ParsedSqlNode>($2);
    sql_result->add_sql_node(std::move(sql_node));
  }
  ;

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

exit_stmt:      
    EXIT {
      (void)yynerrs;  // 这么写为了消除yynerrs未使用的告警。如果你有更好的方法欢迎提PR
      $$ = new ParsedSqlNode(SCF_EXIT);
    };

help_stmt:
    HELP {
      $$ = new ParsedSqlNode(SCF_HELP);
    };

sync_stmt:
    SYNC {
      $$ = new ParsedSqlNode(SCF_SYNC);
    }
    ;

begin_stmt:
    TRX_BEGIN  {
      $$ = new ParsedSqlNode(SCF_BEGIN);
    }
    ;

commit_stmt:
    TRX_COMMIT {
      $$ = new ParsedSqlNode(SCF_COMMIT);
    }
    ;

rollback_stmt:
    TRX_ROLLBACK  {
      $$ = new ParsedSqlNode(SCF_ROLLBACK);
    }
    ;

drop_table_stmt:    /*drop table 语句的语法解析树*/
    DROP TABLE ID {
      $$ = new ParsedSqlNode(SCF_DROP_TABLE);
      $$->drop_table.relation_name = $3;
    };

analyze_table_stmt:  /* analyze table 语法的语法解析树*/
    ANALYZE TABLE ID {
      $$ = new ParsedSqlNode(SCF_ANALYZE_TABLE);
      $$->analyze_table.relation_name = $3;
    }
    ;

show_tables_stmt:
    SHOW TABLES {
      $$ = new ParsedSqlNode(SCF_SHOW_TABLES);
    }
    ;

desc_table_stmt:
    DESC ID  {
      $$ = new ParsedSqlNode(SCF_DESC_TABLE);
      $$->desc_table.relation_name = $2;
    }
    ;

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

drop_index_stmt:      /*drop index 语句的语法解析树*/
    DROP INDEX ID ON ID
    {
      $$ = new ParsedSqlNode(SCF_DROP_INDEX);
      $$->drop_index.index_name = $3;
      $$->drop_index.relation_name = $5;
    }
    ;
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
number:
    NUMBER {$$ = $1;}
    ;
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    | DATE     { $$ = static_cast<int>(AttrType::DATES); }
    ;
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
calc_stmt:
    CALC expression_list
    {
      $$ = new ParsedSqlNode(SCF_CALC);
      $$->calc.expressions.swap(*$2);
      delete $2;
    }
    ;

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

aggregate_expression:
    ID LBRACE expression RBRACE {
      $$ = create_aggregate_expression($1, $3, sql_string, &@$);
    }
    ;

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

relation:
    ID {
      $$ = $1;
    }
    ;
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

boolean_expr:
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

comp_op:
      EQ { $$ = EQUAL_TO; }
    | LT { $$ = LESS_THAN; }
    | GT { $$ = GREAT_THAN; }
    | LE { $$ = LESS_EQUAL; }
    | GE { $$ = GREAT_EQUAL; }
    | NE { $$ = NOT_EQUAL; }
    ;

// your code here
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

fields_terminated_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | FIELDS TERMINATED BY SSS
    {
      $$ = $4;
    };

enclosed_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ENCLOSED BY SSS
    {
      $$ = $3;
    };

explain_stmt:
    EXPLAIN command_wrapper
    {
      $$ = new ParsedSqlNode(SCF_EXPLAIN);
      $$->explain.sql_node = unique_ptr<ParsedSqlNode>($2);
    }
    ;

set_variable_stmt:
    SET ID EQ value
    {
      $$ = new ParsedSqlNode(SCF_SET_VARIABLE);
      $$->set_variable.name  = $2;
      $$->set_variable.value = *$4;
      delete $4;
    }
    ;

opt_semicolon: /*empty*/
    | SEMICOLON
    ;
%%
//_____________________________________________________________________
extern void scan_string(const char *str, yyscan_t scanner);

#include "sql/parser/expected_tokens.h"

// %define parse.error custom 时由 bison 调用，可拿到完整的期望符号集合
namespace {
// 自动补全：以线程本地槽位收集光标处的期望符号，避免与正常报错路径相互影响
thread_local bool                       g_expected_collecting = false;
thread_local bool                       g_expected_reported   = false;
thread_local std::vector<std::string>  *g_expected_tokens     = nullptr;
thread_local int                        g_expected_line       = 0;
thread_local int                        g_expected_column     = 0;
}  // namespace

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

int collect_expected_tokens(const char *sql, std::vector<std::string> &tokens, int &line, int &column)
{
  tokens.clear();
  line   = 0;
  column = 0;
  if (sql == nullptr) {
    return -1;
  }

  ParsedSqlResult result;
  g_expected_tokens     = &tokens;
  g_expected_collecting = true;
  g_expected_reported   = false;
  sql_parse(sql, &result);
  g_expected_collecting = false;
  g_expected_reported   = false;
  g_expected_tokens     = nullptr;

  line   = g_expected_line;
  column = g_expected_column;
  return static_cast<int>(tokens.size());
}
