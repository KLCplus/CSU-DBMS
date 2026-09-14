# CSUDB Component Documentation

本目录集中保存当前产品入口与 OS/Storage 的源码地图：

- [PRODUCT_INTERFACES.md](PRODUCT_INTERFACES.md)：csudbd、csudb CLI、Web Console、Native 服务、Python SDK、JDBC 驱动及相关目录。
- [OS_STORAGE.md](OS_STORAGE.md)：Page、Frame、Buffer Pool、LRU/FIFO/CLOCK、Page I/O、统计、Trace、Snapshot 和持久化。

两部分通过同一条真实链路连接：

```text
CLI / Web / JDBC / Python
            -> Native protocol
            -> DatabaseService
            -> SQL Engine
            -> Table / Record
            -> Buffer Pool / Page
            -> File / Disk
```

