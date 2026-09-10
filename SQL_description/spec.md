# SQL 编译器详细规格说明书（Spec）

- 版本：v1.0
- 日期：2026-09-10
- 适用范围：`/root/CSU-DBMS`（OceanBase Miniob 改造版，含 CSUDB 产品级存储引擎）
- 上游文档：
  - `SQL_description/requirements.md`（验收需求拆解）
  - `SQL_description/文件结构规划.md`（代码目录与职责划分）
  - `SQL_description/检录报告.md`（需求对照与现状盘点）
- 路径约定：未写前缀的文件路径相对于 `src/observer/sql/`；`storage/`、`common/`、`type/` 相对于 `src/observer/`。

---

## 1. 概述

本文件是 SQL 编译器的**详细设计与实现规格**，将 `requirements.md` 中的验收能力落实到**具体的数据结构、接口、算法与行为契约**，用于指导实现、评审与回归测试。

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

### 2.2 当前缺口（暂缓项）

以下能力在 `检录报告.md` 中被列为缺口或部分完成，本 spec 仅给出目标规格，标注为「未实现 / 部分实现」：

| 缺口 | 状态 | 目标规格所在章节 |
| --- | --- | --- |
| 字符串转义 `Tom''s` | ❌ 未实现 | §5.4 |
| 投影裁剪 / Project[*] 消除 | ❌ 未实现 | §9.3 |
| 语义错误结构化定位 | ⚠️ 部分 | §8.5 |
| INSERT 逐列 TypeMismatch 报告 | ⚠️ 部分 | §8.4 |
| 类型一致性严格校验 | ⚠️ 部分 | §7.3、§8.3 |
| 语法 `expected:` 期望集合 | ⚠️ 部分 | §6.4 |

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
| 元组（Tuple） | 执行期一行数据的抽象，`get/cell_at` 返回 `Value` |
| 三值逻辑 | `TRUE / FALSE / UNKNOWN`，用于 `NULL` 参与布尔运算 |
| Catalog | 元数据目录，提供 `create_table / find_table / find_field` 等接口 |
| NULL 位图 | 记录中用于标识各字段是否为 `NULL` 的位图 |

---

## 4. 总体架构与数据流

### 4.1 Stage 编排

| 阶段 | 实现 | 输入 | 输出 |
| --- | --- | --- | --- |
| ParseStage | `parser/parse_stage.*` | SQL 字符串 | `ParsedSqlResult`（AST 中间节点） |
| ResolveStage | `parser/resolve_stage.*` | 中间 AST | `Stmt *`（语句对象） |
| OptimizeStage | `optimizer/optimize_stage.*` | `Stmt *` | `PhysicalOperator *` 物理计划 |
| ExecuteStage | `executor/execute_stage.*` | 物理计划 | `SqlResult` 结果集 |
| PlanCacheStage | `plan_cache/plan_cache_stage.*` | 物理计划 | 缓存计划（可选） |
| QueryCacheStage | `query_cache/query_cache_stage.*` | 查询 | 缓存结果（可选） |

### 4.2 表达式语义模型

表达式是 SQL 中所有「可求值」元素的核心抽象。一个表达式：

- 通过 `get_value(const Tuple &, Value &)` 在具体元组上求值；
- 通过 `type()` 返回 `ExprType` 定位子类；
- 通过 `value_type()` 返回结果 `AttrType`（静态推导）；
- 通过 `line()/column()/set_location()` 记录原始 SQL 位置（用于语义错误定位）。

`ExprType` 枚举：

```
NONE, STAR, UNBOUND_FIELD, UNBOUND_AGGREGATION,
FIELD, VALUE, CAST, COMPARISON, CONJUNCTION, ARITHMETIC, AGGREGATION
```

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

定义于 `common/value.h`，是「值 + 类型」的统一封装：

- `attr_type_`：值类型；`length_`：值长度；`value_`：存储数据（union）。
- **NULL 表示**：`is_null_` 标志位与类型解耦。`set_null()` 置 `is_null_ = true`；`is_null()` 查询；`Value::null_value()` 构造 NULL。
- **类型转换**：`Value::cast_to(value, to_type, result)` —— 若源为 `NULL` 直接产出 `NULL`；若目标是 `DATES` 走 `DateType::cast_to`；否则按源类型分发到 `DataType::type_instance(...)->cast_to`。
- 值获取：`get_int/get_float/get_string/get_boolean/get_string_t`（类型不符时触发转换）。

### 5.3 类型推断规则

表达式的 `value_type()` 即类型推断契约：

- `ValueExpr::value_type()` → `value_.attr_type()`。
- `FieldExpr::value_type()` → `field_.attr_type()`。
- `ComparisonExpr::value_type()` → `BOOLEANS`。
- `ConjunctionExpr::value_type()` → `BOOLEANS`。
- `CastExpr::value_type()` → `cast_type_`。
- `ArithmeticExpr::value_type()` → 由 `calc_value` 结果类型决定。
- `AggregateExpr::value_type()`：`COUNT` → `INTS`；`AVG` → `FLOATS`；否则 → `child_->value_type()`。

集中管理的类型规则（目标形态，见 `requirements.md` §语义分析 9）：

```
INT  +  INT      -> INT
INT  >  INT      -> BOOL
VARCHAR = VARCHAR -> BOOL
BOOL AND BOOL    -> BOOL
NOT BOOL         -> BOOL
INT  +  VARCHAR  -> ERROR   // 需严格报错（当前为 ⚠️ 部分实现）
```

---

## 5.4 词法分析规范

### 5.4.1 输入输出

- 输入：SQL 文件 / 单条 SQL / 多条 SQL（分号分隔）。
- 输出：`<TokenType, Lexeme, Line, Column>`。

### 5.4.2 Token 类别

- 关键字：`SELECT/FROM/WHERE/AND/OR/NOT/IS/NULL/CREATE/INSERT/...`（`lex_sql.l` 规则段，大小写不敏感）。
- 标识符：`[A-Za-z_][A-Za-z0-9_]*`。
- 常量：整数 `NUMBER`、浮点 `FLOAT`、字符串 `SSS`（单/双引号）。
- 运算符：`== = < <= <> != > >= + - * /`。其中 `==`、`=` → `EQ`；`!=`、`<>` → `NE`。
- 分隔符：`( ) , ; .`。

### 5.4.3 状态机与注释

- `%x COMMENT`：多行注释 `/* ... */`（`BEGIN(COMMENT)` / `<COMMENT>"*/"`）。
- 单行注释：`--[^\n]*`。
- `%x STR`：已声明保留给字符串状态机（**当前未使用**，见 §5.4.4）。

### 5.4.4 目标规格（缺口）

- 字符串转义：`'Tom''s'` → `Tom's`（连续单引号表示一个引号），`''` 表示空串。当前 `'[^']*\'` 无法匹配连续引号，需用 `%x STR` 状态实现。
- 未闭合字符串、非法字符（如 `@`）必须返回词法错误（类型 + 位置 + 原因），不崩溃。

---

## 6. 语法分析规范

### 6.1 输入输出

- 输入：词法分析产生的 Token Stream。
- 输出：AST（`parse_defs.h` 中间节点，经 ResolveStage 转为 `stmt/*`）。

### 6.2 优先级与结合性

`yacc_sql.y` 通过 `%left/%precedence` 声明：

- 优先级自低到高：`OR < AND < 比较(LT/GT/LE/GE/EQ/NE) < NOT`。
- 括号 `( boolean_expr )` 显式透传、改变结合结构。

### 6.3 核心 AST 节点（`parse_defs.h`）

| 节点 | 关键字段 |
| --- | --- |
| `RelAttrSqlNode` | `relation_name` / `attribute_name` |
| `CompOp` | `EQUAL_TO, LESS_EQUAL, NOT_EQUAL, LESS_THAN, GREAT_EQUAL, GREAT_THAN, IS_NULL, IS_NOT_NULL, NO_OP` |
| `ConditionSqlNode` | 左右 `left_is_attr/left_value/left_attr`、`comp`、`right_is_attr/right_value/right_attr` |
| `JoinSqlNode` | `relation_name` + `unique_ptr<Expression> condition` |
| `SelectSqlNode` | `expressions / relations / conditions / where_expression / group_by / order_by / joins` |
| `InsertSqlNode` | `relation_name` + `vector<Value> values` |
| `DeleteSqlNode` | `relation_name` + `conditions` |
| `UpdateSqlNode` | `relation_name / attribute_name / value / conditions` |
| `AttrInfoSqlNode` | `type / name / length / nullable`（`nullable` 默认 `true`） |
| `CreateTableSqlNode` | `relation_name / attr_infos / primary_keys / storage_format / storage_engine` |
| `ErrorSqlNode` | `error_msg / line / column` |

### 6.4 目标规格（缺口）

- 语法诊断应输出 `错误位置 + 实际符号 + 期望集合`：
  ```
  SyntaxError at line 3, column 19
  unexpected token: ';'
  expected: IDENTIFIER | CONST | '(' | NOT
  ```
  当前通过 `yyerror` + `%define parse.error verbose` 输出部分信息，但缺 `expected:` 期望符号集合。

---

## 7. 语义分析规范

### 7.1 名字绑定

- `UnboundFieldExpr` → `FieldExpr`：由 `parser/expression_binder.cpp::ExpressionBinder::bind_unbound_field_expression` 完成。
- `UnboundAggregateExpr` → `AggregateExpr`：由 `bind_aggregate_expression` 完成。
- `StarExpr`（`*` / `t.*`）在投影列中展开为全部字段。

### 7.2 存在性校验

- 表存在性：`SelectStmt::create` 通过 `Db::find_table` 查 Catalog。
- 列存在性：绑定 `FieldExpr` 时查 `TableMeta::find_field`；不存在报语义错误。

### 7.3 类型一致性（部分实现）

- 算术/比较运算的左右操作数类型需满足 §5.3 的类型规则，不满足（如 `INT + VARCHAR`）应报「operator '+' cannot be applied to INT and VARCHAR」。
- 当前在 `optimizer/logical_plan_generator.cpp` 通过 `cast_cost` 插 `CastExpr` 做隐式转换，尚未严格区分「可隐式转换」与「必须报错」。

### 7.4 INSERT 匹配（部分实现）

- `insert_stmt.cpp::InsertStmt::create` 逐个字段校验值与目标列的类型/个数。
- 允许 `NULL` 写入任何可空列；允许 `CHARS/INTS` 字面量写入 `DATES` 列（由 `Table::make_record` 做转换）。
- 目标形态（未实现）：逐列结构化报告 `TypeMismatch`：
  ```
  TypeMismatch:
  student.id expects INT, but VARCHAR found.
  student.name expects VARCHAR, but INT found.
  ```

### 7.5 语义错误定位（部分实现）

- 目标：`SemanticError at line L, column C` + 原因。
- 现状：表达式已携带行列（`expression.h::set_location/line/column`），但语义错误响应仍以 `RC` 码返回，未结构化输出行列 + 原因。

---

## 8. NULL 语义规范

### 8.1 表示

- 存储层：每张表维护 **NULL 位图**，`TableMeta` 记录其偏移与大小；`FieldMeta` 记录字段是否 `nullable`。
- 值层：`Value::is_null_` 标志位（与类型解耦）。
- 记录创建：`Table::make_record` 校验非空约束（`NOT NULL` 违反报 `SCHEMA_FIELD_NOT_NULL`），并写入 NULL 位图；`Table::set_value_to_record` 设置具体字段值。
- 记录读取：`expr/tuple.h::RowTuple::cell_at` 检查 NULL 位图，对应字段为 NULL 时返回 `Value::null_value()`。

### 8.2 三值逻辑

- 比较运算：`ComparisonExpr::compare_value` 对 `IS_NULL / IS_NOT_NULL` 特判；普通比较中任一操作数为 `NULL` → 结果为 `UNKNOWN`。
- NOT：`NOT x` 降级为 `x == false`；在 `NULL` 上为 `UNKNOWN`。
- AND/OR：`ConjunctionExpr::get_value` 实现真值表（`TRUE/FALSE/UNKNOWN`）。

### 8.3 聚合函数与 NULL

| 聚合函数 | NULL 处理契约 |
| --- | --- |
| `COUNT` | `accumulate` 跳过 `NULL`（`!is_null()` 才 `++count_`） |
| `SUM` | 跳过 `NULL`，空组返回 `NULL` |
| `AVG` | 跳过 `NULL`，计数与求和只含非空值 |
| `MIN` / `MAX` | 跳过 `NULL`，空组 `evaluate` 返回 `NULL` |

聚合器接口：`Aggregator::accumulate(const Value&)` 与 `evaluate(Value&)`，子类为 `SumAggregator / CountAggregator / AvgAggregator / MinAggregator / MaxAggregator`（`expr/aggregator.h/.cpp`）。

---

## 9. 优化器规范

### 9.1 AST → 逻辑计划 → 物理计划

- `optimizer/logical_plan_generator.cpp::LogicalPlanGenerator::create_plan(SelectStmt*)`：将 SELECT 转为逻辑算子树（`FROM` → `TableGet`，`WHERE` → `Predicate`，`SELECT 列表` → `Project`）。
- `optimizer/physical_plan_generator.cpp::PhysicalPlanGenerator::create_plan(...)`：逻辑算子 → 物理算子。

### 9.2 规则式优化（已实现）

| 规则 | 实现 | 效果 |
| --- | --- | --- |
| 常量折叠 | `arithmetic_simplification_rule.cpp` | `age > 10+8` → `age > 18` |
| 布尔化简 | `conjunction_simplification_rule.cpp` | `x AND TRUE` → `x` |
| 谓词下推 | `predicate_pushdown_rewriter.cpp` | 尽量提前过滤 |
| 冗余节点消除 | `predicate_rewrite.cpp`（部分） | 消除恒真 Filter 等 |

规则执行入口：`optimizer/rewriter.cpp::Rewriter::rewrite` → `expression_rewriter.cpp::ExpressionRewriter::rewrite`（自底向上应用规则）。

### 9.3 投影裁剪（缺口）

- 目标：仅保留查询真正需要的列；`Project[*]` / 恒真 `Filter` 可直接消除。
- 现状：未实现对应重写规则。

### 9.4 计划可视化 / EXPLAIN

- `EXPLAIN` 通过 `explain_physical_operator` 输出物理计划树。
- 测试要求：能从任意 SELECT AST 解释每个 Plan 节点为何产生、节点间数据流关系；能展示优化前后结构变化（如 `WHERE 1=1 AND age>10+8` → `Filter[age>18]`）。

---

## 10. 执行引擎规范

### 10.1 物理算子（火山模型）

物理算子统一实现 `open() / next() / close()` 迭代协议，`next()` 逐行产出 `Tuple`，返回 `RC::RECORD_EOF` 表示结束。

| 算子 | 文件 | 职责 |
| --- | --- | --- |
| TableScan | `operator/table_scan_physical_operator.*` | 顺序扫描 |
| IndexScan | `operator/index_scan_physical_operator.*` | 索引扫描 |
| Predicate | `operator/predicate_physical_operator.*` | 执行 WHERE 过滤 |
| Project | `operator/project_physical_operator.*` | 返回 SELECT 指定列 |
| Sort | `operator/sort_physical_operator.*` | ORDER BY |
| ScalarGroupBy | `operator/scalar_group_by_physical_operator.*` | 无 GROUP BY 的聚合 |
| HashGroupBy | `operator/hash_group_by_physical_operator.*` | 哈希分组聚合 |
| NestedLoopJoin | `operator/nested_loop_join_physical_operator.*` | 嵌套循环连接 |
| HashJoin | `operator/hash_join_physical_operator.*` | 哈希连接 |
| Insert/Delete/Update | 对应 `*_physical_operator.*` | 写操作 |

### 10.2 结果集输出

- `executor/sql_result.*` 的 `SqlResult` 收集 Tuple 并迭代输出。
- 输出形式：JSON（`COMMAND` / `Table Scan` 等算子）。
- 简单命令执行器（`SHOW TABLES`、`DESC`、`HELP`、`EXIT`、`TRX_*`）通过 `executor/command_executor.*` 分发。

---

## 11. 存储引擎接口（Catalog）

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

## 12. 错误处理规范

- 统一使用 `RC` 错误码（`common/sys/rc.h`），关键新增错误码：`SCHEMA_FIELD_NOT_NULL`（非空约束违反）。
- 错误消息必须携带三类信息：
  1. **类型**：`SyntaxError` / `SemanticError` / `TypeMismatch` / `LexError`；
  2. **位置**：`line`、`column`；
  3. **原因**：简短可读描述。
- 非法输入（非法字符、未闭合字符串、缺分号、括号不匹配、表/列不存在、类型不匹配）必须返回错误，且**不崩溃**。

---

## 13. 测试与验收规范

### 13.1 测试体系

| 类别 | 内容 |
| --- | --- |
| 核心正常执行 | CREATE/INSERT/SELECT/DELETE/UPDATE 等正确执行 |
| 词法错误 | 非法字符、字符串未闭合 |
| 语法错误 | 缺分号、括号不匹配、结构错误 |
| 语义错误 | 表/列不存在、类型不匹配 |
| 边界测试 | 空输入、极长标识符、多语句、大小写 |
| 自建测试集 | 覆盖上述全部能力 |

### 13.2 关键验收用例（节选）

- 注释、多字符运算符、字符串、大小写、非法输入（对应 `requirements.md` §词法测试要求）。
- 语义错误定位（`score` 列不存在、`age + 'abc'` 类型错误、`INSERT` 类型不匹配）。
- 规划说明：能从任意 SELECT AST 解释每个 Plan 节点及数据流；能展示优化前后结构变化。
- NULL 端到端：`test/case/test/primary-null.test`（建表 NULL/NOT NULL/DATE、INSERT NULL、非空约束、`IS [NOT] NULL`、三值逻辑、聚合跳过 NULL）。

### 13.3 关注指标

- **Crash**：任何输入不得导致崩溃。
- **Wrong Accept**：非法输入被错误接受。
- **Wrong Reject**：合法输入被错误拒绝。
- **Error Location**：错误行列位置准确。

---

## 14. 能力矩阵与现状

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
| ORDER BY | ✅ | `operator/sort_physical_operator.cpp::SortPhysicalOperator::next` |
| GROUP BY | ✅ | `operator/scalar_group_by_physical_operator.cpp` / `hash_group_by_physical_operator.cpp` |
| JOIN | ✅ | `operator/nested_loop_join_physical_operator.cpp::NestedLoopJoinPhysicalOperator::next` |
| 算术表达式 | ✅ | `expr/expression.cpp::ArithmeticExpr::calc_value/get_value` |
| NULL | ✅ | `expr/expression.cpp`（三值逻辑）、`common/value.h::Value::set_null/is_null`、`storage/table/table.cpp::Table::make_record` |
| DATE | ✅ | `common/type/date_type.cpp::DateType::set_value_from_str`（整数 `YYYYMMDD`） |
| 常量折叠 | ✅ | `optimizer/arithmetic_simplification_rule.cpp::ArithmeticSimplificationRule::rewrite` |
| 布尔化简 | ✅ | `optimizer/conjunction_simplification_rule.cpp::ConjunctionSimplificationRule::rewrite` |
| 谓词下推 | ✅ | `optimizer/predicate_pushdown_rewriter.cpp::PredicatePushdownRewriter::rewrite` |
| 字符串转义 `Tom''s` | ❌ | `parser/lex_sql.l`（`%x STR` 未使用） |
| 投影裁剪 / Project[*] 消除 | ❌ | 未实现 |
| 语义错误定位 | ⚠️ | `expr/expression.h::set_location/line/column`（响应仍为 RC 码） |
| INSERT 逐列 TypeMismatch | ⚠️ | `stmt/insert_stmt.cpp::InsertStmt::create` |
| 类型一致性严格校验 | ⚠️ | `optimizer/logical_plan_generator.cpp`（`cast_cost` 隐式转换） |
| 语法 `expected:` 集合 | ⚠️ | `parser/yacc_sql.y::yyerror` + `%define parse.error verbose` |

---

## 15. 术语表（补充）

| 术语 | 位置 | 说明 |
| --- | --- | --- |
| `Stmt` | `stmt/stmt.h` | 语句对象基类，`Stmt::create()` 工厂分发 |
| `SqlResult` | `executor/sql_result.*` | 结果集封装 |
| `ExpressionBinder` | `parser/expression_binder.*` | 表达式/名字绑定器 |
| `Rewriter` | `optimizer/rewriter.*` | 重写器框架入口 |
| `Cascades` | `optimizer/cascade/` | 高级优化器框架（代价模型等） |