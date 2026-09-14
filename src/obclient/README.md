# CSUDB 2026 产品入口

CSUDB 提供三种外部入口，但只有一套数据库逻辑：

    csudb CLI ───────────────┐
    Local Web Console ───────┼─> Native protocol -> DatabaseService
    Python Driver / SDK ─────┘      -> Session/Auth -> SQLTaskHandler
                                             -> SQL Engine -> Storage

CLI、Web 和 SDK 都不会自行解析或执行 SQL。它们只消费稳定的 QueryResult
数据传输对象，数据库内部的 Page、Frame、Tuple 和算子指针不会暴露给外部。

## 1. 启动服务端

首次使用需要初始化，并由使用者交互设置 root 密码：

    ./build_debug/bin/csudbd --initialize
    ./build_debug/bin/csudbd

默认监听 127.0.0.1:6789，默认数据目录由 csudbd --help 显示。常用选项：

    csudbd --host 127.0.0.1 --port 6789
    csudbd --data-dir /path/to/data
    csudbd --buffer-size 20971520
    csudbd --replacement lru       # lru | fifo | clock
    csudbd --io-backend legacy     # legacy | positional

native 是 csudb、Web Console 和 Python SDK 使用的正式协议。mysql 目前只是
实验兼容层，不代表完整 MySQL 兼容。

## 2. 使用命令行客户端

    ./build_debug/bin/csudb -u root -p

也可执行单条 SQL 或 SQL 文件：

    csudb -u root -p -D school -e "SHOW TABLES;"
    csudb -u root -p -D school -f schema.sql
    csudb --ping

进入 Shell 后：

    /help                       查看全部 Shell 命令
    /status                     服务、会话和 Buffer Pool 状态
    /use school                 切换数据库
    /database                   当前数据库
    /timing on                  显示耗时
    /buffer                     Buffer Pool 摘要
    /pages 20                   查看有限数量 Frame/Page
    /web                        打开本地 Web Console
    /web 9000                   在 127.0.0.1:9000 打开
    /web status                 查看 Web Console 状态
    /web stop                   停止 Web Console
    /q                          退出

命令以 / 开头；输入 / 后按 Tab 可列出命令，部分输入可补全，拼错时会显示
相近候选。SQL 支持多行输入，遇到字符串外的分号或末尾反斜杠 g 才执行。

## 3. Local Web Console

先保证 csudbd 正在运行并进入 csudb：

    csudb [sys]> /web
    CSUDB Web Console started at http://127.0.0.1:8765/

浏览器页面会要求输入 CSUDB 用户、密码和数据库。密码不会作为进程参数或 URL
参数传递；登录态只保存在 Web Gateway 内存中，并通过 HttpOnly、
SameSite=Strict Cookie 标识。

当前 Web 功能：

- Server、Session、Page Size、替换策略、I/O backend 状态；
- 请求、命中、未命中、命中率、物理分页文件读写统计；
- Buffer Pool 容量、使用、Pinned、Dirty 图表；
- Frame/Page 热力格与只读快照表；
- 数据库与表浏览器，表内容最多预览 100 行；
- SQL 工作区、结果表格和执行耗时；
- SQL → Compiler → Plan → Executor → Record → Buffer → Disk 主链说明。

安全边界：

- Web 服务只允许监听 loopback，不对局域网或公网开放；
- Web Gateway 复用数据库认证，不保存密码；
- 它不是生产级管理平台，目前没有 TLS、审计、多人共享和流式大结果集；
- /web stop 只终止经 PID 和命令行双重确认的 CSUDB Web 进程。

日志位于 ~/.csudb/web.log。

## 4. Python 开发者接口

源码驱动位于 sdk/python/csudb.py，只依赖 Python 标准库：

    import sys
    sys.path.insert(0, "sdk/python")
    import csudb

    with csudb.connect(
        host="127.0.0.1", port=6789, user="root",
        password="your-password", database="school",
    ) as connection:
        cursor = connection.cursor()
        cursor.execute(
            "INSERT INTO student VALUES (%s, %s, %s);",
            (1, "Alice", 20),
        )
        cursor.execute("SELECT id, name FROM student WHERE age > %s;", (18,))
        for row in cursor:
            print(row)

接口采用 Python DB-API 风格，提供 connect、Connection、Cursor、execute、
executemany、fetchone/fetchmany/fetchall、description、rowcount、
commit/rollback，以及 server_info 和 buffer_snapshot 诊断扩展。

当前参数绑定在驱动端进行字面量转义，并不是服务端 PreparedStatement。所有
第三方驱动都应继续复用 Native 协议，不得绕过 DatabaseService。

## 5. JDBC 接入说明

正式 JDBC 驱动位于 `sdk/java`，使用标准 URL：

    jdbc:csudb://127.0.0.1:6789/school

构建及运行示例：

    ./sdk/java/build.sh
    javac -cp sdk/java/build/csudb-jdbc-2026.1.0.jar \
      -d sdk/java/build/example examples/JdbcExample.java
    java -cp sdk/java/build/csudb-jdbc-2026.1.0.jar:sdk/java/build/example \
      JdbcExample

它支持 DriverManager 自动发现、Connection、Statement、PreparedStatement、
ResultSet、结果元数据和基本事务入口。当前参数绑定仍在驱动端完成，结果集会
完整物化；TLS、流式结果、服务端 PreparedStatement 和连接池尚未实现。

原有 MySQL communicator 继续作为实验兼容层，可通过 `--protocol mysql`
启动，但不作为 CSUDB JDBC 驱动的依赖，也不承诺完整 MySQL 协议兼容。

## 6. 安装

    cmake --install build_debug --prefix ~/.local

安装内容包括 csudb、csudbd、Web helper、HTML 资源、Java/Python SDK、示例
配置和 shell completion。卸载脚本只删除程序与资源，永远不删除数据库数据目录：

    ./scripts/uninstall.sh --user
