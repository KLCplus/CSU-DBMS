# CSUDB Product Interfaces

本文件集中说明 CSUDB 2026 的产品入口、真实源码位置和请求调用链。CLI、Web Console、Python SDK 与 JDBC Driver 只是不同客户端，不包含第二套 SQL Parser、Executor 或 Storage Engine。

## 1. 总体结构

```text
csudb CLI ───────────┐
Web Console ─────────┼── Native JSON protocol
Python SDK ──────────┤           |
JDBC Driver ─────────┘           v
                         NativeCommunicator
                                |
                                v
                         DatabaseService
                                |
                  Session / Auth / SystemCatalog
                                |
                                v
                         SQLTaskHandler
                                |
               Parse -> Resolve -> Plan -> Execute
                                |
                                v
                       Table / Record / OS Storage
```

稳定边界是 Native protocol、`DatabaseService` 和 `QueryResult`。外部客户端不得直接访问 `Page *`、`Frame *`、`Tuple *` 或执行算子。

## 2. 服务端 csudbd

| 文件 | 职责 |
| --- | --- |
| `src/observer/main.cpp` | 参数解析、初始化、Server 启动、终端启动界面 |
| `src/observer/net/server.cpp/.h` | TCP 监听、连接接收、Communicator 创建 |
| `src/observer/net/server_param.h` | 监听地址、端口、协议等运行参数 |
| `src/observer/net/native_communicator.cpp/.h` | CSUDB Native JSON 协议 |
| `src/observer/net/plain_communicator.cpp/.h` | Plain TCP 兼容入口 |
| `src/observer/net/mysql_communicator.cpp/.h` | 实验性 MySQL 协议兼容层 |
| `src/observer/net/sql_task_handler.cpp/.h` | 把网络请求交给统一数据库服务 |
| `src/observer/service/database_service.cpp/.h` | 登录、管理命令、鉴权、SQL 执行和服务状态 |
| `src/observer/service/query_result.cpp/.h` | 对外稳定结果 DTO |

常用命令：

```bash
csudbd --initialize
csudbd
csudbd --host 127.0.0.1 --port 6789
csudbd --data-dir /path/to/data
csudbd --buffer-size 20971520
csudbd --replacement lru
csudbd --replacement fifo
csudbd --replacement clock
csudbd --io-backend legacy
csudbd --io-backend positional
```

正式客户端应使用 `native`。当前 `mysql` 只是实验兼容层，不代表完整 MySQL 兼容。

## 3. CLI 产品外壳

| 文件 | 职责 |
| --- | --- |
| `src/obclient/client.cpp` | 参数、认证、REPL、多行 SQL、输出表格、历史、补全、Meta Command |
| `src/obclient/web_console_launcher.cpp/.h` | 启动、查询和停止本地 Web Console |
| `src/common/terminal/terminal_ui.cpp/.h` | ANSI 色彩、终端宽度、响应式 Banner 和降级 |
| `scripts/completion/csudb.bash` | Shell 参数补全 |
| `src/obclient/CMakeLists.txt` | csudb、Web helper 和静态资源构建安装 |

连接示例：

```bash
csudb -h 127.0.0.1 -P 6789 -u root -p
csudb -D school -u root -p
csudb -D school -u root -p -e "SHOW TABLES;"
csudb -D school -u root -p -f schema.sql
csudb --batch -D school -u root -p -e "SELECT * FROM student;"
csudb --ping
```

CLI 主要能力：

- 交互密码输入不回显；
- SQL 多行输入和分号完成判断；
- table 与 batch 两种输出；
- replxx 输入编辑、历史和补全；
- 敏感密码 SQL 不写入历史；
- Meta Command、SQL grammar、Catalog 和可选模型补全；
- 命令行参数、环境变量和 profile 配置合并。

Meta Command：

| 命令 | 作用 |
| --- | --- |
| `/help`、`/?` | 帮助 |
| `/q`、`/quit`、`/exit` | 退出 |
| `/status`、`/server` | Server 与 Session 状态 |
| `/connect` | 重新连接 |
| `/use DATABASE` | 切换数据库 |
| `/database` | 当前数据库 |
| `/timing on|off` | 查询计时 |
| `/history` | 当前会话历史 |
| `/source FILE` | 执行 SQL 文件 |
| `/output FILE` | 重定向结果 |
| `/buffer` | Buffer Pool 摘要 |
| `/pages N` | 有限 Frame/Page 快照 |
| `/complete SQL` | 输出补全候选 |
| `/web [PORT]` | 打开本地 Web Console |
| `/web status` | Web Console 状态 |
| `/web stop` | 停止 Web Console |

请求链：

```text
client.cpp
  -> Native socket request
  -> NativeCommunicator
  -> SQLTaskHandler
  -> DatabaseService::handle_event
  -> DatabaseService::execute
  -> SQL Engine
  -> QueryResult
  -> CLI table renderer
```

## 4. Web Console

| 文件 | 职责 |
| --- | --- |
| `src/obclient/web/index.html` | Login、导航及 Overview/SQL/Schema/Data/Internals 页面结构 |
| `src/obclient/web/app.css` | 深色数据库 UI、响应式布局和图表样式 |
| `src/obclient/web/app.js` | Session、API、数据库选择、SQL、表浏览、Schema、实时指标和 Frame Map |
| `src/obclient/web/csudb_web.py` | 本地 HTTP Gateway、Session 与 Native 协议转发 |
| `src/obclient/web_console_launcher.cpp/.h` | CLI 内 `/web` 生命周期管理 |

使用：

```text
csudb [sys] > /web
http://127.0.0.1:8765
```

Web Console 只监听 loopback。登录固定使用 `root`，密码通过 Native 协议交给数据库认证，不写入 URL。Session 保存在 Gateway 内存，浏览器使用 HttpOnly Cookie 和会话令牌。

页面和数据来源：

- Overview：数据库数、表数、Buffer 请求、命中率、磁盘读写、Buffer 使用和真实计数器图表；
- SQL Editor：`POST /api/query`；
- Schema：`DESC table`；
- Data Explorer：表数据预览，当前最多 100 行；
- Internals：`server_info`、BufferPoolSnapshot、Frame Map。

主要路由：

| 路由 | 数据来源 |
| --- | --- |
| `POST /api/connect` | Native login |
| `POST /api/logout` | Gateway Session |
| `POST /api/query` | DatabaseService SQL |
| `POST /api/use` | `USE database` |
| `GET /api/status` | `server_info` 与 Buffer Snapshot |
| `GET /api/databases` | `SHOW DATABASES` |
| `GET /api/tables` | `SHOW TABLES` |
| `GET /api/schema?name=T` | `DESC T` |
| `GET /api/table?name=T` | 表数据预览 |

Web 不伪造 CPU、QPS、外键或索引信息。首页图表只对真实累计计数器做有限窗口差分采样。

## 5. Python 第三方接口

文件：

- `sdk/python/csudb.py`：驱动；
- `sdk/python/README.md`：说明；
- `examples/python_client.py`：示例。

```python
import csudb

with csudb.connect(
    host="127.0.0.1",
    port=6789,
    user="root",
    password="your-password",
    database="school",
) as connection:
    cursor = connection.cursor()
    cursor.execute("SELECT * FROM student WHERE id = %s;", (1,))
    print(cursor.fetchall())
```

提供 `Connection`、`Cursor`、`execute`、`executemany`、`fetchone`、`fetchmany`、`fetchall`、`commit`、`rollback`、`server_info` 和 `buffer_snapshot`。

参数在客户端转义为 SQL 字面量，不是服务端 PreparedStatement；结果一次性物化，尚无 TLS 和流式结果。

## 6. JDBC 第三方接口

```text
sdk/java/
├── build.sh
├── README.md
└── src/main/
    ├── java/edu/csu/csudb/jdbc/
    │   ├── CsuDbDriver.java
    │   ├── NativeClient.java
    │   ├── JdbcProxies.java
    │   └── Json.java
    └── resources/META-INF/services/java.sql.Driver
```

| 文件 | 职责 |
| --- | --- |
| `CsuDbDriver.java` | JDBC URL 解析、Driver 注册和 Connection 创建 |
| `NativeClient.java` | Native TCP/JSON 请求 |
| `JdbcProxies.java` | Connection、Statement、PreparedStatement、ResultSet 适配 |
| `Json.java` | 零依赖 JSON 编解码 |
| `META-INF/services/java.sql.Driver` | DriverManager 自动发现 |

构建：

```bash
./sdk/java/build.sh
```

连接：

```java
Properties properties = new Properties();
properties.setProperty("user", "root");
properties.setProperty("password", password);

Connection connection = DriverManager.getConnection(
    "jdbc:csudb://127.0.0.1:6789/school",
    properties
);
```

支持 `Statement`、`PreparedStatement` 客户端参数绑定、批处理、只读物化 `ResultSet`、ResultSetMetaData 和基本事务入口。不支持的方法抛出 `SQLFeatureNotSupportedException`。

## 7. 安装相关文件

| 文件 | 作用 |
| --- | --- |
| `scripts/install.sh` | 用户级、系统级和开发链接安装 |
| `scripts/uninstall.sh` | 删除 CSUDB 程序资源，不删除数据目录 |
| `CMakeLists.txt` | 顶层版本与安装规则 |
| `examples/` | Python/JDBC 示例 |
| `etc/csudb.ini` | 服务端配置 |
| `etc/csudb-client.toml` | 客户端 profile |
| `etc/sql_completion.json` | SQL 补全配置 |

开发刷新：

```bash
./build.sh debug --make -j4
./scripts/install.sh --dev
```

## 8. 修改边界

- CLI、Web、JDBC 和 Python 新能力应复用 Native protocol 和 DatabaseService。
- 认证与权限在 Session、SystemCatalog、DatabaseService 层统一处理。
- 外部结果继续使用 QueryResult DTO，不泄漏内部对象。
- Web 的 OS 页面只消费 BufferPoolSnapshot。
- 服务端 PreparedStatement 应新增协议能力，不能在客户端复制 Parser。

