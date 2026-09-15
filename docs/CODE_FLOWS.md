# CSUDB 代码流程总览

本文按四条主线说明代码的实际执行路径，与源码注释配套阅读：

1. 操作系统与存储层（page / buffer / replacement / io / diagnostics）
2. 命令行外壳 CLI
3. 本地 Web 控制台
4. 第三方接口（Python SDK 与 JDBC 驱动）

文中所有函数名与文件路径都取自当前源码。

---

## 0. 全局链路

```text
CLI / Web Console / Python SDK / JDBC
        |
        v
  Native JSON 协议（UTF-8 JSON + 末尾 NUL）
        |
        v
  NativeCommunicator
        |
        v
  DatabaseService（认证、授权、管理命令分派）
        |
        v
  SQLTaskHandler（Parse → Resolve → Optimize → Execute）
        |
        v
  Table / Record / Index
        |
        v
  RecordPageHandler（RID = page_num + slot_num）
        |
        v
  DiskBufferPool
        |
        v
  BPFrameManager + ReplacementPolicy
        |
        v
  Frame / Page（8192 字节）
        |
        v
  DoubleWriteBuffer
        |
        v
  PageIOBackend（legacy / positional）
        |
        v
  Linux VFS / 文件系统 / 磁盘
```

四种入口共用同一条链路，不存在第二套 Parser、执行器或存储引擎。

---

## 1. OS 与存储层流程

### 1.1 读一页（SELECT 路径）

```text
TableScanPhysicalOperator
  → Table::get_record_scanner
  → RecordFileScanner / HeapRecordScanner
  → RecordPageHandler::init(page_num)
  → DiskBufferPool::get_this_page(page_num)
```

进入 `get_this_page` 之后：

```text
check_page_num(page_num)                      校验页号在范围内且位图标记为已分配
frame_manager_.get(id(), page_num)            乐观查找，不持文件锁
  ├─ 命中 → record_page_request(true)
  │        → on_access + Frame::access()      更新替换策略的访问记录
  │        → 返回已 pin 的 Frame
  └─ 未命中 → record_page_request(false)，输出 MISS Trace
             → scoped_lock(lock_)             加文件锁
             → 复查 frame_manager_.get(...)   防两个线程重复装载同一页
             → allocate_frame(page_num, &frame)
             → load_page(page_num, frame)
             → 返回已 pin 的 Frame

使用结束后：RecordPageHandler::cleanup → DiskBufferPool::unpin_page
```

`allocate_frame` 内部是一个「申请—淘汰—重试」循环：

```text
while (true) {
  frame = frame_manager_.alloc(id(), page_num);
  if (frame != nullptr) return SUCCESS;
  purged = frame_manager_.purge_frames(1, purger);
  if (purged == 0) { record_no_buffer_failure(); return BUFFERPOOL_NOBUF; }
}
```

`load_page` 的取数顺序：

```text
dblwr_manager_.read_page(this, page_num, page)   先问双写缓冲要
  ├─ 取到 → 直接返回（那里的内容一定比真实文件新）
  └─ 取不到 → io_backend_->read_page(...)        经 I/O 后端读真实分页文件
              → record_disk_read(bytes, latency)  记 DISK_READ Trace
```

命中缓存或命中双写缓冲都不增加 `disk_reads`。

### 1.2 写一页（INSERT 路径）

```text
InsertPhysicalOperator
  → Table::insert_record
  → RecordFileHandler::insert_record
      → 找到有空闲 slot 的页，否则 DiskBufferPool::allocate_page
  → RowRecordPageHandler::insert_record
      → 修改 slot bitmap 与记录区
      → 写日志并把 LSN 记到页头
      → Frame::mark_dirty()
  → DiskBufferPool::unpin_page
```

此时数据只在内存。真正落盘走 `flush_page_internal`，顺序不可变：

```text
DiskBufferPool::flush_page_internal(frame, reason)
  ① log_handler_.flush_page(frame.page())
       → 实质是 log_handler_.wait_lsn(page.lsn)，保证 WAL 先于数据页落盘
  ② frame.set_check_sum(crc32(page.data, BP_PAGE_DATA_SIZE))
  ③ dblwr_manager_.add_page(this, page_num, page)
       → 整页写入共享表空间文件；真实分页文件的写入被推迟
  ④ frame.clear_dirty()
       → 到这里才清脏标记，因为页已安全存在于 WAL 与双写缓冲中
```

真实分页文件的成批写回由 `DiskDoubleWriteBuffer::flush_page` 完成，触发点是
双写缓冲攒满 16 页、数据库关闭，或对象析构。

### 1.3 淘汰一页

```text
DiskBufferPool::allocate_frame → BPFrameManager::purge_frames(1, purger)
  → 持 lock_
  → 构造 is_replaceable 回调交给策略
       is_replaceable = frames_.peek(...) && frame->can_purge()
       判据是 Frame::can_purge()，即 pin_count == 0
  → replacement_policy_->choose_victim(is_replaceable, victim)
  → 再确认一次 can_purge()        策略只存元数据，不知道实时的 pin 状态
  → frame->pin()                  立刻锁住，防止刷盘期间被别人抢走
  → purger(frame)
       脏页 → flush_page_internal(frame, EVICTION)
       非本文件的 victim → bp_manager_.flush_page(frame, EVICTION)
  → free_internal(frame_id, frame)
       断言 pin_count == 1：调用方必须持有最后一次 pin
       清脏、清页号、unpin、通知策略 on_unpin/on_remove、移出缓存与内存池
```

四种策略的差别只在 `choose_victim`：

| 策略 | 选择规则 |
| --- | --- |
| LRU | 链表队首最旧的可替换页 |
| FIFO | 与 LRU 共用实现，命中不更新顺序，取最早进入的可替换页 |
| CLOCK | 沿环扫描，引用位为 1 的清零并给第二次机会，为 0 才选为 victim；扫描上限为环长两倍 |
| LRU-K（K=2） | 访问不足两次的冷页优先；冷热相同时比较倒数第二次访问时间，更旧的先淘汰 |

LRU-K 每个页保存最近两次访问的逻辑时间戳，`record_access` 在队列满时先弹出最旧的一个。
这个设计让一次性顺序扫描产生的页停留在冷页集合中被优先换出。

### 1.4 页分配与释放

```text
DiskBufferPool::allocate_page
  位图中还有空洞 → 扫出第一个未分配页
      → log_handler_.allocate_page(i, lsn)   先写分配日志
      → 更新 allocated_pages 与位图，标脏文件头并记 LSN
      → get_this_page(i, frame) → clear_page() 整页清零
  没有空洞 → 页数达到 MAX_PAGE_NUM 则返回 BUFFERPOOL_NOBUF
           → 否则在文件末尾新增一页并 flush 扩展文件

DiskBufferPool::dispose_page
  第 0 页（文件头）禁止释放
  引用计数不为 1 → 说明还有扫描器在用，返回 LOCKED_UNLOCK 拒绝释放
  否则 → log_handler_.deallocate_page → 清位图、递减 allocated_pages、标脏文件头
```

释放页改的是磁盘上的分配状态，淘汰 Frame 只动内存缓存，两者不是一件事。

### 1.5 持久化与恢复

```text
正常关闭  DiskBufferPool::close_file
            → unpin 文件头页 → log_pinned_frames("shutdown") 泄漏检查
            → purge_all_pages(SHUTDOWN)
            → dblwr_manager_.clear_pages(this)
            → 关闭描述符 → 通知 BufferPoolManager 移除表项

启动      DiskDoubleWriteBuffer::open_file
            → load_pages()      逐页读共享表空间，CRC32 校验通过的才进内存
            → recover()         = flush_page()，把残留页补写回真实文件

日志重放  BufferPoolLogReplayer::replay
            → 从日志取出 buffer_pool_id 与 page_num
            → bp_manager_.get_buffer_pool(id, bp)
            → bp->redo_allocate_page / redo_deallocate_page
               两者都用文件头 LSN 做幂等判断，重复重放不会出错
```

### 1.6 本层关键不变量

- `BP_PAGE_SIZE` 恒为 8192，页头 12 字节 + 数据区 8180 字节，整页可一次读写
- `pin_count > 0` 的 Frame 永远不能成为 victim
- 拿到 Frame 必须配对 `unpin_page`
- 缓存命中不增加 `disk_reads`
- WAL 与双写缓冲的 I/O 不混入目标分页文件的统计
- 快照只暴露值对象，不向 CLI 或 Web 暴露 `Frame *`、`Page *` 或锁

---

## 2. CLI 流程

### 2.1 启动

```text
main(argc, argv)
  → parse_options
      优先级从低到高：内置默认 → config.toml 的 [default] → profile 节
                      → CSUDB_* 环境变量 → 命令行参数
  → Shell::run
      → NativeConnection::connect_to(host, port)
      → 需要密码时 read_password()
           标准输入不是终端则改读 /dev/tty；读取期间关闭终端回显
           -p 只表示「提示输入」，从不接收密码值
      → 发送 login 请求，随后立即把内存中的口令抹掉
      → banner() 打印版本、主机、用户、数据库
      → 按选项选择：execute_sql / file / complete / 非终端脚本 / interactive()
```

退出码区分：连接失败 2、认证失败 3、语句失败 1、参数错误 64。

### 2.2 一条 SQL 的四个环节

```text
① interactive()
     replxx 读取一行；一级提示符带数据库名，续行提示符缩进
② complete_sql()
     逐字符跟踪单引号、双引号与反斜杠转义
     字符串之外的分号且其后只剩空白 → 完整
     或整段以 \g 结尾 → 完整
     否则继续累积到 sql_buffer_，回到 ①
③ execute() → request({"type": "query", "sql": ...})
     发送 UTF-8 JSON 加末尾 NUL；接收循环累积到 NUL，上限 16 MiB
④ render_table()
     错误 → 打印错误码与消息
     无结果集 → 打印影响行数，可选耗时
     有结果集 → 制表符分隔或带边框表格，NULL 单元格高亮
```

`execute()` 会在响应带回当前数据库时同步更新提示符上的库名，因此 `USE` 语句执行后
提示符自动跟着变。

`split_sql_script` 用与 `complete_sql` 相同的状态机把脚本切成多条语句，
所以字符串里的分号不会被误切。

### 2.3 元命令

全部在 `dispatch_meta` 中分派，`/status` 与 `\status` 等价（先归一化再比较）：

| 命令 | 去向 |
| --- | --- |
| `/status`、`/server` | `show_status(false, 0)` → 请求 `server_info` |
| `/buffer` | `show_status(false, 0)` → 请求 `server_info` |
| `/pages N` | `show_status(true, N)` → 请求 `buffer_snapshot`，上限 200 |
| `/use DB` | 唯一转成 SQL 发给服务端的元命令 |
| `/web [PORT\|status\|stop]` | 启停本地 Web 控制台 |
| `/help`、`/timing`、`/clear`、`/history`、`/source`、`/output`、`/connect` | 完全在本地完成 |

拼错的元命令用编辑距离给出相近候选：距离不超过 2 或是输入的子序列，最多 4 个。

### 2.4 补全与提示

```text
Tab → complete_dispatch → Shell::complete_line
        元命令 → 本地前缀匹配
        SQL    → fetch_completion 发送 complete 请求
                 服务端返回八列结果集：
                 插入文本、显示文本、类型、来源、替换起止、分数、说明
                 客户端只负责展示并换算 context_length
行内提示 hint_line 只展示服务端返回的 ghost text（模型未启用时为空）
```

补全候选由服务端计算，客户端不重复实现 SQL 语法。

含 `PASSWORD` 或 `IDENTIFIED BY` 的语句不写入历史文件，避免口令落盘。

---

## 3. Web 控制台流程

### 3.1 启动

```text
CLI 中执行 /web
  → start_web_console(options, message)
      find_web_script()     环境变量 CSUDB_WEB_SCRIPT → csudb 同目录 → ../libexec/csudb/csudb-web
      read_pid + is_web_process
            读 pid 文件；再读 /proc/<pid>/cmdline 确认含 csudb-web 或 csudb_web.py
            只在两处都命中时才认为进程是我们的，避免 pid 复用后误杀无关进程
      已在运行 → 只打开浏览器并返回地址
      fork()
        子进程：setsid() 脱离终端
                stdin → /dev/null，stdout/stderr → ~/.csudb/web.log
                execlp("python3", script, --db-host, --db-port,
                       --listen-host 127.0.0.1, --listen-port, --pid-file)
      父进程：轮询 25 次 × 40 毫秒确认 pid 文件出现且进程在跑
```

`csudb_web.py` 侧：

```text
main()
  → 校验 listen-host 必须是 127.0.0.1 / ::1 / localhost，否则报错退出
  → 写 pid 文件（权限 0600）
  → 注册 SIGTERM / SIGINT，在另一个线程里关闭服务
  → serve_forever(poll_interval=0.25)
  → 退出时关闭全部会话连接、删 pid 文件
```

### 3.2 登录

```text
浏览器 POST /api/connect {password}
  → _read_json 校验 Content-Type 为 application/json、长度 1..1 MiB、顶层是对象
  → csudb.connect(host, port, "root", password, "sys")

    注意：Web 网关复用 Python SDK 作为唯一的协议客户端实现，
    自己不做协议编解码。

  → SessionStore.create(connection)
       token = secrets.token_urlsafe(32)     密码学安全随机源
       内存字典 {token: connection}
  → 响应同时下发
       Cookie: csudb_session=<token>; Path=/; HttpOnly; SameSite=Lax
       响应体: {"success": true, "session_token": "<token>", ...}
  → 前端把 token 存入 localStorage
```

之后每个请求前端都会带上 `X-CSUDB-Session` 请求头（优先级更高）与 Cookie；
网关 `_session_token()` 先看请求头再看 Cookie。

认证失效时返回 401 与 `{"error": {"message": "login required"}}`，
前端根据错误文本是否包含 `login` 决定弹回登录页。

会话只存在于网关内存中，进程退出即失效；密码只在登录那一次使用。

### 3.3 执行 SQL

```text
POST /api/query {sql}
  → connection._request({"type": "query", "sql": sql})
  → public_result(response)
       只放行 success、error、message、columns、rows、
       affected_rows、execution_time_us、warnings、attributes 九个字段
  → 前端渲染表格或影响行数
```

`public_result` 是一道白名单，保证服务端的内部结构不会原样透传到浏览器。

### 3.4 接口清单

| 接口 | 方法 | 用途 |
| --- | --- | --- |
| `/api/health` | GET | 健康检查，登录页显示端点 |
| `/api/connect` | POST | 登录 |
| `/api/logout` | POST | 注销 |
| `/api/status` | GET | `server_info` 与 `buffer_snapshot(80)` 合并返回 |
| `/api/pages` | GET | 页快照，可带 limit（当前前端未调用） |
| `/api/databases` | GET | `SHOW DATABASES` |
| `/api/tables` | GET | `SHOW TABLES` |
| `/api/table` | GET | `SELECT * FROM x`，截取前 100 行 |
| `/api/schema` | GET | `DESC x` |
| `/api/query` | POST | 执行任意 SQL |
| `/api/use` | POST | 切换数据库 |
| `/api/complete` | POST | SQL 补全 |

`/api/table` 的响应会带 `limited_to: 100` 与 `server_pagination: false`，
明确说明当前不是服务端分页。

### 3.5 安全边界

- 只监听回环地址，`main()` 中硬校验，不对局域网或公网开放
- 会话只存内存，不落盘；网关不保存密码
- 表名与库名拼进 SQL 前必须通过 `IDENTIFIER` 正则白名单校验
- 每个响应带 CSP、`X-Frame-Options: DENY`、`X-Content-Type-Options: nosniff`、
  `Cache-Control: no-store`
- 终止进程前用 pid 与 `/proc` 命令行双重确认
- 尚未实现：TLS、审计日志、多人共享、流式大结果集

---

## 4. 第三方接口流程

### 4.1 Python SDK

```text
csudb.connect(host, port, user, password, database, timeout)
  → 拒绝任何未知关键字参数
  → 端口范围校验
  → Connection.__init__
        socket.create_connection 建连并设超时
        立即发送 login 请求完成认证
        认证失败要关闭套接字再抛错，避免留下已连接未登录的会话

cursor = connection.cursor()
cursor.execute(operation, parameters)
  → _bind(operation, parameters)
       按 %s 切开，占位符个数必须与参数个数一致，否则抛 ProgrammingError
       _quote 逐个转换：None → NULL、bool → 0/1、bytes → 十六进制串、
       字符串把内部单引号加倍
  → connection._request({"type": "query", "sql": ...})
  → description  由 columns 生成列元数据
  → rowcount     有结果集时是行数，否则取 affected_rows
  → _rows        所有行在这里一次性转成字符串元组

fetchone / fetchmany / fetchall
  → 只在内存列表中移动下标，不再访问网络

connection.close()
  → 尽力发送 logout，无论成败都释放套接字
  → 支持 with 语句，退出时自动关闭
```

诊断扩展：`server_info()`、`buffer_snapshot(limit)`（limit 被夹在 0 到 200 之间）。

接口属性：`apilevel = "2.0"`、`threadsafety = 1`、`paramstyle = "format"`。

边界：参数绑定在驱动端做字面量转义，不是服务端 PreparedStatement；
结果集完整物化，没有流式读取。

### 4.2 JDBC 驱动

```text
DriverManager.getConnection("jdbc:csudb://host:port/db?user=&password=&connectTimeout=")
  → CsuDbDriver 静态块已把自身注册进 DriverManager，JDBC 4 自动发现，无需 Class.forName
  → connect(url, properties)
       acceptsURL 不匹配则返回 null
       URI 解析：主机缺省 127.0.0.1、端口缺省 6789、库缺省 sys
       查询参数做百分号解码，缺省用户 root、超时 5000 毫秒
  → new NativeClient(host, port, timeout, user, password, database)
       socket.connect 并立即 login
  → JdbcProxies.connection(client, url, user, database) 返回动态代理

Connection.createStatement() / prepareStatement(sql)
  → StatementHandler
      execute(sql) → NativeClient.query(sql) → 解析响应
      PreparedStatement 的问号由 bind() 在客户端替换
         扫描时遇到字符串字面量整段跳过，避免把字符串里的问号当占位符
  → ResultSetHandler
      行数据在创建时物化；getString / getInt 等按下标取列
      列类型经 sqlType() 映射为 java.sql.Types 常量

未实现的方法
  → 显式抛出 SQLFeatureNotSupportedException，并把方法名带在消息里
```

`JdbcProxies` 用 `java.lang.reflect.Proxy` 为 Connection、Statement、ResultSet
各实现一个 `InvocationHandler`，因此不必手写 JDBC 的数百个接口方法。

`Json.java` 是自写的极简 JSON 编解码，目的是让驱动只依赖 JDK，不引入第三方库。

边界：`jdbcCompliant()` 返回 false；参数绑定仍在客户端完成；
结果集完整物化；TLS、流式结果与连接池尚未实现。

### 4.3 四种入口对照

| 入口 | 协议实现 | 参数绑定 | 结果集 |
| --- | --- | --- | --- |
| CLI（C++） | `NativeConnection` | 不支持 | 流式打印 |
| Python SDK | `_NativeConnection` | 客户端 `%s` 转义 | 全量物化 |
| JDBC | `NativeClient` | 客户端 `?` 转义 | 全量物化 |
| Web 网关 | 复用 Python SDK | 复用 SDK | 复用 SDK |

四种入口共用同一套 Native 协议与同一套数据库内核。

---

## 5. 不得破坏的边界

- 不修改 `BP_PAGE_SIZE` 与 Page、RID、slot、分配位图格式
- 不允许 Executor 或 RecordManager 绕过 Buffer Pool 直接访问表文件
- 不让替换策略执行磁盘 I/O，策略只返回 FrameId
- 不把内部 Frame 或 Page 指针暴露给 CLI 与 Web
- 不改变 WAL 与 Double Write 的先后顺序
- 外部入口自身不解析也不执行 SQL，一律经 Native 协议交给服务端
- 未实现的接口必须显式报错，不能静默返回空结果
