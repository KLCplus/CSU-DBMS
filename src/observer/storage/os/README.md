# CSUDB OS / Storage module

This directory is the stable reading and extension entrance for the operating-system part of CSUDB.
It organizes the existing storage implementation without moving or duplicating mature MiniOB code.

## 中文学习入口

这里是 CSUDB 操作系统与存储部分的统一入口，不是第二套实现。建议按照下面的顺序阅读：

1. `page/page_model.h`：先理解固定 8 KiB 的 Page，以及 Page 在内存中的 Frame。
2. `buffer/buffer_pool.h`：理解页如何被分配、Pin、Unpin、标脏、刷新和释放。
3. `replacement/replacement.h`：理解内存不足时 LRU、FIFO、CLOCK 如何选择 victim。
4. `io/page_io.h`：理解页如何通过 legacy 或 positional 后端到达 Linux 文件接口。
5. `diagnostics/diagnostics.h`：理解命中率、磁盘 I/O、Trace 和 Frame Snapshot。
6. `record/record_page.h`：最后回到数据库层，看 Record、RID、slot 如何映射到 Page。

这些入口只聚合真实源码，类和函数仍只有一份。调试时应继续在表格“Existing implementation”列出的真实文件中下断点。

```text
Database executor
       |
       v
record/          Record, RID and RecordPage bridge
       |
       v
buffer/          DiskBufferPool and BufferPoolManager
       |
       +------> page/          Page and Frame memory model
       +------> replacement/   LRU, FIFO and CLOCK policy
       +------> diagnostics/   statistics, snapshots and lifecycle trace
       |
       v
io/              PageIOBackend and reliable file I/O
       |
       v
Linux VFS / filesystem / disk
```

## Directory responsibilities

| Entrance | Responsibility | Existing implementation |
| --- | --- | --- |
| `record/record_page.h` | Record/RID to page mapping and scanning | `storage/record/` |
| `page/page_model.h` | 8 KiB Page and in-memory Frame | `storage/buffer/page.h`, `frame.h` |
| `buffer/buffer_pool.h` | allocation, pin/unpin, dirty pages, load/flush | `storage/buffer/disk_buffer_pool.*` |
| `replacement/replacement.h` | replaceable-frame selection | `storage/buffer/replacement/` |
| `io/page_io.h` | legacy and positional page-file I/O | `storage/buffer/page_io_backend.*`, `common/io/` |
| `diagnostics/diagnostics.h` | stats, snapshots, traces and flush reasons | `storage/buffer/buffer_pool_*` |
| `os_storage.h` | umbrella entrance for tools and course experiments | all entrances above |

## Why the implementation is not physically moved

The original paths are included by Record, Table, B+Tree, transaction, WAL and recovery code. Moving them
would create a large mechanical rewrite with no runtime value and a high regression risk. These facade headers
provide one coherent OS module boundary while preserving every existing include path, symbol, file format and
lock boundary.

New course-level OS tools should include the narrowest entrance header. Use `os_storage.h` only when a tool
needs the complete storage view. New production implementation should remain next to the subsystem that owns
it; this directory is a public organization layer, not a second implementation.
