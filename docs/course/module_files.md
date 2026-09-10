# CSUDB 2026 三门课程源码文件清单

> 本文按当前仓库真实文件整理编译器、数据库、操作系统/存储及其集成边界。清单基于 2026-09-08 的 `main` 分支；只列课程主线、运行基础设施、测试和明确保留的高级模块，不把第三方依赖 `deps/` 纳入课程源码。

## 1. 怎么看这份清单

优先级定义：

- **P0**：课程主链，必须理解。
- **P1**：重要支撑，修改主链时经常涉及。
- **P2**：Advanced / Reserved，后续索引、事务、恢复、优化实验使用。
- **P3**：运行基础设施或暂时可忽略的外围内容。

同一个文件可能同时属于两门课程。例如：

| 交界文件 | 编译器/数据库/OS 关系 |
| --- | --- |
| `src/observer/sql/parser/resolve_stage.cpp` | 编译器语义分析通过 `Db` 查询数据库模式 |
| `src/observer/sql/optimizer/logical_plan_generator.cpp` | 编译器 IR 生成与数据库逻辑算子的交界 |
| `src/observer/sql/operator/table_scan_physical_operator.cpp` | 数据库执行器向存储层发起扫描 |
| `src/observer/storage/record/record_manager.cpp` | 数据库的 Record/RID 映射到 OS 页式存储 |
| `src/observer/storage/os/buffer/disk_buffer_pool.cpp` | 数据库 Page/Frame 通过 ReplacementPolicy 与 PageIOBackend 映射到 OS 文件 I/O |
| `src/observer/storage/db/db.cpp` | 数据库生命周期组织 Buffer Pool、Log、Transaction 和 Recovery |

当前完整主链：

```text
CLI / TCP / MySQL
  → SessionEvent / SQLStageEvent
  → Lexer / Parser
  → ParsedSqlNode
  → Resolver / Statement
  → Logical Plan
  → Rewrite / Optimizer
  → Physical Plan
  → Physical Operator / Executor
  → Db / Table / Record / Index / Transaction
  → RecordPage / DiskBufferPool
  → Frame / Page
  → read / write / lseek
  → Disk File
```

## 2. 编译器课程相关文件

### 2.1 编译请求入口与阶段载体

| 文件 | 责任 | 优先级 |
| --- | --- | --- |
| `src/observer/net/sql_task_handler.h` | 声明 SQL 总流水线入口 | P0 |
| `src/observer/net/sql_task_handler.cpp` | 顺序执行 Query Cache、Parse、Resolve、Optimize、Execute | P0 |
| `src/observer/event/sql_event.h` | 保存 SQL、Parsed SQL、Stmt 和 PhysicalOperator | P0 |
| `src/observer/event/sql_event.cpp` | `SQLStageEvent` 生命周期 | P1 |
| `src/observer/event/sql_debug.h` | SQL 调试信息接口 | P1 |
| `src/observer/event/sql_debug.cpp` | SQL 调试信息实现 | P1 |
| `src/observer/event/session_event.h` | 一次会话请求和 `SqlResult` | P1 |
| `src/observer/event/session_event.cpp` | 会话请求生命周期 | P1 |

### 2.2 Lexer、Parser 与 Parsed SQL

目录：`src/observer/sql/parser/`

| 文件 | 责任 | 优先级 |
| --- | --- | --- |
| `lex_sql.l` | Flex 词法定义；关键字、标识符、常量、运算符、分隔符 | P0 |
| `yacc_sql.y` | Bison 语法定义；构造 `ParsedSqlNode` 和表达式 | P0 |
| `parse_defs.h` | Parsed SQL/AST-like 节点、命令枚举和条件结构 | P0 |
| `parse.h` | `parse()` 接口 | P0 |
| `parse.cpp` | 对生成解析器的包装 | P0 |
| `parse_stage.h` | Parse Stage 声明 | P0 |
| `parse_stage.cpp` | 调用 Parser，处理语法失败并写入 `SQLStageEvent` | P0 |
| `resolve_stage.h` | Semantic/Resolver Stage 声明 | P0 |
| `resolve_stage.cpp` | 取得当前 `Db`，把 Parsed SQL 转为 `Stmt` | P0 |
| `expression_binder.h` | 表达式/字段绑定接口 | P0 |
| `expression_binder.cpp` | 字段解析、`*` 展开、表达式与聚合绑定 | P0 |

以下文件由 Flex/Bison 在构建时生成，不直接修改，也不提交：

```text
src/observer/sql/parser/lex_sql.cpp
src/observer/sql/parser/lex_sql.h
src/observer/sql/parser/yacc_sql.cpp
src/observer/sql/parser/yacc_sql.hpp
```

### 2.3 Semantic Analysis 与 Statement

目录：`src/observer/sql/stmt/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `stmt.h`, `stmt.cpp` | `Stmt` 基类、StmtType 和 Parsed SQL → 具体 Stmt 分派 | P0 |
| `select_stmt.h`, `select_stmt.cpp` | SELECT 表、列、表达式、Filter、Group By 语义绑定 | P0 |
| `insert_stmt.h`, `insert_stmt.cpp` | INSERT 表查找、列数与值类型检查 | P0 |
| `delete_stmt.h`, `delete_stmt.cpp` | DELETE 目标表与条件语义绑定 | P0 |
| `create_table_stmt.h`, `create_table_stmt.cpp` | CREATE TABLE 字段定义语义对象 | P0 |
| `filter_stmt.h`, `filter_stmt.cpp` | WHERE 条件中的字段、常量和比较绑定 | P0 |
| `update_stmt.h`, `update_stmt.cpp` | UPDATE 的现有扩展语义 | P1 |
| `create_index_stmt.h`, `create_index_stmt.cpp` | CREATE INDEX 语义 | P1 |
| `explain_stmt.h`, `explain_stmt.cpp` | EXPLAIN 包装内部 Stmt | P1 |
| `analyze_table_stmt.h`, `analyze_table_stmt.cpp` | ANALYZE TABLE 与统计信息 | P2 |
| `desc_table_stmt.h`, `desc_table_stmt.cpp` | DESC TABLE | P1 |
| `load_data_stmt.h`, `load_data_stmt.cpp` | LOAD DATA | P2 |
| `calc_stmt.h` | 无表表达式计算 | P2 |
| `show_tables_stmt.h` | SHOW TABLES | P1 |
| `help_stmt.h` | HELP | P3 |
| `exit_stmt.h` | EXIT | P3 |
| `set_variable_stmt.h` | SET VARIABLE | P2 |
| `trx_begin_stmt.h` | BEGIN | P2 |
| `trx_end_stmt.h` | COMMIT/ROLLBACK | P2 |

### 2.4 表达式、类型和 Tuple

目录：`src/observer/sql/expr/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `expression.h`, `expression.cpp` | 表达式树基类、比较/算术/字段/值/聚合表达式 | P0 |
| `expression_iterator.h`, `expression_iterator.cpp` | 遍历表达式树 | P1 |
| `arithmetic_operator.hpp` | 算术模板和 SIMD 计算 | P1 |
| `tuple.h` | Tuple 抽象接口 | P0 |
| `tuple_cell.h`, `tuple_cell.cpp` | Tuple 单元显示与转换 | P1 |
| `expression_tuple.h` | 根据表达式生成输出 Tuple | P0 |
| `composite_tuple.h`, `composite_tuple.cpp` | 组合多个 Tuple | P1 |
| `aggregator.h`, `aggregator.cpp` | 聚合器抽象与实现 | P2 |
| `aggregate_state.h`, `aggregate_state.cpp` | 聚合状态 | P2 |
| `aggregate_hash_table.h`, `aggregate_hash_table.cpp` | Hash Group By 状态表 | P2 |

跨目录类型文件：

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `src/observer/common/value.h`, `src/observer/common/value.cpp` | SQL 标量值、比较和类型转换 | P0 |
| `src/observer/common/type/attr_type.h`, `src/observer/common/type/attr_type.cpp` | 数据类型枚举和名称 | P0 |
| `src/observer/common/type/data_type.h`, `src/observer/common/type/data_type.cpp` | DataType 多态接口 | P1 |
| `src/observer/common/type/integer_type.h`, `src/observer/common/type/integer_type.cpp` | INT 类型行为 | P1 |
| `src/observer/common/type/float_type.h`, `src/observer/common/type/float_type.cpp` | FLOAT 类型行为 | P1 |
| `src/observer/common/type/char_type.h`, `src/observer/common/type/char_type.cpp` | CHAR 类型行为 | P1 |
| `src/observer/common/type/vector_type.h` | VECTOR 类型行为 | P2 |
| `src/observer/common/type/string_t.h` | 内部字符串表示 | P2 |

### 2.5 Logical Plan、Rewrite 与 Physical Plan 生成

目录：`src/observer/sql/optimizer/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `optimize_stage.h`, `optimize_stage.cpp` | Logical Plan → Rewrite → Physical Plan 总入口 | P0 |
| `logical_plan_generator.h`, `logical_plan_generator.cpp` | `Stmt` 转 LogicalOperator Tree | P0 |
| `physical_plan_generator.h`, `physical_plan_generator.cpp` | LogicalOperator 转 PhysicalOperator Tree | P0 |
| `rewriter.h`, `rewriter.cpp` | 规则重写调度 | P1 |
| `rewrite_rule.h` | Rewrite Rule 接口 | P1 |
| `expression_rewriter.h`, `expression_rewriter.cpp` | 表达式规则重写 | P1 |
| `predicate_rewrite.h`, `predicate_rewrite.cpp` | 谓词表达式重写 | P1 |
| `predicate_pushdown_rewriter.h`, `predicate_pushdown_rewriter.cpp` | 谓词下推 | P1 |
| `predicate_to_join_rule.h`, `predicate_to_join_rule.cpp` | 谓词转 Join 条件 | P2 |
| `comparison_simplification_rule.h`, `comparison_simplification_rule.cpp` | 比较表达式化简 | P1 |
| `conjunction_simplification_rule.h`, `conjunction_simplification_rule.cpp` | AND 条件化简 | P1 |
| `optimizer_utils.h`, `optimizer_utils.cpp` | Plan dump 等辅助功能 | P1 |
| `statistics/table_statistics.h` | 优化器表统计结构 | P2 |

Cascade/Cost Optimizer 保留文件（P2）：

```text
src/observer/sql/optimizer/cascade/README.md
src/observer/sql/optimizer/cascade/cost_model.h
src/observer/sql/optimizer/cascade/cost_model.cpp
src/observer/sql/optimizer/cascade/group.h
src/observer/sql/optimizer/cascade/group.cpp
src/observer/sql/optimizer/cascade/group_expr.h
src/observer/sql/optimizer/cascade/group_expr.cpp
src/observer/sql/optimizer/cascade/implementation_rules.h
src/observer/sql/optimizer/cascade/implementation_rules.cpp
src/observer/sql/optimizer/cascade/memo.h
src/observer/sql/optimizer/cascade/memo.cpp
src/observer/sql/optimizer/cascade/optimizer.h
src/observer/sql/optimizer/cascade/optimizer.cpp
src/observer/sql/optimizer/cascade/optimizer_context.h
src/observer/sql/optimizer/cascade/optimizer_context.cpp
src/observer/sql/optimizer/cascade/pattern.h
src/observer/sql/optimizer/cascade/pending_tasks.h
src/observer/sql/optimizer/cascade/property.h
src/observer/sql/optimizer/cascade/property_set.h
src/observer/sql/optimizer/cascade/rules.h
src/observer/sql/optimizer/cascade/rules.cpp
src/observer/sql/optimizer/cascade/tasks/apply_rule_task.h
src/observer/sql/optimizer/cascade/tasks/apply_rule_task.cpp
src/observer/sql/optimizer/cascade/tasks/cascade_task.h
src/observer/sql/optimizer/cascade/tasks/cascade_task.cpp
src/observer/sql/optimizer/cascade/tasks/e_group_task.h
src/observer/sql/optimizer/cascade/tasks/e_group_task.cpp
src/observer/sql/optimizer/cascade/tasks/o_expr_task.h
src/observer/sql/optimizer/cascade/tasks/o_expr_task.cpp
src/observer/sql/optimizer/cascade/tasks/o_group_task.h
src/observer/sql/optimizer/cascade/tasks/o_group_task.cpp
src/observer/sql/optimizer/cascade/tasks/o_input_task.h
src/observer/sql/optimizer/cascade/tasks/o_input_task.cpp
```

### 2.6 编译器缓存阶段

```text
src/observer/sql/query_cache/query_cache_stage.h
src/observer/sql/query_cache/query_cache_stage.cpp
src/observer/sql/plan_cache/plan_cache_stage.h
src/observer/sql/plan_cache/plan_cache_stage.cpp
```

当前均属于 P2；主请求链会经过 QueryCache，但不应把课程核心建立在尚未完善的缓存行为上。

## 3. 数据库课程相关文件

### 3.1 Catalog、Schema 与 Metadata

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `src/observer/catalog/catalog.h`, `src/observer/catalog/catalog.cpp` | Catalog 单例与表统计数据 | P1 |
| `src/observer/catalog/table_stats.h` | 表统计信息 | P2 |
| `src/observer/storage/db/db.h`, `src/observer/storage/db/db.cpp` | Database、表集合、Buffer/Trx/Log/Recovery 生命周期 | P0 |
| `src/observer/storage/table/table_meta.h`, `src/observer/storage/table/table_meta.cpp` | 表模式、字段、索引和存储格式元数据 | P0 |
| `src/observer/storage/field/field_meta.h`, `src/observer/storage/field/field_meta.cpp` | 字段名、类型、offset、长度 | P0 |
| `src/observer/storage/field/field.h`, `src/observer/storage/field/field.cpp` | 绑定到具体 Table 的字段对象 | P0 |
| `src/observer/storage/index/index_meta.h`, `src/observer/storage/index/index_meta.cpp` | 索引元数据 | P1 |
| `src/observer/storage/common/meta_util.h`, `src/observer/storage/common/meta_util.cpp` | 元数据、数据和索引文件命名 | P1 |

### 3.2 Logical Operator 文件

目录：`src/observer/sql/operator/`

```text
logical_operator.h / logical_operator.cpp
operator_node.h
calc_logical_operator.h
delete_logical_operator.h / delete_logical_operator.cpp
explain_logical_operator.h
group_by_logical_operator.h / group_by_logical_operator.cpp
insert_logical_operator.h / insert_logical_operator.cpp
join_logical_operator.h
predicate_logical_operator.h / predicate_logical_operator.cpp
project_logical_operator.h / project_logical_operator.cpp
table_get_logical_operator.h / table_get_logical_operator.cpp
```

DDL 通常没有 LogicalOperator，而是经 `CommandExecutor` 执行。课程 Core 重点是 `TableGet`、`Predicate`、`Project`、`Insert`、`Delete`。

### 3.3 Physical Operator 文件

目录：`src/observer/sql/operator/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `physical_operator.h`, `physical_operator.cpp` | Pull-based 物理算子基类 | P0 |
| `table_scan_physical_operator.h`, `table_scan_physical_operator.cpp` | 行式 SeqScan | P0 |
| `predicate_physical_operator.h`, `predicate_physical_operator.cpp` | Filter | P0 |
| `project_physical_operator.h`, `project_physical_operator.cpp` | Project | P0 |
| `insert_physical_operator.h`, `insert_physical_operator.cpp` | INSERT | P0 |
| `delete_physical_operator.h`, `delete_physical_operator.cpp` | DELETE | P0 |
| `index_scan_physical_operator.h`, `index_scan_physical_operator.cpp` | IndexScan | P2 |
| `explain_physical_operator.h`, `explain_physical_operator.cpp` | 输出物理计划树 | P1 |
| `calc_physical_operator.h` | 表达式计算 | P2 |
| `string_list_physical_operator.h` | HELP/SHOW 等字符串列表结果 | P3 |
| `join_physical_operator.h`, `join_physical_operator.cpp` | Join 基础算子 | P2 |
| `nested_loop_join_physical_operator.h`, `nested_loop_join_physical_operator.cpp` | Nested Loop Join | P2 |
| `hash_join_physical_operator.h`, `hash_join_physical_operator.cpp` | Hash Join | P2 |
| `group_by_physical_operator.h`, `group_by_physical_operator.cpp` | Group By 基类/通用实现 | P2 |
| `scalar_group_by_physical_operator.h`, `scalar_group_by_physical_operator.cpp` | Scalar Aggregate | P2 |
| `hash_group_by_physical_operator.h`, `hash_group_by_physical_operator.cpp` | Hash Group By | P2 |
| `aggregate_vec_physical_operator.h`, `aggregate_vec_physical_operator.cpp` | 向量化聚合 | P2 |
| `expr_vec_physical_operator.h`, `expr_vec_physical_operator.cpp` | 向量化表达式 | P2 |
| `group_by_vec_physical_operator.h`, `group_by_vec_physical_operator.cpp` | 向量化 Group By | P2 |
| `project_vec_physical_operator.h`, `project_vec_physical_operator.cpp` | 向量化 Project | P2 |
| `table_scan_vec_physical_operator.h`, `table_scan_vec_physical_operator.cpp` | 向量化/PAX Scan | P2 |

### 3.4 Executor 与结果驱动

目录：`src/observer/sql/executor/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `execute_stage.h`, `execute_stage.cpp` | 执行入口；选择 PhysicalOperator 或 CommandExecutor | P0 |
| `sql_result.h`, `sql_result.cpp` | 打开/拉取/关闭算子，事务提交回滚，承载结果 | P0 |
| `command_executor.h`, `command_executor.cpp` | DDL、管理命令与事务命令分派 | P0 |
| `create_table_executor.h`, `create_table_executor.cpp` | CREATE TABLE | P0 |
| `create_index_executor.h`, `create_index_executor.cpp` | CREATE INDEX | P2 |
| `desc_table_executor.h`, `desc_table_executor.cpp` | DESC | P1 |
| `show_tables_executor.h` | SHOW TABLES | P1 |
| `help_executor.h` | HELP | P3 |
| `load_data_executor.h`, `load_data_executor.cpp` | LOAD DATA | P2 |
| `analyze_table_executor.h`, `analyze_table_executor.cpp` | 生成表统计 | P2 |
| `set_variable_executor.h`, `set_variable_executor.cpp` | 会话变量 | P2 |
| `trx_begin_executor.h` | BEGIN | P2 |
| `trx_end_executor.h` | COMMIT/ROLLBACK | P2 |

### 3.5 Table、Record 与存储引擎门面

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `src/observer/storage/default/default_handler.h`, `src/observer/storage/default/default_handler.cpp` | 默认数据库处理器和 `sys` 库启动 | P1 |
| `src/observer/storage/table/table.h`, `src/observer/storage/table/table.cpp` | Table 门面；Record 构造、CRUD、Scanner、Index | P0 |
| `src/observer/storage/table/table_engine.h` | Heap/LSM TableEngine 抽象 | P1 |
| `src/observer/storage/table/heap_table_engine.h`, `src/observer/storage/table/heap_table_engine.cpp` | Heap Record 与 B+Tree 协调 | P0 |
| `src/observer/storage/table/lsm_table_engine.h`, `src/observer/storage/table/lsm_table_engine.cpp` | LSM 表后端适配 | P2 |
| `src/observer/storage/record/record.h` | `RID(page_num, slot_num)` 和 Record 数据 | P0 |
| `src/observer/storage/record/record_scanner.h`, `src/observer/storage/record/record_scanner.cpp` | RecordScanner 接口/公共行为 | P0 |
| `src/observer/storage/record/heap_record_scanner.h`, `src/observer/storage/record/heap_record_scanner.cpp` | Heap 数据文件顺序扫描 | P0 |
| `src/observer/storage/record/lsm_record_scanner.h`, `src/observer/storage/record/lsm_record_scanner.cpp` | LSM 扫描适配 | P2 |
| `src/observer/storage/record/record_manager.h`, `src/observer/storage/record/record_manager.cpp` | RecordPage 布局、slot bitmap、记录 CRUD | P0 |
| `src/observer/storage/record/lob_handler.h`, `src/observer/storage/record/lob_handler.cpp` | 大对象文件处理 | P2 |
| `src/observer/storage/record/record_log.h`, `src/observer/storage/record/record_log.cpp` | Record WAL/Redo | P2 |

### 3.6 Storage Common 与向量化数据结构

```text
src/observer/storage/common/arena_allocator.h
src/observer/storage/common/arena_allocator.cpp
src/observer/storage/common/chunk.h
src/observer/storage/common/chunk.cpp
src/observer/storage/common/codec.h
src/observer/storage/common/codec.cpp
src/observer/storage/common/column.h
src/observer/storage/common/column.cpp
src/observer/storage/common/condition_filter.h
src/observer/storage/common/condition_filter.cpp
src/observer/storage/common/vector_buffer.h
```

其中 `condition_filter` 属于 P1；Chunk/Column/Codec/Arena 属于向量化与 PAX 路径，当前为 P2。

### 3.7 Index（Advanced / Reserved）

目录：`src/observer/storage/index/`

```text
index.h / index.cpp
index_meta.h / index_meta.cpp
bplus_tree.h / bplus_tree.cpp
bplus_tree_index.h / bplus_tree_index.cpp
bplus_tree_log.h / bplus_tree_log.cpp
bplus_tree_log_entry.h / bplus_tree_log_entry.cpp
latch_memo.h / latch_memo.cpp
ivfflat_index.h
```

全部保留。`index`/`index_meta` 是抽象与元数据；`bplus_tree*` 是后续 B+Tree Visualizer、IndexScan、索引 Page Trace、WAL/Recovery 的 P2 主体；`ivfflat_index.h` 为向量索引预留。

### 3.8 Transaction（Advanced / Reserved）

目录：`src/observer/storage/trx/`

```text
trx.h / trx.cpp
vacuous_trx.h / vacuous_trx.cpp
mvcc_trx.h / mvcc_trx.cpp
mvcc_trx_log.h / mvcc_trx_log.cpp
lsm_mvcc_trx.h / lsm_mvcc_trx.cpp
```

`trx.h/.cpp` 是统一事务接口；`vacuous_trx` 是默认基础路径；MVCC 与 LSM MVCC 为 P2。当前 `mvcc_trx_log_test` 有已记录并发问题，不能通过删除断言规避。

## 4. 操作系统课程相关文件

### 4.1 Page、Frame、Buffer Pool 与替换策略

目录：`src/observer/storage/os/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `page/page.h` | 固定 8 KiB Page 的二进制布局、LSN 和 checksum | P0 |
| `page/frame.h`, `page/frame.cpp` | 内存 Frame、Page、dirty、pin、latch、访问时间 | P0 |
| `buffer/disk_buffer_pool.h`, `buffer/disk_buffer_pool.cpp` | 页文件、页分配/释放、Frame 管理、策略与 I/O 委托、Flush | P0 |
| `replacement/replacement_policy.h`, `.cpp` | 可插拔 LRU/FIFO/CLOCK 与 victim 选择 | P0 |
| `io/page_io_backend.h`, `io/page_io_backend.cpp` | legacy/positional Page I/O 后端 | P0 |
| `diagnostics/buffer_pool_stats.h`, `.cpp` | hit/miss、pin、I/O/延迟、淘汰、dirty、分类 flush | P0 |
| `diagnostics/buffer_pool_diagnostics.h`, `.cpp` | Snapshot DTO、Dirty Flush 结果、Trace event sequence | P0 |
| `diagnostics/buffer_pool_log.h`, `.cpp` | Buffer Pool WAL 与回放 | P2 |
| `buffer/double_write_buffer.h`, `.cpp` | Double Write 防止 torn page | P2 |

当前 `page.h` 中：

```cpp
BP_PAGE_SIZE = 1 << 13;  // 8192 bytes
```

指导书中的 4 KiB 只是示例。不要为了匹配示例修改现有 8 KiB 文件格式。

替换策略使用的公共容器：

```text
src/common/lang/lru_cache.h
src/common/lang/list.h
src/common/lang/unordered_map.h
src/common/lang/mutex.h
src/common/lang/atomic.h
```

### 4.2 Record/Page 映射边界

这些文件同时属于数据库与 OS：

```text
src/observer/storage/record/record.h
src/observer/storage/record/record_manager.h
src/observer/storage/record/record_manager.cpp
src/observer/storage/record/record_scanner.h
src/observer/storage/record/record_scanner.cpp
src/observer/storage/record/heap_record_scanner.h
src/observer/storage/record/heap_record_scanner.cpp
src/observer/storage/table/heap_table_engine.h
src/observer/storage/table/heap_table_engine.cpp
```

其中 `RecordPageHandler` 将 `RID.page_num/slot_num` 映射到固定页中的 slot；它通过 `DiskBufferPool` 获取 Frame。它是课程中“Row → Page”最关键的交界。

### 4.3 Disk/File I/O 与持久化

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `src/common/io/io.h`, `src/common/io/io.cpp` | `readn/writen/preadn/pwriten` 可靠文件描述符 I/O | P0 |
| `src/observer/storage/persist/persist.h`, `src/observer/storage/persist/persist.cpp` | 通用持久化文件辅助 | P2 |
| `src/observer/storage/common/meta_util.h`, `src/observer/storage/common/meta_util.cpp` | 存储文件路径和命名 | P1 |
| `src/common/os/path.h`, `src/common/os/path.cpp` | 路径处理 | P1 |
| `src/common/os/os.h`, `src/common/os/os.cpp` | OS 公共功能 | P2 |
| `src/common/math/crc.h`, `src/common/math/crc.cpp` | Page checksum | P1 |

目标分页文件的页级 I/O 由 `disk_buffer_pool.cpp` 委托给 `page_io_backend.cpp`；legacy 使用 `lseek + read/write`，positional 使用 `pread/pwrite`。Double Write Buffer 和 WAL 有独立文件路径与统计边界。不能绕开 Buffer Pool 从 Executor 直接读文件。

### 4.4 WAL、Redo 与 Recovery（Advanced / Reserved）

目录：`src/observer/storage/clog/`

```text
log_module.h
log_entry.h / log_entry.cpp
log_buffer.h / log_buffer.cpp
log_file.h / log_file.cpp
log_handler.h / log_handler.cpp
log_replayer.h
disk_log_handler.h / disk_log_handler.cpp
integrated_log_replayer.h / integrated_log_replayer.cpp
vacuous_log_handler.h
```

相关跨模块日志文件：

```text
src/observer/storage/os/diagnostics/buffer_pool_log.h
src/observer/storage/os/diagnostics/buffer_pool_log.cpp
src/observer/storage/record/record_log.h
src/observer/storage/record/record_log.cpp
src/observer/storage/index/bplus_tree_log.h
src/observer/storage/index/bplus_tree_log.cpp
src/observer/storage/index/bplus_tree_log_entry.h
src/observer/storage/index/bplus_tree_log_entry.cpp
src/observer/storage/trx/mvcc_trx_log.h
src/observer/storage/trx/mvcc_trx_log.cpp
tools/clog_dump.cpp
```

### 4.5 进程、线程、同步与内存基础设施

以下是 OS 相关基础设施，但不是页式存储主线：

```text
src/common/os/process.h / process.cpp
src/common/os/process_param.h / process_param.cpp
src/common/os/signal.h / signal.cpp
src/common/os/pidfile.h / pidfile.cpp
src/common/thread/thread_pool_executor.h / thread_pool_executor.cpp
src/common/thread/thread_util.h / thread_util.cpp
src/common/thread/runnable.h
src/common/lang/thread.h
src/common/lang/mutex.h / mutex.cpp
src/common/lang/condition_variable.h
src/common/lang/atomic.h
src/common/mm/mem_pool.h / mem_pool.cpp
src/common/mm/debug_new.h
```

这些文件通常是 P2/P3。课程 OS 核心仍然是 Page、Frame、Buffer Pool、替换策略、统计、日志与文件 I/O。

## 5. CLI、网络和未来第三方接口文件

### 5.1 CSUDB 2026 外观与启动

```text
csudb
etc/csudb.ini
etc/csudb-client.toml
src/common/version.h
src/observer/main.cpp
src/observer/common/init.h
src/observer/common/init.cpp
src/observer/common/global_context.h
src/observer/common/global_context.cpp
src/observer/mainpage.md
```

### 5.2 本地 CLI 与网络协议

目录：`src/observer/net/`

| 文件组 | 责任 | 优先级 |
| --- | --- | --- |
| `communicator.h`, `communicator.cpp` | CLI/Plain/MySQL 请求与结果抽象、工厂 | P1 |
| `native_communicator.h`, `native_communicator.cpp` | CSUDB 认证 native protocol 与 QueryResult JSON | P0 |
| `cli_communicator.h`, `cli_communicator.cpp` | 本地 stdin/stdout Shell | P1 |
| `plain_communicator.h`, `plain_communicator.cpp` | 简单 `\0` 结尾 TCP 文本协议 | P1 |
| `mysql_communicator.h`, `mysql_communicator.cpp` | MySQL wire protocol | P2 |
| `server.h`, `server.cpp` | CLI Server、TCP Server、accept/serve 生命周期 | P1 |
| `server_param.h` | 地址、端口、协议和线程模型 | P1 |
| `sql_task_handler.h`, `sql_task_handler.cpp` | 所有接口共用的 SQL 流水线入口 | P0 |
| `thread_handler.h`, `thread_handler.cpp` | 连接线程处理抽象/工厂 | P2 |
| `one_thread_per_connection_thread_handler.h`, `one_thread_per_connection_thread_handler.cpp` | 每连接一线程 | P2 |
| `java_thread_pool_thread_handler.h`, `java_thread_pool_thread_handler.cpp` | 线程池事件模型 | P2 |
| `buffered_writer.h`, `buffered_writer.cpp` | 网络/CLI 缓冲输出 | P1 |
| `ring_buffer.h`, `ring_buffer.cpp` | MySQL/网络输入缓冲 | P2 |

独立客户端：

```text
src/obclient/CMakeLists.txt
src/obclient/client.cpp
```

产品服务与认证边界：

```text
src/observer/service/database_service.h / database_service.cpp
src/observer/service/query_result.h / query_result.cpp
src/observer/auth/system_catalog.h / system_catalog.cpp
src/observer/session/session.h / session.cpp
docs/product/COMMANDS.md
docs/product/COMMAND_AUDIT.md
docs/product/ARCHITECTURE.md
```

行编辑与历史：

```text
src/common/linereader/line_reader.h
src/common/linereader/line_reader.cpp
```

当前不存在正式的 HTTP/REST、gRPC、Python SDK 或 Java SDK。后续应先建立统一 `DatabaseService` 和稳定的 QueryResult DTO，再让 CLI、网络协议、Web UI 和第三方 SDK 复用它；不要让第三方接口直接访问 `Page*`、`Frame*`、`Table*` 或 `PhysicalOperator*`。

### 5.3 Session 和请求上下文

```text
src/observer/session/session.h
src/observer/session/session.cpp
src/observer/session/session_stage.h
src/observer/session/session_stage.cpp
src/observer/session/thread_data.h
src/observer/session/thread_data.cpp
src/observer/event/session_event.h
src/observer/event/session_event.cpp
src/observer/event/sql_event.h
src/observer/event/sql_event.cpp
src/observer/event/storage_event.h
```

## 6. LSM 存储后端（Advanced / Reserved）

`src/oblsm/` 当前被 `observer_static` 直接链接，因此不能作为无关模块删除。课程基础主线可先忽略，后续对比 Heap/LSM 时再读。

```text
src/oblsm/include/                 对外 LSM 接口与 Options/Iterator
src/oblsm/memtable/                MemTable/SkipList
src/oblsm/table/                   Block、SSTable、Builder、Merger
src/oblsm/compaction/              Compaction 与 Picker
src/oblsm/wal/                     LSM WAL
src/oblsm/util/                    Arena、BloomFilter、Coding、Comparator、File I/O、LRU
src/oblsm/client/                  LSM 实验 CLI
src/oblsm/ob_lsm_impl.*            LSM 主实现
src/oblsm/ob_lsm_transaction.*     LSM Transaction
src/oblsm/ob_manifest.*            Manifest/Version metadata
src/oblsm/ob_user_iterator.*       用户迭代器
```

## 7. 相关测试文件

### 7.1 编译器与表达式

```text
unittest/observer/parser_test.cpp
unittest/observer/expression_test.cpp
unittest/observer/arithmetic_operator_test.cpp
unittest/observer/composite_tuple_test.cpp
unittest/observer/aggregate_hash_table_test.cpp
```

### 7.2 数据库核心

```text
unittest/observer/catalog_test.cpp
unittest/observer/record_manager_test.cpp
unittest/observer/bplus_tree_test.cpp
unittest/observer/bplus_tree_log_entry_test.cpp
unittest/observer/bplus_tree_log_test.cpp
unittest/observer/chunk_test.cpp
unittest/observer/codec_test.cpp
unittest/observer/pax_storage_test.cpp
unittest/observer/mvcc_trx_log_test.cpp
test/integration_test/
test/case/
```

### 7.3 OS、Buffer、日志与持久化

```text
unittest/observer/bp_manager_test.cpp
unittest/observer/buffer_pool_os_test.cpp
unittest/observer/buffer_pool_log_test.cpp
unittest/observer/disk_buffer_pool_test.cpp
unittest/observer/double_write_buffer_test.cpp
unittest/observer/persist_test.cpp
unittest/observer/log_buffer_test.cpp
unittest/observer/log_entry_test.cpp
unittest/observer/log_file_test.cpp
unittest/observer/disk_log_handler_test.cpp
unittest/observer/ring_buffer_test.cpp
```

### 7.4 测试构建入口

```text
unittest/CMakeLists.txt
unittest/observer/CMakeLists.txt
unittest/common/CMakeLists.txt
unittest/oblsm/CMakeLists.txt
test/integration_test/README.md
test/integration_test/miniob_test_config.py
```

## 8. 构建、配置与课程文档

```text
CMakeLists.txt
build.sh
src/CMakeLists.txt
src/common/CMakeLists.txt
src/observer/CMakeLists.txt
src/obclient/CMakeLists.txt
src/oblsm/CMakeLists.txt
etc/csudb.ini
etc/observer.ini
README.md
docs/course/grammar.md
docs/course/architecture.md
docs/course/source_map.md
docs/course/module_files.md
docs/course/baseline.md
docs/course/os_storage.md
docs/course/cli.md
```

`source_map.md` 是精选阅读地图；本文是按课程边界展开的文件清单。二者用途不同，不相互替代。

## 9. 建议的开发分工边界

| 开发线 | 主要负责目录 | 允许依赖 | 不应直接修改 |
| --- | --- | --- | --- |
| Compiler | `sql/parser`, `sql/stmt`, `sql/expr`, Plan Generator | Catalog/Metadata 只读接口 | Page/Frame/File I/O |
| Database | `sql/operator`, `sql/executor`, `catalog`, `storage/table`, `storage/index`, `storage/trx` | Compiler 输出、Record/Storage 接口 | CLI 展示细节 |
| OS/Storage | `storage/buffer`, `storage/record`, `storage/persist`, `storage/clog` | 文件系统、日志、同步原语 | Parser/Stmt/SQL 语法 |
| CLI/API | `main.cpp`, `net`, `session`, `event`, `obclient`, 未来接口层 | 统一 SQL Service/Result | 直接暴露或操作 Frame/Page |

最容易产生多人冲突的共享文件：

```text
src/observer/net/sql_task_handler.cpp
src/observer/event/sql_event.h
src/observer/sql/optimizer/optimize_stage.cpp
src/observer/sql/executor/sql_result.cpp
src/observer/storage/table/table.cpp
src/observer/storage/record/record_manager.cpp
src/observer/storage/os/buffer/disk_buffer_pool.cpp
src/observer/storage/db/db.cpp
```

修改这些文件前应明确接口负责人，并同时运行 Parser、Buffer Pool 和 CLI SQL 回归。

## 10. 文件存在性检查

文档中的显式源码路径可用下面的命令重新盘点：

```bash
rg --files \
  src/observer/sql \
  src/observer/catalog \
  src/observer/storage \
  src/observer/event \
  src/observer/session \
  src/observer/net \
  src/observer/common \
  src/obclient \
  src/oblsm \
  unittest/observer \
  test | sort
```

修改课程核心后至少执行：

```bash
./build.sh debug --make -j4
ctest --test-dir build_debug --output-on-failure \
  -R 'parser_test|buffer_pool_os_test|record_manager_test'
./csudb
```
