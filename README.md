# MiniDB Baseline

MiniDB Baseline v0.1 是基于 [OceanBase MiniOB](https://github.com/oceanbase/miniob) 整理的课程项目基座。它服务于“编译原理 + 数据库系统 + 操作系统”融合课程，目标是保留 MiniOB 成熟、可运行、可扩展的 DBMS 骨架，并为后续实验提供清晰的源码入口。

> Baseline 原则：稳定性 > 可读性 > 精简程度。本次没有重写 Parser、Executor、Storage、Buffer Pool、B+Tree、Transaction 或日志实现。

## 1. 项目定位

课程主线是：

```text
SQL
  -> Lexer / Parser
  -> Parsed SQL (AST-like parse result)
  -> Resolver / Semantic Analysis
  -> Statement
  -> Logical Plan
  -> Rewrite / Optimizer
  -> Physical Plan
  -> Executor
  -> Table / Record / Index
  -> Buffer Pool
  -> Page / Frame
  -> Disk / File
```

SQL 前端对应编译原理，计划与执行对应数据库系统，页、缓冲池、日志与文件 I/O 对应数据库存储及操作系统。当前版本只建立稳定基座，不实现可视化、Trace、新 MVCC、新 WAL 或新的缓存管理器。

## 2. 当前基座保留的功能

### Core

- SQL Lexer/Parser：Flex + Bison，入口位于 `src/observer/sql/parser/`。
- Semantic/Resolver：`ResolveStage`、`Stmt::create_stmt`、各类 `*Stmt::create` 与 `ExpressionBinder`。
- Catalog/Metadata：`Catalog`、`Db`、`TableMeta`、`FieldMeta`、`IndexMeta`。
- Logical Plan、规则重写、Physical Plan。
- Pull-based Physical Operator 与 `SqlResult` 驱动的执行。
- Heap Table、Record/RID、Page/Frame、Buffer Pool 与磁盘文件。
- 基本 SQL：`CREATE TABLE`、`INSERT`、`SELECT`、`WHERE`、`DELETE`。

### Advanced / Reserved

- B+Tree、`IndexScanPhysicalOperator` 与索引日志。
- Vacuous/MVCC/LSM 事务接口。
- CLog、Redo、Recovery、Double Write Buffer。
- Cascade Optimizer、统计信息、向量化执行、PAX/LSM 存储后端。
- `EXPLAIN`、聚合、Group By、Join、Update、Create Index 等现有扩展语法与实现。

这些模块当前不作为课程基础主线，但源码与构建能力完整保留。

### Infrastructure

- `session/`、`event/`、`net/`、`common/`：observer 启动、请求传递、协议、线程与公共设施。
- `src/obclient/`：plain 协议客户端。
- `test/`、`unittest/`：集成与单元测试。
- `docker/`、`.devcontainer/`：可选开发环境。

### Peripheral / Removed from baseline

- `src/cpplings/`：独立 C++ 语法练习，不被 observer、obclient 或核心测试依赖。
- `src/memtracer/`：独立 `LD_PRELOAD` 内存监控库，默认不参与构建，核心目标无链接依赖。
- 与 MemTracer 一一对应的 unittest、benchmark 和旧文档页。

其余 benchmark 保留，因为覆盖 B+Tree、Record、并发及执行性能，适合后续高级实验。`src/oblsm/` 也保留，因为 `observer_static` 当前直接链接 `oblsm`，并且 `Db::init` 会初始化 LSM 实例。

## 3. 项目目录总览

| 路径 | 作用 | 阅读建议 |
| --- | --- | --- |
| `src/observer/main.cpp` | observer 进程入口与参数解析 | 必看入口 |
| `src/observer/net/` | CLI/plain/MySQL 协议与请求接入 | 基础运行需要，先看 `sql_task_handler.cpp` |
| `src/observer/session/` | 会话、事务上下文 | 重要基础设施 |
| `src/observer/event/` | SQL 各阶段共享事件对象 | 必看 `sql_event.h` |
| `src/observer/sql/` | Parser、Resolver、Plan、Optimizer、Operator、Executor | 课程 P0 |
| `src/observer/catalog/` | 表统计信息 Catalog | 课程 P1 |
| `src/observer/storage/` | Db、Table、Record、Index、Buffer、Transaction、Log | 课程 P0/P2 |
| `src/common/` | 日志、配置、线程、内存池、基础类型 | 按需阅读 |
| `src/obclient/` | 网络客户端 | CLI 模式学习时可暂时忽略 |
| `src/oblsm/` | LSM 后端及其 WAL | 构建依赖；基础主线暂时忽略 |
| `test/` | 官方集成测试框架与 case | 功能回归必看 |
| `unittest/` | Parser、Buffer、Record、B+Tree、Log 等单测 | 模块修改时优先运行 |
| `benchmark/` | 核心模块性能/并发基准 | Advanced/Reserved |
| `etc/` | observer 配置 | 启动必需 |
| `docs/course/` | 本课程架构、语法、源码地图和验收记录 | 新开发者先读 |
| `docs/docs/` | MiniOB 上游文档 | 深入时查阅 |

推荐阅读顺序：本 README → `docs/course/architecture.md` → `docs/course/source_map.md` → `sql_task_handler.cpp` → SQL 各 Stage → Table/Record/Buffer。

## 4. SQL 模块源码地图

### parser

文件：`src/observer/sql/parser/lex_sql.l`

- 职责：将 SQL 字符串识别为关键字、标识符、常量和运算符 token。
- 输入：SQL 字符流。
- 输出：Bison 消费的 token 与语义值。
- 下一步：`yacc_sql.y`。
- 后续可扩展：Token Trace、关键字与操作符语法；不要直接修改生成的 `lex_sql.cpp/.h`。

文件：`src/observer/sql/parser/yacc_sql.y`

- 职责：定义语法产生式，构造 `ParsedSqlNode`、`ConditionSqlNode` 和未绑定表达式。
- 输入：Lexer token。
- 输出：`ParsedSqlResult` 中的 `ParsedSqlNode`。
- 下一步：`ParseStage` 将首个节点放入 `SQLStageEvent`。
- 后续可扩展：AST/Parse Result 可视化、课程语法扩展；不要直接修改生成的 `yacc_sql.cpp/.hpp`。

文件：`parse.cpp`、`parse_defs.h`、`parse_stage.cpp`

- 职责：`parse()` 包装生成的解析器；`parse_defs.h` 定义 Parsed SQL 数据结构；`ParseStage::handle_request` 处理语法错误和事件传递。
- 输入：`SQLStageEvent::sql()`。
- 输出：`SQLStageEvent::sql_node()`。
- 下一步：`ResolveStage`。
- 后续可扩展：Parser Trace、Parsed SQL dump。

### stmt / Resolver

文件：`src/observer/sql/parser/resolve_stage.cpp`、`src/observer/sql/stmt/stmt.cpp`

- 职责：从当前 Session 获取 `Db`，由 `Stmt::create_stmt` 按命令类型分派到 `SelectStmt::create`、`InsertStmt::create`、`DeleteStmt::create` 等。
- 输入：`ParsedSqlNode` + 当前 `Db`。
- 输出：类型安全且已解析元数据引用的 `Stmt`。
- 下一步：`OptimizeStage` 或命令执行器。
- 后续可扩展：Semantic Trace、符号表/元数据访问记录。

文件：`select_stmt.cpp`、`filter_stmt.cpp`、`expression_binder.cpp`

- 职责：查表和字段，展开 `*`，将 `UnboundFieldExpr` 绑定为 `FieldExpr`，创建过滤条件。
- 输入：`SelectSqlNode`、`Db/TableMeta`。
- 输出：`SelectStmt`、绑定后的表达式与 `FilterStmt`。
- 下一步：`LogicalPlanGenerator`。
- 后续可扩展：类型检查诊断、Name Resolution 可观测。

### expr

文件：`src/observer/sql/expr/expression.h/.cpp`

- 职责：Value、Field、Comparison、Conjunction、Arithmetic、Aggregation 等表达式节点及求值。
- 输入：解析阶段构造的表达式、Resolver 绑定信息、执行时 Tuple。
- 输出：绑定表达式或运行时 `Value`。
- 下一步：Logical/Physical Operator。
- 后续可扩展：表达式树 dump、类型推导和求值 Trace。

文件：`tuple.h`、`composite_tuple.*`、`expression_tuple.h`

- 职责：为算子提供行视图和表达式求值上下文。

### optimizer

文件：`logical_plan_generator.cpp/.h`

- 职责：把 `Stmt` 转成 `TableGet`、`Predicate`、`Project`、`Insert`、`Delete` 等逻辑算子树。
- 输入：`Stmt *`。
- 输出：`unique_ptr<LogicalOperator>`。
- 下一步：`OptimizeStage::rewrite`。
- 后续可扩展：Logical Plan dump、规则前后对比。

文件：`rewriter.cpp`、`predicate_rewrite.cpp`、`predicate_pushdown_rewriter.cpp` 及其他 `*_rule.cpp`

- 职责：循环应用规则进行表达式简化、谓词下推和连接条件改写。
- 输入/输出：逻辑算子树。
- 下一步：Physical Plan Generator 或 Cascade Optimizer。
- 后续可扩展：Rule Trace、规则开关。

文件：`physical_plan_generator.cpp/.h`、`optimize_stage.cpp/.h`

- 职责：选择 TableScan/IndexScan 等物理实现，递归生成物理算子树；`OptimizeStage` 串起逻辑生成、重写和物理生成。
- 输入：逻辑算子树 + Session 执行模式。
- 输出：`PhysicalOperator` 树，写入 `SQLStageEvent`。
- 下一步：`ExecuteStage`。
- 后续可扩展：Plan Visualizer、代价模型、SeqScan/IndexScan 对比。

### operator

文件：`logical_operator.h/.cpp`、`physical_operator.h/.cpp`

- 职责：逻辑/物理算子公共接口与树形结构。

文件：`table_scan_physical_operator.cpp`、`index_scan_physical_operator.cpp`、`predicate_physical_operator.cpp`、`project_physical_operator.cpp`

- 职责：SELECT 的扫描、索引访问、过滤与投影；使用 `open/next/current_tuple/close` 迭代协议。
- 输入：Table、Index、Expression、Trx。
- 输出：Tuple 流。
- 下一步：父算子与 `SqlResult`。
- 后续可扩展：Operator Trace、行数统计、运行时耗时。

文件：`insert_physical_operator.cpp`、`delete_physical_operator.cpp`

- 职责：通过 `Trx` 调用 Table 的记录写入/删除链。

### executor

文件：`execute_stage.cpp`、`sql_result.cpp`

- 职责：把物理计划交给 `SqlResult`；结果输出时由 `SqlResult::open/next_tuple/close` 真正驱动物理算子。
- 输入：`SQLStageEvent::physical_operator()`。
- 输出：结果集或状态码。
- 后续可扩展：Execution Trace、执行统计。

文件：`command_executor.cpp`、`create_table_executor.cpp`

- 职责：没有物理计划的 DDL/管理命令分派；CREATE TABLE 最终调用 `Db::create_table`。

## 5. Storage 模块源码地图

| 概念 | 真实文件与核心类 | 输入 → 输出 | 适合的扩展点 |
| --- | --- | --- | --- |
| Page | `storage/buffer/page.h`：`Page` | 8 KiB 页（LSN、checksum、data） | Page dump/Trace |
| Frame | `storage/buffer/frame.h/.cpp`：`Frame` | Page + page id + dirty + pin + latch + LRU 时间 | Pin/dirty/LRU 可视化 |
| Buffer Pool | `storage/buffer/disk_buffer_pool.h/.cpp`：`BPFrameManager`、`DiskBufferPool`、`BufferPoolManager` | `(buffer_pool_id, page_num)` → pinned `Frame *` | 命中率、替换策略、页访问事件 |
| Disk I/O | `disk_buffer_pool.cpp`：`load_page`、`write_page` | `pread/pwrite` 与 Page | OS I/O Trace |
| Record/RID | `storage/record/record.h`、`record_manager.h/.cpp` | 字节记录/RID ↔ record page slot | Record/Page 布局展示 |
| Record Scan | `heap_record_scanner.cpp` | RecordPage iterator → Record 流 | SeqScan Trace |
| Table | `storage/table/table.cpp`、`heap_table_engine.cpp` | Value → Record；扫描/写入/索引维护 | 表级统计与访问 Trace |
| Metadata | `table_meta.*`、`field/field_meta.*`、`index/index_meta.*` | schema ↔ JSON metadata | Catalog/Symbol Table 可视化 |
| Index | `storage/index/bplus_tree.*`、`bplus_tree_index.*` | key/RID ↔ B+Tree Page | B+Tree Visualizer、Index Trace |
| Transaction | `storage/trx/trx.h`、`vacuous_trx.*`、`mvcc_trx.*` | record operation → visibility/commit/rollback | MVCC timeline |
| Log/Recovery | `storage/clog/log_handler.*`、`disk_log_handler.*`、`integrated_log_replayer.*` | log entry ↔ WAL files/replay | WAL/Recovery Visualizer |
| Database | `storage/db/db.*` | 数据库目录 → tables、buffer manager、trx kit、log handler | 生命周期和恢复入口 |

Heap SELECT 的关键落盘链为：

```text
TableScanPhysicalOperator::open
  -> Table::get_record_scanner
  -> HeapTableEngine::get_record_scanner
  -> HeapRecordScanner / RecordPageIterator
  -> RecordPageHandler
  -> DiskBufferPool::get_this_page
  -> BPFrameManager::get/alloc
  -> DiskBufferPool::load_page
  -> pread(file descriptor, Page)
```

脏页刷新方向相反：`Frame` → `DiskBufferPool::flush_page_internal/write_page` → Double Write Buffer（disk durability 模式）/`pwrite` → 文件。

## 6. 一条 SELECT 的完整执行链

```sql
SELECT name
FROM student
WHERE id = 1;
```

1. `net/sql_task_handler.cpp` 的 `SqlTaskHandler::handle_sql` 顺序调用 Parse、Resolve、Optimize、Execute Stage。
2. `parser/lex_sql.l` 产生 token，`parser/yacc_sql.y` 的 `select_stmt/where/condition` 产生式构造 `SelectSqlNode`，`parse_stage.cpp` 保存 `ParsedSqlNode`。
3. `resolve_stage.cpp` 调用 `Stmt::create_stmt`；`select_stmt.cpp` 查找 `student`，`expression_binder.cpp` 把 `name` 绑定为字段；`filter_stmt.cpp` 解析 `id = 1`。
4. `logical_plan_generator.cpp` 构造 `Project(Predicate(TableGet(student)))`。重写器可把条件下推到 `TableGet`。
5. `physical_plan_generator.cpp` 查找 `id` 上可用索引；没有索引时生成 `ProjectPhysicalOperator(TableScanPhysicalOperator)`，有等值索引时可生成 `IndexScanPhysicalOperator`。
6. `execute_stage.cpp` 把物理树放入 `SqlResult`。Communicator 输出结果时调用 `SqlResult::open/next_tuple/close`。
7. `table_scan_physical_operator.cpp` 通过 `Table::get_record_scanner` 获取 Record，并对下推谓词求值；Project 返回 `name`。
8. Heap 路径进入 `heap_table_engine.cpp`、`heap_record_scanner.cpp`、`record_manager.cpp`。
9. Record Page 通过 `DiskBufferPool::get_this_page` 映射到 `Frame`；缓存未命中时 `load_page` 使用 `pread` 从表 `.data` 文件读取 `Page`。

## 7. CREATE / INSERT / DELETE 调用入口

- CREATE TABLE：`yacc_sql.y` → `CreateTableStmt::create` → `CommandExecutor` → `CreateTableExecutor::execute` → `Db::create_table` → `Table::create` → `HeapTableEngine`/`BufferPoolManager::create_file`。
- INSERT：`yacc_sql.y` → `InsertStmt::create` → `LogicalPlanGenerator` → `InsertPhysicalOperator::open` → `Trx::insert_record` → `Table::insert_record_with_trx` → `HeapTableEngine::insert_record` → `RecordFileHandler::insert_record`，随后维护索引并标记页为 dirty。
- DELETE：`yacc_sql.y` → `DeleteStmt::create`/`FilterStmt` → `DeleteLogicalOperator` → `DeletePhysicalOperator::open`；先扫描收集目标 Record，再由 `Trx::delete_record` → `Table` → `HeapTableEngine` 删除索引项和 Record slot。

## 8. 开发环境

推荐 Windows 11 + WSL2 Ubuntu 22.04 LTS，或原生 Ubuntu。仓库约束来自顶层 CMake 与上游构建文档：

- GCC 11+ 或 Clang 14+，且支持 C++20。
- CMake 3.13+。
- Flex 2.5+、Bison 3.7+。
- Make、Git；GDB 用于调试。

本次实际验证环境为 Ubuntu 24.04.4 LTS、GCC/G++ 13.3.0、CMake 3.28.3、Flex 2.6.4、Bison 3.8.2、GDB 15.1。版本只记录本次结果，不代表必须完全一致。

Ubuntu/WSL 通常可安装：

```bash
sudo apt update
sudo apt install -y build-essential cmake flex bison gdb git
```

## 9. 构建

本次实际执行并成功的命令：

```bash
./build.sh init
./build.sh debug --make -j4
```

产物位于 `build_debug/bin/observer`、`build_debug/bin/obclient` 和 `build_debug/lib/libobserver.a`。`build` 是脚本创建的指向 `build_debug` 的符号链接。首次构建必须初始化 submodule 和第三方库。若 CMake 报找不到 FLEX/BISON，应先安装环境依赖，不要修改 Parser 生成流程。

## 10. 启动

CLI 模式最适合单步调试。observer 以当前工作目录为根创建 `miniob/db/sys`，因此建议使用独立运行目录：

```bash
mkdir -p /tmp/minidb-baseline
cd /tmp/minidb-baseline
/path/to/miniob/build_debug/bin/observer \
  -f /path/to/miniob/etc/observer.ini \
  -P cli
```

本次验证使用同样的 CLI 调用方式和独立 `/tmp` 数据目录。网络模式仍保留，参见 `docs/docs/how_to_run.md`。

## 11. 基础 SQL Demo

以下语句已在本次构建中实际通过：

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

关键结果：首次全表查询得到 Alice、Bob；WHERE 查询得到 Alice；删除后再次查询只得到 Bob。退出 observer 并在相同目录重启后，Bob 仍存在，持久化验证通过。

## 12. Debug 入门

```bash
./build.sh debug --make -j4
gdb --args build_debug/bin/observer -f etc/observer.ini -P cli
```

若从仓库根目录启动，数据会落在仓库下的 `miniob/`；更推荐先进入独立运行目录并使用绝对路径启动。

第一次追 SELECT 建议按顺序在这些位置下断点：

1. `SqlTaskHandler::handle_sql`（`net/sql_task_handler.cpp`）
2. `ParseStage::handle_request`（`parser/parse_stage.cpp`）
3. `ResolveStage::handle_request`、`SelectStmt::create`
4. `LogicalPlanGenerator::create_plan(SelectStmt *)`
5. `OptimizeStage::handle_request`
6. `PhysicalPlanGenerator::create_plan(TableGetLogicalOperator &)`
7. `SqlResult::open/next_tuple`
8. `TableScanPhysicalOperator::open/next`
9. `RecordFileHandler::get_record` 或 Record Page iterator
10. `DiskBufferPool::get_this_page/load_page`

可输入 `EXPLAIN SELECT name FROM student WHERE id = 1;` 观察当前物理计划。

## 13. 后续扩展点

| 功能 | 建议挂载位置 |
| --- | --- |
| Compiler/Token Trace | `lex_sql.l`、`parse_stage.cpp`、`SQLStageEvent` |
| AST/Parsed SQL Visualizer | `ParsedSqlNode`/`parse_defs.h` 只读 visitor |
| Logical Plan Visualizer | `LogicalOperator`、`LogicalPlanGenerator` 输出边界 |
| EXPLAIN 增强 | `explain_physical_operator.cpp`、`OptimizerUtils` |
| Buffer Pool/Page Trace | `DiskBufferPool::get_this_page/allocate_page/unpin_page/flush_page` |
| LRU/FIFO 对比 | `BPFrameManager`；先抽象策略，避免改 Page 格式 |
| OS I/O Trace | `DiskBufferPool::load_page/write_page` 的 `pread/pwrite` 边界 |
| B+Tree Visualizer | `BplusTreeHandler`、node handler 与 scanner 的只读快照接口 |
| WAL/Recovery | `LogHandler`、`DiskLogHandler`、`IntegratedLogReplayer` |
| MVCC | `Trx/TrxKit` 接口与 `MvccTrx`，不要绕开 Table/Record 日志链 |
| Cost Optimizer | `optimizer/cascade/`、`Catalog::TableStats` |

详细架构、语法事实、源码优先级和本次验收记录见：

- `docs/course/architecture.md`
- `docs/course/grammar.md`
- `docs/course/source_map.md`
- `docs/course/baseline.md`

## License and upstream

本项目派生自 OceanBase MiniOB。课程仓库已按项目要求移除根目录治理与许可文本文件；上游许可仍适用于派生代码，发布或再分发前应核对 [MiniOB 上游仓库](https://github.com/oceanbase/miniob) 的许可与声明。
