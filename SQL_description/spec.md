# SQL 编译器详细规格说明书（Spec）

- 版本：v3.0（编译器层，函数级）
- 日期：2026-09-14
- 适用范围：**SQL 编译器层** —— `src/observer/sql/parser/`（词法 / 语法 / 语义分析）与 `src/observer/sql/autocomplete/`（SQL 输入补全）
- 上游文档：
  - `SQL_description/requirements.md`（验收需求拆解）
  - `SQL_description/文件结构规划.md`（代码目录与职责划分）
  - `SQL_description/检录报告.md`（需求对照与现状盘点）
- 路径约定：未写前缀的文件路径相对于 `src/observer/sql/`；`common/` 相对于 `src/observer/`。
- 范围说明：本文档**只覆盖本人负责的编译器层**。查询处理 / 执行（`stmt/`、`expr/`、`optimizer/`、`operator/`、`executor/`）与存储（`storage/`）属队友负责，不在本文档范围。


## 1. 概述

本文件是 SQL 编译器的**详细设计与实现规格**，将 `requirements.md` 中的验收能力落实到**具体的数据结构、接口、算法、行为契约，并细化到函数级别**，用于指导实现、评审与回归测试。

SQL 引擎采用经典的**流水线（Pipeline）式**处理模型，一条 SQL 语句依次经过四个必选阶段与两个可选阶段：

```
SQL 文本
  → ParseStage    （词法 lex + 语法 yacc → parse_defs.* 中间 AST）
  → ResolveStage  （名字/表达式绑定 → stmt/* 语句对象）
  → OptimizeStage （AST → 逻辑算子树 → 规则重写 → 物理算子树）
  → ExecuteStage  （语句执行器 + 物理算子执行 → SqlResult）
  （可选）PlanCacheStage / QueryCacheStage
```

数据自左向右单向下游；错误在任一阶段产生后向上抛出（以 `RC` 错误码 + 行列位置 + 错误消息表达），终止后续阶段。

---


## 2. 目标与非目标

### 2.1 目标（编译器层）

- 词法：识别关键字 / 标识符 / 常量 / 运算符 / 分隔符、跳过空白与注释、字符串与转义、大小写不敏感、输出 Token 行列位置。
- 语法：优先级（`NOT > 比较 > AND > OR`）、括号显式改变结合、构造 AST、多语句输入、语法诊断（位置 + 实际符号 + 期望集合）。
- 语义：表/列存在性校验、名字绑定、类型一致性、`INSERT` 匹配、语义错误定位。
- SQL 输入补全：确定性（Parser + Catalog）+ 可选 FIM 模型。
- 语法/语义错误必须携带**类型 + 行列位置 + 原因**，且不崩溃。

### 2.2 编译器层现状

| 能力 | 状态 | 目标规格所在章节 |
| --- | --- | --- |
| 字符串转义 `Tom''s` | ✅ 已实现（`''`/`""`/`\x`） | §6.4 |
| 语法 `expected:` 期望集合 | ✅ 已实现 | §7.5 |
| 语义错误结构化定位 | ✅ 已实现 | §8.5 |
| INSERT 逐列 TypeMismatch 报告 | ✅ 已实现（含列清单） | §8.4 |
| 类型一致性严格校验 | ✅ 已实现 | §5.3、§8.3 |
| SQL 输入自动补全 | ✅ 新增（CLI + Web，确定性 + 可选 FIM 模型） | §18 |

### 2.3 非目标

- 完整 SQL 标准（不支持子查询、`HAVING`、`UNION`、窗口函数等）。
- 计划、优化、算子、执行、存储等编译器下游能力（队友负责，不在本文档范围）。

---


## 3. 术语

| 术语 | 含义 |
| --- | --- |
| AST | 抽象语法树，本项目中 `parse_defs.h` 的中间节点（如 `SelectSqlNode`） |
| 语句对象（Stmt） | ResolveStage 产出的面向对象表示（如 `SelectStmt`） |
| 逻辑算子 | 与执行无关的计划节点（`TableGet`、`Predicate`、`Project` …） |
| 物理算子 | 可执行算子，实现 `open/next/close` 火山模型 |
| 元组（Tuple） | 执行期一行数据的抽象，`cell_at` 返回 `Value` |
| 三值逻辑 | `TRUE / FALSE / UNKNOWN`，用于 `NULL` 参与布尔运算 |
| Catalog | 元数据目录，提供 `create_table / find_table / find_field` 等接口 |
| NULL 位图 | 记录中用于标识各字段是否为 `NULL` 的位图 |

---


## 4. 总体架构与数据流


### 4.1 Stage 编排（函数级）

| 阶段 | 实现（文件） | 关键函数 | 输入 → 输出 |
| --- | --- | --- | --- |
| ParseStage | `parser/parse_stage.*` | `ParseStage::handle_request` | SQL 字符串 → `ParsedSqlResult`（AST） |
| ResolveStage | `parser/resolve_stage.*` | `ResolveStage::handle_request` | AST → `Stmt *`（名字 / 表达式绑定） |

> 之后的 `OptimizeStage` / `ExecuteStage`（计划、优化、执行）属查询处理层，由队友负责。


### 4.2 表达式语义模型

表达式是 SQL 中所有「可求值」元素的核心抽象。基类 `Expression`（`expr/expression.h`）的契约函数：

| 函数 | 契约 |
| --- | --- |
| `get_value(const Tuple &, Value &)` | 在具体元组上求值 |
| `try_get_value(Value &)` | 无元组时尝试求值（常量） |
| `get_column(Chunk &, Column &)` | 向量化列式求值 |
| `type()` | 返回 `ExprType` 定位子类 |
| `value_type()` / `value_length()` | 静态类型/长度推导 |
| `copy()` / `equal()` | 克隆 / 相等判断 |
| `line()/column()/set_location()` | 记录原始 SQL 位置（语义错误定位） |
| `pos()/set_pos()` | 在下层 chunk 中的列位置 |

`ExprType` 枚举：`NONE, STAR, UNBOUND_FIELD, UNBOUND_AGGREGATION, FIELD, VALUE, CAST, COMPARISON, CONJUNCTION, ARITHMETIC, AGGREGATION`。

---


## 5. 类型系统

### 5.1 基础类型 `AttrType`

定义于 `common/type/attr_type.h`：

| 枚举 | 说明 | 存储 |
| --- | --- | --- |
| `UNDEFINED` | 未知/占位 | — |
| `CHARS` | 字符串 | `char*` + 长度 |
| `INTS` | 32 位整数 | `int32_t` |
| `FLOATS` | 32 位浮点 | `float` |
| `VECTORS` | 向量（占位） | — |
| `BOOLEANS` | 布尔（内部类型） | `bool` |
| `DATES` | 日期，按整数 `YYYYMMDD` 存储 | `int32_t` |

辅助函数：`attr_type_to_string / attr_type_from_string / is_numerical_type / is_string_type`。

### 5.2 值 `Value`

定义于 `common/value.h`，是「值 + 类型」的统一封装。关键函数：

| 函数 | 说明 |
| --- | --- |
| `Value(AttrType, char*, int)` / 各类型构造函数 | 构造值 |
| `null_value()`（静态）/ `set_null()` / `is_null()` | NULL 构造与判断（`is_null_` 标志与类型解耦） |
| `cast_to(value, to_type, result)`（静态） | 类型转换：NULL→NULL；`DATES` 走 `DateType::cast_to`；其余按源类型分发 |
| `get_int/get_float/get_string/get_boolean/get_string_t` | 取值（类型不符时触发转换） |
| `set_int/set_float/set_string/...` | 设值 |
| `add/subtract/multiply/divide/negative`（静态） | 算术（委托 `DataType::type_instance(...)`） |
| `compare(other)` | 比较；内置 NULL 三值逻辑与 `BOOLEANS` 比较（供 NOT 使用） |

### 5.3 类型推断规则

表达式的 `value_type()` 即类型推断契约：

| 表达式 | `value_type()` |
| --- | --- |
| `ValueExpr` | `value_.attr_type()` |
| `FieldExpr` | `field_.attr_type()` |
| `ComparisonExpr` | `BOOLEANS` |
| `ConjunctionExpr` | `BOOLEANS` |
| `CastExpr` | `cast_type_` |
| `ArithmeticExpr` | 由 `calc_value` 结果类型决定 |
| `AggregateExpr` | `COUNT`→`INTS`；`AVG`→`FLOATS`；否则 `child_->value_type()` |

集中管理的类型规则（见 `requirements.md` §语义分析 9，已实现于 `parser/expression_binder.cpp::bind_arithmetic_expression`）：

```
INT  +  INT      -> INT
INT  >  INT      -> BOOL
VARCHAR = VARCHAR -> BOOL
BOOL AND BOOL    -> BOOL
NOT BOOL         -> BOOL
INT  +  VARCHAR  -> ERROR   // 已严格报错：operator '+' cannot be applied to INT and VARCHAR
```

错误消息使用面向用户的类型名 `common/type/attr_type.cpp::attr_type_to_sql_string`（`INT/VARCHAR/FLOAT/BOOL/DATE`）。

---


## 6. 词法分析规范

### 6.1 输入输出

- 输入：SQL 文件 / 单条 SQL / 多条 SQL（分号分隔）。
- 输出：`<TokenType, Lexeme, Line, Column>`。

### 6.2 Token 类别

- 关键字：`SELECT/FROM/WHERE/AND/OR/NOT/IS/NULL/CREATE/INSERT/...`（`lex_sql.l` 规则段，大小写不敏感）。
- 标识符：`[A-Za-z_][A-Za-z0-9_]*`。
- 常量：整数 `NUMBER`、浮点 `FLOAT`、字符串 `SSS`（单/双引号）。
- 运算符：`== = < <= <> != > >= + - * /`。其中 `==`、`=` → `EQ`；`!=`、`<>` → `NE`。
- 分隔符：`( ) , ; .`。

### 6.3 状态机、注释与实现函数

- `%x COMMENT`：多行注释 `/* ... */`（`BEGIN(COMMENT)` / `<COMMENT>"*/"`）。
- 单行注释：`--[^\n]*`。
- 字符串状态：单/双引号规则直接匹配转义（`''`/`""`/`\x`），`%x STR` 不再需要。

`parser/lex_sql.l` 中的入口/辅助函数：

| 函数 | 说明 |
| --- | --- |
| `scan_string(const char*, yyscan_t)` | 绑定输入缓冲（`yy_switch_to_buffer` + `yy_scan_string`） |
| `YY_USER_INIT`（宏） | 每次解析初始化 `yylineno = 1; yycolumn = 1;` |
| `YY_USER_ACTION`（宏） | 写 `yylloc`（1-based 行/列），换行时 `yycolumn` 重置 |
| `yylex`（生成于 `lex_sql.cpp`） | 词法扫描主循环（`%option yylineno` 跟踪行号、`%option case-insensitive` 折叠关键字） |

### 6.4 已实现的行为

- 字符串转义：`'Tom''s'` → `Tom's`；`''` 表示空串；`"a""b"` → `a"b`；反斜杠转义 `\x` → `x`。由 `parser/yacc_sql.y::unescape_quoted_string` 完成。
- 未闭合字符串、非法字符（如 `@`）返回词法/语法错误（类型 + 位置 + 原因），不崩溃。

---


## 7. 语法分析规范

### 7.1 输入输出

- 输入：词法分析产生的 Token Stream。
- 输出：AST（`parse_defs.h` 中间节点，经 ResolveStage 转为 `stmt/*`）。

### 7.2 优先级与结合性

`yacc_sql.y` 通过 `%left/%precedence` 声明：

- 优先级自低到高：`OR < AND < 比较(LT/GT/LE/GE/EQ/NE) < NOT`。
- 括号 `( boolean_expr )` 显式透传、改变结合结构。

### 7.3 核心 AST 节点（`parse_defs.h`）

| 节点 | 关键字段 |
| --- | --- |
| `RelAttrSqlNode` | `relation_name` / `attribute_name` |
| `CompOp`（枚举） | `EQUAL_TO, LESS_EQUAL, NOT_EQUAL, LESS_THAN, GREAT_EQUAL, GREAT_THAN, IS_NULL, IS_NOT_NULL, NO_OP` |
| `ConditionSqlNode` | `left_is_attr/left_value/left_attr`、`comp`、`right_is_attr/right_value/right_attr` |
| `JoinSqlNode` | `relation_name` + `unique_ptr<Expression> condition` |
| `SelectSqlNode` | `expressions / relations / conditions / where_expression / group_by / order_by / joins` |
| `InsertSqlNode` | `relation_name` + `vector<Value> values` |
| `DeleteSqlNode` | `relation_name` + `conditions` |
| `UpdateSqlNode` | `relation_name / attribute_name / value / conditions` |
| `AttrInfoSqlNode` | `type / name / length / nullable`（`nullable` 默认 `true`） |
| `CreateTableSqlNode` | `relation_name / attr_infos / primary_keys / storage_format / storage_engine` |
| `ErrorSqlNode` | `error_msg / line / column` |

### 7.4 解析器实现函数（`parser/yacc_sql.y` / `parse.*`）

| 函数 | 说明 |
| --- | --- |
| `yyerror()` | 语法/解析错误回调（`error: cannot back up` 等回退路径） |
| `yyreport_syntax_error()` | `%define parse.error custom` 回调：输出 `位置 + unexpected token + expected` 集合；补全模式下收集期望符号 |
| `collect_expected_tokens()`（`expected_tokens.h`） | 在 SQL 前缀末尾追加哨兵，复用 bison `yypcontext_expected_tokens` 取期望 terminal 集合，供自动补全使用 |
| `token_name()` | 由 `llocp` 截取 token 原文 |
| `unescape_quoted_string()` | 字符串转义（`''`→`'`、`""`→`"`、`\\`→`\`） |
| `create_arithmetic_expression()` | 构造 `ArithmeticExpr` 并记录**运算符**行列 |
| `create_aggregate_expression()` | 构造 `UnboundAggregateExpr` |
| `set_expr_name_and_location()` | 为表达式记录名字 + 行列 |
| `append_join_node()` | 追加 `JoinSqlNode` 到 JOIN 列表 |
| `parse()`（`parse.h/.cpp`） | 语法解析总入口，`ParsedSqlResult::add_sql_node` 收集节点 |

### 7.5 语法诊断（已实现）

语法诊断输出 `错误位置 + 实际符号 + 期望集合`：

```
SyntaxError at line 3, column 16
unexpected token: SEMICOLON
expected: LBRACE | NULL_T | NUMBER | FLOAT | ID | SSS | '-' | '*' | NOT
```

实现：`%define parse.error custom` + `yyreport_syntax_error`（`yypcontext_token` / `yypcontext_expected_tokens`），错误消息经 `ParseStage::handle_request` 透出；多语句输入中任一条解析失败也会被反馈。

---


## 8. 语义分析规范

### 8.1 名字绑定（函数级）

`parser/expression_binder.*` 中 `ExpressionBinder` 的绑定函数全家桶：

| 函数 | 契约 |
| --- | --- |
| `bind_expression` | 绑定分发入口（按 `ExprType` 路由） |
| `bind_star_expression` | `*` / `t.*` 展开为字段列表 |
| `bind_unbound_field_expression` | `UnboundFieldExpr`→`FieldExpr`，做表/列存在性校验 |
| `bind_field_expression` | 已绑定字段的处理 |
| `bind_value_expression` | 常量值表达式 |
| `bind_cast_expression` | `CastExpr` 类型转换绑定 |
| `bind_comparison_expression` | 比较（含 `IS [NOT] NULL`） |
| `bind_conjunction_expression` | `AND`/`OR` |
| `bind_arithmetic_expression` | 算术 + 类型检查 |
| `bind_aggregate_expression` | `UnboundAggregateExpr`→`AggregateExpr` |

辅助：`BinderContext::add_table`、`ExpressionBinder::set_binder_error_message / reset_binder_error_message`。

### 8.2 存在性校验

- 表存在性：`SelectStmt::create` 通过 `Db::find_table` 查 Catalog。
- 列存在性：`bind_unbound_field_expression` 查 `TableMeta::find_field`；不存在报语义错误。

### 8.3 类型一致性（已实现）

- 算术/比较运算的左右操作数类型需满足 §5.3 的类型规则，不满足（如 `INT + VARCHAR`）报「operator '+' cannot be applied to INT and VARCHAR」。
- 实现：`parser/expression_binder.cpp::bind_arithmetic_expression` 校验左右 `value_type()` 是否为数值类型，`set_arithmetic_type_error` 生成带位置的语义错误；
  算术表达式的行列记录为**运算符位置**（`yacc_sql.y::create_arithmetic_expression`）。



### 8.4 INSERT 匹配（已实现）

- `stmt/insert_stmt.cpp::InsertStmt::create` 支持两种形式：`INSERT INTO t VALUES (...)` 与 `INSERT INTO t(col1,col2) VALUES (...)`（列清单映射、未列出列填 `NULL`）。
- 逐列校验类型/个数；允许 `NULL` 写入可空列；允许 `CHARS/INTS` 字面量写入 `DATES` 列（由 `Table::make_record` 转换）。
- 逐列结构化报告 `TypeMismatch`：
  ```
  TypeMismatch:
  student.id expects INT, but VARCHAR found.
  student.name expects VARCHAR, but INT found.
  ```
- 列清单中列不存在时报 `SemanticError: column 'x' does not exist in table 't'.`

### 8.5 语义错误定位（已实现）

- 目标：`SemanticError at line L, column C` + 原因。
- 实现：表达式携带行列（`expr/expression.h::set_location/line/column`）；`parser/expression_binder.cpp` 将结构化消息写入线程本地槽位，
  `parser/resolve_stage.cpp::ResolveStage::handle_request` 在 `Stmt::create_stmt` 失败时透出该消息；`stmt/insert_stmt.cpp` 复用同一槽位报告 `TypeMismatch`。

---

> 说明：原 §9–§12（NULL 执行语义、优化器规范、执行引擎规范、存储引擎接口）属于查询处理 / 执行 / 存储层，由队友负责，本文档不再收录，故章节号从 §8 直接跳到 §13。

## 13. 错误处理规范

- 统一使用 `RC` 错误码（`common/sys/rc.h`），关键新增错误码：`SCHEMA_FIELD_NOT_NULL`（非空约束违反）。
- 错误消息必须携带三类信息：
  1. **类型**：`SyntaxError` / `SemanticError` / `TypeMismatch` / `LexError`；
  2. **位置**：`line`、`column`；
  3. **原因**：简短可读描述。
- 非法输入（非法字符、未闭合字符串、缺分号、括号不匹配、表/列不存在、类型不匹配）必须返回错误，且**不崩溃**。

---


## 14. 测试与验收规范（编译器层）

### 14.1 测试体系

| 类别 | 内容 |
| --- | --- |
| 词法错误 | 非法字符、字符串未闭合 |
| 语法错误 | 缺操作数、括号不匹配、结构错误 |
| 语义错误 | 表/列不存在、类型不匹配 |
| 边界测试 | 空输入、极长标识符、多语句、大小写 |
| 补全测试 | 关键字/表/列/INSERT 补全、方言门禁、注释/字符串/多语句/大小写 |
| 自建测试集 | 覆盖上述编译器能力 |

自动化测试集位置：`SQL_description/test/`（`run_tests.py` + `cases/{lexer,parser,semantic,autocomplete}.py`）；
补全单元测试：`unittest/observer/autocomplete_test.cpp`。运行方式：

```bash
./build.sh debug --make -j4
python3 SQL_description/test/run_tests.py --filter lexer        # 词法
python3 SQL_description/test/run_tests.py --filter parser       # 语法
python3 SQL_description/test/run_tests.py --filter semantic     # 语义
python3 SQL_description/test/run_tests.py --filter autocomplete # 补全
ctest -R autocomplete_test
```

### 14.2 关键验收用例（节选）

- 注释、多字符运算符、字符串、大小写、非法输入（对应 `requirements.md` 词法测试要求）。
- 语法优先级、括号、语法诊断（`expected:` 期望集合）。
- 语义错误定位（`score` 列不存在、`age + 'abc'` 类型错误、`INSERT` 类型不匹配）。

### 14.3 关注指标

- **Crash**：任何输入不得导致崩溃。
- **Wrong Accept**：非法输入被错误接受。
- **Wrong Reject**：合法输入被错误拒绝。
- **Error Location**：错误行列位置准确。

---


## 15. 编译器能力矩阵与现状

| 能力 | 状态 | 关键实现（文件:函数） |
| --- | --- | --- |
| 词法：关键字 / 标识符 / 常量 | ✅ | `parser/lex_sql.l` 规则段 |
| 词法：运算符 / 分隔符 | ✅ | `parser/lex_sql.l`（`== = <= <> != < > >= + - * /`、`( ) , ; .`） |
| 词法：注释 / 空白 | ✅ | `parser/lex_sql.l`（`--`、`/* */` + `%x COMMENT`、`WHITE_SPACE`） |
| 词法：字符串与转义 | ✅ | `parser/lex_sql.l`（字符串规则）、`parser/yacc_sql.y::unescape_quoted_string` |
| 词法：大小写不敏感 | ✅ | `parser/lex_sql.l`（`%option case-insensitive`） |
| 词法：行列位置 | ✅ | `parser/lex_sql.l`（`%option yylineno` + `YY_USER_ACTION`） |
| 语法：优先级 / 括号 | ✅ | `parser/yacc_sql.y`（`%left/%precedence`、`LBRACE boolean_expr RBRACE`） |
| 语法：AST / 多语句 | ✅ | `parser/yacc_sql.y`、`parser/parse_defs.h` |
| 语法：诊断（位置 + 实际符号 + 期望集合） | ✅ | `parser/yacc_sql.y::yyreport_syntax_error`、`parser/expected_tokens.h::collect_expected_tokens` |
| 语义：表 / 列存在性 | ✅ | `parser/expression_binder.cpp::bind_unbound_field_expression` |
| 语义：名字绑定 | ✅ | `parser/expression_binder.cpp::bind_expression` 及 `bind_*` |
| 语义：类型一致性 | ✅ | `parser/expression_binder.cpp::bind_arithmetic_expression` |
| 语义：INSERT 匹配（逐列 TypeMismatch） | ✅ | `stmt/insert_stmt.cpp::InsertStmt::create` |
| 语义：错误定位 | ✅ | `parser/expression_binder.cpp`（消息槽）、`parser/resolve_stage.cpp` |
| SQL 输入补全 | ✅ | `autocomplete/*`、`service/database_service.cpp::complete_sql`、`obclient/client.cpp`、`obclient/web/*` |
| 编译器测试集 | ✅ | `SQL_description/test/`、`unittest/observer/autocomplete_test.cpp` |

---


## 16. 函数级实现索引（编译器层）

> 完整「文件 → 类 → 函数」清单见 `SQL_description/文件结构规划.md` §3。

### 16.1 parser

- `parse.h/.cpp`：`parse()`、`ParsedSqlResult::add_sql_node`
- `parse_defs.h`：AST 节点结构体 + `CompOp`/`SqlCommandFlag` 枚举 + `ParsedSqlNode`、`ParsedSqlResult`
- `parse_stage.*`：`ParseStage::handle_request`
- `resolve_stage.*`：`ResolveStage::handle_request`
- `expression_binder.*`：`ExpressionBinder`（`bind_*` 函数，见 §8.1）、`BinderContext::add_table`
- `expected_tokens.h`：`collect_expected_tokens()`
- `lex_sql.l` / `yacc_sql.y`：生成 `lex_sql.*`、`yacc_sql.*`（入口见 §6.3、§7.4）

### 16.2 autocomplete

- `completion_engine.*`：`CompletionEngine::complete`
- `current_statement_extractor.*`、`sql_text_scanner.*`、`sql_capabilities.*`
- `grammar_completion_provider.*`、`catalog_completion_provider.*`、`completion_scope.*`
- `sql_completion_config.*`、`llama_completion_client.*`、`model_context_builder.*`、`model_completion_provider.*`、`model_completion_validator.*`

---


## 17. 术语表（编译器层）

| 术语 | 位置 | 说明 |
| --- | --- | --- |
| `ParsedSqlNode` / `ParsedSqlResult` | `parser/parse_defs.h` | 语法解析产出的 AST 节点与结果集合 |
| `ExpressionBinder` | `parser/expression_binder.*` | 表达式 / 名字绑定器（存在性、类型检查） |
| `expected_tokens` | `parser/expected_tokens.h` | 复用 bison 期望集合，供语法诊断与补全 |
| `CompletionEngine` | `autocomplete/completion_engine.*` | SQL 输入补全统一入口 |
| `SqlCapabilities` | `autocomplete/sql_capabilities.*` | 运行时 SQL 能力表（方言门禁） |

---


## 18. SQL 输入补全规范（新增）

### 18.1 目标与边界

- 只做「用户输入 SQL 命令时的自动补全」，不做 NL2SQL / 聊天 / C++ 代码补全。
- **不让 LLM 决定 SQL 是否合法**；关键字来自 Parser，表/列来自 Catalog。
- **不引入第二套 SQL Parser**：复用 `%define parse.error custom` 的期望集合（`collect_expected_tokens`）。
- 模型是增强项：不可用/超时/校验失败时确定性补全照常工作，不阻塞、不报错。

### 18.2 数据结构（`autocomplete/completion_types.h`）

| 类型 | 关键字段 |
| --- | --- |
| `CompletionItem` | `insert_text / display_text / kind / source / replace_start / replace_end / score / detail` |
| `CompletionRequest` | `sql / cursor_offset / max_items / want_model_completion` |
| `CompletionResponse` | `items / ghost_text / model_used` |
| `CompletionKind` | `Keyword / Table / Column / Alias / Operator / Type / Literal / Snippet / Model` |
| `CompletionSource` | `Grammar / Catalog / Semantic / Model` |

### 18.3 确定性补全流程（`CompletionEngine::complete`）

1. `find_statement_at_cursor` 取光标所在单条语句（分号只在字符串/注释外分隔）。
2. `cursor_in_string_or_comment` 命中则直接返回空（不调用模型）。
3. 计算光标处半截 token（`partial`）与 `statement_prefix`。
4. `collect_expected_tokens(statement_prefix + 哨兵)` 取期望 terminal 集合；若哨兵前已确定语法错误则返回空（避免误导）。
5. `complete_grammar`：把期望符号映射为关键字/运算符/类型；并用**同一个 Parser 试探**确认续写关键字（`SELECT * FROM t ` → `WHERE/GROUP/ORDER/JOIN` 等）。关键字大小写跟随用户语句风格。
6. `complete_catalog`：`FROM/JOIN/INSERT INTO` 后给表；`SELECT/WHERE/ON/GROUP/ORDER` 给 scope 列；`INSERT INTO t(` 给列；`t.` 给限定列；`CREATE TABLE` 给真实类型。
7. 按 `score`（试探确认关键字 > Catalog > 期望集合）+ 名称排序、去重、截断到 `max_items`。
8. 可选模型：`ModelCompletionProvider` 生成 ghost text，经 `ModelCompletionValidator` 校验后返回。触发策略：仅当确定性结果恰为**唯一关键字**（如 `SEL`→`SELECT`、`FR`→`FROM`）时跳过模型；列/表候选、运算符上下文等均允许模型给出 ghost。

### 18.4 能力门禁（`SqlCapabilities`）

- 只登记 grammar 中真实存在的 terminal 与 executor 已实现的语句；`table_alias = false`（当前 grammar 不支持 `FROM t a`）。
- 禁止项（`is_forbidden`）包括 `HAVING/LIMIT/OFFSET/UNION/INTERSECT/DISTINCT/ALTER/WITH/窗口/子查询关键字/` 以及项目未使用类型（如 `VARCHAR/TEXT/DECIMAL/...`）。确定性补全与模型输出都受其约束。

### 18.5 模型侧（可选）

- 推理：`llama.cpp` `/infill`，模型 `Qwen/Qwen2.5-Coder-1.5B`（Base，非 Instruct），GGUF `Q8_0`。
- 上下文：`SqlModelContextBuilder` 生成 `dialect.sql` + 相关 `schema.sql`（受 `max_schema_tables/columns` 预算约束）。
- 客户端：`LlamaCompletionClient`（`health` 带 TTL、HTTP deadline、失败静默）；超时/不可用丢弃。
- 校验：`ModelCompletionValidator::validate` = 清洗（去掉 Markdown 围栏/FIM 特殊 token/解释性前缀）→ 长度限制 → 方言白名单截断 → Parser 校验（最长合法前缀）→ Catalog 校验（`t.c` 必须存在）。
- P40：CUDA 12.x / `CMAKE_CUDA_ARCHITECTURES=61` / `GGML_CUDA_FORCE_MMQ=ON`；见 `scripts/build_llama_p40.sh`、`scripts/run_sql_completion_model.sh`。
- 实测部署：Tesla P40（sm_61，CUDA 13 已不支持 Pascal，故安装 CUDA 12.4）→ 编译 llama.cpp → 本地 `Qwen2.5-Coder-1.5B Q8_0 GGUF` → `llama-server` 监听 `127.0.0.1:8012`；`/infill` 正常，DB 端 ghost 延迟约 100–300ms。`run_sql_completion_model.sh` 支持本地 GGUF（`SQL_COMPLETION_MODEL_FILE`）或 `-hf` 下载。

### 18.6 协议与客户端

- native 协议新增 `complete` 请求：`{type, sql, cursor, max_items, want_model}`；响应用通用结果结构承载候选（8 列行）与 `attributes.ghost_text/model_used`。
- 服务端：`service/database_service.cpp::DatabaseService::complete_sql`（进程级 `CompletionEngine` 单例）。
- CLI：`obclient/client.cpp` replxx Tab 触发确定性补全、ghost text hint；`--complete "SQL"` 与 `/complete SQL` 用于演示。
- Web Console：`obclient/web/csudb_web.py` 提供 `POST /api/complete`（`X-CSUDB-Session`/Cookie 鉴权）；`app.js` 在 SQL 编辑器上实现候选下拉 + AI ghost（下拉首项为带 `AI` 标签的模型补全，编辑器上方显示 `AI ⟶ ... (Tab 接受)`，Tab/Enter/点击接受），候选/ghost 分别来自确定性补全与模型。
- 配置：`etc/sql_completion.json`（`enabled/model_enabled/llama_base_url/timeout/max_*` 等），支持环境变量 `CSUDB_SQL_COMPLETION_CONFIG`。

---


## 19. 错误与降级契约（补全）

```
Parser completion 出错   -> 返回 Catalog 基础 prefix match
Catalog completion 出错  -> 返回 Grammar keyword
llama-server down        -> 不显示 ghost text
llama-server timeout     -> 丢弃
模型输出 invalid          -> 丢弃或 longest-valid-prefix
所有 provider 都失败      -> 返回空列表
```

任何情况下：不 crash、不阻塞 SQL 执行、不修改用户 SQL、不自动执行补全。

