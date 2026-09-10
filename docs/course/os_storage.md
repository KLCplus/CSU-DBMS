# OS Page and Buffer Pool

本文把课程要求与 CSUDB 的高级扩展分开说明。实现继续使用既有磁盘格式和 Record/B+Tree/WAL 主链，没有重新设计 Page、RID 或 Record Page。

## 源码统一入口

操作系统部分的真实实现已迁移到 `src/observer/storage/os/`，目录依次为：

```text
storage/os/
├── page/          Page / Frame
├── buffer/        DiskBufferPool / BufferPoolManager
├── replacement/   LRU / FIFO / CLOCK
├── io/            PageIOBackend / Linux file I/O
├── diagnostics/   Stats / Snapshot / Trace
├── record/        Record / RID / RecordPage bridge
└── os_storage.h   完整聚合入口
```

Page、Frame、Buffer Pool、替换策略、Page I/O 与诊断源码都已物理迁入对应子目录。Table、Record、B+Tree、WAL 和事务模块只更新 include 路径，调用关系、类型、锁边界和磁盘格式保持不变。详细阅读顺序见 `src/observer/storage/os/README.md`。

## 1. 课程要求

Course Core 已覆盖：固定大小页的分配、释放、读写；Record/RID 到 Page 的映射；有限容量缓存；LRU/FIFO；命中与 I/O 统计；页替换日志；正常关闭与重启持久化。

本阶段 Advanced Extensions 包括：可插拔替换策略、CLOCK、可替换 Page I/O 后端、positional I/O、状态快照、生命周期 Trace、Flush 原因、安全批量脏页刷新和 pin starvation 诊断。

## 2. 当前实现

```text
Table / Index
  -> RecordFileHandler / RecordPageHandler
  -> DiskBufferPool
  -> BPFrameManager
       -> ReplacementPolicy (LRU / FIFO / CLOCK)
  -> Frame -> Page (8192 bytes)
  -> PageIOBackend (legacy / positional)
  -> Linux VFS / file system
  -> Disk / SSD
```

目标分页文件的 I/O 必须经过 `DiskBufferPool`，Executor、Table 和 RecordManager 没有新增绕过 Buffer Pool 的路径。WAL 与 Double Write Buffer 仍执行原有职责，其文件 I/O 不计入目标分页文件的 `disk_reads/disk_writes`。

## 3. Page / Frame 模型

`src/observer/storage/os/page/page.h` 定义 `BP_PAGE_SIZE = 8192`：

```text
Page (8192 bytes)
+------------------+
| LSN              |
+------------------+
| CheckSum         |
+------------------+
| Data             |
+------------------+
```

8 KiB 是磁盘格式的一部分，本阶段没有修改 Page header、file header page、allocation bitmap 或 checksum 范围。

`Frame` 是 Page 的内存容器，持有 `FrameId(buffer_pool_id, page_num)`、dirty、原子 pin count、latch 和最后访问时间。pin count 大于 0 的 Frame 永远不会被替换策略选为 victim。

## 4. Record / RID → Page

Heap Record 由 `RID(page_num, slot_num)` 定位：

```text
RecordFileHandler
  -> RecordPageHandler
  -> RID(page_num, slot_num)
  -> Record Page header + slot bitmap + fixed-size slots
  -> DiskBufferPool::get_this_page
```

核心代码是 `storage/record/record.h`、`record_manager.h/.cpp` 和 `heap_record_scanner.cpp`。本阶段没有修改记录布局、RID 格式或扫描语义。

## 5. Buffer Pool

`BufferPoolManager` 管理一个数据库中的多个 `DiskBufferPool`；表数据文件和索引文件共享 `BPFrameManager` 的容量与替换策略。核心接口：

| 操作 | 接口 |
| --- | --- |
| 获取/Pin | `DiskBufferPool::get_this_page` |
| Unpin | `DiskBufferPool::unpin_page` |
| 分配/释放 | `allocate_page` / `dispose_page` |
| 单页刷新 | `flush_page` |
| 安全批量刷新 | `flush_dirty_pages` |
| 诊断快照 | `BufferPoolManager::snapshot` / `DiskBufferPool::snapshot` |

容量由 `--buffer-size BYTES` 配置，实际 Frame 数至少为 1，按 `BYTES / 8192` 计算。未设置或小于等于 0 时使用既有默认容量。

## 6. Replacement Policy Architecture

`storage/os/replacement/replacement_policy.h/.cpp` 定义统一 `ReplacementPolicy`：

```text
BPFrameManager owns Frame cache
  -> on_insert
  -> on_access
  -> on_pin / on_unpin
  -> on_remove
  -> choose_victim(is_replaceable)
       -> LRUReplacementPolicy
       -> FIFOReplacementPolicy
       -> ClockReplacementPolicy
```

策略只保存淘汰元数据，不拥有或暴露 `Frame *`。`BPFrameManager` 提供 `is_replaceable` 判定，最终以实际 `pin_count == 0` 为准。`DiskBufferPool` 不包含按策略分支的 `if/else`。

## 7. LRU

新 Frame 位于新近端；缓存命中调用 `on_access`，更新新近性；从最久未访问且未 pin 的 Frame 开始选 victim。既有确定性语义保持：容量 3，访问 `1,2,3,1,4`，victim 为 2。

## 8. FIFO

新 Frame 按首次进入顺序排队；命中不改变顺序；选择最早进入且未 pin 的 Frame。相同序列的 victim 为 1。

## 9. CLOCK

CLOCK 使用环形 FrameId 列表、clock hand 和 reference bit：插入或访问设为 1；扫描遇到已 pin Frame 直接跳过；遇到 reference=1 清零并给予 second chance；遇到 reference=0 选为 victim并推进 hand。空缓存、部分/全部 pinned、删除和重新插入均有明确处理。

```bash
./csudb --replacement clock
```

策略名称大小写不敏感。未知值记录 WARN 并回退 LRU。

## 10. Page I/O Backend

`storage/os/io/page_io_backend.h/.cpp` 是目标分页文件的 Page I/O 边界：

```text
DiskBufferPool::load_page / write_page
  -> PageIOBackend::read_page / write_page / sync
       -> legacy
       -> positional
```

接口使用现有 `RC` 返回错误，日志包含 backend、operation、file、page、offset、errno。默认是 legacy，避免改变 Baseline 行为。

## 11. legacy：lseek + read/write

legacy 先 `lseek` 到 `page_num * 8192`，再调用 `common::readn/writen` 完整传输一页。`DiskBufferPool::wr_lock_` 保留原有序列化边界。

```bash
./csudb --io-backend legacy
```

## 12. positional：pread/pwrite

positional 调用新增的 `common::preadn/pwriten`。helper 循环处理短读写和 EINTR/EAGAIN；提前 EOF 返回读错误；零长度写入按 I/O 错误处理；每轮推进 buffer 和显式 offset，不依赖共享文件位置。

```bash
./csudb --io-backend positional
```

本阶段为兼容既有锁语义，positional 仍位于 `wr_lock_` 内；后续可在并发审计后缩小该锁，而无需改高层调用。

## 13. BufferPoolStats

全局统计由 `BPFrameManager` 持有；每个 `DiskBufferPool` 另有分页文件维度统计。新增指标：

- `pin_requests`、`unpin_requests`、`no_buffer_failures`；
- `current_pinned_frames`、`peak_pinned_frames`；
- `dirty_pages_current`、`peak_dirty_pages`；
- `bytes_read`、`bytes_written`；
- 读、写、Flush 总延迟与读写最大延迟；
- `eviction_flushes`、`explicit_flushes`、`shutdown_flushes`。

所有累计计数使用 relaxed atomic；峰值使用 CAS 更新。`disk_reads/disk_writes` 只在 PageIOBackend 真正成功访问目标分页文件后增加，Buffer hit 不增加 `disk_reads`。

进程关闭时的汇总示例：

```text
[BUFFER_POOL_STATS] policy=CLOCK,io_backend=positional,requests=...,bytes_read=...,...
```

## 14. Frame Snapshot

`buffer_pool_diagnostics.h` 定义稳定 DTO：

- `FrameSnapshot`：frame_id、buffer_pool_id、page_num、pin_count、dirty、replaceable、last_access_ns、policy_metadata；
- `BufferPoolSnapshot`：capacity、used/free/pinned/dirty frame 数、policy、I/O backend、文件信息、统计和 Frame 列表。

快照复制值，不暴露 `Frame *`、`Page *`、mutex 或内部缓存容器。未来 CLI/GUI 应只消费该 DTO。

## 15. Page Lifecycle Trace

继续使用唯一的 `[BUFFER_POOL_TRACE]`，保持 TRACE 级别。事件包括：`HIT`、`MISS`、`DISK_READ`、`DISK_WRITE`、`EVICT`、`FLUSH`、`ALLOCATE`、`DISPOSE`、`PIN`、`UNPIN`、`NO_BUFFER`。

每个事件带进程内单调 `event_seq`；能安全获得时还带 frame_id、duration_ns、flush_reason、policy 与 io_backend。默认 SQL 输出不受影响。筛选方式：

```bash
rg 'BUFFER_POOL_(TRACE|STATS)' csudb.log.*
```

## 16. Dirty Page Flush

`FlushReason` 包括 `EXPLICIT`、`EVICTION`、`SHUTDOWN`、`CHECKPOINT`、`OTHER`。仅真实路径使用对应原因：淘汰写回是 EVICTION，正常关闭 purge 是 SHUTDOWN，公开 Flush API 是 EXPLICIT；本阶段没有伪造 checkpoint。

`flush_dirty_pages(max_pages, result, reason)`：

- `max_pages == 0` 表示全部安全脏页；
- 跳过业务仍 pin 的页；
- 返回 flushed、skipped pinned、failed 数量；
- 返回首个 I/O `RC`，同时继续释放诊断快照所加的临时 pin。

本阶段不创建后台 cleaner thread。

## 17. Pin / No Buffer Diagnostics

分配失败且找不到 victim 时增加 `no_buffer_failures`，输出 `NO_BUFFER` Trace 和 WARN，包含 capacity、used、pinned、dirty。随后最多记录 8 个 pinned Frame 的 id、pool、page、pin count 和 dirty 状态，避免日志爆量。

正常关闭时也做只读 pin leak 检查；只告警，不强制 unpin、不篡改 pin count、不偷偷释放页。

## 18. Persistence

实测命令：

```bash
./build_debug/bin/csudbd --config etc/csudb.ini \
  --data-dir /tmp/csudb-os-lab \
  --replacement clock --io-backend positional

# 另一个终端
./build_debug/bin/csudb -u root -p
```

实测 SQL：

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

结果依次为两行、`Alice`、成功删除、仅 `2 | Bob`。退出后用 positional 重启，再以 LRU+legacy、FIFO+legacy 打开同一数据目录，均查询到 `2 | Bob`。

## 19. Current Limitations

- 为保持并发语义，FrameManager 淘汰路径仍可能持有管理锁执行慢 I/O；这是已知的 Future Optimization。
- positional 当前仍受每个 DiskBufferPool 的 `wr_lock_` 保护，尚未释放并发潜力。
- Snapshot 是调用时刻的只读副本，不是跨多个后续操作的一致性事务视图。
- Global Stats 是完整生命周期统计；Per DiskBufferPool Stats 重点保证请求、目标 I/O、淘汰与 Flush 维度，跨文件共享容量仍以 Global Snapshot 为准。
- 没有后台 dirty cleaner、read-ahead、async writer、mmap 或 O_DIRECT。
- `bplus_tree_log_test` 和 `mvcc_trx_log_test` 的 Baseline 上游问题不属于本阶段，本次未修改其实现或断言。

## 20. Future Extensions

| 功能 | 扩展入口 |
| --- | --- |
| LRU-K / 2Q / ARC | 实现新的 `ReplacementPolicy`，接入 factory |
| mmap | 实现新的 `PageIOBackend`，定义映射与 `msync` 生命周期 |
| O_DIRECT | 实现新的 `PageIOBackend`，先解决 buffer/offset/filesystem alignment |
| Background Cleaner | 调度现有 `flush_dirty_pages`，并先验证 WAL/double-write ordering |
| Sequential Read-Ahead / Async Prefetch | 从 `load_page` miss 与 PageIOBackend 边界扩展 |
| Async Page Writer | 从 FlushReason、批量 Flush 和 PageIOBackend 扩展 |

验证命令：

```bash
./build.sh debug --make -j4
./build_debug/unittest/buffer_pool_os_test
```

本阶段 `buffer_pool_os_test` 6/6 通过，覆盖 LRU/FIFO/CLOCK、策略/后端解析、dirty eviction、统计和全 pinned 的 `BUFFERPOOL_NOBUF`。
