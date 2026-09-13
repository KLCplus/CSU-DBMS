# CSUDB Python Driver

csudb.py 是 CSUDB 2026 Native JSON 协议的轻量 Python DB-API 风格驱动。
它不依赖第三方包，适用于课程实验、自动化脚本和 Web/API 适配器。

示例：

    import csudb

    with csudb.connect(
        host="127.0.0.1",
        port=6789,
        user="root",
        password="secret",
        database="school",
    ) as db:
        rows = db.execute(
            "SELECT name FROM student WHERE id = %s;",
            (1,),
        ).fetchall()
        print(rows)

实现的主要接口：

| API | 作用 |
| --- | --- |
| connect(...) | 登录并建立 Session |
| Connection.cursor() | 新建游标 |
| Connection.execute(...) | 便利执行接口 |
| Cursor.execute(...) | 执行一条 SQL |
| Cursor.executemany(...) | 顺序执行多组参数 |
| fetchone/fetchmany/fetchall | 读取物化结果 |
| description | 结果列名与服务端类型名 |
| rowcount | 查询行数或已知 affected rows |
| server_info() | 服务与 Buffer Pool 摘要 |
| buffer_snapshot(limit) | 只读 Page/Frame DTO |

占位符采用 %s，值会按 SQL 字面量规则转义。

限制：

- 当前协议一次响应会物化完整结果，不适合超大结果集；
- 当前不是服务端 PreparedStatement；
- 返回单元格当前统一为字符串，类型转换由应用完成；
- threadsafety = 1，不要跨线程共享 Connection；
- TLS 和连接池尚未实现。

安装树中的源码位于 share/csudb/sdk/python/csudb.py。
