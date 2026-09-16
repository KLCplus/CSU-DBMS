# CSU-DBMS 答辩 PPT：待核验信息清单（已核验版）

> 本文件已按 **当前工作区源码**逐条核验完成。
> 行号以当前 `main` 分支（含功能索引/注释提交后）为准。
> 图例：✅ 准确　⚠️ 需修正（PPT 必须改）　❔ 无法确认（仓库无证据）。
>
> 核验结论的“关键修正”集中列在文末 [十三、关键修正与无法确认](#十三关键修正与无法确认汇总)。

---

## 0. 核验时统一记录格式

（保留原文要求）每项确认：文件路径、函数/类/规则名、准确行号、实现原理是否准确、答辩边界。

---

# 一、编译器：核验结果

## 1.1 词法分析器 `lex_sql.l`

| 文件路径 | 函数 / 规则 | 当前已知行号 | 需要核验的代码位置 | 需要核验的实现原理 | 核验结果 |
|---|---|---:|---|---|---|
| `src/observer/sql/parser/lex_sql.l` | `YY_USER_INIT` | L61 | 行号是否仍正确 | 是否在每次 scanner 开始时重置行/列位置 | ✅ 准确，L61-L63，置 `yylineno=1; yycolumn=1` |
| `src/observer/sql/parser/lex_sql.l` | `YY_USER_ACTION` | L67 | 行号是否仍正确 | 是否对每个 token 写入 `first_line/column` 与 `last_line/column`；换行后列号如何更新 | ✅ 准确，L67-L74；换行由 L137 `\n { yycolumn=1; }` 重置列号，行号由 flex `yylineno` 自增 |
| `src/observer/sql/parser/lex_sql.l` | `RETURN_TOKEN` | L101 | 行号是否仍正确 | 是否只是统一返回 token，还是还承担日志/位置等逻辑 | ✅ 准确，L101；`#define RETURN_TOKEN(token) LOG_DEBUG("%s",#token); return token`（含日志，位置逻辑不在宏内） |
| `src/observer/sql/parser/lex_sql.l` | Flex `%option` | L105 | 行号是否仍正确 | 是否确实包含 `case-insensitive`、`yylineno`、`reentrant` 等；PPT 不要写不存在的 option | ✅ 准确，L104-L114：`noyywrap/nounput/noinput/bison-bridge/reentrant/yylineno/case-insensitive/bison-locations` |
| `src/observer/sql/parser/lex_sql.l` | 空白 / 单行注释 / 块注释规则 | L135 起 | 准确规则行范围 | `--` 与 `/* */` 如何跳过；块注释是否使用 `%x COMMENT` 状态机 | ✅ 准确，L136-L142；`%x COMMENT` 声明在 L126（`%x STR` L125 声明但未使用） |
| `src/observer/sql/parser/lex_sql.l` | 数字规则 | L144 起 | NUMBER / FLOAT 对应行号 | 整数、浮点分别如何写入 `yylval` | ✅ 准确，L145-L146；`NUMBER`→`atoi`、`FLOAT`→`(float)atof`，靠最长匹配优先 |
| `src/observer/sql/parser/lex_sql.l` | 关键字规则 | L148 起 | SELECT/WHERE/AND/OR/NOT 等准确行号 | 是否依靠 Flex 大小写不敏感直接识别关键字 | ✅ 准确，L149-L209；由 `%option case-insensitive` 统一识别，无逐条多大小写 |
| `src/observer/sql/parser/lex_sql.l` | `ID` 规则 | L210 | 行号是否仍正确 | 标识符原文如何保存；内存由谁释放 | ⚠️ 精确规则在 **L211**（L210 是组注释）；`strdup` 后登记 `yyextra`，由 `sql_parse` L1491-L1493 统一 `free` |
| `src/observer/sql/parser/lex_sql.l` | 比较运算符规则 | L216 起 | `== = != <> <= >= < >` 准确行号 | 是否利用 Flex 最长匹配保证多字符运算符优先 | ✅ 准确，L217-L225；最长匹配保证 `==/<=/<>/!=/>=` 优先 |
| `src/observer/sql/parser/lex_sql.l` | 算术运算符规则 | L227 起 | 行号 | 是否直接返回字符 `+ - * /`，由 Bison 优先级处理 | ✅ 准确，L228-L231，动作 `{ return yytext[0]; }` |
| `src/observer/sql/parser/lex_sql.l` | 字符串规则 | L232 起 | 单/双引号准确行号 | 是否支持 `''`、`""`、反斜杠转义；未闭合字符串如何进入错误链 | ✅ 准确，L233-L234；支持 `\x` 与 `''/""`；未闭合不匹配本规则，落兜底 L237，由 `yyreport_syntax_error`（yacc L1428-L1437）判为 `unterminated string literal` |
| `src/observer/sql/parser/lex_sql.l` | 兜底 `.` 规则 | L236 | 行号 | 非法字符是否原样返回给 parser，从而触发结构化语法错误 | ✅ 准确，L237；`LOG_DEBUG` 后 `return yytext[0]` 交 Bison |
| `src/observer/sql/parser/lex_sql.l` | `scan_string()` | L247 | 行号 | 是否把完整 SQL 文本绑定到 reentrant scanner 的输入 buffer | ✅ 准确，L247-L249，`yy_switch_to_buffer(yy_scan_string(...))` |

### “词法实现方法”核验

- [x] **位置跟踪**：`YY_USER_ACTION` 保存 1-based 行列 — ✅ 准确（L67-L74）。
- [x] **注释处理**：单行直接跳过；块注释用 `COMMENT` 状态机吞到 `*/` — ✅ 准确（L138-L142）。
- [x] **大小写不敏感**：由 `%option case-insensitive` 完成 — ✅ 准确（L113）。
- [x] **非法字符**：词法器不崩溃，非法字符交 Bison 生成 `unexpected + expected` — ✅ 准确（L237 + yacc L1393）。
- [x] **字符串转义**：token 层识别字符串，解码在 `unescape_quoted_string()` — ✅ 准确（L233-L234 + yacc L265-L286）。
- [x] **多字符运算符**：靠 Flex 最长匹配避免 `>=` 被拆开 — ✅ 准确（L217-L225）。

---

## 1.2 语法分析器 `yacc_sql.y`

| 文件路径 | 函数 / 规则 | 当前已知行号 | 需要核验的代码位置 | 需要核验的实现原理 | 核验结果 |
|---|---|---:|---|---|---|
| `src/observer/sql/parser/yacc_sql.y` | `create_arithmetic_expression()` | L200 | 起止行号 | 是否创建未绑定算术表达式，并记录运算符位置 | ✅ 准确，L200-L212，`op_locp` 记录运算符行列 |
| `src/observer/sql/parser/yacc_sql.y` | `create_aggregate_expression()` | L224 | 起止行号 | 聚合函数 AST 如何构造 | ✅ 准确，L224-L233，构造 `UnboundAggregateExpr`，类型留绑定期 |
| `src/observer/sql/parser/yacc_sql.y` | `set_expr_name_and_location()` | L245 | 行号 | 是否同时写表达式显示名与源码位置 | ✅ 准确，L245-L252 |
| `src/observer/sql/parser/yacc_sql.y` | `unescape_quoted_string()` | L265 | 起止行号 | `''`、`""`、`\x` 的具体还原规则 | ✅ 准确，L265-L286：`\x`→`x`、`''/""`→单引号 |
| `src/observer/sql/parser/yacc_sql.y` | `append_join_node()` | L298 | 行号 | JOIN 节点如何追加到 SELECT AST | ✅ 准确，L298-L308 |
| `src/observer/sql/parser/yacc_sql.y` | `commands` | L521 | 规则范围 | 多 SQL 是否递归累积到 `ParsedSqlResult` | ✅ 准确，L521-L527；空产生式允许空输入，`commands command_wrapper` 递归累积 |
| `src/observer/sql/parser/yacc_sql.y` | `create_table_stmt` | L657 | 规则范围 | CREATE TABLE AST 的字段、类型、NULL/PRIMARY KEY 如何写入 | ✅ 准确，L657-L676 |
| `src/observer/sql/parser/yacc_sql.y` | `insert_stmt` | L778 | 规则范围 | INSERT AST 是否支持列清单 + VALUES | ✅ 准确，L778-L795；无列清单 L779 与带列清单 L786 两种写法 |
| `src/observer/sql/parser/yacc_sql.y` | `delete_stmt` | L848 | 规则范围 | DELETE + WHERE 如何生成 AST | ✅ 准确，L848-L858 |
| `src/observer/sql/parser/yacc_sql.y` | `update_stmt` | L864 | 规则范围 | UPDATE/SET/WHERE 的 AST 结构 | ✅ 准确，L864-L876 |
| `src/observer/sql/parser/yacc_sql.y` | `select_stmt` | L884 | 规则范围 | SELECT/FROM/JOIN/WHERE/GROUP BY/ORDER BY 的组合方式 | ✅ 准确，L884-L917 |
| `src/observer/sql/parser/yacc_sql.y` | `expression` | L950 | 规则范围 | 算术、字段、常量、聚合表达式的 AST 组合 | ✅ 准确，L950-L985；一元负号 `%prec UMINUS` L967，裸 `*` L970 |
| `src/observer/sql/parser/yacc_sql.y` | `join_clause` | L1035 | 规则范围 | 显式 JOIN 的表与 ON 条件如何保存 | ✅ 准确，L1035-L1048（`JOIN` 与 `INNER JOIN` 均追加 `JoinSqlNode`） |
| `src/observer/sql/parser/yacc_sql.y` | `boolean_expr` | L1081 | 规则范围 | `AND / OR / NOT / ()` 的实际文法写法与优先级 | ✅ 准确，L1081-L1106；`NOT` 改写为 `(expr == FALSE)` L1095，括号 `LBRACE boolean_expr RBRACE` L1100 |
| `src/observer/sql/parser/yacc_sql.y` | `comparison_predicate` | L1112 | 规则范围 | 比较表达式、`IS [NOT] NULL` 的具体生成方式 | ✅ 准确，L1112-L1122 |
| `src/observer/sql/parser/yacc_sql.y` | `group_by` | L1210 | 规则范围 | GROUP BY 字段列表写入 AST 的方法 | ✅ 准确，L1210-L1221（直接复用 expr_list，未限制 `*`） |
| `src/observer/sql/parser/yacc_sql.y` | `order_by` | L1223 | 规则范围 | ORDER BY 与 ASC/DESC 如何保存 | ✅ 准确，L1223-L1232；方向在 `order_by_unit` L1236-L1256，缺省 ASC |
| `src/observer/sql/parser/yacc_sql.y` | `yyreport_syntax_error()` | L1393 | 起止行号 | 如何调用 `yypcontext_token` / `yypcontext_expected_tokens` 拼结构化错误 | ✅ 准确，L1393-L1471；`yypcontext_expected_tokens` L1412/L1445，`yypcontext_token` L1423 |
| `src/observer/sql/parser/yacc_sql.y` | `sql_parse()` | L1484 | 起止行号 | scanner 初始化、`scan_string`、`yyparse`、释放资源的完整流程 | ✅ 准确，L1484-L1498：`yylex_init_extra`→`scan_string`→`yyparse`→`free`→`yylex_destroy` |
| `src/observer/sql/parser/yacc_sql.y` | `collect_expected_tokens()` | L1513 | 起止行号 | 是否真的通过“前缀 + 哨兵/错误上下文”获取当前位置合法 terminal 集合 | ⚠️ **部分修正**：函数本身 L1513-L1536 **不追加哨兵**，直接 `sql_parse(sql)` 取期望集合；**哨兵 `'\x01'` 由补全层** `completion_engine.cpp:248` / `model_completion_validator.cpp:158` 追加。整体机制仍成立（复用同一 Bison 文法）|

### “语法实现方法”核验

- [x] 解析器为 **Bison LALR(1)** — ✅：`%define api.pure full`(L313)、`%define parse.error custom`(L320)、`%locations`(L322)，**无 `%glr`**。
- [x] `FIRST/FOLLOW/分析表`由 Bison 生成器内部计算 — ✅。
- [x] `NOT > 比较 > AND > OR` 由 `%left/%precedence` 声明顺序实现 — ✅：`%left OR`(L510) < `%left AND`(L511) < 比较 `%left`(L512) < `%precedence NOT`(L513)（越后越紧）。
- [x] 括号通过 `LBRACE boolean_expr RBRACE` 强制子树先组合 — ✅（L1100-L1102）。
- [x] 左递归直接支持，无需手工消除 — ✅（`commands`、`expression` 等为左递归，LALR 自底向上）。
- [x] 语法错误输出包含 行号/列号/unexpected token/expected 集合 — ✅（L1393-L1471）。
- [x] `collect_expected_tokens()` 与正常 Parser 共用同一份 Bison 文法 — ✅（同一状态机，无第二套）。
- [x] “在光标处制造语法错误取 expected”的具体实现 — ⚠️：**哨兵由补全层追加 `'\x01'`**，`collect_expected_tokens` 自身不加。

---

## 1.3 ParseStage / ResolveStage

| 文件路径 | 函数 | 当前已知行号 | 需要核验的实现原理 | 核验结果 |
|---|---|---:|---|---|
| `src/observer/sql/parser/parse.cpp` | `parse()` | L81 | 是否只负责调 `sql_parse/yyparse` 并把多个 `ParsedSqlNode` 写入结果 | ✅ 准确，L81-L85，仅转调 `sql_parse` 并恒返回 `RC::SUCCESS` |
| `src/observer/sql/parser/parse_stage.cpp` | `ParseStage::handle_request()` | L59 | 空输入、多语句、任意 `SCF_ERROR` 的准确处理策略；到底是否“多语句只执行第一条” | ✅ 准确，L59-L102：空输入 L70-L75；多语句仅 `LOG_WARN` L77-L80；**任一 `SCF_ERROR` 即整次返回 `RC::SQL_SYNTAX`** L83-L95；只把第一条交事件 L98-L99 |
| `src/observer/sql/parser/resolve_stage.cpp` | `ResolveStage::handle_request()` | L59 | 是否在此调用 `Stmt::create_stmt`；binder 错误消息何时 reset / 读取 / 返回客户端 | ✅ 准确，L59-L98：`reset_binder_error_message()` L79 → `Stmt::create_stmt` L81 → 失败时读 `get_binder_error_message()` 写 `set_state_string` L85-L90 |

### “两阶段编译管线”核验

- [x] 数据流 `SQL → Flex token → Bison AST → ParseStage → ResolveStage → Stmt` — ✅ 与代码一致。
- [x] `ParseStage` 是语法层边界，`ResolveStage` 是语义层边界 — ✅。
- [x] 多语句错误策略：任一节点错误即整次返回语法错误 — ✅（不是静默丢弃尾部）。
- [x] Resolve 失败后错误经 `SqlResult::set_return_code` + `set_state_string` 返回客户端 — ✅。
- [x] 执行计划/优化/算子/存储不属于本人编译器实现 — ✅（本清单范围外）。

---

## 1.4 `expression_binder.cpp`

| 文件路径 | 函数 | 当前已知行号 | 需要核验的实现原理 | 核验结果 |
|---|---|---:|---|---|
| `src/observer/sql/parser/expression_binder.cpp` | `set_arithmetic_type_error()` | L92 | 错误文本格式、错误位置取自哪里 | ✅ 准确，L92-L107；位置取 `expr.line()/column()`（即运算符位置） |
| `src/observer/sql/parser/expression_binder.cpp` | `reset_binder_error_message()` | L114 | 是否为 `thread_local` 错误槽 | ✅ 准确，`thread_local std::string g_binder_error_message` L64 |
| `src/observer/sql/parser/expression_binder.cpp` | `set_binder_error_message()` | L120 | thread-local 变量的具体写入方式 | ✅ 准确，L120 |
| `src/observer/sql/parser/expression_binder.cpp` | `get_binder_error_message()` | L126 | 读取后是否清除，生命周期如何 | ✅ 准确；**读取不清除**，由 `ResolveStage` 每条语句前 `reset`（resolve_stage.cpp:79） |
| `src/observer/sql/parser/expression_binder.cpp` | `find_table()` | L135 | 是否大小写不敏感；搜索范围仅当前 query table 集合还是整个 DB | ✅ 准确，`strcasecmp`；仅在 `BinderContext::query_tables_` 查找（不搜整库） |
| `src/observer/sql/parser/expression_binder.cpp` | `wildcard_fields()` | L154 | `*` 展开时是否过滤系统字段 | ✅ 准确，L154-L164；从 `sys_field_num()` 到 `field_num()`，跳过系统字段 |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_expression()` | L176 | 实际按哪些 ExpressionType 分派 | ✅ 准确，L176-L229；分派 `STAR/UNBOUND_FIELD/UNBOUND_AGGREGATION/FIELD/VALUE/CAST/COMPARISON/CONJUNCTION/ARITHMETIC`（`AGGREGATION` 命中 `ASSERT(false)` L219） |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_star_expression()` | L240 | `*` 与 `table.*` 的区别和错误处理 | ⚠️ **需修正**：`table.*` 分支（L251-L264）**文法不可达**（`expression` 无 `ID DOT '*'` 产生式）；实际只有裸 `*` 可解析 |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_unbound_field_expression()` | L288 | 列存在性、歧义列、限定表名检查逻辑 | ⚠️ **描述修正**：“歧义列”实为“省略表名时要求查询仅一张表”（L301-L312），并非逐表命中判定；带表名用 `find_table` L314 |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_comparison_expression()` | L428 | 比较两端的类型规则与 cast 策略 | ⚠️ **需修正**：L428-L476 只递归绑定左右并原位替换，**binder 内不做类型规则/cast** |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_conjunction_expression()` | L487 | AND/OR 是否强制 BOOL 类型 | ⚠️ **需修正**：L487-L522 只绑定 children，**不强制 BOOL 类型** |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_arithmetic_expression()` | L534 | 允许哪些数值类型；结果类型如何推导 | ✅ 准确，L534-L597；仅允许 `is_numerical_type`，否则 `set_arithmetic_type_error` + `SCHEMA_FIELD_TYPE_MISMATCH`；结果类型由表达式/执行层推导 |
| `src/observer/sql/parser/expression_binder.cpp` | `check_aggregate_expression()` | L609 | 聚合嵌套/非法参数等检查 | ✅ 准确，L609-L652；`SUM/AVG` 需数值类型，递归禁止聚合嵌套聚合 |
| `src/observer/sql/parser/expression_binder.cpp` | `bind_aggregate_expression()` | L664 | SUM/AVG/COUNT/MAX/MIN 的类型规则 | ✅ 准确，L664-L712；`COUNT(*)` 特化为常量 `1` |

### “语义分析实现方法”核验

- [x] 字段绑定流程 `UnboundFieldExpr → 查表/查列 → FieldExpr` — ✅ 准确。
- [x] `SELECT *` 展开在 binder；`table.*` — ⚠️ 代码在但**文法不可达**。
- [x] 算术类型检查在 bind 阶段，非法组合不进入执行 — ✅ 准确。
- [x] 比较、布尔连接、聚合都有独立 binder 函数 — ✅（比较/布尔仅绑定，类型校验缺）。
- [x] 结构化语义错误经 **thread-local 错误槽**回传 — ✅ 准确（`g_binder_error_message` L64）。
- [x] 表达式节点携带源码行列 — ✅（`expr/expression.h` L123-L128 定位接口，`line_/column_` L154-L155）。
- [x] 表/列存在性是否部分在 `Stmt::create` — ✅ **是**：SELECT 表存在性 `select_stmt.cpp:53,72`、INSERT `insert_stmt.cpp:40,72-88`、DELETE `delete_stmt.cpp:40`、UPDATE `update_stmt.cpp:46,54`。PPT 必须写“Parser/Resolve + Stmt 边界共同完成”。

---

# 二、代码自动补全：核验结果

## 2.1 确定性补全链

| 完整文件路径 | 函数 / 类 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/observer/sql/autocomplete/current_statement_extractor.cpp` | `find_statement_at_cursor()` | L44-L77 | 如何处理分号、单/双引号、注释；怎样只截取 cursor 所在 statement | ✅ 用 `scan_sql_text` 分词，只取字符串/注释外的 `;` 作分隔；cursor 越界钳制；`found` 恒 true |
| `src/observer/sql/autocomplete/sql_text_scanner.cpp` | `scan_sql_text()` | L76-L215 | 轻量 scanner 输出什么 token/context；明确它不是第二套 SQL parser | ✅ 无语法分词，输出 `Kind∈{Comment,String,Number,Word,Symbol}`+区间；确非第二套 parser |
| `src/observer/sql/autocomplete/sql_text_scanner.cpp` | `cursor_in_string_or_comment()` | L229-L260 | 怎样避免在字符串和注释中触发普通补全 | ✅ 全文分词后按 token 闭合性判断，光标在 String/Comment 内即禁用 |
| `src/observer/sql/autocomplete/sql_capabilities.cpp` | `SqlCapabilities::instance()` | L79-L83 | 单例如何构造 / 方言表数据来自哪里 | ✅ 函数内 `static`（C++11 线程安全）；`build()` L43 **手工硬编码**三个 `unordered_set` |
| `src/observer/sql/autocomplete/sql_capabilities.cpp` | `is_keyword()` | L91 | 关键字集合是否与 parser 同步，是否存在手工列表 | ⚠️ **手工列表**（L48-L54 硬编码），运行时不与 parser 自动同步 |
| `src/observer/sql/autocomplete/sql_capabilities.cpp` | `is_type()` | L107 | 可补全类型列表的来源 | ✅ 手工表 `{INT,CHAR,FLOAT,VECTOR,DATE}`（L64） |
| `src/observer/sql/autocomplete/sql_capabilities.cpp` | `is_forbidden()` | L116-L127 | HAVING/LIMIT/ALTER 等未实现方言如何禁止 | ✅ 黑名单 `forbidden`（含 HAVING/LIMIT/OFFSET/ALTER/TRUNCATE/DISTINCT 等），Grammar L202/L246 与 Validator L275 双重生效 |
| `src/observer/sql/autocomplete/grammar_completion_provider.cpp` | `complete_grammar()` | L185-L268 | 怎样调用 `collect_expected_tokens()`；是否还会对 candidate 做 parser 试探确认 | ⚠️ **职责修正**：本函数**不调用** `collect_expected_tokens`、也**不做** parser 试探（只做映射/过滤/大小写/去重，`score=10`，额外关键字 `score=12`）。调用与试探在 `CompletionEngine::complete` L241-L283 |
| `src/observer/sql/autocomplete/catalog_completion_provider.cpp` | `complete_catalog()` | L364-L415 | 表/列候选具体从哪个 Catalog/Db/TableMeta 接口获取 | ✅ 表：`Db::all_tables()` L295；列：`Db::find_table()`→`Table::table_meta()`→`sys_field_num/field_num/field(i)` L326-L341（跳过系统列） |
| `src/observer/sql/autocomplete/completion_scope.cpp` | `build_completion_scope()` | L113-L166 | 怎样识别 FROM / JOIN / INSERT INTO 的 scope、alias、目标表 | ⚠️ FROM 支持逗号多表、JOIN 单表、INTO 取下一 Word；**不解析 alias**（`TableBinding.alias` 恒空，`table_alias=false`） |
| `src/observer/sql/autocomplete/completion_engine.cpp` | `CompletionEngine::complete()` | L212-L324 | deterministic 与 model ghost 的调用顺序、合并、排序、去重、max_items | ✅ 顺序：cursor 钳制/字符串注释判定 → 语句提取 → 追加 `'\x01'` 取 expected → grammar → **scope** → catalog → `sort_and_trim` → 满足条件才调 model（只进 ghost）。详见下表 |

### 确定性链关键事实

- [x] **第一层：当前语句提取** — ✅（`find_statement_at_cursor` L44）。
- [x] **第二层：Parser expected set** — ✅ 来自同一 Bison Parser（engine L241-L255 追加哨兵后 `collect_expected_tokens`）。
- [x] **第三层：Catalog** — ✅ 表/列来自 DBMS Catalog（非模型猜）。
- [x] **第四层：Scope** — ✅ 位置：`complete_grammar`(L283) **之后**、`complete_catalog`(L297) **之前**（scope 构建在 L287）。
- [x] **第五层：Dialect gate** — ✅ `is_forbidden` 双处生效。
- [x] **候选排序机制** — ⚠️ **只有 `score`**（`sort_and_trim` L179-L190；Grammar=10、extra=12、Catalog 表=20/列=22、NULL=15、空=14，同分按文本序）；**无 kind/source 权重**；前缀匹配仅过滤。
- [x] **去重** — ⚠️ **无全局 merge 去重**：分别在 `complete_grammar`（`seen` L190）与 `catalog_completion_provider::add_item`（对 `out` 查重 L263-L267）。
- [x] **大小写处理** — ✅ `prefer_lowercase`（L84-L115）只作用于关键字/类型，Catalog 名不改。
- [x] **replace_start / replace_end** — ✅ `[word_start, cursor)` 绝对偏移（L229-L232、L261-L262）。

## 2.2 模型补全链

| 完整文件路径 | 函数 / 类 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/observer/sql/autocomplete/sql_completion_config.cpp` | `SqlCompletionConfig::load()` | L85-L107 | 配置文件路径、默认值、模型开关、timeout/context/max tokens 等具体字段 | ✅ 路径优先级 `CSUDB_SQL_COMPLETION_CONFIG`→`config/`→`etc/`→`../etc/`→`../../etc/`（`load_from_file` L46）；字段 11 个（见下） |
| `src/observer/sql/autocomplete/model_context_builder.cpp` | `SqlModelContextBuilder::build()` | L81-L143 | prompt/context 到底包含哪些 dialect / schema 信息；如何限制长度 | ⚠️ 内容=dialect 段（按 caps）+ schema 段（scope 表，不足用 `all_tables` 补，逐表 `CREATE TABLE` 文本）；**只限制表数 `max_schema_tables`**，`max_schema_columns`/`max_context_tokens` 未使用 |
| `src/observer/sql/autocomplete/llama_completion_client.cpp` | `LlamaCompletionClient::health()` | L111-L174 | 健康检查 URL、timeout、失败条件 | ✅ 2000ms TTL 缓存（L115）；非阻塞 connect+poll；发 `GET /health`（L168）**仅以写满判定**，不校验响应码；失败=getaddrinfo/连接/写失败 |
| `src/observer/sql/autocomplete/llama_completion_client.cpp` | `LlamaCompletionClient::infill()` | L190-L347 | `/infill` 请求 JSON、FIM 参数、deadline、响应字段 | ✅ FIM：`POST /infill`，字段 `input_prefix/input_suffix/input_extra/n_predict/temperature/top_k=1/cache_prompt=true/n_cache_reuse=64/t_max_predict_ms=150`（L197-L214）；响应须 `" 200 "`，取 body `content`（L320-L341） |
| `src/observer/sql/autocomplete/model_completion_provider.cpp` | `ModelCompletionProvider::available()` | L52-L55 | 健康检查是否缓存；调用频率 | ✅ `model_enabled && enabled && client_!=nullptr && health()`；health 有 2s TTL 缓存 |
| `src/observer/sql/autocomplete/model_completion_provider.cpp` | `ModelCompletionProvider::complete()` | L69-L95 | 何时调用模型、失败时如何降级、是否会影响 deterministic list | ✅ available→builder→`infill`→`validate`；任一步失败返回 `nullopt`，**不影响确定性列表** |
| `src/observer/sql/autocomplete/model_completion_validator.cpp` | `ModelCompletionValidator::validate()` | L250-L306 | 清洗、方言截断、Parser/Catalog 校验的真实顺序 | ✅ 顺序=**clean(L92)→方言(is_forbidden 截断)→Parser(prefix_is_valid)→Catalog(catalog_identifiers_valid)** |

### 模型链关键事实

- [x] 模型只做 **ghost text**，不判断 SQL 合法性 — ✅ 只写 `response.ghost_text`（L316-L319）。
- [x] 模型不可用/超时/非法时确定性补全照常 — ✅ 各层返回 `nullopt` 后被静默忽略。
- [x] 上下文只含“允许方言 + 相关 schema” — ✅（但**只按表数限长**）。
- [x] 使用 FIM `/infill` 而非 chat — ✅。
- [x] 输出裁剪为最长合法前缀；“合法”= Parser 语法 **+ `表.列` 限定名存在**（`longest_valid_prefix` L214-L233 + `catalog_identifiers_valid` L183-L203）；⚠️ **不含裸表/裸列/类型**。
- [x] 禁止 HAVING/LIMIT/ALTER — ✅（`is_forbidden`）。
- [x] 新表/新字段是否被删 — ⚠️ **只有 `Word . Word` 限定名**会被 Catalog 校验发现并截断。
- [x] 降级路径层 — ✅ provider（health）/client（网络与 HTTP）/validator（校验）。
- [x] 触发策略 — ✅ 存在：`!partial.empty() && items.size()==1 && kind==Keyword` 时跳过模型（L304-L309）。
- [ ] P40 100–300ms 延迟 — ❔ 源码无该指标；需实测日志/测试数据，PPT 不应凭空写。

**`SqlCompletionConfig` 默认值**：`enabled=true`、`model_enabled=true`、`llama_base_url="http://127.0.0.1:8012"`、`model_debounce_ms=150`、`model_http_timeout_ms=800`、`model_max_tokens=32`、`model_temperature=0.0`、`max_completion_items=12`、`max_schema_tables=8`、`max_schema_columns=128`、`max_context_tokens=4096`。
> ⚠️ 服务端实际未使用：`model_debounce_ms`、`max_completion_items`、`max_schema_columns`、`max_context_tokens`；引擎 max_items 取 `request.max_items`（0 回退 12，L299）。

---

# 三、补全协议与服务端：核验结果

| 完整文件路径 | 函数 / 类型 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/observer/event/session_event.h` | `ClientRequestType::COMPLETE` | L33 | enum 的准确位置；completion 请求字段有哪些 | ✅ enum L24-L34，`COMPLETE` 在 L33 |
| `src/observer/event/session_event.h` | `set/get_completion_*` | L70-L77 | 实际 getter/setter 名称、字段类型 | ✅ `sql`(string)、`cursor`(size_t)、`max_items`(size_t)、`want_model`(bool)；私有字段 L90-L93，默认 `cursor=0,max_items=12,want_model=true` |
| `src/observer/net/native_communicator.cpp` | `read_event()` | L122-L131 | `type == "complete"` 的 JSON 解析位置与字段默认值 | ✅ `type` 默认 `"query"`（L107）；complete 分支读 `sql/cursor(缺省 sql.size())/max_items(12)/want_model(true)` |
| `src/observer/service/database_service.cpp` | `DatabaseService::complete_sql()` | L356-L384 | 如何取得 Db/Catalog、调用 engine、生成 `QueryResult` | ✅ 取 `session->get_current_db()`（无库 `SCHEMA_DB_NOT_EXIST`）→ 组装 `CompletionRequest` → `completion_engine().complete` → 生成 `QueryResult` |
| `src/observer/service/database_service.cpp` | `completion_engine()` | L34-L46 | 是否进程级单例、生命周期与线程安全 | ✅ 函数局部 `static`（进程级、至进程结束）；C++11 初始化线程安全，但 **`complete()` 无显式互斥锁** |
| `src/obclient/client.cpp` | `fetch_completion()` | L1030-L1058 | complete 请求 JSON 的精确字段和响应解析 | ✅ `{"type":"complete","sql","cursor","max_items":12,"want_model"}`；解析 `rows`（要求 ≥8 列）与 `attributes.ghost_text/model_used` |
| `src/obclient/client.cpp` | `complete_line()` | L1096-L1119 | replxx Tab callback 如何转成 CompletionItem | ✅ Tab 回调；`/`、`\` 转命令补全；SQL 以 `want_model=false` 请求，只取 `insert_text` |
| `src/obclient/client.cpp` | `hint_line()` | L1130-L1146 | ghost hint 与 deterministic list 的关系 | ✅ 行内提示回调，`want_model=true`，仅 ghost 非空时提示（与候选列表相互独立） |
| `src/obclient/client.cpp` | `print_completion()` | L1067-L1084 | `--complete` / `/complete` 的输出逻辑 | ✅ `want_model=false`，逐行输出 `insert_text/kind/source/detail`，有 ghost 再打印 |

### 协议层原理核验

- [x] 请求为 `{"type":"complete","sql":...,"cursor":...,"max_items":...,"want_model":...}` — ✅（native_communicator L122-L131）。
- [x] 响应 `rows` 列顺序 `insert_text, display_text, kind, source, replace_start, replace_end, score, detail` — ✅（database_service L373-L375；score 以字符串写出）。
- [x] `ghost_text`、`model_used` 位于 `attributes` — ✅（L381-L382）。
- [x] Web 与 CLI 都只是协议消费者，不自行解析 SQL — ✅ CLI 不解析（client.cpp L45-L47、L1160）；Web 经 Python SDK 的 native 连接。
- [x] complete 与普通 query 共享 Session / 当前数据库 — ✅（`complete_sql` 用 `event.session()` 的当前库）。
- [x] framing 仍为 `JSON + '\0'` — ✅ 写 L152-L156、读 L75；complete 无特殊 framing。

---

# 四、Web 公网访问：核验结果

## 4.1 Python Web Gateway

| 文件路径 | 函数 / 类 / 路由 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/obclient/web/csudb_web.py` | `SessionStore` | L67-L98 | token→connection 的数据结构、锁与过期策略 | ✅ `dict[str,csudb.Connection]`+`threading.Lock`；**无过期/TTL 策略**（仅 logout/进程退出释放） |
| `src/obclient/web/csudb_web.py` | `POST /api/connect` handler | L304-L324 | 如何创建 SDK/native connection、token 如何返回 | ✅ `csudb.connect(...)`，失败 401；成功发 `HttpOnly; SameSite=Lax` Cookie 并在 JSON 返回 `session_token` |
| `src/obclient/web/csudb_web.py` | `POST /api/query` handler | L334-L338 | SQL 如何转发到 SDK / native 连接 | ✅ `connection._request({"type":"query","sql":sql})` 走 native |
| `src/obclient/web/csudb_web.py` | `POST /api/complete` handler | L345-L359 | 补全请求如何转发；cookie/header session 如何读取 | ✅ 转发 `{"type":"complete",...}`，`max_items` 固定 12；session 取 `X-CSUDB-Session` 头优先，其次 Cookie（L210-L217） |
| `src/obclient/web/csudb_web.py` | `OPTIONS` / CORS 代码 | L146-L161 / L164-L167 | `Access-Control-Allow-Origin` 是否回显 Origin；允许哪些 header/method | ✅ 回显 `Origin`（无则 `*`）L157-L158；headers `Content-Type, X-CSUDB-Session`；methods `GET,POST,OPTIONS`；`Vary: Origin`；OPTIONS→204 |
| `src/obclient/web/csudb_web.py` | server startup | L368-L384 | 是否默认只监听 loopback | ✅ 默认 `127.0.0.1`（L372），`main` 强制校验非回环即报错退出（L382-L384） |

## 4.2 前端 JS

| 文件路径 | 函数 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/obclient/web/app.js` | `acRequest()` | L82 | debounce、cursor、want_model 等请求字段 | ⚠️ `want_model` **前端硬编码 `true`**、**不发 `max_items`**；debounce 在 L84（输入事件，100ms）；用 `acGen` 丢弃过期响应 |
| `src/obclient/web/app.js` | `acRender()` | L78 | 候选 DOM 渲染与选择状态 | ✅ 建 `div.completion-item`（`active`/`ai`），显示 text/kind/detail，`onmousedown→acAccept(i)` |
| `src/obclient/web/app.js` | `acAccept()` | L79 | 使用 replace_start/end 替换输入文本的逻辑 | ✅ `v.slice(0,it.start)+it.text+v.slice(it.end)`（start/end 来自响应列 4/5） |
| `src/obclient/web/app.js` | `acShowGhost()` | L81 | ghost overlay 如何显示、接受 | ✅ 写 `#sql-ghost` 显示“AI ⟶ …（Tab 接受）”，`onclick=acAcceptGhost` |
| `src/obclient/web/app.js` | API_BASE 相关函数/变量 | L39 / L41 | `window.CSUDB_API_BASE` 的实际读取与 URL 拼接方式 | ✅ L39 读取并去尾斜杠，缺省空串；`api()` L41 `fetch(API_BASE+path,...)` |

## 4.3 公网代理 / 隧道

| 文件路径 | 函数 / 入口 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `scripts/tcp_proxy.py` | 主监听/accept loop | L88-L98 | 监听地址、端口、并发模型 | ✅ `0.0.0.0:8157`（L29-L31），`listen(128)`，每连接一个守护线程 |
| `scripts/tcp_proxy.py` | 协议嗅探函数 | L59-L85（判定 L71/L75） | 是否确实用 `recv(8, MSG_PEEK)`；识别哪些 HTTP method | ✅ `recv(8, socket.MSG_PEEK)`；前缀 `GET/POST/PUT/HEAD/OPTI/DELE/CONN/PATC`（L33） |
| `scripts/tcp_proxy.py` | 转发逻辑 | L75/L84-L85 | HTTP→8765、native→6789 是否固定 | ✅ 目标固定：HTTP→`127.0.0.1:8765`，否则→`127.0.0.1:6789`；双向 `pipe` 全双工 |
| `src/obclient/web_console_launcher.cpp` | launcher 相关函数 | `start_web_console` L238-L309 | `/web` 如何启动 Web Console | ✅ `fork+setsid`，stdout/stderr→`~/.csudb/web.log`，`execlp("python3", script, ...)`；父进程轮询 pid；归属用 `/proc/<pid>/cmdline` 双确认 |

### Web 原理核验

- [x] 一个公网 TCP 端口同时服务 Web 与 native 靠协议嗅探代理 — ✅。
- [x] `MSG_PEEK` 只看不消费，后续报文透明转发 — ✅。
- [x] HTTP→Web Gateway，其他→DB native port — ✅。
- [ ] Cloudflare Tunnel 只负责 HTTP/HTTPS、不负责 native TCP — ❔ **无法确认**（仓库无 tunnel 配置/证据；`tcp_proxy.py` 是纯 TCP，native 走 8157 直连）。
- [x] GitHub Pages 静态前端通过 CORS + `CSUDB_API_BASE` 调公网 API — ✅。
- [ ] 隧道地址是否仍为临时 `trycloudflare.com` — ❔ 无法确认（仓库无该字符串）。
- [ ] `117.50.163.43:8157` 是否仍可访问 — ❔ 离线无法探活；但仓库已把它写死在 `scripts/client_readme.md`、`scripts/sdk_readme.md`。
- [x] 架构事实 — ✅ 公网端口 **8157**；**8157 同时服务 HTTP 与 native**；HTTP 网关 **8765**、DB native **6789** 均只监听回环；**本地 proxy 无 TLS**（HTTPS 终止点无法确认）。

---

# 五、Python SDK：核验结果

| 完整文件路径 | 函数 / 类 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `sdk/python/csudb.py` | `connect()` | L485-L501 | 参数签名、登录流程、默认端口/数据库 | ✅ `(host,port=6789,user="root",password="",database="sys",timeout=5.0)`；未知 kwarg→`TypeError`；端口校验 |
| `sdk/python/csudb.py` | `_NativeConnection` | L182-L238 | socket 生命周期、JSON + `\0` framing、recv buffer | ✅ `create_connection`+`settimeout`；紧凑 JSON+`\0`，`recv(4096)` 累积找 `\0`，上限 16MiB（L78）；close 幂等 |
| `sdk/python/csudb.py` | `_bind()` | L165-L174 | `%s` placeholder 数量校验与转义规则 | ✅ `split("%s")` 数量不符→`ProgrammingError`；转义见 `_quote` L147-L156 |
| `sdk/python/csudb.py` | `Cursor.execute()` | L383-L399 | 调 connection 的哪个方法；结果如何物化 | ✅ `connection._request({"type":"query","sql":sql})`；行物化为 `tuple(str(cell))`（L395） |
| `sdk/python/csudb.py` | `Cursor.fetchone()` | L417-L422 | cursor index / None 语义 | ✅ 推进本地 `_position`，取尽返回 `None`，不联网 |
| `sdk/python/csudb.py` | `Cursor.fetchmany()` | L428-L432 | size 默认值 | ✅ `size=None` 用 `arraysize`（类属性 L362 =1），负数归零 |
| `sdk/python/csudb.py` | `Cursor.fetchall()` | L437-L440 | 返回剩余还是全部 | ✅ 返回剩余全部并移动 position 到末尾 |
| `sdk/python/csudb.py` | `description` / `rowcount` | L391-L394 / L398 | 是否符合 DB-API 2.0 风格 | ✅ description 7 元组；rowcount 有列=行数，无列=affected（缺失 -1） |
| `sdk/python/csudb.py` | `server_info` / `buffer_snapshot` | L311-L312 / L317-L318 | 是否确有诊断扩展接口 | ✅ 均存在；`buffer_snapshot(limit=20)` 夹在 0..200 |

### Python SDK 原理核验

- [x] 零第三方依赖，仅标准库 — ✅（`json/socket/threading/typing`）。
- [x] framing 为 UTF-8 JSON 后跟 `\0` — ✅。
- [x] `%s` 为客户端字面量转义，非服务端 prepared — ✅。
- [x] `None→NULL`、bool→`1/0`、数字→`str`、字符串→加引号并 `'` 加倍 — ✅；⚠️ **bytes 实际为带单引号的 hex 字符串**（L155），L144 注释“无引号”需更正。
- [x] 自动登录/选库；失败抛异常 — ✅；`_raise_for_error`（L468-L476）恒抛基类 `DatabaseError`（不按 name 派生子类）。
- [x] DB-API 异常层级 — ✅ `Warning/Error/InterfaceError/DatabaseError(Data/Operational/Integrity/Internal/Programming/NotSupported)`。
- [x] 结果类型统一字符串 — ✅（L395）。
- [x] 直连公网完全复用 native，不经 HTTP Gateway — ✅。

---

# 六、JDBC：核验结果

| 完整文件路径 | 函数 / 类 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `sdk/java/.../CsuDbDriver.java` | `CsuDbDriver` | L42-L48 | 静态注册块、`META-INF/services` 是否两者都有 | ✅ 静态块 `registerDriver`；service 文件内容 `edu.csu.csudb.jdbc.CsuDbDriver`，已打进 jar |
| `sdk/java/.../CsuDbDriver.java` | `connect()` | L66-L96 | JDBC URL 解析、properties 合并、NativeClient 创建 | ✅ `URI.create(url.substring(5))`；properties 显式优先（`putIfAbsent`）；默认 host `127.0.0.1`/port `6789`/db `sys` |
| `sdk/java/.../CsuDbDriver.java` | `acceptsURL()` | L105-L107 | 接受的 URL 格式 | ✅ `jdbc:csudb://` 前缀 |
| `sdk/java/.../JdbcProxies.java` | Connection proxy handler | L169-L203 | 哪些 JDBC 方法真正实现 | ✅ createStatement/prepareStatement/close/isClosed/isValid/autoCommit/commit/rollback/catalog&schema/metaData/readOnly/isolation/networkTimeout/abort 等 |
| `sdk/java/.../JdbcProxies.java` | Statement proxy handler | L291-L338 | execute/executeQuery/executeUpdate 的行为 | ✅ 三类 execute + batch + maxRows/fetchSize/queryTimeout 等；行物化 |
| `sdk/java/.../JdbcProxies.java` | PreparedStatement proxy | L608-L626 / L639-L651 | `?` 参数如何绑定和转义 | ✅ `bind` 跳过引号内 `?`，缺参 `SQLState 07001`；`literal/quote` **客户端字面量拼接** |
| `sdk/java/.../JdbcProxies.java` | ResultSet proxy | L453-L494 | `next/getString/getInt/...` 实现范围 | ✅ 定位+getString/getObject/getInt/getLong/getFloat/getDouble/getBigDecimal/getBoolean/findColumn/wasNull 等 |
| `sdk/java/.../JdbcProxies.java` | unsupported path | L125-L127 | 未实现方法是否统一 `SQLFeatureNotSupportedException` | ✅ 各 handler `default -> throw unsupported(method)`（L201/L336/L492/L542/L567），SQLState `0A000` |
| `sdk/java/.../NativeClient.java` | `NativeClient` | L35 / L51-L73 / L76-L81 | socket/framing/login/query 生命周期 | ✅ connect+setSoTimeout，构造器内 login；`synchronized`；framing 写 `\0`，**逐字节读**直到 `\0`，上限 16MiB |
| `sdk/java/.../Json.java` | JSON encode/decode | L35-L289 | 是否确实为自实现、无第三方 JSON 库 | ✅ 自实现递归下降，import 全为 `java.*` |

### JDBC 原理核验

- [x] JDBC 4 自动发现机制 — ✅（静态块 + service file 双保险）。
- [x] URL 为 `jdbc:csudb://HOST:PORT/DATABASE` — ✅。
- [x] user/password/connectTimeout 属性名 — ✅（默认 `root`/`""`/`"5000"` 毫秒）。
- [x] Dynamic Proxy 用于 Connection/Statement/PreparedStatement/ResultSet/Metadata — ✅。
- [x] `?` 绑定仍是客户端拼接/转义 — ✅。
- [x] 类型映射 DBMS→`java.sql.Types` — ⚠️ **粗粒度**：含 `INT→INTEGER`、`FLOAT/DOUBLE/DECIMAL→FLOAT`、`DATE→DATE`、`BOOL→BOOLEAN`、其余 `VARCHAR`（L593-L600）。
- [x] 服务端错误码写入 `SQLException.getErrorCode()` — ✅（`NativeClient.checked` L141-L148，SQLState `HY000`）。
- [x] 未支持 API 的异常类型与错误信息 — ✅ `SQLFeatureNotSupportedException("... does not yet support "+name, "0A000")`。
- [x] 零第三方依赖 — ✅。
- ⚠️ 附注：`jdbcCompliant()` 恒返回 `false`（L135）；`getColumnClassName()` 恒 `String`（L534），`getColumnDisplaySize=255`、`getPrecision/Scale=0`。

---

# 七、CLI 公网客户端：核验结果

| 完整文件路径 | 函数 | 行号 | 需要核验的实现方法 | 核验结果 |
|---|---|---:|---|---|
| `src/obclient/client.cpp` | `apply_url()` | L294-L328 | 接受 `host[:port]` / `native://` / `http://` / `https://` 的准确规则 | ⚠️ **任意 `xxx://` 前缀都会被剥离**（不只 native/http/https）；截掉 `/` 后路径；IPv6 `[::1]:6789` 单独处理；其余按最后一个冒号切 host:port |
| `src/obclient/client.cpp` | `-U/--url` 解析与优先级 | L353/L380；L397-L401 | 参数名与优先级 | ✅ 优先级：默认<配置文件<profile<环境变量<命令行；`-h/-P` 最后覆盖（L400-L401） |
| `src/obclient/client.cpp` | `fetch_completion()` | L1030-L1058 | native complete 请求 | ✅ 8 字段请求/8 列响应/attributes ghost |
| `src/obclient/client.cpp` | `complete_line()` | L1096-L1119 | replxx Tab callback | ✅ Tab 回调，`want_model=false` |
| `src/obclient/client.cpp` | `hint_line()` | L1130-L1146 | ghost hint callback | ✅ `want_model=true`，仅 ghost 非空提示 |
| `src/obclient/client.cpp` | `/complete` command handler | L1243-L1248 | meta command 的解析与输出 | ✅ `lower=="/complete"` → `print_completion(rest)` |
| `src/obclient/client.cpp` | `--complete` 非交互路径 | 选项 L199/L358；执行 L968 | 非交互补全入口 | ✅ `print_completion(options_.complete_sql)` |
| `scripts/package_client.sh` | build/package logic | L37-L64 | Release、STATIC_STDLIB、tar/sha256 的真实流程 | ✅ Release+ASAN OFF+STATIC_STDLIB ON+单测 OFF；`tar -czf`+`sha256sum` |
| `src/obclient/CMakeLists.txt` | static stdlib option | — | `STATIC_STDLIB` 是否确实只静态 libstdc++/libgcc | ⚠️ **需修正**：`STATIC_STDLIB` 定义/生效在**根 `CMakeLists.txt` L32/L82-L84**，`src/obclient/CMakeLists.txt` 中无此项；静态的是 `-static-libgcc -static-libstdc++`（ASan/TSan 时再静态对应运行时） |

### CLI 原理核验

- [x] `--url` 与 `-h/-P`、`CSUDB_URL`、配置文件的优先级 — ✅ `-h/-P` > `--url`/`CSUDB_URL` > 配置文件。
- [x] `http(s)://` 只为解析 host:port，最终仍是 native TCP — ✅。
- [x] CLI 不解析 SQL — ✅（只发文本、消费 `QueryResult`）。
- [x] replxx 是实际交互库 — ✅（`common/linereader/line_reader.h` L18/L60）。
- [x] 候选列表与 ghost hint 用两个 callback — ✅ `complete_line`(L1096) 与 `hint_line`(L1130)，经 `complete_dispatch`/`hint_dispatch`(L1384/L1399)。
- [x] 静态打包后真实依赖只有 `libc/libm` — ✅ `ldd build_release/bin/csudb` 实测仅 `libm.so.6`、`libc.so.6`、`ld-linux`（`dist/` 同）。
- [x] “解压即用”平台/glibc — ✅ 已在 `scripts/client_readme.md` L8-L11 写明（Linux x86_64、glibc ≥ 2.35）。

---

# 八、requirements.md 对照：核验结果

## 8.1 必备 SQL

| 功能 | Lexer token | Bison 产生式 | AST 结构 | 语义校验 | 结论 |
|---|---|---|---|---|---|
| CREATE | L155/L157 | `create_table_stmt` L657-L676 | `CreateTableSqlNode` parse_defs.h L235-243 | `create_table_stmt.cpp` L20-29 | ✅ 支持 |
| INSERT | L173/L174/L175 | `insert_stmt` L778-L795 | `InsertSqlNode` L188-193 | `insert_stmt.cpp` L30-126 | ✅ 支持 |
| SELECT | L166/L168 | `select_stmt` L884-L917 | `SelectSqlNode` L163-172 | `select_stmt.cpp` L34-201 | ✅ 支持 |
| DELETE | L176 | `delete_stmt` L848-L858 | `DeleteSqlNode` L199-203 | `delete_stmt.cpp` L31-58 | ✅ 支持 |
| WHERE | L169 | `where` L1054 / `select_where` L1065 | `SelectSqlNode.where_*` L167-168 | 绑定 `select_stmt.cpp` L138-152 | ✅ SELECT 完整；⚠️ DELETE/UPDATE 的 WHERE 仅 **AND 连接的简单比较**（`condition_list` L1127），**不支持 OR/NOT/括号** |
| 比较运算 | L218-L225 | `comparison_predicate` L1112 / `comp_op` L1199 | `CompOp` L90-101 | `bind_comparison_expression` L428（**仅绑定**） | ✅ 支持 |
| AND | L170 | `boolean_expr` L1089 / `condition_list` L1137 | `ConjunctionExpr` expression.h L360 | `bind_conjunction_expression` L487 | ✅ 支持 |
| OR | L171 | `boolean_expr` L1083 | 同上 | 同上 | ✅ 仅 SELECT WHERE |
| NOT | L186 | `boolean_expr` L1095（改写为 `expr==FALSE`）；`null_def` NOT NULL L726 | 复用 `ComparisonExpr` | 走 compare 绑定 | ✅ 支持（**无独立 NOT AST 节点**） |
| 括号 | L213/L214 | `expression` L963 / `boolean_expr` L1100 | 透明传递，无独立节点 | — | ✅ 支持 |

## 8.2 进阶功能

| 功能 | Lexer token | Bison 产生式 | AST 结构 | 结论 |
|---|---|---|---|---|
| UPDATE | L177/L178 | `update_stmt` L864-L876 | `UpdateSqlNode` L209-215 | ✅ 支持（单字段 SET） |
| ORDER BY | L200/L153/L154 | `order_by` L1223 / `order_by_unit` L1236 / `order_by_list` L1258 | `OrderByUnit`/`OrderDirection` L147-161 | ✅ 每项独立 ASC/DESC |
| GROUP BY | L198/L199 | `group_by` L1210 | `SelectSqlNode.group_by` L169 | ✅（未拒绝 `group by *`） |
| JOIN | L161/L162/L160 | `join_clause` L1035 | `JoinSqlNode` L141-145 / `joins` L171 | ✅ 语法；语义等价转为表集合+ON 与 WHERE 合并（**无独立 join 算子**） |
| 算术表达式 | L228-L231 | `expression` L950 / 优先级 L506-L508 | `ArithmeticExpr` expression.h L399 | ✅ 类型校验收敛于 `bind_arithmetic_expression` L534 |
| NULL | L184/L185 | `null_def` L717 / `value` L829 / IS [NOT] NULL L1116 | `CompOp.IS_NULL/IS_NOT_NULL` L98-99 | ✅ 建表可空、插入 NULL、IS [NOT] NULL |

> 执行实现（算子/优化/存储）不属本人，按清单口径归队友；此处仅确认**编译器语法+语义**支持。

---

# 九、语义分析边界：核验结果

| requirements 要求 | 文件 / 函数 | 行号 | 归属 | 核验结果 |
|---|---|---|---|---|
| 表存在性 | `select_stmt.cpp:SelectStmt::create` / `db.cpp:Db::find_table` | L53-57/L72-76；L218-234 | **stmt/storage（队友）** | ✅ 走 `db->find_table()`（大小写不敏感）；binder 只校验“表名是否在 FROM 集合内”（expression_binder.cpp L314-322） |
| 列存在性（SELECT/WHERE） | `expression_binder.cpp:bind_unbound_field_expression` + `TableMeta::field` | L288-347；`table_meta.cpp` L164-175 | **本人核心** | ✅ 用 `table->table_meta().field(name)` 查列 |
| 列存在性（INSERT/UPDATE） | `insert_stmt.cpp` / `update_stmt.cpp` | insert L72-88；update L52-58 | **stmt（队友）** | ⚠️ 走 stmt 层；`insert_stmt` 复用本人 `set_binder_error_message` |
| 名字绑定 | `expression_binder.cpp` | `wildcard_fields` L154；`bind_expression` L176；`bind_unbound_field_expression` L288；`bind_star_expression` L240 | **本人核心** | ✅ `UnboundFieldExpr→FieldExpr`，`*` 展开跳过系统字段 |
| 类型一致性 | `expression_binder.cpp` | `bind_arithmetic_expression` L534；`set_arithmetic_type_error` L92；`check_aggregate_expression` L609 | **部分本人** | ⚠️ binder 只对**算术**（数值）与**聚合**校验；比较/布尔连接**未做**；INSERT/UPDATE 类型在 stmt（insert_stmt.cpp L91-121、update_stmt.cpp L60-82） |
| INSERT 匹配 | `insert_stmt.cpp:InsertStmt::create` | L30-126 | **队友** | ⚠️ 表存在、列对齐、缺列填 NULL、逐列类型匹配；PPT 写“下游 stmt 层已有接口完成” |
| Catalog create/find | `db.cpp:create_table/find_table`；`schema_catalog.cpp:register_table` | L172-211/L218-234；L111-141 | **队友/storage** | ⚠️ PPT 写“复用 Catalog 接口” |
| Catalog 持久化 | `schema_catalog.cpp` / `db.cpp` | L111/L143/L149-247；L371-408/L483-526 | **队友/storage** | ⚠️ 不计入个人编译器成果 |

**边界结论（PPT 措辞用）**
- 属**本人（Parser/Binder）**：lex/yacc 全部产生式与 AST（`parse_defs.h`/`expression.h`）、`parse_stage`/`resolve_stage`、`expression_binder`（名字绑定、`*` 展开、算术/聚合类型校验、thread-local 语义错误槽）。
- 属**复用/调用队友**：`Stmt::create_stmt` 及 `SelectStmt/InsertStmt/UpdateStmt/DeleteStmt::create` 中的表存在性、INSERT 匹配与列/类型校验；`Db::create_table/find_table`；`SchemaCatalog` 的 create/find/持久化；执行算子与优化。

---

# 十、原理页核验结果

## 10.1 编译器总流程图

```text
SQL 文本 → Flex Lexer → Token+Location → Bison LALR(1) → ParsedSqlNode/AST
→ ParseStage → ResolveStage → (Stmt::create 内部) ExpressionBinder → 已绑定 Stmt
→ 执行/优化层（队友）
```

- [x] 每个箭头对应真实调用链 — ✅。
- [x] `Stmt::create` 位置与职责 — ✅ `Stmt::create_stmt`（`stmt.cpp:49`）由 `resolve_stage.cpp:81` 调用。
- [x] `ExpressionBinder` 在 `Stmt::create` 内还是 ResolveStage 直接调 — ✅ **在 `Stmt::create` 内**（如 `select_stmt.cpp:88` 调 `bind_expression`）。

## 10.2 语法错误诊断原理图

- [x] 未闭合字符串走相同路径 — ✅（落兜底 L237→`yyreport_syntax_error` L1428-L1437 识别 `unterminated string literal`）。
- [x] expected 集合最大数量/截断 — ✅ 收集模式 128（L1412），正常报错 64（L1445）。
- [x] 原始非法字符恢复成可读文本 — ✅ 用列号回原串取字符（L1428-L1437）。

## 10.3 语义错误透传原理图

- [x] `thread_local` 真实存在 — ✅ `g_binder_error_message` L64。
- [x] reset 准确时机 — ✅ 每条语句处理前 `resolve_stage.cpp:79`。
- [x] INSERT 类型错误是否走同一槽位 — ✅ 是（`insert_stmt.cpp:83/L119` 调 `set_binder_error_message`）。

## 10.4 自动补全确定性链图

- [x] 实际调用顺序 — ✅ grammar → **scope** → catalog → `sort_and_trim`。（原图 order 需调整为：Scope 在 Grammar 之后、Catalog 之前）
- [x] rank/dedupe 发生在哪个函数 — ✅ rank+截断 `sort_and_trim`（engine L179）；去重在两个 provider（grammar `seen` L190 / catalog `add_item` L263）。

## 10.5 模型补全链图

- [x] 是否存在 suffix/FIM — ✅ `input_suffix`，`POST /infill`。
- [x] validator 真实步骤顺序 — ✅ clean→方言→Parser→Catalog。
- [x] “最长合法前缀”算法 — ✅ 按 token 从大到小，需同时过 Parser 语法 + `表.列` 存在。
- [x] 模型输出与 dropdown 的关系 — ✅ **只进 ghost**，不进 rows。

## 10.6 公网访问架构图

- [x] 当前真实端口 — ✅ 公网 **8157**；HTTP **8765**；native **6789**（后两者仅回环）。
- [ ] Cloudflare Tunnel 在架构中的位置 — ❔ 无法确认。
- [ ] GitHub Pages 实际指向哪个 API_BASE — ❔ 仓库内 `config.js` 不在本工作区（在 `gh-pages` 分支）；当前值需另行确认。
- [x] 8157 是否 HTTP 与 native 共用 — ✅ 是。
- [ ] HTTPS 是否终止在 Cloudflare — ❔ 无法确认；可确认**本地 proxy 无 TLS**。

## 10.7 SDK/JDBC 调用链图

- [x] 登录与普通 query 同一连接 — ✅（同一 socket/`NativeClient`/`Connection`）。
- [x] DatabaseService 真实入口 — ✅ `DatabaseService::execute`（COMPLETE 分派 L487-L490）。
- [x] SDK/JDBC 复用相同协议字段 — ✅（SDK 与 JDBC 均走 native JSON+`\0`）。
- [x] 返回结果带 columns/types/rows/error code — ✅。

---

# 十一、最优先核验顺序

（已完成，保留原顺序作为索引）
1. 自动补全 `.cpp` 行号与调用链 — 见 [二](#二代码自动补全核验结果)。
2. `database_service.cpp` / `native_communicator.cpp` / `session_event.h` 的 complete 协议 — 见 [三](#三补全协议与服务端核验结果)。
3. Python SDK — 见 [五](#五python-sdk核验结果)。
4. JDBC — 见 [六](#六jdbc核验结果)。
5. Web Gateway 路由 — 见 [4.1](#41-python-web-gateway)。
6. `app.js` 补全 — 见 [4.2](#42-前端-js)。
7. `tcp_proxy.py` 协议嗅探 — 见 [4.3](#43-公网代理--隧道)。
8. CLI `apply_url/fetch_completion/complete_line/hint_line` — 见 [七](#七cli-公网客户端核验结果)。
9. parser 索引行号漂移抽查 — 见 [一](#一编译器核验结果)。

---

# 十二、回填示例（格式保留）

```text
[已核验]
src/observer/sql/autocomplete/completion_engine.cpp
CompletionEngine::complete
L212-L324
原理：语句提取 → 追加哨兵 '\x01' 取 expected → grammar → scope → catalog
→ sort_and_trim（score 降序）→ 满足条件才调 model，model 只进 ghost_text。
```

---

# 十三、关键修正与无法确认（汇总）

## 13.1 PPT 必须修正（⚠️）

1. **哨兵归属**：`collect_expected_tokens()` 自身不追加哨兵；哨兵 `'\x01'` 由补全层（`completion_engine.cpp:248`、`model_completion_validator.cpp:158`）追加。`expected_tokens.h` 原头注释已同步修正。
2. **`complete_grammar` 职责**：不调用 `collect_expected_tokens`、不做 parser 试探；调用与试探都在 `CompletionEngine::complete`（L241-L283）。
3. **排序/去重**：只有 `score`（同分按文本），**无 kind/source 权重**；**无全局 merge 去重**（provider 内各自去重）。
4. **Validator 的 Catalog 校验**：只覆盖 `表.列` 限定名；“最长合法前缀”= Parser 语法 + 限定名存在，**不含裸表/裸列/类型**；不存在的新字段并非一律被删。
5. **上下文限长**：只限制表数（`max_schema_tables`）；`max_schema_columns`/`max_context_tokens`/`max_completion_items`/`model_debounce_ms` 服务端未使用。
6. **Scope 不支持 alias**：`TableBinding.alias` 恒空。
7. **关键字表为手工硬编码**，运行时不与 parser 自动同步。
8. **`table.*` 文法不可达**（无 `ID DOT '*'` 产生式）；只有裸 `*`。
9. **比较/布尔连接 binder 不做类型校验、不强制 BOOL**；只有算术（数值）与聚合校验。
10. **“歧义列”实为“省略表名时要求查询仅一张表”**（expression_binder.cpp L301-L312）。
11. **DELETE/UPDATE 的 WHERE 仅 AND 连接的简单比较**，不支持 OR/NOT/括号；只有 SELECT 走完整 `boolean_expr`。
12. **CLI `apply_url` 对任意 `xxx://` 都剥离 scheme**（不只 native/http/https）。
13. **`STATIC_STDLIB` 在根 `CMakeLists.txt`**（L32/L82-L84），非 `src/obclient/CMakeLists.txt`；静态 `-static-libgcc -static-libstdc++`；实测只依赖 `libc/libm`。
14. **JDBC**：`jdbcCompliant()` 恒 `false`；`sqlType()` 把 `DOUBLE/DECIMAL` 也映射为 `Types.FLOAT`；`getColumnClassName()` 恒 `String`。
15. **Python `_quote` 对 bytes 返回带引号的 hex**（`sdk/python/csudb.py` L155），L144 注释需更正。
16. **`RETURN_TOKEN` 含 `LOG_DEBUG`**，不只是返回 token。
17. 补全链图中 Scope 的实际位置是 **Grammar 之后、Catalog 之前**（`completion_engine.cpp:287`）。

## 13.2 无法确认（❔，需现场/部署或实测证据）

1. Cloudflare Tunnel 的存在与“只映射 HTTP/HTTPS、不映射 native TCP”。
2. 隧道地址是否仍为临时 `trycloudflare.com`。
3. `117.50.163.43:8157` 当前是否仍可访问（离线无法探活）。
4. HTTPS 是否在 Cloudflare 终止（只能确认本地 `tcp_proxy.py` 无 TLS）。
5. GitHub Pages 当前 `config.js` 指向的 API_BASE（在 `gh-pages` 分支）。
6. P40 模型 100–300ms 延迟（源码无该指标，需实测日志/测试数据）。

## 13.3 与源码一致的亮点（可直接写 PPT）

- 两段式编译管线与多语句错误策略（任一 `SCF_ERROR` 即整次语法错误）。
- 语义错误 **thread-local 错误槽** + 行列定位透传到客户端。
- 补全复用**同一份 Bison 文法**取 expected 集合，不引入第二套 parser。
- 确定性补全五层链（语句提取 / Parser expected / Catalog / Scope / 方言门禁）。
- 模型仅做 ghost，且 FIM `/infill` + clean→方言→Parser→Catalog 四段校验后裁最长合法前缀，失败静默降级。
- 公网 **8157 一个端口**靠 `MSG_PEEK` 协议嗅探同时服务 Web 与 native。
- SDK/JDBC/CLI 全部复用 native `JSON + '\0'` 协议，零第三方依赖；CLI 静态打包实测只依赖 `libc/libm`。
