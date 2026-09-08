# MiniDB Source Map

优先级：P0 必须理解；P1 重要；P2 后续高级功能；P3 暂时忽略。表中 “Called By / Calls” 描述主要调用关系，不试图列出每个 include 或辅助函数。

## 1. 请求入口与跨阶段对象

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/main.cpp` | 参数解析、初始化、启动 Server | OS process | `init`, `Server::serve` | OS/Infrastructure | P1 | 可加启动配置，慎改生命周期 |
| `src/observer/net/sql_task_handler.cpp` | 一条 SQL 的 Stage 总编排 | ThreadHandler/Communicator | QueryCache, Parse, Resolve, Optimize, Execute | DB/Compiler | P0 | 最适合加跨阶段 Trace id |
| `src/observer/event/sql_event.h` | 保存 SQL、ParsedSqlNode、Stmt、PhysicalOperator | SQL task/stages | ownership setters/getters | Compiler/DB | P0 | 可加只读诊断字段，慎改所有权 |
| `src/observer/session/session.cpp` | 当前数据库、事务与执行模式 | Communicator/SqlResult | Db, TrxKit, Trx | DB | P1 | 可加会话级开关 |
| `src/observer/net/cli_communicator.cpp` | 直接终端读写 SQL | Server | SqlResult | Infrastructure | P1 | 可扩展 Debug 输出，保持协议 |

## 2. SQL Frontend 与 Semantic

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/sql/parser/lex_sql.l` | 正则识别 token | generated parser | token return, `yylval` | Compiler | P0 | Token Trace/新词法；不改生成文件 |
| `src/observer/sql/parser/yacc_sql.y` | 语法与 Parsed SQL 构造 | `sql_parse` | `ParsedSqlNode`, Expression constructors | Compiler | P0 | 新语法/AST；必须补 Parser 测试 |
| `src/observer/sql/parser/parse_defs.h` | Parsed SQL node、命令枚举、条件结构 | Parser/Stmt | Value/Expression containers | Compiler | P0 | AST dump；慎改 ABI/所有权 |
| `src/observer/sql/parser/parse.cpp` | `parse()` 包装器 | ParseStage/parser tests | generated `sql_parse` | Compiler | P0 | 可加解析级观测 |
| `src/observer/sql/parser/parse_stage.cpp` | 语法错误处理，节点写入事件 | SqlTaskHandler | `parse` | Compiler | P0 | Compiler Trace 接入点 |
| `src/observer/sql/parser/resolve_stage.cpp` | 取当前 Db 并创建 Stmt | SqlTaskHandler | `Stmt::create_stmt` | Compiler/DB | P0 | Semantic Trace 接入点 |
| `src/observer/sql/stmt/stmt.cpp` | Parsed command 到具体 Stmt 分派 | ResolveStage | `SelectStmt::create` 等 | Compiler/DB | P0 | 增加命令类型时修改 |
| `src/observer/sql/stmt/select_stmt.cpp` | 表解析、投影/Group By 绑定、Filter 创建 | `Stmt::create_stmt` | Db, ExpressionBinder, FilterStmt | Compiler/DB | P0 | 类型诊断/Resolver 扩展 |
| `src/observer/sql/stmt/filter_stmt.cpp` | WHERE 字段/值绑定为 FilterUnit | Select/Delete/Update Stmt | Db, TableMeta, Field | Compiler/DB | P0 | 布尔表达式升级时重点修改 |
| `src/observer/sql/parser/expression_binder.cpp` | `*` 展开、字段和聚合名称绑定 | SelectStmt | BinderContext, TableMeta | Compiler | P0 | Symbol Table/类型 Trace |
| `src/observer/sql/expr/expression.h/.cpp` | 表达式节点、类型和求值 | Parser/Binder/Operators | Tuple, Value, DataType | Compiler/DB | P0 | AST/表达式可视化、函数扩展 |
| `src/observer/common/value.h/.cpp` | SQL 标量值与转换 | Parser/Expr/Table | DataType | Compiler/DB | P1 | 新类型时修改 |

## 3. Logical Plan、Optimizer 与 Physical Plan

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/sql/optimizer/optimize_stage.cpp` | 生成逻辑树、rewrite、生成物理树 | SqlTaskHandler | LogicalPlanGenerator, Rewriter, PhysicalPlanGenerator/Cascade | DB | P0 | Plan Trace/optimizer switch |
| `src/observer/sql/optimizer/logical_plan_generator.cpp` | Stmt → LogicalOperator tree | OptimizeStage | TableGet/Predicate/Project/Insert/Delete logical ops | Compiler/DB | P0 | Logical Plan Visualizer 接入点 |
| `src/observer/sql/operator/logical_operator.h/.cpp` | 逻辑算子基类与树 | Plan generator/rewriter | child/expression management | DB | P0 | 可加稳定 dump visitor |
| `src/observer/sql/optimizer/rewriter.cpp` | 循环/顺序应用 rewrite rules | OptimizeStage | Predicate rewrite/pushdown rules | DB | P1 | Rule Trace/规则开关 |
| `src/observer/sql/optimizer/predicate_pushdown_rewriter.cpp` | 谓词向扫描/连接下推 | Rewriter | LogicalOperator/Expression | DB | P1 | 优化规则实验 |
| `src/observer/sql/optimizer/physical_plan_generator.cpp` | LogicalOperator → PhysicalOperator | OptimizeStage | scan/join/project/DML physical ops | DB | P0 | SeqScan/IndexScan 决策、Plan dump |
| `src/observer/sql/optimizer/cascade/optimizer.cpp` | 可选 Cascade 优化入口 | OptimizeStage when enabled | memo/tasks/rules/cost | DB | P2 | Cost Optimizer |
| `src/observer/sql/optimizer/cascade/cost_model.cpp` | 物理实现代价估计 | Cascade optimizer | statistics/properties | DB | P2 | 代价模型实验 |
| `src/observer/catalog/catalog.cpp` | 内存表统计 Catalog singleton | analyze/optimizer | TableStats map | Compiler/DB | P1 | Catalog 持久化/统计信息 |

## 4. Execution

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/sql/executor/execute_stage.cpp` | 物理计划转交 SqlResult，或执行 command | SqlTaskHandler | SqlResult, CommandExecutor | DB | P0 | Execution Trace 接入点 |
| `src/observer/sql/executor/sql_result.cpp` | 启停事务并 pull tuple/chunk | Communicator | PhysicalOperator::open/next/close, Trx | DB | P0 | 运行统计/结果计时 |
| `src/observer/sql/executor/command_executor.cpp` | DDL/管理命令分派 | ExecuteStage | specific executors | DB | P1 | 新 command executor |
| `src/observer/sql/executor/create_table_executor.cpp` | CREATE TABLE → Db | CommandExecutor | `Db::create_table` | DB | P0 | DDL Trace |
| `src/observer/sql/operator/physical_operator.h/.cpp` | pull-based 物理算子接口 | SqlResult/parent operator | child operators | DB | P0 | 统一 metrics，慎改接口 |
| `src/observer/sql/operator/table_scan_physical_operator.cpp` | Heap/LSM record scan 与下推过滤 | parent/SqlResult | Table, RecordScanner, Expression | DB | P0 | SeqScan/Page Trace |
| `src/observer/sql/operator/index_scan_physical_operator.cpp` | 索引范围扫描并回表 | parent/SqlResult | IndexScanner, Table, Trx | DB | P2 | Index/Page Trace |
| `src/observer/sql/operator/predicate_physical_operator.cpp` | 对 child tuple 求布尔表达式 | parent/SqlResult | Expression, child operator | DB | P0 | Predicate metrics |
| `src/observer/sql/operator/project_physical_operator.cpp` | 投影表达式组成结果 tuple | SqlResult | ExpressionTuple/child | DB | P0 | 输出表达式 Trace |
| `src/observer/sql/operator/insert_physical_operator.cpp` | 构造 Record 并经 Trx 插入 | SqlResult | Table::make_record, Trx::insert_record | DB | P0 | 写入 Trace |
| `src/observer/sql/operator/delete_physical_operator.cpp` | 扫描、收集并经 Trx 删除 | SqlResult | child operator, Trx::delete_record | DB | P0 | 删除/锁 Trace |
| `src/observer/sql/operator/explain_physical_operator.cpp` | 输出物理算子树 | SqlResult | operator name/param | DB | P1 | EXPLAIN 增强 |

## 5. Metadata、Table 与 Record

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/storage/db/db.cpp` | DB 生命周期、表集合、BPM、TrxKit、Log、Recovery | DefaultHandler/Session | Table, BufferPoolManager, LogHandler | DB/OS | P0 | DB 级观测；慎改恢复顺序 |
| `src/observer/storage/table/table_meta.h/.cpp` | 表/字段/索引/存储格式元数据 | Db/Table/Binder | FieldMeta, IndexMeta, JSON | Compiler/DB | P0 | Schema Visualizer |
| `src/observer/storage/field/field_meta.h/.cpp` | 字段名、类型、offset、长度 | TableMeta/Binder/Record | AttrType/JSON | Compiler/DB | P0 | 新数据类型 |
| `src/observer/storage/index/index_meta.h/.cpp` | 索引名与字段元数据 | TableMeta/Index | FieldMeta/JSON | DB | P1 | 多列索引时扩展 |
| `src/observer/storage/table/table.cpp` | SQL/Trx 与 TableEngine 门面；Value→Record | Operators/Trx/Db | TableMeta, TableEngine | DB | P0 | 表级 Trace，慎改接口 |
| `src/observer/storage/table/table_engine.h` | Heap/LSM table engine 抽象 | Table | Record/Index scanner APIs | DB | P1 | 新存储引擎扩展点 |
| `src/observer/storage/table/heap_table_engine.cpp` | RecordFileHandler 与 B+Tree 协调 | Table | Record manager, Buffer pool, Index | DB/OS | P0 | 表/索引访问 Trace |
| `src/observer/storage/record/record.h` | RID、Record 数据与所有权 | Scanner/Table/Trx/Index | page/slot identifiers | DB | P0 | Record dump，慎改布局 |
| `src/observer/storage/record/record_manager.h/.cpp` | RecordPage layout、slot bitmap、记录 CRUD | HeapTableEngine/scanner | DiskBufferPool, Frame, Log | DB/OS | P0 | Page layout visualizer/Record Trace |
| `src/observer/storage/record/heap_record_scanner.cpp` | 顺序遍历 RecordPage/slot | TableScan/HeapTableEngine | RecordPageIterator, Trx visibility | DB | P0 | Scan Trace |
| `src/observer/storage/common/meta_util.cpp` | 元数据/数据/索引文件命名 | Db/Table/Index | filesystem paths | OS | P1 | 保持文件兼容，慎改 |

## 6. Page、Buffer 与 Disk

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/storage/buffer/page.h` | 固定 8 KiB Page 布局 | Frame/DiskBufferPool | LSN/checksum/data | OS/DB | P0 | 可视化可读；不可随意改格式 |
| `src/observer/storage/buffer/frame.h/.cpp` | 内存 Page、dirty、pin、latch、访问时间 | BPFrameManager/Record/B+Tree | Page, mutex/session debug id | OS/DB | P0 | Frame/Pin Trace |
| `src/observer/storage/buffer/disk_buffer_pool.h/.cpp` | Frame manager、文件页管理、LRU/FIFO、加载/刷新 | Record/B+Tree/Db | lseek/read/write, DoubleWrite, Log | OS/DB | P0 | Buffer Trace、替换策略、I/O Trace |
| `src/observer/storage/buffer/buffer_pool_stats.h/.cpp` | 缓存命中、I/O、淘汰与刷新统计 | DiskBufferPool/BPFrameManager | atomic counters | OS | P0 | 实验统计输出 |
| `src/observer/storage/buffer/double_write_buffer.cpp` | 先写 double-write 文件再落目标页 | DiskBufferPool/Db | file I/O | OS/DB | P2 | 崩溃恢复实验 |
| `src/common/lang/lru_cache.h` | 通用有序缓存，支持触碰/不触碰顺序查询 | BPFrameManager/common/oblsm users | containers | OS | P1 | LRU/FIFO 顺序基础 |
| `src/observer/storage/persist/persist.cpp` | 通用文件持久化辅助 | tests/consumers | open/read/write | OS | P2 | I/O 实验 |

关键路径：`RecordPageHandler` pin Page → `DiskBufferPool::get_this_page` → `BPFrameManager::get/alloc` → miss 时 `load_page`/`lseek + read`；更新后 `Frame::mark_dirty`，最终 `flush_page`/`write_page`/`lseek + write`。

## 7. Index、Transaction 与 Log（Advanced / Reserved）

| File | Responsibility | Called By | Calls | Course | Priority | Can Modify Later? |
| --- | --- | --- | --- | --- | --- | --- |
| `src/observer/storage/index/index.h/.cpp` | Index/IndexScanner 抽象 | TableEngine/IndexScan | RID, metadata | DB | P2 | 多索引/新索引接口 |
| `src/observer/storage/index/bplus_tree.h/.cpp` | B+Tree 页节点、CRUD、scanner | BplusTreeIndex/tests | DiskBufferPool, latch/log | DB/OS | P2 | B+Tree Visualizer，慎改并发协议 |
| `src/observer/storage/index/bplus_tree_index.cpp` | Table Index 到 B+TreeHandler 适配 | HeapTableEngine/IndexScan | BplusTreeHandler | DB | P2 | Index Trace 接入点 |
| `src/observer/storage/index/bplus_tree_log.cpp` | B+Tree WAL 与 replay | B+Tree/IntegratedLogReplayer | LogHandler, BufferPool | DB/OS | P2 | Recovery 展示 |
| `src/observer/storage/trx/trx.h/.cpp` | Trx/TrxKit 抽象与工厂 | Session/SqlResult/Operators/Db | Vacuous/MVCC/LSM implementations | DB | P2 | 事务模型扩展主接口 |
| `src/observer/storage/trx/vacuous_trx.cpp` | 默认无并发控制事务 | TrxKit | Table CRUD | DB | P1 | 基础路径，少改 |
| `src/observer/storage/trx/mvcc_trx.cpp` | MVCC 可见性、提交回滚 | optional `-t mvcc` | Table/Log/Record | DB | P2 | MVCC 实验；必须并发回归 |
| `src/observer/storage/clog/log_handler.h/.cpp` | WAL append/wait/replay 接口与工厂 | Db/Record/B+Tree/Trx | concrete handler | DB/OS | P2 | WAL metrics/格式版本化 |
| `src/observer/storage/clog/disk_log_handler.cpp` | 日志 buffer、后台落盘、文件迭代 | LogHandler factory | LogBuffer/LogFile | OS/DB | P2 | WAL/flush Trace |
| `src/observer/storage/clog/integrated_log_replayer.cpp` | 按模块分发恢复日志 | `Db::recover` | Buffer/Record/B+Tree/Trx replayers | DB/OS | P2 | Recovery timeline |
| `tools/clog_dump.cpp` | 离线查看 CLog | developer | LogFile/LogEntry | DB/Tools | P2 | WAL 可观测工具 |

## 8. 测试、后端与外围

| Path | Responsibility | Course | Priority | Baseline decision |
| --- | --- | --- | --- | --- |
| `unittest/observer/` | Parser、Buffer、Record、Index、Log、MVCC 单测 | All | P1 | 保留 |
| `unittest/common/` | 公共容器、日志、线程等单测 | Infrastructure | P1 | 保留 |
| `unittest/oblsm/` | LSM 后端回归 | Advanced | P2 | 保留 |
| `test/integration_test/` | observer 端到端测试框架 | DB | P1 | 保留 |
| `benchmark/` | B+Tree/Record/执行/并发性能 | DB/OS | P2 | 保留，默认不构建 |
| `src/oblsm/` | LSM storage backend | DB/OS | P2/P3 | 保留；observer 当前直接链接 |
| `src/obclient/` | plain 协议客户端 | Infrastructure | P3 | 保留 |
| `docker/`, `.devcontainer/` | 开发环境复现 | Infrastructure | P3 | 保留 |
| `src/cpplings/` | 独立 C++ 练习 | Peripheral | P3 | v0.1 删除 |
| `src/memtracer/` | 独立 preload 内存监控 | Peripheral | P3 | v0.1 删除 |

## 9. 推荐断点路径

追 SELECT：`SqlTaskHandler::handle_sql` → `ParseStage::handle_request` → `ResolveStage::handle_request` → `SelectStmt::create` → `LogicalPlanGenerator::create_plan` → `OptimizeStage::rewrite` → `PhysicalPlanGenerator::create_plan` → `SqlResult::open/next_tuple` → `TableScanPhysicalOperator::next` → `DiskBufferPool::get_this_page/load_page`。

追 INSERT：前半段相同 → `InsertPhysicalOperator::open` → `Trx::insert_record` → `Table::insert_record_with_trx` → `HeapTableEngine::insert_record` → `RecordFileHandler::insert_record` → `RecordPageHandler` → `DiskBufferPool`。
