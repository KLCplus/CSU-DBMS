# CSUDB OS and Storage

本文件集中说明 CSUDB 当前操作系统与存储部分的实现、源码目录、调用链和课程覆盖情况。这里列出的文件是真实生产路径，不是独立实验副本。

## 1. OS 子系统位置

```text
src/observer/storage/os/
├── page/
│   ├── page.h
│   ├── frame.h
│   ├── frame.cpp
│   └── page_model.h
├── buffer/
│   ├── buffer_pool.h
│   ├── disk_buffer_pool.h
│   ├── disk_buffer_pool.cpp
│   ├── double_write_buffer.h
│   └── double_write_buffer.cpp
├── replacement/
│   ├── replacement.h
│   ├── replacement_policy.h
│   └── replacement_policy.cpp
├── io/
│   ├── page_io.h
│   ├── page_io_backend.h
│   └── page_io_backend.cpp
├── diagnostics/
│   ├── diagnostics.h
│   ├── buffer_pool_stats.h
│   ├── buffer_pool_stats.cpp
│   ├── buffer_pool_diagnostics.h
│   ├── buffer_pool_diagnostics.cpp
│   ├── buffer_pool_log.h
│   └── buffer_pool_log.cpp
├── record/
│   └── record_page.h
├── os_storage.h
└── os_storage.cpp
```

入口文档：

- `src/observer/storage/os/README.md`：目录和阅读顺序；
- `src/observer/storage/os/os_storage.md`：功能与设计；
- `src/observer/storage/os/os_source_guide.md`：逐文件、逐调用链阅读地图；
- `src/observer/storage/os/DEMO.md`：演示流程。

## 2. 从数据库到磁盘

```text
SELECT / INSERT / DELETE
          |
          v
Physical Operator
          |
          v
Table / Index
          |
          v
RecordFileHandler / RecordPageHandler
          |
          v
RID(page_num, slot_num)
          |
          v
DiskBufferPool
          |
          v
BPFrameManager
    |             |
    v             v
Frame/Page   ReplacementPolicy
                  |
             LRU/FIFO/CLOCK
          |
          v
PageIOBackend
    |              |
 legacy         positional
lseek+rw       pread+pwrite
          |
          v
Linux VFS / filesystem
          |
          v
Disk / SSD
```

Executor、Table 和 RecordManager 不直接读写分页文件。目标数据页必须经过 DiskBufferPool，因此缓存、替换、统计和 Trace 都处于真实数据库链路中。

## 3. Page

核心文件：

| 文件 | 职责 |
| --- | --- |
| `page/page.h` | 固定大小 Page 及页头 |
| `page/page_model.h` | Page 模块聚合入口 |
| `record/record_page.h` | Record/RID 到 RecordPage 的边界入口 |
| `storage/record/record_manager.cpp` | RecordPage 的插入、读取、删除和扫描 |

当前页大小：

```text
BP_PAGE_SIZE = 8192 bytes
```

8 KiB 是现有磁盘格式的一部分。不要改成课程示例中的 4 KiB，也不要修改 Page header、RID、slot bitmap、file header page 或 allocation bitmap。

Record 通过 `RID(page_num, slot_num)` 定位到页与槽。Table 对应数据文件，数据文件由多个 Page 组成。

## 4. Frame

核心文件：

- `page/frame.h`
- `page/frame.cpp`

Frame 是 Page 在内存 Buffer Pool 中的容器，负责：

- `FrameId(buffer_pool_id, page_num)`；
- 原子 pin count；
- dirty 状态；
- 页级 latch；
- 访问时间和诊断元数据；
- 关联 BufferPoolStats。

基本生命周期：

```text
load/allocate
   -> Frame inserted
   -> pin
   -> read or modify
   -> mark dirty when modified
   -> unpin
   -> replaceable when pin_count == 0
   -> flush before dirty eviction
   -> remove/reuse
```

`pin_count > 0` 的 Frame 不能成为 victim。诊断功能只能报告 pin leak，不能偷偷修改 pin count。

## 5. Buffer Pool

核心文件：

| 文件 | 核心类型 | 作用 |
| --- | --- | --- |
| `buffer/disk_buffer_pool.h/.cpp` | `DiskBufferPool` | 单个分页文件的页分配、加载、释放、刷新 |
| 同上 | `BPFrameManager` | 全局 Frame 缓存、查找、容量和 victim 回收 |
| 同上 | `BufferPoolManager` | 管理数据库中的多个 DiskBufferPool |
| `buffer/double_write_buffer.h/.cpp` | `DoubleWriteBuffer` | 保留既有双写保护链路 |
| `buffer/buffer_pool.h` | 聚合入口 | 对上层提供 Buffer Pool 类型 |

主要接口：

| 操作 | 接口 |
| --- | --- |
| 获取并 pin | `get_this_page` |
| 解除 pin | `unpin_page` |
| 分配页 | `allocate_page` |
| 释放页 | `dispose_page` |
| 刷新页 | `flush_page` |
| 批量安全刷新 | `flush_dirty_pages` |
| 状态快照 | `snapshot` |
| 统计快照 | `stats` |

命中路径：

```text
get_this_page
  -> BPFrameManager::get
  -> HIT
  -> pin
  -> ReplacementPolicy::on_access
  -> return Frame
```

未命中路径：

```text
get_this_page
  -> MISS
  -> 取空闲 Frame 或 choose_victim
  -> dirty victim 先 flush
  -> PageIOBackend::read_page
  -> insert/pin Frame
  -> return Frame
```

## 6. 页分配、释放和持久化

页分配由 `DiskBufferPool::allocate_page` 完成，使用文件头和 allocation bitmap 管理唯一 page number。释放由 `dispose_page` 更新分配状态并清理对应缓存 Frame。

写入链：

```text
INSERT / UPDATE / DELETE
  -> RecordPage changed
  -> Frame marked dirty
  -> unpin
  -> explicit flush / eviction / shutdown
  -> DoubleWrite and WAL rules
  -> PageIOBackend::write_page
  -> data file
```

正常关闭会走 shutdown flush。程序重启后，Catalog、Table metadata 和 Record 数据从原数据目录重新加载。

## 7. ReplacementPolicy

文件：

- `replacement/replacement_policy.h`
- `replacement/replacement_policy.cpp`
- `replacement/replacement.h`

统一接口处理：

- Frame 加入；
- Frame 访问；
- pin；
- unpin；
- 删除；
- 选择 victim。

```text
BPFrameManager
    |
    v
ReplacementPolicy
├── LRUReplacementPolicy
├── FIFOReplacementPolicy
└── ClockReplacementPolicy
```

策略对象只管理 FrameId 和替换元数据，不拥有 Frame，也不执行磁盘 I/O。

### LRU

命中会更新 recency，victim 是最久未访问的可替换 Frame。

容量 3，访问 `1, 2, 3, 1, 4`：

```text
victim = 2
```

### FIFO

命中不改变进入顺序，victim 是最早进入的可替换 Frame。

相同序列：

```text
victim = 1
```

### CLOCK

维护环形 FrameId、clock hand 和 reference bit。访问设置 reference=1；扫描时 pinned Frame 跳过，reference=1 清零并给予 second chance，reference=0 才选为 victim。

配置：

```bash
csudbd --replacement lru
csudbd --replacement fifo
csudbd --replacement clock
```

未知名称记录警告并回退 LRU。

## 8. PageIOBackend

文件：

- `io/page_io_backend.h`
- `io/page_io_backend.cpp`
- `io/page_io.h`
- `src/common/io/io.h/.cpp`

```text
DiskBufferPool
      |
      v
PageIOBackend
├── legacy
│   └── lseek + readn/writen
└── positional
    └── preadn/pwriten
```

legacy 保持 Baseline 行为。positional 使用显式 offset，不依赖共享文件位置，并循环处理 EINTR、短读、短写和 EOF。

配置：

```bash
csudbd --io-backend legacy
csudbd --io-backend positional
```

I/O 失败继续使用 RC 返回，并记录 operation、file、page、offset 和 errno 上下文。

mmap 和 O_DIRECT 尚未实现；未来可以新增 PageIOBackend，不需要改 DiskBufferPool 主流程。

## 9. BufferPoolStats

文件：

- `diagnostics/buffer_pool_stats.h`
- `diagnostics/buffer_pool_stats.cpp`

现有指标包括：

- requests、hits、misses、hit rate；
- disk_reads、disk_writes；
- bytes_read、bytes_written；
- evictions、dirty_evictions；
- allocations、disposals；
- pin_requests、unpin_requests；
- current/peak pinned frames；
- current/peak dirty pages；
- no_buffer_failures；
- read/write/flush latency；
- explicit、eviction、shutdown flushes。

统计优先使用 relaxed atomic，避免引入明显的大锁。Buffer hit 不增加 disk_reads；WAL 和 DoubleWrite 自身 I/O 不混入目标分页文件统计。

## 10. Snapshot 与 Web/CLI

文件：

- `diagnostics/buffer_pool_diagnostics.h/.cpp`
- `src/observer/service/database_service.cpp`
- `src/obclient/client.cpp`
- `src/obclient/web/app.js`

`FrameSnapshot` 提供 frame id、pool id、page number、pin count、dirty、replaceable 和安全的策略元数据。

`BufferPoolSnapshot` 提供 capacity、used、free、pinned、dirty、replacement policy、I/O backend、统计和有限 Frame 列表。

对外链路：

```text
BufferPoolManager::snapshot
  -> DatabaseService::server_info
  -> QueryResult attributes/rows
  -> CLI /buffer and /pages
  -> Web Internals and Overview
```

快照只复制 DTO，不暴露 Frame 指针、Page 指针、mutex 或内部容器。

## 11. Page Lifecycle Trace

文件：

- `diagnostics/buffer_pool_log.h`
- `diagnostics/buffer_pool_log.cpp`

统一前缀：

```text
[BUFFER_POOL_TRACE]
```

事件：

```text
HIT MISS DISK_READ DISK_WRITE
PIN UNPIN EVICT FLUSH
ALLOCATE DISPOSE NO_BUFFER
```

每个事件包含进程内单调 event_seq；可以安全取得时还包含 frame_id、page、duration_ns、flush_reason、policy 和 io_backend。Trace 使用 TRACE 级别，不污染默认 SQL 输出。

## 12. FlushReason 与批量刷新

FlushReason：

- EXPLICIT；
- EVICTION；
- SHUTDOWN；
- CHECKPOINT；
- OTHER。

只在真实路径使用对应原因。当前没有为了枚举伪造 checkpoint。

`flush_dirty_pages(max_pages, result, reason)`：

- `max_pages == 0` 表示刷新全部安全脏页；
- 默认跳过业务仍然 pinned 的页；
- 返回 flushed、skipped pinned、failed；
- 不创建后台线程。

这为后续 checkpoint 和 background cleaner 提供稳定入口，但后台 cleaner 必须先验证 WAL、DoubleWrite、锁和 shutdown 顺序。

## 13. Pin starvation 诊断

全部 Frame pinned、无法选择 victim 时：

- 增加 no_buffer_failures；
- 输出 NO_BUFFER Trace；
- 输出 capacity、used、pinned、dirty；
- 最多列出有限数量 pinned Frame；
- 返回现有 BUFFERPOOL_NOBUF/RC。

正常 shutdown 也会只读检查异常 pinned Frame。诊断不会强制 unpin 或释放 Page。

## 14. 课程要求覆盖

| 课程要求 | CSUDB 实现 |
| --- | --- |
| 固定页结构 | 8 KiB Page |
| 页唯一编号 | page_num |
| 页分配与释放 | allocate_page / dispose_page |
| 页读写 | PageIOBackend |
| 数据表映射页集合 | Table/RecordFile -> DiskBufferPool |
| 缓存命中 | BPFrameManager |
| LRU | 已实现 |
| FIFO | 已实现 |
| CLOCK | 高级扩展，已实现 |
| 缓存刷新 | flush_page / flush_dirty_pages |
| 持久化 | shutdown flush + data files |
| 命中统计 | BufferPoolStats |
| 替换日志 | BUFFER_POOL_TRACE |
| SQL 与存储集成 | Executor -> Table -> Record -> Buffer Pool |
| 插入、查询、删除验证 | 走同一真实存储主链 |

课程示例写 4 KiB 只是示例；本项目必须保留现有 8 KiB 磁盘格式。

## 15. 启动和观察

```bash
csudbd --buffer-size 20971520 --replacement clock --io-backend positional
csudb -u root -p
```

CLI：

```text
/status
/buffer
/pages 20
/web
```

基础 SQL：

```sql
CREATE TABLE os_demo(id INT, name CHAR(20));
INSERT INTO os_demo VALUES (1, 'page-one');
INSERT INTO os_demo VALUES (2, 'page-two');
SELECT * FROM os_demo;
DELETE FROM os_demo WHERE id = 1;
SELECT * FROM os_demo;
```

## 16. 后续扩展位置

| 功能 | 扩展位置 |
| --- | --- |
| LRU-K / 2Q / ARC | 新增 ReplacementPolicy 实现和 factory 分支 |
| mmap | 新增 PageIOBackend |
| O_DIRECT | 新增对齐安全的 PageIOBackend |
| Background Cleaner | 调度 flush_dirty_pages |
| Read-ahead / Prefetch | load_page miss 与 PageIOBackend 之间 |
| Async Writer | FlushReason、批量刷新与 PageIOBackend |
| Buffer 可视化 | 只消费 BufferPoolSnapshot |
| OS I/O Trace | PageIOBackend 边界 |
| WAL/Recovery 观察 | 保留现有 clog、trx、DoubleWrite 顺序 |

## 17. 禁止破坏的边界

- 不修改 BP_PAGE_SIZE；
- 不修改 Page、RID、slot 和 allocation bitmap 格式；
- 不允许 Executor 或 RecordManager 绕过 Buffer Pool；
- 不让 ReplacementPolicy 执行 I/O；
- 不把内部 Frame/Page 指针暴露给 CLI 或 Web；
- 不为后台线程改变 WAL/DoubleWrite 顺序；
- 不把 B+Tree、MVCC 或 Recovery 的问题混进 OS 模块重构。

