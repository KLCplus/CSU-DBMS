# OS Page and Buffer Pool Lab

## 1. 实验目标

本实验直接复用 MiniOB 的 Page、Frame、Record Page 和 DiskBufferPool，不重新设计磁盘格式。完成的课程要求包括：

- 固定大小页的分配、释放、读取和写回；
- 数据表、Record、RID 与 Page 的映射；
- 有限容量页缓存；
- LRU/FIFO 替换策略切换；
- 缓存命中率、磁盘 I/O 与淘汰统计；
- 页访问、替换和脏页刷新日志；
- 数据正常关闭、重启后的持久化验证。

## 2. 页式存储模型

`src/observer/storage/buffer/page.h` 定义固定 8 KiB Page：

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

8 KiB 是当前 MiniOB 磁盘格式的一部分。指导书中的 4 KiB 是示例，本实验不修改页大小。

页面由 `(buffer_pool_id, page_num)` 唯一标识。`DiskBufferPool` 对应一个分页文件，文件第 0 页保存页数、已分配页数和分配 bitmap。核心接口为：

| 操作 | 接口 |
| --- | --- |
| 获取页 | `DiskBufferPool::get_this_page` |
| 分配页 | `DiskBufferPool::allocate_page` |
| 释放页 | `DiskBufferPool::dispose_page` |
| 解除驻留 | `DiskBufferPool::unpin_page` |
| 刷新页 | `DiskBufferPool::flush_page` |
| 加载页 | `DiskBufferPool::load_page` |
| 写回页 | `DiskBufferPool::write_page` |

磁盘 I/O 当前使用 `lseek + read/write`，完整读写由 `src/common/io/io.cpp` 的 `readn/writen` 保证。

## 3. Record 与 Page 映射

Heap Table 的一条记录由 `RID(page_num, slot_num)` 定位。Record Page 布局为：

```text
+------------+----------------+----------------------------+
| PageHeader | slot bitmap    | fixed-size record slots    |
+------------+----------------+----------------------------+
```

`RecordFileHandler` 管理整个表文件，`RecordPageHandler` 管理单页记录，`HeapRecordScanner` 逐页、逐 slot 扫描。插入优先选择 `free_pages_` 中未满页面，没有可用页面时调用 `allocate_page` 扩展文件。删除清理 slot bitmap，并把页面重新加入候选空闲页集合。

## 4. Buffer Pool

`Page` 是持久化内容，`Frame` 是其内存容器。Frame 额外维护：

- `FrameId`；
- dirty 标记；
- pin count；
- 读写 latch；
- Page 内容。

页面使用期间 pin count 大于 0，替换器只能选择 pin count 为 0 的 Frame。如果所有 Frame 都被 pin，系统返回 `RC::BUFFERPOOL_NOBUF`，不会无限等待。

Buffer Pool 默认容量约 20 MiB。可通过 observer 参数按字节配置：

```bash
observer -n 262144
```

上例提供 `262144 / 8192 = 32` 个 Frame。容量包括同一数据库内表、索引等分页文件的缓存 Frame 及其文件头页。

## 5. LRU 与 FIFO

启动参数：

```bash
observer -r lru
observer -r fifo
```

默认策略为 LRU，名称不区分大小写；未知名称记录警告并回退到 LRU。

内部缓存链表的新页面从头部插入，victim 从尾部选择，并跳过 pinned Frame：

- LRU：缓存命中时把页面移动到头部，尾部是最近最少访问页面；
- FIFO：缓存命中时不改变顺序，尾部是最早进入缓存的页面。

确定性访问序列：

```text
容量：3
访问：1, 2, 3, 1, 4
LRU victim：2
FIFO victim：1
```

## 6. 统计指标

`BufferPoolStats` 提供以下累计指标：

| 指标 | 含义 |
| --- | --- |
| `requests` | `get_this_page` 请求数 |
| `hits` | 在 Frame Cache 中找到页面 |
| `misses` | 需要加载页面 |
| `hit_rate` | `hits / requests` |
| `disk_reads` | 从目标分页文件实际读取的页数 |
| `disk_writes` | 向目标分页文件实际写入的页数 |
| `evictions` | 被淘汰的 Frame 数 |
| `dirty_evictions` | 淘汰前为 dirty 的 Frame 数 |
| `flushes` | Page 进入刷新流程的次数 |
| `allocations` | 页分配次数 |
| `disposals` | 页释放次数 |

`BufferPoolManager::stats()` 返回只读快照，`reset_stats()` 用于划分实验阶段。进程退出时日志中会产生：

```text
[BUFFER_POOL_STATS] policy=LRU,requests=...,hits=...,misses=...,hit_rate=...,...
```

这些统计针对 Buffer Pool 的目标分页文件访问。Double Write Buffer 和 WAL 自身的额外文件 I/O 不混入 `disk_reads/disk_writes`。

## 7. Page Trace

Trace 使用现有日志系统，在 TRACE 级别输出，不改变 SQL 返回结果。稳定事件格式示例：

```text
[BUFFER_POOL_TRACE] event=HIT policy=LRU buffer_pool_id=1 page_num=2 pin_count=1
[BUFFER_POOL_TRACE] event=MISS policy=LRU buffer_pool_id=1 page_num=3
[BUFFER_POOL_TRACE] event=DISK_READ buffer_pool_id=1 page_num=3 bytes=8192
[BUFFER_POOL_TRACE] event=EVICT policy=LRU buffer_pool_id=1 page_num=2 dirty=0
[BUFFER_POOL_TRACE] event=FLUSH buffer_pool_id=1 page_num=3
[BUFFER_POOL_TRACE] event=DISK_WRITE buffer_pool_id=1 page_num=3 bytes=8192
```

还包括 `ALLOCATE` 和 `DISPOSE`。默认 `etc/observer.ini` 的文件日志级别为 TRACE，可在 observer 工作目录按日期生成的 `observer.log.<日期>` 中筛选：

```bash
rg 'BUFFER_POOL_(TRACE|STATS)' observer.log.*
```

## 8. 构建与自动测试

```bash
./build.sh debug --make -j4
cd build_debug
ctest --output-on-failure \
  -R 'buffer_pool_os_test|bp_manager_test|disk_buffer_pool_test|record_manager_test|double_write_buffer_test'
```

`buffer_pool_os_test` 覆盖：

- 同一访问序列下 LRU/FIFO victim 不同；
- 策略名称解析；
- hit/miss/disk read 统计；
- dirty eviction 统计；
- 全部 Frame pinned 时返回 `BUFFERPOOL_NOBUF`。

原有测试继续覆盖页分配/释放、重启后 bitmap、Record Page 和 Double Write Buffer。

## 9. CLI 实验步骤

在独立目录启动，避免数据进入仓库：

```bash
mkdir -p /tmp/csudb-os-lru
cd /tmp/csudb-os-lru
/path/to/CSU-DBMS/csudb \
  --buffer-size 262144 --replacement lru
```

执行：

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

退出后筛选日志，并在另一个空目录以 `-r fifo` 重复相同工作负载。报告至少比较：

| Policy | Frames | Requests | Hits | Misses | Hit Rate | Reads | Writes | Evictions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LRU | 32 |  |  |  |  |  |  |  |
| FIFO | 32 |  |  |  |  |  |  |  |

为了观察稳定差异，应构造超过缓存容量的多页数据，并重复访问部分热点页面。只有两行数据时主要用于正确性和持久化验证，不能有效评价替换算法。

## 10. 结果判定

实验完成应满足：

- Debug Build 成功；
- LRU/FIFO 确定性 victim 测试通过；
- 缓存大小参数实际改变 Frame 数量；
- hit/miss 与 I/O 统计符合访问过程；
- pinned Frame 不被淘汰；
- Trace 能定位 page、event 和 policy；
- CREATE/INSERT/SELECT/WHERE/DELETE 正常；
- observer 重启后数据仍存在。

## 11. 实际验证记录

验证日期：2026-09-08。

```text
Debug Build: 成功
OS/Buffer/Record 专项 CTest: 5/5 targets 通过
全量 CTest: 45/47 targets 通过（96%）
FIFO CLI SQL: 成功
LRU CLI SQL: 成功
FIFO 重启持久化: 成功，重启后仍返回 2 | Bob
```

32 Frame、两行数据的基础 SQL 工作负载实测：

| Policy | Frames | Requests | Hits | Misses | Hit Rate | Reads | Writes | Evictions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LRU | 32 | 7 | 7 | 0 | 1.0 | 1 | 3 | 0 |
| FIFO | 32 | 7 | 7 | 0 | 1.0 | 1 | 3 | 0 |

两组结果相同是合理的：数据仅占一个 Record Page，工作集没有超过缓存容量。替换算法差异由自动化确定性序列验证：LRU 淘汰 Page 2，FIFO 淘汰 Page 1。

全量测试的两个失败项与 Baseline 一致：`bplus_tree_log_test` 的并发 pin-count 断言，以及 `mvcc_trx_log_test` 的上游 ASAN use-after-free/事务断言。本次没有修改 B+Tree、MVCC 或 WAL 实现。

## 12. 后续可选优化

以下不属于本阶段硬性要求：

- `lseek + read/write` 改为 `pread/pwrite`；
- 持久化 free-page list 或多级 bitmap；
- 缩小 `DiskBufferPool` 全局锁粒度；
- 淘汰时避免在 Frame Manager 锁内执行磁盘 I/O；
- CLOCK/2Q 等替换策略；
- 崩溃注入和性能基准。
