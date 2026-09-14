# SQL 编译器详细规格说明书（Spec）

- 版本：v2.1（函数级）
- 日期：2026-09-14
- 适用范围：`/root/CSU-DBMS`（OceanBase Miniob 改造版，含 CSUDB 产品级存储引擎、SQL 输入补全）
- 上游文档：
  - `SQL_description/requirements.md`（验收需求拆解）
  - `SQL_description/文件结构规划.md`（代码目录与职责划分）
  - `SQL_description/检录报告.md`（需求对照与现状盘点）
- 路径约定：未写前缀的文件路径相对于 `src/observer/sql/`；`storage/`、`common/`、`type/` 相对于 `src/observer/`。

---

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

### 2.1 目标（必须完成）

- 必备语句：`CREATE`、`INSERT`、`SELECT`、`DELETE`、`WHERE`。
- 布尔表达式：比较运算、`AND`、`OR`、`NOT`、括号。
- 进阶语句：`UPDATE`、`ORDER BY`、`GROUP BY`、`JOIN`、算术表达式、`NULL`、最小 `DATE`。
- 词法：识别关键字/标识符/常量/运算符/分隔符、跳过空白与注释、字符串、大小写不敏感、输出 Token 行列位置。
- 语法：优先级（`NOT > 比较 > AND > OR`）、括号显式改变结合、构造 AST、多语句输入。
- 语义：表/列存在性校验、名字绑定、类型一致性、`INSERT` 匹配。
- 系统工程：AST→逻辑计划→物理计划、规则式优化（常量折叠、布尔化简、投影裁剪、谓词下推、冗余节点消除）、JSON 输出。
- 语义/语法错误必须携带**类型 + 行列位置 + 原因**，且不崩溃。

### 2.2 现状与补齐情况

| 能力 | 状态 | 目标规格所在章节 |
| --- | --- | --- |
| 字符串转义 `Tom''s` | ✅ 已实现（`''`/`""`/`\x`） | §6.4 |
| 投影裁剪 / Project[*] 消除 | ✅ 已实现（列裁剪） | §10.3 |
| 语义错误结构化定位 | ✅ 已实现 | §8.5 |
| INSERT 逐列 TypeMismatch 报告 | ✅ 已实现（含列清单） | §8.4 |
| 类型一致性严格校验 | ✅ 已实现 | §5.3、§8.3 |
| 语法 `expected:` 期望集合 | ✅ 已实现 | §7.4 |
| SQL 输入自动补全 | ✅ 新增 | §18 |
| 自动化测试集 | ✅ 新增 | §14 |

> 本版状态：必备能力、进阶能力、词法/语法/语义关键要求均已实现；另修复了词法器缺 `OR`、NOT 优先级、标识符大小写、空字符串崩溃、多余右括号、一元负号、NOT 布尔比较、NULL 分组、UPDATE NULL 位图、OR 谓词下推等基础缺陷。

### 2.3 非目标

- 事务隔离级别与并发控制的完整实现（仅保留 `TRX_BEGIN/COMMIT/ROLLBACK` 语法外壳）。
- 完整的 SQL 标准（不支持子查询、`HAVING`、`UNION`、窗口函数等）。
- 向量检索（`VECTORS` 类型仅保留类型定义，不作为本阶段验收重点）。

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
| ParseStage | `parser/parse_stage.*` | `ParseStage::handle_request` | SQL 字符串 → `ParsedSqlResult` |
| ResolveStage | `parser/resolve_stage.*` | `ResolveStage::handle_request` | 中间 AST → `Stmt *` |
| OptimizeStage | `optimizer/optimize_stage.*` | `handle_request` → `create_logical_plan` → `rewrite` → `generate_physical_plan` | `Stmt *` → `PhysicalOperator *` |
| ExecuteStage | `executor/execute_stage.*` | `handle_request` / `handle_request_with_physical_operator` | 物理计划 → `SqlResult` |
| PlanCacheStage | `plan_cache/plan_cache_stage.*` | —（当前空实现） | 缓存计划（可选） |
| QueryCacheStage | `query_cache/query_cache_stage.*` | `QueryCacheStage::handle_request` | 缓存结果（可选） |

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
- `optimizer/logical_plan_generator.cpp::implicit_cast_cost` 仍用于 WHERE 旧路径中可隐式转换的数值类型（`INT`↔`FLOAT`）。

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

## 9. NULL 语义规范

### 9.1 表示

- 存储层：每张表维护 **NULL 位图**，`TableMeta` 记录其偏移与大小；`FieldMeta` 记录字段是否 `nullable`。
- 值层：`Value::is_null_` 标志位（与类型解耦）。
- 记录创建：`Table::make_record` 校验非空约束（`NOT NULL` 违反报 `SCHEMA_FIELD_NOT_NULL`），并写入 NULL 位图；`Table::set_value_to_record` 设置具体字段值。
- 记录读取：`expr/tuple.h::RowTuple::cell_at` 检查 NULL 位图，对应字段为 NULL 时返回 `Value::null_value()`。

### 9.2 三值逻辑

- 比较运算：`ComparisonExpr::compare_value` 对 `IS_NULL / IS_NOT_NULL` 特判；普通比较中任一操作数为 `NULL` → 结果为 `UNKNOWN`。
- NOT：`NOT x` 降级为 `x == false`；在 `NULL` 上为 `UNKNOWN`。
- AND/OR：`ConjunctionExpr::get_value` 实现真值表（`TRUE/FALSE/UNKNOWN`）。

### 9.3 聚合函数与 NULL

| 聚合函数 | 类（`expr/aggregator.*`） | NULL 处理契约 |
| --- | --- | --- |
| `COUNT` | `CountAggregator` | `accumulate` 跳过 `NULL`（`!is_null()` 才 `++count_`） |
| `SUM` | `SumAggregator` | 跳过 `NULL`，空组返回 `NULL` |
| `AVG` | `AvgAggregator` | 跳过 `NULL`，计数与求和只含非空值 |
| `MIN` | `MinAggregator` | 跳过 `NULL`，空组 `evaluate` 返回 `NULL` |
| `MAX` | `MaxAggregator` | 跳过 `NULL`，空组 `evaluate` 返回 `NULL` |

聚合器接口：`Aggregator::accumulate(const Value&)` 与 `evaluate(Value&)`。聚合入口：`AggregateExpr::create_aggregator` / `AggregateExpr::get_value`（`expr/expression.cpp`）。

---

## 10. 优化器规范

### 10.1 AST → 逻辑计划 → 物理计划（函数级）

| 文件 | 类 | 关键函数 | 契约 |
| --- | --- | --- | --- |
| `optimizer/logical_plan_generator.*` | `LogicalPlanGenerator` | `create`（分发）、`create_plan`（各 Stmt 重载）、`create_group_by_plan`、`implicit_cast_cost` | AST → 逻辑算子树（`FROM`→`TableGet`、`WHERE`→`Predicate`、`SELECT 列表`→`Project`） |
| `optimizer/physical_plan_generator.*` | `PhysicalPlanGenerator` | `create`、`create_plan`（各逻辑算子重载）、`create_vec`、`create_vec_plan`、`can_use_hash_join` | 逻辑 → 物理算子树（含向量化） |
| `optimizer/optimize_stage.*` | `OptimizeStage` | `handle_request`、`create_logical_plan`、`rewrite`、`optimize`、`generate_physical_plan` | 优化阶段编排 |

### 10.2 规则式优化（函数级）

| 规则 | 类 | 关键函数 | 效果 |
| --- | --- | --- | --- |
| 常量折叠 | `ArithmeticSimplificationRule` | `rewrite` | `age > 10+8` → `age > 18` |
| 布尔化简 | `ConjunctionSimplificationRule` | `rewrite` | `x AND TRUE` → `x` |
| 比较化简 | `ComparisonSimplificationRule` | `rewrite` | 常量比较化简 |
| 谓词下推 | `PredicatePushdownRewriter` | `rewrite`、`is_empty_predicate`、`get_exprs_can_pushdown` | 尽量提前过滤（OR 保留在 Predicate 执行） |
| 冗余节点消除 | `PredicateRewriteRule` | `rewrite` | 消除恒真/恒假 Filter |
| 谓词转 JOIN | `PredicateToJoinRewriter` | — | 隐式 JOIN → 显式 JOIN |
| 投影裁剪 | `ProjectionPruningRule` | `rewrite` | 只保留查询真正需要的列 |

框架与执行入口：

| 类 | 关键函数 | 说明 |
| --- | --- | --- |
| `Rewriter` | `rewrite` | 重写器入口 |
| `ExpressionRewriter` | `rewrite`、`rewrite_expression` | 表达式自底向上重写 |

### 10.3 投影裁剪（已实现）

- 目标：仅保留查询真正需要的列。
- 实现：`optimizer/projection_pruning_rule.cpp::ProjectionPruningRule::rewrite` 在 Projection 节点收集整棵子树引用到的字段
  （含 WHERE/ORDER BY/GROUP BY/JOIN/聚合/下推谓词），按表定义顺序写入 `TableGetLogicalOperator::set_used_fields`；
  物理计划据此只让 `TableScanPhysicalOperator` 输出这些列（`set_used_fields`）。
- `expr/tuple.h::RowTuple::cell_at` 在裁剪后按字段原始下标访问 NULL 位图，保证 NULL 语义正确。
- 恒真 `Filter` 由 `PredicateRewriteRule` 消除；向量化路径与索引扫描路径保守地不裁剪（返回全字段，正确性不受影响）。

### 10.4 计划可视化 / EXPLAIN

- `EXPLAIN` 通过 `operator/explain_physical_operator.*::ExplainPhysicalOperator::generate_physical_plan` 输出物理计划树。
- 计划打印：`optimizer/optimizer_utils.*::OptimizerUtils::dump_physical_plan`。
- 测试要求：能从任意 SELECT AST 解释每个 Plan 节点为何产生、节点间数据流关系；能展示优化前后结构变化（如 `WHERE 1=1 AND age>10+8` → `Filter[age>18]`）。

### 10.5 Cascades 优化器框架（`optimizer/cascade/`）

| 文件 | 类 | 关键函数 |
| --- | --- | --- |
| `optimizer.*` | `Optimizer` | `optimize`、`choose_best_plan`、`optimize_loop`、`execute_task_stack` |
| `optimizer_context.*` | `OptimizerContext` | `record_operator_node_in_memo`、`record_node_into_group`、`push_task`、`get_cost_upper_bound` |
| `memo.*` | `Memo` | `record_operator`、`release_operator`、`add_new_group`、`dump` |
| `group.*` | `Group` | `add_expr`、`set_expr_cost`、`get_cost_lb`、`dump` |
| `group_expr.*` | `GroupExpr` | `hash`、`set_local_cost`、`get_cost`、`dump` |
| `cost_model.*` | `CostModel` | `calculate_cost`、`cpu_op`、`hash_cost`、`hash_probe`、`index_probe`、`io` |
| `implementation_rules.*` | `LogicalGetToPhysicalSeqScan` 等 | `transform` |
| `tasks/*` | `CascadeTask`、`OptimizeGroup`、`OptimizeExpression`、`OptimizeInputs`、`ExploreGroup`、`ApplyRule` | `perform`、`push_task` |

---

## 11. 执行引擎规范

### 11.1 物理算子（火山模型，函数级）

物理算子统一实现 `open() / next() / close()` 迭代协议，`next()` 逐行产出 `Tuple`，返回 `RC::RECORD_EOF` 表示结束。

| 算子 | 文件 | 关键函数 |
| --- | --- | --- |
| TableScan | `operator/table_scan_physical_operator.*` | `open/next/close`、`filter`、`set_predicates`、`param` |
| IndexScan | `operator/index_scan_physical_operator.*` | `open/next/close`、`filter`、`set_predicates`、`param` |
| Predicate | `operator/predicate_physical_operator.*` | `open/next/close`、`tuple_schema` |
| Project | `operator/project_physical_operator.*` | `open/next/close`、`tuple_schema` |
| Sort | `operator/sort_physical_operator.*` | `open/next/close`、`evaluate_order_by`、`param` |
| GroupBy（基类） | `operator/group_by_physical_operator.*` | `create_aggregator_list`、`aggregate`、`evaluate` |
| ScalarGroupBy | `operator/scalar_group_by_physical_operator.*` | `open/next/close` |
| HashGroupBy | `operator/hash_group_by_physical_operator.*` | `open/next/close`、`find_group` |
| NestedLoopJoin | `operator/nested_loop_join_physical_operator.*` | `open/next/close`、`left_next`、`right_next` |
| HashJoin | `operator/hash_join_physical_operator.*` | — |
| Insert | `operator/insert_physical_operator.*` | `open/next/close` |
| Delete | `operator/delete_physical_operator.*` | `open/next/close` |
| Update | `operator/update_physical_operator.*` | `open/next/close` |
| Explain | `operator/explain_physical_operator.*` | `open/next/close`、`generate_physical_plan` |

向量化算子（`*_vec_physical_operator.*`）：`TableScanVec`、`ProjectVec`、`ExprVec`、`AggregateVec`、`GroupByVec`，均实现 `open/next/close`。

### 11.2 结果集输出

- `executor/sql_result.*::SqlResult`：`set_tuple_schema`、`set_operator`、`open`、`close`、`next_tuple`、`next_chunk` 收集 Tuple 并迭代输出。
- 输出形式：JSON。
- 简单命令执行器（`SHOW TABLES`、`DESC`、`HELP`、`EXIT`、`TRX_*`）通过 `executor/command_executor.*::CommandExecutor::execute` 分发。

---

## 12. 存储引擎接口（Catalog）

| 接口 | 实现 | 说明 |
| --- | --- | --- |
| `create_table` | `storage/db/db.cpp::Db::create_table` | 建表并注册元数据 |
| `find_table` | `Db::find_table` | 查表（存在性校验） |
| `insert_record` | `storage/table/table.cpp::Table::insert_record` | 写入记录 |
| `make_record` | `Table::make_record` | 构造记录 + 非空校验 + NULL 位图 |
| `set_value_to_record` | `Table::set_value_to_record` | 设置字段值 |
| 元数据 | `storage/table/table_meta.*`、`storage/field/field_meta.*` | 表/字段元数据，含 NULL 位图偏移/大小、nullable |

**Catalog 需求**（`requirements.md`）：Catalog 需提供 `createTable / findTable / findColumn / getType`，同时服务执行引擎与持久化。

---

## 13. 错误处理规范

- 统一使用 `RC` 错误码（`common/sys/rc.h`），关键新增错误码：`SCHEMA_FIELD_NOT_NULL`（非空约束违反）。
- 错误消息必须携带三类信息：
  1. **类型**：`SyntaxError` / `SemanticError` / `TypeMismatch` / `LexError`；
  2. **位置**：`line`、`column`；
  3. **原因**：简短可读描述。
- 非法输入（非法字符、未闭合字符串、缺分号、括号不匹配、表/列不存在、类型不匹配）必须返回错误，且**不崩溃**。

---

## 14. 测试与验收规范

### 14.1 测试体系

| 类别 | 内容 |
| --- | --- |
| 核心正常执行 | CREATE/INSERT/SELECT/DELETE/UPDATE 等正确执行 |
| 词法错误 | 非法字符、字符串未闭合 |
| 语法错误 | 缺分号、括号不匹配、结构错误 |
| 语义错误 | 表/列不存在、类型不匹配 |
| 边界测试 | 空输入、极长标识符、多语句、大小写 |
| 自建测试集 | 覆盖上述全部能力 |
| 补全测试 | 关键字/表/列/INSERT 补全、方言门禁、注释/字符串/多语句/大小写 |

自动化测试集位置：`SQL_description/test/`（`run_tests.py` + `cases/{lexer,parser,semantic,core_sql,boundary,autocomplete}.py`）；
补全单元测试：`unittest/observer/autocomplete_test.cpp`。运行方式：

```bash
./build.sh debug --make -j4
python3 SQL_description/test/run_tests.py            # 自动拉起临时 csudbd
python3 SQL_description/test/run_tests.py --filter autocomplete
ctest -R autocomplete_test
```

### 14.2 关键验收用例（节选）

- 注释、多字符运算符、字符串、大小写、非法输入（对应 `requirements.md` §词法测试要求）。
- 语义错误定位（`score` 列不存在、`age + 'abc'` 类型错误、`INSERT` 类型不匹配）。
- 规划说明：能从任意 SELECT AST 解释每个 Plan 节点及数据流；能展示优化前后结构变化。
- NULL 端到端：`test/case/test/primary-null.test`（建表 NULL/NOT NULL/DATE、INSERT NULL、非空约束、`IS [NOT] NULL`、三值逻辑、聚合跳过 NULL）。

### 14.3 关注指标

- **Crash**：任何输入不得导致崩溃。
- **Wrong Accept**：非法输入被错误接受。
- **Wrong Reject**：合法输入被错误拒绝。
- **Error Location**：错误行列位置准确。

---

## 15. 能力矩阵与现状

| 能力 | 状态 | 关键实现（文件:函数） |
| --- | --- | --- |
| CREATE | ✅ | `stmt/create_table_stmt.cpp::CreateTableStmt::create`；`executor/create_table_executor.cpp::CreateTableExecutor::execute` |
| INSERT | ✅ | `stmt/insert_stmt.cpp::InsertStmt::create`；`operator/insert_physical_operator.cpp::InsertPhysicalOperator::open` |
| SELECT | ✅ | `stmt/select_stmt.cpp::SelectStmt::create`；`operator/project_physical_operator.cpp::ProjectPhysicalOperator::next` |
| DELETE | ✅ | `stmt/delete_stmt.cpp::DeleteStmt::create`；`operator/delete_physical_operator.cpp::DeletePhysicalOperator::next` |
| WHERE | ✅ | `operator/predicate_physical_operator.cpp::PredicatePhysicalOperator::next` |
| 比较/AND/OR/NOT | ✅ | `expr/expression.cpp::ComparisonExpr::compare_value/get_value`、`ConjunctionExpr::get_value` |
| 括号 | ✅ | `parser/yacc_sql.y`（`LBRACE boolean_expr RBRACE`） |
| UPDATE | ✅ | `stmt/update_stmt.cpp::UpdateStmt::create`；`operator/update_physical_operator.cpp::UpdatePhysicalOperator::next` |
| ORDER BY | ✅ | `operator/sort_physical_operator.cpp::SortPhysicalOperator::evaluate_order_by/next` |
| GROUP BY | ✅ | `operator/scalar_group_by_physical_operator.cpp` / `hash_group_by_physical_operator.cpp` |
| JOIN | ✅ | `operator/nested_loop_join_physical_operator.cpp::NestedLoopJoinPhysicalOperator::next` |
| 算术表达式 | ✅ | `expr/expression.cpp::ArithmeticExpr::calc_value/get_value` |
| NULL | ✅ | `expr/expression.cpp`（三值逻辑）、`common/value.h::Value::set_null/is_null`、`storage/table/table.cpp::Table::make_record` |
| DATE | ✅ | `common/type/date_type.cpp::DateType::set_value_from_str`（整数 `YYYYMMDD`） |
| 常量折叠 | ✅ | `optimizer/arithmetic_simplification_rule.cpp::ArithmeticSimplificationRule::rewrite` |
| 布尔化简 | ✅ | `optimizer/conjunction_simplification_rule.cpp::ConjunctionSimplificationRule::rewrite` |
| 谓词下推 | ✅ | `optimizer/predicate_pushdown_rewriter.cpp::PredicatePushdownRewriter::rewrite` |
| 字符串转义 `Tom''s` | ✅ | `parser/lex_sql.l`（字符串规则）、`parser/yacc_sql.y::unescape_quoted_string` |
| 投影裁剪 / Project[*] 消除 | ✅ | `optimizer/projection_pruning_rule.cpp::ProjectionPruningRule::rewrite` |
| 语义错误定位 | ✅ | `parser/expression_binder.cpp`（消息槽）、`parser/resolve_stage.cpp` |
| INSERT 逐列 TypeMismatch | ✅ | `stmt/insert_stmt.cpp::InsertStmt::create` |
| 类型一致性严格校验 | ✅ | `parser/expression_binder.cpp::bind_arithmetic_expression` |
| 语法 `expected:` 集合 | ✅ | `parser/yacc_sql.y::yyreport_syntax_error` + `%define parse.error custom` |
| SQL 输入补全 | ✅ | `sql/autocomplete/*`、`service/database_service.cpp::complete_sql`、`obclient/client.cpp` |
| 自动化测试集 | ✅ | `SQL_description/test/`、`unittest/observer/autocomplete_test.cpp` |

---

## 16. 函数级实现索引（按模块）

> 完整「文件 → 类 → 函数」清单见 `SQL_description/文件结构规划.md` §3。此处按模块列出关键类与函数，作为 spec 的快速索引。

### 16.1 parser

- `parse.h/.cpp`：`parse()`、`ParsedSqlResult::add_sql_node`
- `parse_defs.h`：AST 节点结构体 + `CompOp`/`SqlCommandFlag` 枚举 + `ParsedSqlNode`、`ParsedSqlResult`
- `parse_stage.*`：`ParseStage::handle_request`
- `resolve_stage.*`：`ResolveStage::handle_request`
- `expression_binder.*`：`ExpressionBinder`（10 个 `bind_*` 函数，见 §8.1）、`BinderContext::add_table`
- `lex_sql.l` / `yacc_sql.y`：生成 `lex_sql.*`、`yacc_sql.*`（入口见 §6.3、§7.4）

### 16.2 stmt

- `stmt.h/.cpp`：`Stmt::create_stmt`（分发）、`Stmt::stmt_type_ddl`
- 各语句：`SelectStmt/InsertStmt/DeleteStmt/UpdateStmt/CreateTableStmt/CreateIndexStmt/DescTableStmt/ExplainStmt/FilterStmt/LoadDataStmt/AnalyzeTableStmt/...` 的 `create`
- 简单语句头文件：`CalcStmt/ExitStmt/HelpStmt/SetVariableStmt/ShowTablesStmt/TrxBeginStmt/TrxEndStmt` 的 `create`

### 16.3 expr

- `expression.*`：`Expression` 基类 + `StarExpr/UnboundFieldExpr/FieldExpr/ValueExpr/CastExpr/ComparisonExpr/ConjunctionExpr/ArithmeticExpr/UnboundAggregateExpr/AggregateExpr`（函数见 §4.2、§5.3、§9.2、§9.3）
- `expression_iterator.*`：`ExpressionIterator::iterate_child_expr`
- `arithmetic_operator.hpp`：算术/比较模板 `AddOperator...Equal...`
- `aggregator.*`：`Aggregator` + `Sum/Count/Avg/Min/MaxAggregator::accumulate/evaluate`
- `aggregate_state.*`：`SumState/CountState/AvgState::update/finalize`
- `aggregate_hash_table.*`：`StandardAggregateHashTable::add_chunk/aggregate` 等
- `tuple.h`：`Tuple`、`RowTuple::set_record/cell_at`、`JoinedTuple`、`ProjectTuple`、`ValueListTuple`
- `composite_tuple.*`：`CompositeTuple::add_tuple/cell_at`
- `expression_tuple.h`：`ExpressionTuple::get_value`
- `tuple_cell.*`：`TupleCellSpec::equals`

### 16.4 operator

- 基类：`LogicalOperator`（`add_child/add_expressions`）、`PhysicalOperator`（`open/next/close/name/param`）、`OperatorNode`（`find_log_prop/calculate_cost`）
- 各逻辑/物理算子：见 §11.1

### 16.5 optimizer

- 计划生成：`LogicalPlanGenerator`、`PhysicalPlanGenerator`、`OptimizeStage`（见 §10.1）
- 重写：`Rewriter`、`ExpressionRewriter`、`PredicatePushdownRewriter`、`PredicateRewriteRule`、`ArithmeticSimplificationRule`、`ConjunctionSimplificationRule`、`ComparisonSimplificationRule`、`PredicateToJoinRewriter`（见 §10.2）
- Cascades：`Optimizer`、`OptimizerContext`、`Memo`、`Group`、`GroupExpr`、`CostModel`、`implementation_rules`、`tasks/*`（见 §10.5）

### 16.6 executor

- `ExecuteStage`、`CommandExecutor`、`SqlResult`，及各执行器 `CreateTableExecutor/CreateIndexExecutor/DescTableExecutor/LoadDataExecutor/SetVariableExecutor/AnalyzeTableExecutor/ShowTablesExecutor/HelpExecutor/TrxBeginExecutor/TrxEndExecutor` 的 `execute`
- `LoadDataExecutor::load_data`、`SetVariableExecutor::var_value_to_boolean/get_execution_mode`

### 16.7 plan_cache / query_cache

- `PlanCacheStage`（空实现）
- `QueryCacheStage::handle_request`

---

## 17. 术语表（补充）

| 术语 | 位置 | 说明 |
| --- | --- | --- |
| `Stmt` | `stmt/stmt.h` | 语句对象基类，`Stmt::create_stmt()` 工厂分发 |
| `SqlResult` | `executor/sql_result.*` | 结果集封装 |
| `ExpressionBinder` | `parser/expression_binder.*` | 表达式/名字绑定器 |
| `Rewriter` | `optimizer/rewriter.*` | 重写器框架入口 |
| `Cascades` | `optimizer/cascade/` | 高级优化器框架（代价模型等） |
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
8. 可选模型：`ModelCompletionProvider` 生成 ghost text，经 `ModelCompletionValidator` 校验后返回。

### 18.4 能力门禁（`SqlCapabilities`）

- 只登记 grammar 中真实存在的 terminal 与 executor 已实现的语句；`table_alias = false`（当前 grammar 不支持 `FROM t a`）。
- 禁止项（`is_forbidden`）包括 `HAVING/LIMIT/OFFSET/UNION/INTERSECT/DISTINCT/ALTER/WITH/窗口/子查询关键字/` 以及项目未使用类型（如 `VARCHAR/TEXT/DECIMAL/...`）。确定性补全与模型输出都受其约束。

### 18.5 模型侧（可选）

- 推理：`llama.cpp` `/infill`，模型 `Qwen/Qwen2.5-Coder-1.5B`（Base，非 Instruct），GGUF `Q8_0`。
- 上下文：`SqlModelContextBuilder` 生成 `dialect.sql` + 相关 `schema.sql`（受 `max_schema_tables/columns` 预算约束）。
- 客户端：`LlamaCompletionClient`（`health` 带 TTL、HTTP deadline、失败静默）；超时/不可用丢弃。
- 校验：`ModelCompletionValidator::validate` = 清洗（去掉 Markdown 围栏/FIM 特殊 token/解释性前缀）→ 长度限制 → 方言白名单截断 → Parser 校验（最长合法前缀）→ Catalog 校验（`t.c` 必须存在）。
- P40：CUDA 12.x / `CMAKE_CUDA_ARCHITECTURES=61` / `GGML_CUDA_FORCE_MMQ=ON`；见 `scripts/build_llama_p40.sh`、`scripts/run_sql_completion_model.sh`。

### 18.6 协议与客户端

- native 协议新增 `complete` 请求：`{type, sql, cursor, max_items, want_model}`；响应用通用结果结构承载候选（8 列行）与 `attributes.ghost_text/model_used`。
- 客户端 `obclient/client.cpp`：replxx Tab 触发确定性补全、ghost text hint；`--complete "SQL"` 与 `/complete SQL` 用于演示。
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