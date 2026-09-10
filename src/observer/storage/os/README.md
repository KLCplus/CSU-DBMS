# CSUDB OS / Storage module

This directory contains the real operating-system storage implementation of CSUDB. The former
`storage/buffer/` sources were physically reorganized here and every internal consumer now includes
the new paths. The implementation and runtime behavior remain single-source.

## 中文学习入口

这里是 CSUDB 操作系统与存储部分的统一入口，不是第二套实现。建议按照下面的顺序阅读：

1. `page/page_model.h`：先理解固定 8 KiB 的 Page，以及 Page 在内存中的 Frame。
2. `buffer/buffer_pool.h`：理解页如何被分配、Pin、Unpin、标脏、刷新和释放。
3. `replacement/replacement.h`：理解内存不足时 LRU、FIFO、CLOCK 如何选择 victim。
4. `io/page_io.h`：理解页如何通过 legacy 或 positional 后端到达 Linux 文件接口。
5. `diagnostics/diagnostics.h`：理解命中率、磁盘 I/O、Trace 和 Frame Snapshot。
6. `record/record_page.h`：最后回到数据库层，看 Record、RID、slot 如何映射到 Page。

`page/`、`buffer/`、`replacement/`、`io/` 和 `diagnostics/` 中放置的是真实实现；`record/` 是通往现有数据库 Record 模块的边界入口。类和函数仍只有一份。

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
| `page/page_model.h` | 8 KiB Page and in-memory Frame | `storage/os/page/page.h`, `frame.h` |
| `buffer/buffer_pool.h` | allocation, pin/unpin, dirty pages, load/flush | `storage/os/buffer/disk_buffer_pool.*` |
| `replacement/replacement.h` | replaceable-frame selection | `storage/os/replacement/` |
| `io/page_io.h` | legacy and positional page-file I/O | `storage/os/io/page_io_backend.*`, `common/io/` |
| `diagnostics/diagnostics.h` | stats, snapshots, traces and flush reasons | `storage/os/diagnostics/buffer_pool_*` |
| `os_storage.h` | umbrella entrance for tools and course experiments | all entrances above |

## Refactoring boundary

Page, Frame, Buffer Pool, replacement policies, page I/O backends and diagnostics were physically moved into
this directory. Record, Table, B+Tree, transaction, WAL and recovery remain in their database-owned modules;
only their include paths changed. Public symbols, on-disk formats, ownership rules and lock boundaries were not
redesigned.

New course-level OS tools should include the narrowest entrance header. Use `os_storage.h` only when a tool
needs the complete storage view. New implementation belongs in the matching OS subdirectory; do not recreate
compatibility copies under the removed `storage/buffer/` path.
