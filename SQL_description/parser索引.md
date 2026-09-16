# CSUDB 编译器（parser）文件功能索引

> 在 VSCode / Cursor 中打开本文件并按 `Ctrl+Shift+V` 预览，点击链接可直接跳到源码对应行。

## `parse.cpp`

- [`ParsedSqlNode`](../src/observer/sql/parser/parse.cpp#L47) — 默认构造函数：把语句类型初始化为 SCF_ERROR
- [`ParsedSqlNode`](../src/observer/sql/parser/parse.cpp#L55) — 以指定语句类型构造 ParsedSqlNode
- [`add_sql_node`](../src/observer/sql/parser/parse.cpp#L63) — 向结果集中追加一条已解析的 SQL 语句
- [`parse`](../src/observer/sql/parser/parse.cpp#L81) — 解析 SQL 文本的对外实现

## `parse.h`

- [`parse`](../src/observer/sql/parser/parse.h#L49) — 解析一段 SQL 文本，并把解析结果写入 ParsedSqlResult

## `parse_defs.h`

- [`RelAttrSqlNode`](../src/observer/sql/parser/parse_defs.h#L80) — 描述一个属性
- [`CompOp`](../src/observer/sql/parser/parse_defs.h#L90) — 描述比较运算符
- [`ConditionSqlNode`](../src/observer/sql/parser/parse_defs.h#L111) — 表示一个条件比较
- [`JoinSqlNode`](../src/observer/sql/parser/parse_defs.h#L141) — 描述一个显式 JOIN 子句
- [`OrderDirection`](../src/observer/sql/parser/parse_defs.h#L147) — ORDER BY 的排序方向（ASC / DESC）
- [`OrderByUnit`](../src/observer/sql/parser/parse_defs.h#L157) — ORDER BY 的单个排序项
- [`SelectSqlNode`](../src/observer/sql/parser/parse_defs.h#L163) — 描述一个 select 语句
- [`CalcSqlNode`](../src/observer/sql/parser/parse_defs.h#L178) — 算术表达式计算的语法树
- [`InsertSqlNode`](../src/observer/sql/parser/parse_defs.h#L188) — 描述一个insert语句
- [`DeleteSqlNode`](../src/observer/sql/parser/parse_defs.h#L199) — 描述一个delete语句
- [`UpdateSqlNode`](../src/observer/sql/parser/parse_defs.h#L209) — 描述一个update语句
- [`AttrInfoSqlNode`](../src/observer/sql/parser/parse_defs.h#L222) — 描述一个属性
- [`CreateTableSqlNode`](../src/observer/sql/parser/parse_defs.h#L235) — 描述一个create table语句
- [`DropTableSqlNode`](../src/observer/sql/parser/parse_defs.h#L249) — 描述一个drop table语句
- [`AnalyzeTableSqlNode`](../src/observer/sql/parser/parse_defs.h#L258) — 描述一个analyze table语句
- [`CreateIndexSqlNode`](../src/observer/sql/parser/parse_defs.h#L269) — 描述一个create index语句
- [`DropIndexSqlNode`](../src/observer/sql/parser/parse_defs.h#L280) — 描述一个drop index语句
- [`DescTableSqlNode`](../src/observer/sql/parser/parse_defs.h#L291) — 描述一个desc table语句
- [`LoadDataSqlNode`](../src/observer/sql/parser/parse_defs.h#L301) — 描述一个load data语句
- [`SetVariableSqlNode`](../src/observer/sql/parser/parse_defs.h#L314) — 设置变量的值
- [`ExplainSqlNode`](../src/observer/sql/parser/parse_defs.h#L329) — 描述一个explain语句
- [`ErrorSqlNode`](../src/observer/sql/parser/parse_defs.h#L339) — 解析SQL语句出现了错误
- [`SqlCommandFlag`](../src/observer/sql/parser/parse_defs.h#L350) — 表示一个SQL语句的类型
- [`ParsedSqlNode`](../src/observer/sql/parser/parse_defs.h#L380) — 表示一个SQL语句
- [`ParsedSqlNode`](../src/observer/sql/parser/parse_defs.h#L410) — 以指定命令类型构造节点
- [`ParsedSqlResult`](../src/observer/sql/parser/parse_defs.h#L417) — 表示语法解析后的数据
- [`add_sql_node`](../src/observer/sql/parser/parse_defs.h#L425) — 追加一条已解析的 SQL 语句

## `parse_stage.cpp`

- [`handle_request`](../src/observer/sql/parser/parse_stage.cpp#L59) — 处理一次解析请求：SQL 文本 -> ParsedSqlNode，并写入事件

## `parse_stage.h`

- [`ParseStage`](../src/observer/sql/parser/parse_stage.h#L44) — 解析SQL语句，解析后的结果可以参考parse_defs.h
- [`handle_request`](../src/observer/sql/parser/parse_stage.h#L55) — 处理一次解析请求

## `resolve_stage.cpp`

- [`handle_request`](../src/observer/sql/parser/resolve_stage.cpp#L59) — 处理一次语义解析请求：ParsedSqlNode -> Stmt

## `resolve_stage.h`

- [`ResolveStage`](../src/observer/sql/parser/resolve_stage.h#L44) — 执行Resolve，将解析后的SQL语句，转换成各种Stmt(Statement), 同时会做错误检查
- [`handle_request`](../src/observer/sql/parser/resolve_stage.h#L55) — 处理一次语义解析请求

## `expression_binder.cpp`

- [`arithmetic_type_to_string`](../src/observer/sql/parser/expression_binder.cpp#L72) — 把算术运算符枚举转换为可读符号
- [`set_arithmetic_type_error`](../src/observer/sql/parser/expression_binder.cpp#L92) — 生成并记录“运算符不能作用于该类型”的语义错误消息
- [`reset_binder_error_message`](../src/observer/sql/parser/expression_binder.cpp#L114) — 清空线程本地绑定错误消息
- [`set_binder_error_message`](../src/observer/sql/parser/expression_binder.cpp#L120) — 写入线程本地绑定错误消息
- [`get_binder_error_message`](../src/observer/sql/parser/expression_binder.cpp#L126) — 读取线程本地绑定错误消息
- [`find_table`](../src/observer/sql/parser/expression_binder.cpp#L135) — 按表名在查询表集合中查找表（忽略大小写）
- [`wildcard_fields`](../src/observer/sql/parser/expression_binder.cpp#L154) — 把一张表的用户字段全部展开为字段表达式
- [`bind_expression`](../src/observer/sql/parser/expression_binder.cpp#L176) — 绑定表达式总入口：按表达式类型分派到具体绑定函数
- [`bind_star_expression`](../src/observer/sql/parser/expression_binder.cpp#L240) — 绑定并展开星号表达式 `*` / `table.*`
- [`bind_unbound_field_expression`](../src/observer/sql/parser/expression_binder.cpp#L288) — 绑定未解析的字段表达式（如 `a` 或 `t.a`）
- [`bind_field_expression`](../src/observer/sql/parser/expression_binder.cpp#L356) — 绑定已解析的字段表达式
- [`bind_value_expression`](../src/observer/sql/parser/expression_binder.cpp#L370) — 绑定常量值表达式
- [`bind_cast_expression`](../src/observer/sql/parser/expression_binder.cpp#L386) — 绑定 CAST 类型转换表达式
- [`bind_comparison_expression`](../src/observer/sql/parser/expression_binder.cpp#L428) — 绑定比较表达式（= <> < <= > >= 及 IS [NOT] NULL）
- [`bind_conjunction_expression`](../src/observer/sql/parser/expression_binder.cpp#L487) — 绑定布尔连接表达式（AND / OR）
- [`bind_arithmetic_expression`](../src/observer/sql/parser/expression_binder.cpp#L534) — 绑定算术表达式（+ - * / 以及一元负号）
- [`check_aggregate_expression`](../src/observer/sql/parser/expression_binder.cpp#L609) — 校验聚合表达式是否合法
- [`bind_aggregate_expression`](../src/observer/sql/parser/expression_binder.cpp#L664) — 绑定聚合表达式（SUM/AVG/COUNT/MAX/MIN）

## `expression_binder.h`

- [`BinderContext`](../src/observer/sql/parser/expression_binder.h#L59) — 表达式绑定上下文：提供查询涉及的物理表集合
- [`BinderContext`](../src/observer/sql/parser/expression_binder.h#L65) — 虚析构（默认实现），保证可作为多态基类安全使用
- [`find_table`](../src/observer/sql/parser/expression_binder.h#L81) — 按表名查找查询涉及的表（大小写不敏感）
- [`ExpressionBinder`](../src/observer/sql/parser/expression_binder.h#L99) — 绑定表达式
- [`ExpressionBinder`](../src/observer/sql/parser/expression_binder.h#L108) — 虚析构（默认实现），便于将来扩展为多态绑定器
- [`bind_expression`](../src/observer/sql/parser/expression_binder.h#L118) — 绑定一个表达式（递归入口/分派器）
- [`bind_star_expression`](../src/observer/sql/parser/expression_binder.h#L129) — 绑定星号表达式（SELECT * / SELECT t.*）
- [`bind_unbound_field_expression`](../src/observer/sql/parser/expression_binder.h#L137) — 绑定未解析字段表达式（字段名在语法阶段尚未与具体表关联）
- [`bind_field_expression`](../src/observer/sql/parser/expression_binder.h#L146) — 绑定已解析字段表达式
- [`bind_value_expression`](../src/observer/sql/parser/expression_binder.h#L154) — 绑定值表达式（常量）
- [`bind_cast_expression`](../src/observer/sql/parser/expression_binder.h#L162) — 绑定 CAST 类型转换表达式
- [`bind_comparison_expression`](../src/observer/sql/parser/expression_binder.h#L170) — 绑定比较表达式
- [`bind_conjunction_expression`](../src/observer/sql/parser/expression_binder.h#L179) — 绑定布尔连接表达式（AND / OR）
- [`bind_arithmetic_expression`](../src/observer/sql/parser/expression_binder.h#L189) — 绑定算术表达式（+ - * / 及一元负号）
- [`bind_aggregate_expression`](../src/observer/sql/parser/expression_binder.h#L198) — 绑定聚合表达式（SUM/AVG/COUNT/MAX/MIN）
- [`reset_binder_error_message`](../src/observer/sql/parser/expression_binder.h#L217) — 清空线程本地的绑定错误消息
- [`set_binder_error_message`](../src/observer/sql/parser/expression_binder.h#L223) — 写入线程本地的绑定错误消息
- [`get_binder_error_message`](../src/observer/sql/parser/expression_binder.h#L229) — 读取线程本地的绑定错误消息

## `expected_tokens.h`

- [`collect_expected_tokens`](../src/observer/sql/parser/expected_tokens.h#L47) — 复用现有 bison 语法分析器，返回给定 SQL 前缀在结尾处合法的 terminal 集合

## `lex_sql.l`

- [`文件说明`](../src/observer/sql/parser/lex_sql.l#L35) — Flex 词法分析器总览与实现原则
- [`YY_USER_INIT`](../src/observer/sql/parser/lex_sql.l#L61) — 每次 yylex 开始重置行/列号
- [`YY_USER_ACTION`](../src/observer/sql/parser/lex_sql.l#L67) — 每个 token 后记录 <行,列> 位置
- [`C 代码段`](../src/observer/sql/parser/lex_sql.l#L78) — include、宏（RETURN_TOKEN 等）
- [`RETURN_TOKEN`](../src/observer/sql/parser/lex_sql.l#L101) — 返回 token 的宏
- [`%option`](../src/observer/sql/parser/lex_sql.l#L105) — no yywrap/不区分大小写/reentrant 等
- [`词法宏定义`](../src/observer/sql/parser/lex_sql.l#L116) — WHITE_SPACE/DIGIT/ID/DOT/QUOTE
- [`起始状态`](../src/observer/sql/parser/lex_sql.l#L124) — %x STR / %x COMMENT
- [`规则区开始`](../src/observer/sql/parser/lex_sql.l#L133) — %% 之后是规则区
- [`空白与注释`](../src/observer/sql/parser/lex_sql.l#L135) — 空白、换行、-- 行注释、块注释
- [`数字字面量`](../src/observer/sql/parser/lex_sql.l#L144) — NUMBER / FLOAT
- [`标点与保留字`](../src/observer/sql/parser/lex_sql.l#L148) — 分号、点号与全部关键字入口
- [`查询类关键字`](../src/observer/sql/parser/lex_sql.l#L165) — SELECT/CALC/FROM/WHERE/AND/OR
- [`DML 关键字`](../src/observer/sql/parser/lex_sql.l#L172) — INSERT/INTO/VALUES/DELETE/UPDATE/SET
- [`事务关键字`](../src/observer/sql/parser/lex_sql.l#L179) — BEGIN/COMMIT/ROLLBACK
- [`NULL 与类型关键字`](../src/observer/sql/parser/lex_sql.l#L183) — NULL/IS/NOT/INT/CHAR/FLOAT/VECTOR/DATE
- [`LOAD DATA 关键字`](../src/observer/sql/parser/lex_sql.l#L192) — LOAD/DATA/INFILE
- [`子句关键字`](../src/observer/sql/parser/lex_sql.l#L196) — EXPLAIN/GROUP/BY/ORDER/STORAGE/FORMAT/PRIMARY/KEY
- [`ANALYZE/LOAD 关键字`](../src/observer/sql/parser/lex_sql.l#L205) — ANALYZE/FIELDS/TERMINATED/ENCLOSED
- [`标识符 ID`](../src/observer/sql/parser/lex_sql.l#L210) — 返回 ID，strdup 登记到 yyextra
- [`括号`](../src/observer/sql/parser/lex_sql.l#L212) — LBRACE / RBRACE
- [`逗号与比较运算符`](../src/observer/sql/parser/lex_sql.l#L216) — EQ/NE/LE/LT/GE/GT
- [`算术运算符`](../src/observer/sql/parser/lex_sql.l#L227) — 返回字符本身 + - * /
- [`字符串字面量`](../src/observer/sql/parser/lex_sql.l#L232) — 单/双引号返回 SSS，登记到 yyextra
- [`兜底规则`](../src/observer/sql/parser/lex_sql.l#L236) — 未识别字符记日志并原样返回
- [`scan_string()`](../src/observer/sql/parser/lex_sql.l#L247) — 把待解析字符串设为扫描器输入缓冲区

## `yacc_sql.y`

- [`token_name`](../src/observer/sql/parser/yacc_sql.y#L114) — 依据位置区间从原始 SQL 文本中截取对应的词素文本
- [`yyerror`](../src/observer/sql/parser/yacc_sql.y#L132) — 传统 yyerror 入口：生成带行列定位的语法错误节点
- [`create_arithmetic_expression`](../src/observer/sql/parser/yacc_sql.y#L200) — 构造一个未绑定的算术表达式节点
- [`create_aggregate_expression`](../src/observer/sql/parser/yacc_sql.y#L224) — 构造一个未绑定的聚合表达式节点
- [`set_expr_name_and_location`](../src/observer/sql/parser/yacc_sql.y#L245) — 为表达式同时记录显示名与 <行,列> 位置
- [`unescape_quoted_string`](../src/observer/sql/parser/yacc_sql.y#L265) — 将带引号的字符串常量还原为原始内容并处理转义
- [`append_join_node`](../src/observer/sql/parser/yacc_sql.y#L298) — 向 JOIN 子句列表追加一个 JOIN 节点
- [`commands`](../src/observer/sql/parser/yacc_sql.y#L521) — 顶层输入：0 或多条命令
- [`command_wrapper`](../src/observer/sql/parser/yacc_sql.y#L533) — 单条命令的分发包装
- [`exit_stmt`](../src/observer/sql/parser/yacc_sql.y#L558) — EXIT 语句
- [`help_stmt`](../src/observer/sql/parser/yacc_sql.y#L565) — HELP 语句
- [`sync_stmt`](../src/observer/sql/parser/yacc_sql.y#L571) — SYNC 语句
- [`begin_stmt`](../src/observer/sql/parser/yacc_sql.y#L578) — BEGIN 事务开始
- [`commit_stmt`](../src/observer/sql/parser/yacc_sql.y#L585) — COMMIT 事务提交
- [`rollback_stmt`](../src/observer/sql/parser/yacc_sql.y#L592) — ROLLBACK 事务回滚
- [`drop_table_stmt`](../src/observer/sql/parser/yacc_sql.y#L599) — drop table 语句的语法解析树
- [`analyze_table_stmt`](../src/observer/sql/parser/yacc_sql.y#L606) — analyze table 语法的语法解析树
- [`show_tables_stmt`](../src/observer/sql/parser/yacc_sql.y#L614) — SHOW TABLES
- [`desc_table_stmt`](../src/observer/sql/parser/yacc_sql.y#L621) — DESC 查看表结构
- [`create_index_stmt`](../src/observer/sql/parser/yacc_sql.y#L632) — create index 语句的语法解析树
- [`drop_index_stmt`](../src/observer/sql/parser/yacc_sql.y#L644) — drop index 语句的语法解析树
- [`create_table_stmt`](../src/observer/sql/parser/yacc_sql.y#L657) — create table 语句的语法解析树
- [`attr_def_list`](../src/observer/sql/parser/yacc_sql.y#L679) — 列定义列表
- [`attr_def`](../src/observer/sql/parser/yacc_sql.y#L698) — 单个列定义
- [`null_def`](../src/observer/sql/parser/yacc_sql.y#L717) — NULL / NOT NULL
- [`number`](../src/observer/sql/parser/yacc_sql.y#L732) — 列长度等整数字面量
- [`type`](../src/observer/sql/parser/yacc_sql.y#L736) — 列类型
- [`primary_key`](../src/observer/sql/parser/yacc_sql.y#L744) — 主键定义
- [`attr_list`](../src/observer/sql/parser/yacc_sql.y#L756) — 列名列表
- [`insert_stmt`](../src/observer/sql/parser/yacc_sql.y#L778) — insert 语句的语法解析树
- [`value_list`](../src/observer/sql/parser/yacc_sql.y#L798) — VALUES 值列表
- [`value`](../src/observer/sql/parser/yacc_sql.y#L815) — 单个值
- [`storage_format`](../src/observer/sql/parser/yacc_sql.y#L836) — 建表存储格式子句
- [`delete_stmt`](../src/observer/sql/parser/yacc_sql.y#L848) — delete 语句的语法解析树
- [`update_stmt`](../src/observer/sql/parser/yacc_sql.y#L864) — update 语句的语法解析树
- [`select_stmt`](../src/observer/sql/parser/yacc_sql.y#L884) — select 语句的语法解析树
- [`calc_stmt`](../src/observer/sql/parser/yacc_sql.y#L919) — CALC 计算语句
- [`expression_list`](../src/observer/sql/parser/yacc_sql.y#L929) — 表达式列表
- [`expression`](../src/observer/sql/parser/yacc_sql.y#L950) — 表达式（比较/算术/聚合/字面量）
- [`aggregate_expression`](../src/observer/sql/parser/yacc_sql.y#L988) — 聚合表达式
- [`rel_attr`](../src/observer/sql/parser/yacc_sql.y#L995) — 字段（可带表名限定）
- [`relation`](../src/observer/sql/parser/yacc_sql.y#L1008) — 表引用
- [`rel_list`](../src/observer/sql/parser/yacc_sql.y#L1014) — 表列表
- [`join_clause`](../src/observer/sql/parser/yacc_sql.y#L1035) — 显式 JOIN 子句
- [`where`](../src/observer/sql/parser/yacc_sql.y#L1054) — WHERE 子句
- [`select_where`](../src/observer/sql/parser/yacc_sql.y#L1065) — SELECT ... WHERE 子句
- [`boolean_expr`](../src/observer/sql/parser/yacc_sql.y#L1081) — 布尔表达式（AND / OR）
- [`comparison_predicate`](../src/observer/sql/parser/yacc_sql.y#L1112) — 比较谓词
- [`condition_list`](../src/observer/sql/parser/yacc_sql.y#L1127) — 条件列表
- [`condition`](../src/observer/sql/parser/yacc_sql.y#L1147) — 单个条件
- [`comp_op`](../src/observer/sql/parser/yacc_sql.y#L1199) — 比较运算符
- [`group_by`](../src/observer/sql/parser/yacc_sql.y#L1210) — GROUP BY 子句
- [`order_by`](../src/observer/sql/parser/yacc_sql.y#L1223) — ORDER BY 子句
- [`order_by_unit`](../src/observer/sql/parser/yacc_sql.y#L1236) — 单个排序项
- [`order_by_list`](../src/observer/sql/parser/yacc_sql.y#L1258) — 排序项列表
- [`load_data_stmt`](../src/observer/sql/parser/yacc_sql.y#L1277) — LOAD DATA 数据导入
- [`fields_terminated_by`](../src/observer/sql/parser/yacc_sql.y#L1300) — FIELDS TERMINATED BY 子句
- [`enclosed_by`](../src/observer/sql/parser/yacc_sql.y#L1311) — ENCLOSED BY 子句
- [`explain_stmt`](../src/observer/sql/parser/yacc_sql.y#L1325) — EXPLAIN 语句
- [`set_variable_stmt`](../src/observer/sql/parser/yacc_sql.y#L1334) — SET 变量语句
- [`opt_semicolon`](../src/observer/sql/parser/yacc_sql.y#L1345) — 可选的结尾分号
- [`yyreport_syntax_error`](../src/observer/sql/parser/yacc_sql.y#L1393) — %define parse.error custom 指定的语法错误回调
- [`sql_parse`](../src/observer/sql/parser/yacc_sql.y#L1484) — 驱动一次完整的词法+语法分析
- [`collect_expected_tokens`](../src/observer/sql/parser/yacc_sql.y#L1513) — 收集给定 SQL 前缀末尾处合法的终结符集合（用于自动补全）
