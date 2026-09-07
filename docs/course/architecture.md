# MiniDB Architecture

MiniDB Baseline v0.1 保留 MiniOB 的分层架构，把三门课程放在同一条真实执行链中观察，而不是把数据库拆成互不相干的示例。

## 1. 总体结构

```text
                         Compiler Layer
 SQL text
    |
    v
 Lexer (Flex: lex_sql.l) -------> Token
    |
    v
 Parser (Bison: yacc_sql.y) ----> ParsedSqlNode / Expression
    |
    v
 Resolver / Semantic -----------> Stmt + bound Field/Table
    |
    v
 LogicalPlanGenerator ----------> LogicalOperator tree
    |                                      |
    |                              rewrite / rules
    |                                      |
    +--------------------------------------+
                                           v
                         Database Layer
 PhysicalPlanGenerator ----------> PhysicalOperator tree
                                           |
                                           v
                                      SqlResult
                                           |
                                open / next / close
                                           |
                                           v
                              TableScan / IndexScan /
                              Predicate / Project /
                              Insert / Delete
                                           |
                                           v
                           Table / Trx / Index / Record
                                           |
                                           v
                      Storage and Operating-System Layer
                              RecordPage / RID / slot
                                           |
                                           v
                         DiskBufferPool / BPFrameManager
                                           |
                                  hit      |      miss
                              in-memory Frame      |
                                           |      v
                                           +-- Page (8 KiB)
                                                  |
                                             pread/pwrite
                                                  |
                                                  v
                                           data/index files
```

横切能力：Session/Event 贯穿请求；Catalog/Metadata 参与语义绑定与优化；Transaction、CLog、Double Write Buffer 和 Recovery 贯穿写入与持久化。

## 2. Compiler Layer

### Lexer 与 Parser

- 目录：`src/observer/sql/parser/`
- 核心：`lex_sql.l`、`yacc_sql.y`、`parse.cpp`、`parse_defs.h`、`parse_stage.cpp`
- 输入：SQL 字符串。
- 输出：`ParsedSqlNode`。它是当前 MiniOB 的 AST-like Parsed SQL 表示，不等同于一个重新设计的通用 AST。

### Semantic / Resolver

- 核心：`resolve_stage.cpp`、`stmt/stmt.cpp`、各类 `*_stmt.cpp`、`expression_binder.cpp`、`filter_stmt.cpp`
- 输入：Parsed SQL + 当前 `Db`。
- 输出：`Stmt`，其中表、字段与表达式已绑定到 Catalog/Metadata 对象。
- 编译原理对应：名称解析、符号表查询、基础类型/结构检查。

### Plan as IR

- 核心：`logical_plan_generator.cpp` 与 `operator/*_logical_operator.*`
- 输出：逻辑算子树，可视为查询编译器的中间表示。
- 重写：`rewriter.cpp` 和各类 rewrite rule 对逻辑树做等价变换。

## 3. Database Layer

### Optimizer 与 Physical Plan

`OptimizeStage` 先生成逻辑计划、循环应用 rewrite，再选择普通 `PhysicalPlanGenerator` 或可选 Cascade Optimizer。普通生成器会递归选择 TableScan、IndexScan、Join、Project、Insert、Delete 等物理算子。

### Execution

`ExecuteStage` 不直接把 SELECT 全部算完，而是把物理树交给 `SqlResult`。输出响应时，`SqlResult::open/next_tuple/close` 驱动 pull-based iterator。DDL 等没有物理计划的命令由 `CommandExecutor` 分派。

### Catalog / Metadata

- `Catalog` 当前主要保存非持久化表统计信息。
- `Db` 管理表、BufferPoolManager、TrxKit、LogHandler 和恢复。
- `TableMeta`、`FieldMeta`、`IndexMeta` 描述持久化 schema。

它们共同承担数据库 Data Dictionary，也对应编译器的 Symbol Table。

## 4. Storage / OS Layer

### Table 与 Record

`Table` 是 SQL/事务与具体 TableEngine 之间的门面。默认 Heap 路径由 `HeapTableEngine` 使用 `RecordFileHandler` 管理记录，并同步维护 B+Tree 索引。`RID` 由页号和 slot 号定位记录。

### Page、Frame 与 Buffer Pool

- `Page` 是固定 8 KiB 的持久化单位，包含 LSN、checksum 和 data。
- `Frame` 是 Page 的内存容器，附带 frame id、dirty、pin count、latch 和访问时间。
- `BPFrameManager` 管理所有缓存 Frame，并按访问时间选择可淘汰页（当前实现为 LRU 思路）。
- `DiskBufferPool` 对应一个磁盘文件，负责页分配、pin/unpin、加载和刷新。
- `BufferPoolManager` 管理多个 DiskBufferPool。

### Disk / File

`DiskBufferPool::load_page`/`write_page` 最终使用 `pread`/`pwrite` 访问表或索引文件。元数据使用文件流/系统调用写入；CLog 使用独立日志文件。这里是 OS I/O Trace 最自然的边界。

### Transaction、WAL 与 Recovery

`Trx`/`TrxKit` 抽象 Vacuous、MVCC 和 LSM 事务模型。`LogHandler` 抽象日志追加、刷盘与 replay；`IntegratedLogReplayer` 把 Buffer、Record、B+Tree 和事务日志分发给对应 replayer。`Db::init` 在打开表后执行 double-write recovery 和 redo replay。

这些能力在 v0.1 中原样保留为 Advanced/Reserved，不修改其协议和文件格式。

## 5. 请求控制流

```text
Communicator::read_event
  -> SqlTaskHandler::handle_event
  -> QueryCacheStage
  -> ParseStage
  -> ResolveStage
  -> OptimizeStage
  -> ExecuteStage
  -> Communicator::write_result
       -> SqlResult::open
       -> SqlResult::next_tuple / next_chunk
       -> SqlResult::close
```

承载对象是 `SQLStageEvent`，它依次保存原始 SQL、`ParsedSqlNode`、`Stmt *` 和 `unique_ptr<PhysicalOperator>`。后续加 Trace 时应避免改变这些对象的所有权和生命周期。

## 6. 模块分类

| 类别 | 模块 | 策略 |
| --- | --- | --- |
| A Core | Parser、Stmt、Expr、Plan、Executor、Db/Table/Record、Page/Buffer | 必须保留；修改后做完整回归 |
| B Advanced Reserved | B+Tree、MVCC、CLog/Recovery、Cascade Optimizer、PAX/LSM、向量化 | 保留源码和构建；按实验逐步启用 |
| C Infrastructure | main、Session、Event、Net、Common、obclient | 保留；仅做必要维护 |
| D Peripheral | Cpplings、MemTracer | 已经依赖验证并从 v0.1 删除 |

benchmark、Docker 与 Dev Container 不属于请求执行链，但分别对后续性能实验和环境复现有价值，因此保留。

## 7. 扩展纪律

1. 优先在 Stage 边界和现有抽象接口旁增加只读观测，不把 Trace 逻辑散落进算法主体。
2. AST/Plan 可视化先实现序列化或 visitor，不改变节点所有权。
3. Buffer/Page Trace 先记录 page id、命中、pin、dirty 与 I/O，再考虑替换算法插件化。
4. WAL/MVCC 实验沿用 `Trx`、`LogHandler`、`LogReplayer` 接口，不新造旁路文件格式。
5. 每次触碰 Parser、Record、Buffer、B+Tree 或 Recovery，都要运行对应 unittest 和端到端 SQL 持久化测试。
