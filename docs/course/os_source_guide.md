# CSUDB OS / Storage 源码职责与阅读指南

本文回答三个问题：

1. CSUDB 的操作系统部分位于哪里；
2. 每个目录、文件和核心类分别负责什么；
3. 一条记录如何经过 Page、Frame、Buffer Pool 和文件 I/O 到达磁盘。

实现源码统一位于 `src/observer/storage/os/`。这里不是独立于数据库的第二套存储系统，而是数据库存储引擎使用的真实底层实现。

## 1. 总体边界

```text
SQL Executor                         数据库执行层
      ↓
Table / Index                       数据库对象层
      ↓
RecordFileHandler                   记录文件层
      ↓
RecordPageHandler + RID             记录到页的映射层
      ↓
DiskBufferPool                      分页文件与缓存协调层
      ↓
BPFrameManager                      内存 Frame 管理层
      ├── ReplacementPolicy         LRU / FIFO / CLOCK
      └── BufferPool diagnostics    Stats / Snapshot / Trace
      ↓
Frame -> Page                       内存页与 8 KiB 数据页
      ↓
DoubleWriteBuffer                   页写入保护
      ↓
PageIOBackend                       legacy / positional
      ↓
Linux VFS / file system             操作系统边界
      ↓
Disk / SSD                          物理设备
```

课程中的“操作系统部分”主要从 `RecordPageHandler` 与 `DiskBufferPool` 的交界处开始，向下延伸到文件 I/O。`Table`、`Record` 和 `RID` 属于数据库与 OS 存储之间的桥梁。

## 2. 目录职责

```text
src/observer/storage/os/
├── page/          Page 数据格式、Frame 内存控制块
├── buffer/        分页文件、缓存、页分配/释放、刷盘
├── replacement/   LRU、FIFO、CLOCK 替换策略
├── io/            可替换的目标分页文件 I/O 后端
├── diagnostics/   统计、快照、Trace、Buffer Pool 日志
├── record/        指向数据库 RecordPage/RID 实现的学习入口
├── os_storage.h   OS 模块聚合头文件
└── README.md      目录级快速阅读说明
```

### `page/`：页和页帧

这一层回答：“磁盘页在内存中是什么样子，系统如何知道它能否被淘汰？”

- `Page` 是固定 8192 字节的持久化数据块；
- `Frame` 是 Page 被读入内存后的管理对象；
- pin count 防止正在使用的 Frame 被淘汰；
- dirty 标记表示内存内容比磁盘新；
- latch 保护并发读写；
- LSN 和 checksum 服务于日志恢复与完整性检查。

### `buffer/`：Buffer Pool 主体

这一层回答：“请求某页时，是命中内存、读取磁盘，还是淘汰旧页？”

- `BufferPoolManager` 管理所有已打开的分页文件；
- `DiskBufferPool` 管理一个具体分页文件；
- `BPFrameManager` 管理多个文件共享的内存 Frame；
- `DoubleWriteBuffer` 在目标页写回前提供额外保护；
- 页的分配、释放、读取、pin/unpin 和 flush 都从这里协调。

### `replacement/`：缓存替换策略

这一层只决定“哪个可替换 Frame 应成为 victim”，不负责磁盘 I/O，也不拥有 Frame。

- LRU：淘汰最久未访问的可替换 Frame；
- FIFO：淘汰最早进入缓存的可替换 Frame，命中不改变顺序；
- CLOCK：使用 reference bit 和 clock hand 实现 Second Chance；
- 最终是否可替换仍由 `pin_count == 0` 决定。

### `io/`：Page 文件 I/O

这一层是 CSUDB 与 Linux 文件接口之间的稳定边界。

- legacy：`lseek + read/write`；
- positional：`pread/pwrite`；
- 两种后端都必须完整传输 8192 字节，处理 EINTR、短读写和 EOF；
- 后端只负责目标分页文件，不把 WAL 或 Double Write 文件 I/O 混入分页文件统计。

### `diagnostics/`：可观测与日志

这一层回答：“Buffer Pool 当前发生了什么，为什么没有可用 Frame？”

- Stats：命中、缺页、I/O、淘汰、pin、dirty 和延迟计数；
- Snapshot：把 Frame/Buffer Pool 状态复制成只读 DTO；
- Trace：记录 Page 生命周期事件；
- FlushReason：区分显式、淘汰和关闭刷盘；
- BufferPoolLog：页分配/回收的日志与重放，不等同于普通调试 Trace。

### `record/`：数据库与 OS 的连接点

此目录的 `record_page.h` 是学习入口，真实 Record 实现仍在 `src/observer/storage/record/`。它说明数据库记录如何通过 `RID(page_num, slot_num)` 落到 Page 中。

## 3. 逐文件源码地图

| 文件 | 核心对象 | 负责什么 | 上游调用者 | 主要下游 |
| --- | --- | --- | --- | --- |
| `os/page/page.h` | `Page` | 定义 8 KiB Page、LSN、checksum 和数据区 | `Frame`、I/O backend | 原始页数据 |
| `os/page/frame.h/.cpp` | `FrameId`、`Frame` | 在内存中包装 Page，维护 pin、dirty、latch、访问时间 | `BPFrameManager`、RecordPage、B+Tree | `Page`、Stats |
| `os/buffer/disk_buffer_pool.h/.cpp` | `BPFileHeader` | 保存分页文件 id、页数和分配 bitmap | `DiskBufferPool` | Header Page |
| 同上 | `BPFrameManager` | 查找、申请、回收 Frame，并调用替换策略选 victim | 所有 `DiskBufferPool` | ReplacementPolicy、Frame |
| 同上 | `DiskBufferPool` | 管理单个分页文件的页分配、读取、释放、pin/unpin、flush | Record、Index、Recovery | FrameManager、DoubleWrite、PageIOBackend |
| 同上 | `BufferPoolManager` | 创建/打开/关闭多个分页文件并共享 FrameManager | `Db`、`Table`、系统初始化 | DiskBufferPool |
| 同上 | `BufferPoolIterator` | 根据文件头 bitmap 遍历已分配 Page | Record/Chunk 扫描 | DiskBufferPool |
| `os/buffer/double_write_buffer.h/.cpp` | `DoubleWriteBuffer` | 定义写页保护接口 | DiskBufferPool | 实际或空实现 |
| 同上 | `DiskDoubleWriteBuffer` | 先写 Double Write 区，再写目标分页文件；支持恢复 | BufferPoolManager、DiskBufferPool | DiskBufferPool、文件 I/O |
| 同上 | `VacuousDoubleWriteBuffer` | 不启用实际 Double Write 时的兼容实现 | BufferPoolManager | 目标分页文件 |
| `os/replacement/replacement_policy.h/.cpp` | `ReplacementPolicy` | 定义 insert/access/pin/unpin/remove/victim 生命周期接口 | BPFrameManager | 策略内部元数据 |
| 同上 | LRU/FIFO/CLOCK 实现 | 保存各算法的顺序或 reference bit，选择 victim | ReplacementPolicy factory | `FrameId` |
| `os/io/page_io_backend.h/.cpp` | `PageIOBackend` | 定义 `read_page/write_page/sync` I/O 边界 | DiskBufferPool | Linux 文件调用 |
| 同上 | legacy backend | 使用 `lseek + readn/writen` 访问 Page | PageIOBackend factory | `common/io` |
| 同上 | positional backend | 使用 `preadn/pwriten` 按 offset 访问 Page | PageIOBackend factory | `common/io` |
| `os/diagnostics/buffer_pool_stats.h/.cpp` | `BufferPoolStats` | 原子记录 hit/miss、I/O、淘汰、pin、dirty、延迟 | Frame、DiskBufferPool、I/O 路径 | StatsSnapshot |
| `os/diagnostics/buffer_pool_diagnostics.h/.cpp` | `FrameSnapshot` | 对单个 Frame 做安全只读快照 | BPFrameManager | CLI/未来 GUI |
| 同上 | `BufferPoolSnapshot` | 汇总容量、使用量、策略、后端、Stats 和 Frame 列表 | BufferPoolManager、DatabaseService | `/buffer`、`/pages` |
| 同上 | `DirtyPageFlushResult` | 返回批量刷脏页的成功、跳过与失败数量 | flush API | 管理工具 |
| `os/diagnostics/buffer_pool_log.h/.cpp` | `BufferPoolLogHandler` | 记录页分配与回收日志、设置 LSN | DiskBufferPool | LogHandler/WAL |
| 同上 | `BufferPoolLogReplayer` | 恢复时重放页分配与回收 | Recovery | DiskBufferPool redo |
| `os/os_storage.h` | 聚合入口 | 让课程工具一次引入整个 OS 模块 | 实验工具 | 各窄接口头文件 |
| `storage/record/record.h` | `RID`、`Record` | 用 page/slot 标识记录并保存记录视图 | Executor、Table | RecordPage |
| `storage/record/record_manager.h/.cpp` | `RecordPageHandler` | 定义页头、slot bitmap 和记录读写 | RecordFileHandler | DiskBufferPool、Frame |
| 同上 | `RecordFileHandler` | 跨多个 Page 插入、删除、读取记录 | Table | RecordPageHandler |
| `storage/record/heap_record_scanner.*` | `RecordFileScanner` | 逐页逐 slot 扫描 Heap 记录 | Table Scan Operator | RecordPageHandler |
| `common/io/io.h/.cpp` | `readn/writen/preadn/pwriten` | 可靠处理短读写和中断 | PageIOBackend 及其他系统模块 | POSIX I/O |

路径表中的 `os/...` 均相对于 `src/observer/storage/`。

## 4. 四个容易混淆的概念

| 概念 | 所在位置 | 生命周期 | 是否直接持久化 |
| --- | --- | --- | --- |
| Page | `page/page.h` | 磁盘格式级 | 是，完整 8192 字节 |
| Frame | `page/frame.*` | 进程内缓存级 | 否，内部的 Page 会写盘 |
| RecordPage | `storage/record/record_manager.*` | 数据库记录组织级 | 布局存放在 Page 的 data 中 |
| RID | `storage/record/record.h` | 记录定位标识 | 值为 `(page_num, slot_num)` |

最重要的区别：Page 是数据块，Frame 是装载这个数据块的内存控制结构。RecordPage 则解释 Page 的 data 区应该怎样存放记录。

## 5. 一次 SELECT 如何读到磁盘页

以表扫描为例：

```text
TableScanPhysicalOperator
  -> Table::get_record_scanner
  -> RecordFileScanner / HeapRecordScanner
  -> RecordPageHandler::init(page_num)
  -> DiskBufferPool::get_this_page(page_num)
```

进入 Buffer Pool 后有两条分支。

### 缓存命中

```text
BPFrameManager::get
  -> 找到 Frame
  -> ReplacementPolicy::on_access
  -> Frame::pin
  -> 返回 Frame/Page
  -> RecordPageHandler 从 slot 中读取 Record
  -> 使用结束后 DiskBufferPool::unpin_page
```

命中不会增加 `disk_reads`。

### 缓存未命中

```text
BPFrameManager::alloc
  -> 有空闲 Frame：直接使用
  -> 没有空闲 Frame：ReplacementPolicy::choose_victim
       -> 只选择 pin_count == 0 的 Frame
       -> victim 为 dirty 时先 flush
  -> DiskBufferPool::load_page
  -> DoubleWriteBuffer::read_page（恢复检查）
  -> PageIOBackend::read_page
  -> Frame::pin
  -> 返回 RecordPageHandler
```

如果所有 Frame 都被 pin，系统返回 `RC::BUFFERPOOL_NOBUF`，同时记录 `NO_BUFFER` 诊断；不会强行降低 pin count。

## 6. 一次 INSERT 如何写入磁盘

```text
InsertPhysicalOperator
  -> Table::insert_record
  -> RecordFileHandler::insert_record
  -> 找到有空闲 slot 的 RecordPage
     或 DiskBufferPool::allocate_page
  -> RowRecordPageHandler::insert_record
  -> 更新 slot bitmap 和 record data
  -> 写入日志并设置 Page LSN
  -> Frame::mark_dirty
  -> DiskBufferPool::unpin_page
```

此时“dirty”不代表数据已经写到磁盘。真正写回发生在显式 flush、脏页淘汰或正常关闭阶段：

```text
DiskBufferPool::flush_page
  -> BufferPoolLogHandler::flush_page       保证 WAL 先行
  -> DoubleWriteBuffer::add_page            先写保护区
  -> DiskBufferPool::write_page
  -> PageIOBackend::write_page              写目标分页文件
  -> Frame::clear_dirty
```

## 7. 页分配与释放

分页文件的第 0 页是 Header Page，其中的 `BPFileHeader` 保存：

- `buffer_pool_id`：分页文件的唯一 id；
- `page_count`：当前文件包含的 Page 数；
- `allocated_pages`：已经分配的 Page 数；
- `bitmap`：每个 Page 是否已分配。

分配流程主要在 `DiskBufferPool::allocate_page`：

1. 优先查找 bitmap 中未分配的 Page；
2. 没有空闲 Page 时扩展文件页数；
3. 为新 Page 分配 Frame；
4. 修改 Header Page bitmap 并记录 allocation log；
5. 返回已 pin 的 Frame。

释放流程主要在 `DiskBufferPool::dispose_page`：更新 bitmap、记录 deallocation log，并从缓存中清理对应 Frame。不要把“释放 Page”与“淘汰 Frame”混淆：释放会改变磁盘页的分配状态，淘汰只释放内存缓存。

## 8. 替换策略由谁调用

`BPFrameManager` 是策略的唯一主要调用方：

```text
Frame 加入缓存   -> on_insert
Frame 缓存命中   -> on_access
Frame 被使用     -> on_pin
Frame 使用结束   -> on_unpin
Frame 被移除     -> on_remove
缓存空间不足     -> choose_victim(is_replaceable, victim)
```

`ReplacementPolicy` 只返回 `FrameId`。真正的 dirty flush、Frame 回收和 Page I/O 仍由 `BPFrameManager` 与 `DiskBufferPool` 完成。这样以后新增 LRU-K、2Q 或 ARC 时，不需要重写 Buffer Pool 主流程。

## 9. I/O 后端由谁调用

每个 `DiskBufferPool` 持有一个 `PageIOBackend`：

```text
open_file       -> backend 读取 Header Page
load_page       -> backend 读取目标 Page
write_page      -> backend 写入目标 Page
create_file     -> backend 写入初始 Header Page
```

后端根据启动参数创建：

```bash
csudbd --io-backend legacy
csudbd --io-backend positional
```

默认 legacy 保留原有运行语义；positional 不依赖共享文件 offset，更适合后续做并发 I/O 扩展。两者都继续返回现有 `RC`，不会向上层抛出另一套异常体系。

## 10. 统计、快照和 Trace 分工

这三者用途不同：

- Stats 是累计数值，用于计算命中率、读写量和延迟；
- Snapshot 是某一时刻的状态副本，用于 `/buffer` 和 `/pages`；
- Trace 是按时间排列的事件流，用于还原 Page 生命周期。

CLI 中可以使用：

```text
/buffer
/pages 20
/status
```

日志中可以筛选：

```bash
rg 'BUFFER_POOL_(TRACE|STATS)' csudb.log.*
```

重点事件包括 `HIT`、`MISS`、`DISK_READ`、`PIN`、`UNPIN`、`EVICT`、`FLUSH`、`DISK_WRITE` 和 `NO_BUFFER`。

## 11. 与操作系统课程知识的对应关系

| 课程知识点 | CSUDB 实现 |
| --- | --- |
| 固定大小页 | `Page`，固定 8192 字节 |
| 页号 | `PageNum` |
| 内存页帧 | `Frame` |
| 页面驻留 | `pin/unpin` |
| 脏页 | `Frame::dirty/mark_dirty` |
| 缓存管理 | `BPFrameManager` |
| 页面替换 | `ReplacementPolicy` 的 LRU/FIFO/CLOCK |
| 磁盘页分配 | Header Page 的 allocation bitmap |
| 页读写接口 | `PageIOBackend::read_page/write_page` |
| 文件 offset | `page_num * BP_PAGE_SIZE` |
| 持久化 | flush + sync + 正常关闭 |
| 缓存命中率 | `BufferPoolStats` |
| I/O 事件观察 | `[BUFFER_POOL_TRACE]` |
| 文件系统边界 | Linux VFS/POSIX I/O；CSUDB 不实现 Linux VFS |

指导书示例写“如 4 KiB”，并非要求必须为 4 KiB。CSUDB 使用既有磁盘格式中的 8 KiB Page，不能为匹配示例随意修改。

## 12. 推荐阅读顺序

第一次阅读不要直接从一千多行的 `disk_buffer_pool.cpp` 开始。

### 第一阶段：理解数据结构

1. `os/page/page.h`：确认 Page 的大小和布局；
2. `os/page/frame.h`：理解 Page、Frame、pin、dirty；
3. `storage/record/record.h`：理解 RID；
4. `storage/record/record_manager.h`：理解 RecordPage 的 header、bitmap 和 slot。

### 第二阶段：理解一次缓存访问

5. `DiskBufferPool::get_this_page`：看 hit/miss 分支；
6. `DiskBufferPool::allocate_frame`：看内存不足时如何申请 victim；
7. `BPFrameManager::get/alloc/purge_frames`：看 Frame 管理；
8. `DiskBufferPool::unpin_page`：看 Page 使用结束。

### 第三阶段：理解策略和 I/O

9. `replacement_policy.h/.cpp`：分别跟踪 LRU、FIFO、CLOCK；
10. `DiskBufferPool::load_page/write_page`：看 Page I/O 调用点；
11. `page_io_backend.cpp`：比较 legacy 与 positional；
12. `common/io/io.cpp`：看可靠短读写循环。

### 第四阶段：理解持久化和诊断

13. `DiskBufferPool::flush_page_internal`：看 WAL、Double Write 和目标页写回顺序；
14. `double_write_buffer.cpp`：看异常写保护；
15. `buffer_pool_stats.*`：看统计在哪些事件增加；
16. `buffer_pool_diagnostics.*`：看 Snapshot DTO；
17. `DiskBufferPool::log_pinned_frames`：看 Buffer starvation 诊断。

## 13. 推荐断点

追踪 SELECT：

```text
RecordPageHandler::init
DiskBufferPool::get_this_page
BPFrameManager::get
DiskBufferPool::load_page
PageIOBackend::read_page（具体实现）
DiskBufferPool::unpin_page
```

追踪 INSERT：

```text
RecordFileHandler::insert_record
DiskBufferPool::allocate_page
RowRecordPageHandler::insert_record
Frame::mark_dirty
DiskBufferPool::flush_page_internal
PageIOBackend::write_page（具体实现）
```

追踪淘汰：

```text
DiskBufferPool::allocate_frame
BPFrameManager::purge_frames
ReplacementPolicy::choose_victim（具体实现）
DiskBufferPool::purge_frame
```

## 14. 阅读时必须保持的系统不变量

- `BP_PAGE_SIZE` 必须保持 8192；
- 正在使用、`pin_count > 0` 的 Frame 不能被淘汰；
- 修改 Page 后必须标记 dirty；
- 获得 Frame 后最终必须 unpin；
- Buffer hit 不能增加 `disk_reads`；
- 释放 Page 与淘汰 Frame 是两种不同操作；
- Record、Executor 不能绕过 DiskBufferPool 直接读取表文件；
- 目标分页文件写回必须服从现有 WAL/Double Write 顺序；
- Snapshot 只能暴露值对象，不能向 CLI/GUI 暴露 `Frame *` 或锁。

## 15. 适合课程演示的三个实验

### 实验一：命中与缺页

重复查询同一张表，使用 `/buffer` 比较第一次与后续查询的 requests、hits、misses 和 disk_reads。预期后续访问命中率上升。

### 实验二：替换策略

使用较小 Buffer Pool，分别以 `--replacement lru`、`fifo`、`clock` 启动相同 workload，比较 evictions 和 hit rate。策略比较必须使用同一数据与同一访问序列。

### 实验三：持久化

执行 CREATE、INSERT、DELETE，正常关闭 `csudbd` 后重新启动并 SELECT。数据仍存在，说明 dirty page 已安全写回分页文件。

## 16. 后续扩展应该放在哪里

| 计划功能 | 推荐扩展点 |
| --- | --- |
| LRU-K / 2Q / ARC | 新增 `ReplacementPolicy` 实现并接入 factory |
| mmap I/O | 新增 `PageIOBackend` 实现 |
| O_DIRECT | 新增对齐安全的 `PageIOBackend` 实现 |
| 后台脏页清理 | 调度现有 `flush_dirty_pages`，先验证 WAL 顺序 |
| Read-ahead / Prefetch | `load_page` miss 与 PageIOBackend 之间 |
| Buffer Pool 可视化 | 只消费 `BufferPoolSnapshot` |
| Page 生命周期图 | 消费 `[BUFFER_POOL_TRACE]` 的 event sequence |

当前不要通过修改 Page 格式、RID 格式或绕过 Buffer Pool 来实现这些功能。
