# CSUDB 2026 Product Architecture

## External boundary

```text
 csudb CLI       Future Web       Future JDBC / SDK
      \              |                 /
       +-------------+----------------+
                     |
         Native protocol / future adapters
                     |
              DatabaseService
              /      |       \
       QueryResult  Session  SystemCatalog/Auth
                     |
               SQLTaskHandler
                     |
       Parse -> Resolve -> Optimize -> Execute
                     |
          Table -> Record -> Buffer -> File
```

`DatabaseService` is the product boundary. It owns no second parser or executor: ordinary SQL is delegated to the existing `SQLTaskHandler::handle_sql`, which retains the Parse → Resolve → Optimize → Execute stages. Product management commands are centralized at the service/catalog boundary. Protocol clients receive only value DTOs (`QueryResult`, server attributes, and Buffer Pool snapshot values), never `Frame *`, `Page *`, `Tuple *`, or physical operators.

## Request path

```text
csudb
  -> NUL-framed native JSON request
  -> NativeCommunicator
  -> SessionEvent
  -> DatabaseService
       -> login/admin: SystemCatalog
       -> core SQL: Authorization -> SQLTaskHandler -> SQL engine
  -> QueryResult JSON
  -> table or batch renderer
```

The protocol is deliberately native and versioned by behavior, not claimed to be MySQL-compatible. The retained MySQL communicator is an experimental compatibility layer.

## Session and security model

Each connection owns one existing `Session`, extended with session ID, authenticated identity, client address, connection time, current database, and autocommit status. Root owns all privileges; other users are denied unless a matching GLOBAL, DATABASE, or TABLE grant exists. Password verification and privilege persistence live in `SystemCatalog`, not in executors.

The catalog currently persists as a mode-0600 JSON file under the selected data directory. This gives restart persistence and a narrow migration boundary, but it is not yet a page-backed transactional system table. Atomic catalog replacement, concurrent DDL/drop coordination, TLS, roles, and audit logging remain future work.

## Storage and OS boundary

```text
Database Record
      |
RID(page, slot)
      |
RecordPage
      |
DiskBufferPool
      |
Frame Manager ---- ReplacementPolicy (LRU/FIFO/CLOCK)
      |
PageIOBackend ---- legacy(lseek/read/write)
             `---- positional(pread/pwrite)
      |
Linux VFS / filesystem
      |
Disk / SSD
```

Linux VFS and the device are system boundaries; CSUDB does not claim to implement them. Product diagnostics request a stable snapshot through DatabaseService rather than reaching into Frame Manager containers.

## Deployment boundary

`csudbd` binds to loopback by default. Remote access requires explicit `--host 0.0.0.0` (or another IPv4 address). The native protocol has authentication but no TLS in this release, so remote use should stay on a trusted network, VPN, or SSH tunnel.
