# CSUDB 外观层与 OS 存储层实现导读

> 本文按当前真实源码说明：用户输入怎样进入数据库、记录怎样经过 Buffer Pool 到达磁盘，以及这些功能怎样使用和调试。

## 1. 整体调用链

```text
csudb CLI ──────────────┐
Web Console ─ Python SDK├─ Native JSON 协议 ─ csudbd
Java JDBC ──────────────┘                    │
                                            ▼
                                    NativeCommunicator
                                            │ SessionEvent
                                            ▼
                                     DatabaseService
                                            │
                                     SQLTaskHandler
                                            │
                         Parse → Resolve → Optimize → Execute
                                            │
                                   Table / Record / Index
                                            │
                                    RecordPageHandler
                                            │
                                     DiskBufferPool
                                            │
                            FrameManager + ReplacementPolicy
                                            │
                                       Frame / Page
                                            │
                                      PageIOBackend
                                            │
                                  Linux 文件系统 / 磁盘
```

外观层不复制 Parser 和 Executor，只负责连接、请求和显示；OS 层也不是单独模拟器，而是 Table、Record 和 B+Tree 真正使用的底层存储。

## 2. 外观与产品接口

### 2.1 服务端 csudbd

关键文件：

- `src/observer/main.cpp`：参数、首次初始化、Server 启动和终端界面。
- `src/observer/net/server.cpp`：TCP/Unix Socket 监听、连接和 shutdown。
- `src/observer/net/native_communicator.cpp`：Native JSON 协议。
- `src/observer/service/database_service.*`：认证、授权、状态和统一 SQL 入口。
- `src/observer/service/query_result.*`：对外稳定结果 DTO。

主要函数：

| 函数 | 作用 |
|---|---|
| `parse_parameter` | 读取 host、port、data-dir、buffer-size、replacement、io-backend |
| `init_server` | 创建 `NetServer` 或标准输入 `CliServer` |
| `NetServer::start/serve` | 建立监听 socket 并进入服务循环 |
| `NativeCommunicator::read_event` | 把 JSON 请求转为 `SessionEvent` |
| `DatabaseService::execute` | 分派 login/query/status/snapshot/complete |
| `DatabaseService::execute_sql` | 管理命令、权限检查和 SQL 主流程 |
| `DatabaseService::materialize` | 将执行器 Tuple 复制成 `QueryResult` |

普通 SQL 的真实入口：

```text
DatabaseService::execute_sql
→ authorize_sql
→ SqlTaskHandler::handle_sql
→ QueryCacheStage
→ ParseStage
→ ResolveStage
→ OptimizeStage
→ ExecuteStage
→ materialize
```

`QueryResult` 只含状态、错误、列、行、影响行数、耗时、warning 和 attributes，不向客户端暴露 `Tuple *`、`Frame *` 或 `Page *`。

### 2.2 Native 协议

协议是“UTF-8 JSON + 末尾 NUL”。例如：

```json
{"type":"login","user":"root","password":"...","database":"sys"}
{"type":"query","sql":"SELECT * FROM employee;"}
{"type":"buffer_snapshot","limit":20}
```

客户端 `NativeConnection::request` 循环 `send/recv`，服务端 `read_packet/read_event` 生成事件，`write_query_result` 返回 JSON。CLI、Web、Python 和 JDBC 因此得到一致行为。

### 2.3 CLI csudb

主要文件：`src/obclient/client.cpp`。

| 函数/结构 | 作用 |
|---|---|
| `ClientOptions` | 保存连接、输出、脚本和 timing 选项 |
| `parse_options` | 合并默认值、profile、环境变量和 CLI 参数 |
| `NativeConnection::connect_to/request` | 建立 TCP 并发送 Native 请求 |
| `complete_sql` | 判断字符串外的分号或 `\\g`，支持多行 SQL |
| `execute/execute_file` | 执行 SQL 或文件 |
| `render_table` | 输出表格、batch、错误和耗时 |
| `dispatch_meta` | 处理 `/status`、`/buffer`、`/pages`、`/web` 等 |
| `interactive` | REPL、history、补全、prompt 和多行输入 |

Meta command 使用前缀和编辑距离做模糊候选；SQL 补全通过服务端 `complete` 请求取得当前库的真实表列。

### 2.4 终端外观

文件：`src/common/terminal/terminal_ui.*`。

- `Capabilities::detect`：通过 `isatty` 和 `ioctl(TIOCGWINSZ)` 获取终端能力。
- `layout`：根据宽度选择 FULL/NORMAL/COMPACT/MINIMAL。
- `Style`：统一 cyan、magenta、green、yellow、red 等颜色。
- `truncate_middle/display_path`：长路径自适应。
- `NO_COLOR=1` 或重定向输出时关闭颜色。

### 2.5 Web Console

关键文件：

- `src/obclient/web_console_launcher.*`：CLI `/web` 的启动、状态和停止。
- `src/obclient/web/csudb_web.py`：本地 HTTP gateway。
- `src/obclient/web/index.html`、`app.css`、`app.js`：浏览器 UI。

数据流：

```text
Browser → HTTP API → csudb_web.py → Python SDK → Native → csudbd
```

`SessionStore` 用随机 token 映射 Python Connection；浏览器不保存密码。API 的真实来源：

| API | 来源 |
|---|---|
| `/api/status` | server_info + buffer_snapshot |
| `/api/databases` | SHOW DATABASES |
| `/api/tables` | SHOW TABLES |
| `/api/table` | SELECT * FROM table，当前展示前 100 行 |
| `/api/schema` | DESC table |
| `/api/query` | Native query |
| `/api/complete` | 服务端 completion engine |

`app.js` 的 `refresh/renderStatus` 消费真实统计；Overview 每两秒读取累积计数并计算差值，不伪造 QPS。Schema、Data、SQL 分别由 `schemaGraph/loadTable/run` 处理。

### 2.6 Python 与 JDBC

- Python：`sdk/python/csudb.py`，主链为 `connect → Connection._request → Cursor.execute/fetch*`。
- JDBC：`sdk/java/.../jdbc/`，主链为 `CsuDbDriver → NativeClient → JdbcProxies`。
- JDBC 将 Native 返回映射成 `Connection/Statement/PreparedStatement/ResultSet`；未支持 API 明确抛 `SQLFeatureNotSupportedException`。
- 当前 Python/JDBC 参数绑定在客户端完成，不是服务端 Prepared Statement。

## 3. OS / Storage

### 3.1 目录

```text
src/observer/storage/os/
├── page/          Page、Frame、pin、dirty、latch
├── buffer/        DiskBufferPool、BufferPoolManager、DoubleWrite
├── replacement/   LRU、LRU-K、FIFO、CLOCK
├── io/            legacy、positional I/O
└── diagnostics/   Stats、Snapshot、Trace、BufferPool Log
```

### 3.2 Page 与 Frame

`page/page.h` 的 `Page` 固定 8192 字节：

```text
Page = LSN + CheckSum + data
```

`data` 可被解释为 Record Page 或 B+Tree Page。8 KiB 已属于磁盘格式，不能改成 4 KiB。

`page/frame.*` 中：

- `FrameId` 用 `buffer_pool_id + page_num` 唯一定位页面。
- `pin/unpin` 表示页面正在使用；pin 大于 0 时禁止淘汰。
- `mark_dirty/clear_dirty` 表示内存是否比磁盘新。
- `read_latch/write_latch` 保护并发读写。
- `access` 记录最近访问时间。

pin 防止淘汰，latch 防止并发冲突，两者不能替代。

### 3.3 Record 到 Page

文件：`src/observer/storage/record/record_manager.*`。

`RID = page_num + slot_num`。Record Page 布局为：

```text
PageHeader → slot bitmap → fixed-size records
```

- `RecordPageHandler::init`：调用 `get_this_page`，得到已 pin Frame，再加 latch。
- `cleanup`：unlatch 后 unpin。
- `RowRecordPageHandler::insert_record`：找空 slot、写行、写日志、mark dirty。
- `delete_record`：清 slot、写日志、mark dirty。
- `RecordFileHandler::insert_record`：优先找未满页，否则 `allocate_page`。
- `RecordFileHandler::delete_record`：页面删空后尝试 `dispose_page`。

### 3.4 Buffer Pool 主函数

文件：`storage/os/buffer/disk_buffer_pool.*`。

`BPFrameManager` 管理所有文件共享的内存 Frame：

- `get`：命中、pin、更新替换策略。
- `alloc/free`：创建或回收 Frame。
- `purge_frames`：选 victim，刷脏后回收。
- `snapshot`：复制只读诊断 DTO。

`DiskBufferPool` 对应一个分页文件：

- `open_file/close_file`：加载 page 0 文件头并在关闭时刷页。
- `get_this_page`：先查缓存，miss 时分配 Frame 并读盘。
- `allocate_page/dispose_page`：修改文件头 allocation bitmap。
- `unpin_page`：结束一次页面使用。
- `flush_page`：脏页经过 WAL/Double Write 写回。
- `flush_dirty_pages`：批量刷新未 pin 的安全脏页。
- `load_page/write_page`：调用 PageIOBackend。

读页流程：

```text
get_this_page
├─ HIT  → pin → 返回 Frame
└─ MISS → allocate_frame
          → 必要时替换 victim
          → 脏 victim flush
          → PageIOBackend::read_page
          → 返回已 pin Frame
```

### 3.5 替换、I/O 和诊断

`replacement_policy.*` 提供 `on_insert/on_access/on_pin/on_unpin/on_remove/choose_victim`：

- LRU：淘汰最久未访问页。
- FIFO：淘汰最早进入页，命中不改变顺序。
- CLOCK：reference bit 为 1 时给 second chance，为 0 且未 pin 时淘汰。

`page_io_backend.*`：

- legacy：`lseek + read/write`。
- positional：`pread/pwrite`，处理 EINTR 和短读写。

`double_write_buffer.*` 在写真实数据文件前保存页面副本，降低半页写损坏风险。

`buffer_pool_stats.*` 用 atomic 统计 request、hit/miss、read/write、eviction、flush、pin/unpin、dirty、bytes 和 latency。`BufferPoolSnapshot/FrameSnapshot` 不暴露内部指针，CLI 和 Web 都从这里取数。

## 4. 三条典型流程

### SELECT

```text
客户端 → DatabaseService → Executor → Record scanner
→ RecordPageHandler::init → DiskBufferPool::get_this_page
→ HIT 或读磁盘 → Record/Tuple → QueryResult → 客户端
→ cleanup/unpin
```

### INSERT

```text
Executor → Table → RecordFileHandler::insert_record
→ 找空闲页或 allocate_page → 写 slot/record → mark_dirty
→ unpin → eviction/flush/shutdown → DoubleWrite → 磁盘
```

### DELETE

```text
Executor 找 RID → delete_record → 清 slot + mark_dirty
→ 非空页继续复用；空页且安全时 dispose_page
→ 后续 flush 持久化
```

## 5. 使用与调试

```bash
./build_debug/bin/csudbd --replacement clock --io-backend positional
./build_debug/bin/csudb -u root -p -D sys
```

CLI 中：

```text
/status
/buffer
/pages 20
/timing on
/web
```

建议断点顺序：

1. `NativeCommunicator::read_event`
2. `DatabaseService::execute_sql`
3. `SqlTaskHandler::handle_sql`
4. `RecordPageHandler::init`
5. `DiskBufferPool::get_this_page`
6. `BPFrameManager::get/alloc`
7. `DiskBufferPool::load_page`
8. `PageIOBackend::read_page/write_page`

## 6. 当前边界

- Web 表数据当前最多展示 100 行，不是真正 server-side 分页。
- Web Schema 主要来自 DESC，未伪造 FK/Index metadata。
- Web Query History 是浏览器会话历史。
- PAX Record 的部分路径仍为 `UNIMPLEMENTED`，默认 ROW_FORMAT 主链可用。
- mmap、O_DIRECT、后台 cleaner 和 prefetch 尚未实现，应从现有 PageIOBackend 和 flush 接口扩展。
