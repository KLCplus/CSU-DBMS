# CSUDB 2026 · SDK（公网接入）

外部项目可通过公网连接到 CSUDB 服务端。本包提供 **Python 驱动**与 **Java JDBC 驱动**。

## 公共演示服务

| 项 | 值 |
| --- | --- |
| host | `117.50.163.43` |
| port | `8157` |
| user | `root` |
| password | `csudb1234`（演示用） |
| database | `sys` |

> 端口 8157 为“协议嗅探”入口：网页走 HTTP，SDK/CLI 走原生协议，二者可共用同一地址。

## Python

```bash
tar -xzf csudb-python-sdk-2026.1.0.tar.gz
export PYTHONPATH="$PWD/csudb-python-sdk-2026.1.0:$PYTHONPATH"
```

```python
import csudb

with csudb.connect(
    host="117.50.163.43",
    port=8157,
    user="root",
    password="csudb1234",
    database="sys",
    timeout=10,
) as db:
    cur = db.execute("SELECT id, name FROM student WHERE age >= %s;", (20,))
    print([d[0] for d in cur.description])
    for row in cur:
        print(row)
```

## Java JDBC

需要 JDK 17+；将 `csudb-jdbc-2026.1.0.jar` 放到 classpath（驱动通过 `META-INF/services` 自动注册）。

```java
import java.sql.*;
import java.util.Properties;

Properties p = new Properties();
p.setProperty("user", "root");
p.setProperty("password", "csudb1234");

Connection c = DriverManager.getConnection("jdbc:csudb://117.50.163.43:8157/sys", p);
Statement s = c.createStatement();
ResultSet rs = s.executeQuery("SELECT id, name FROM student WHERE age >= 20");
while (rs.next()) System.out.println(rs.getString(1) + " " + rs.getString(2));
c.close();
```

URL 格式：`jdbc:csudb://HOST:PORT/DATABASE`；不要把密码写进 URL。

## 命令行客户端

随附的 `csudb` Linux 客户端同样支持公网：

```bash
./csudb --url 117.50.163.43:8157 -u root -p   # 密码 csudb1234
```

## 备注

- SDK 使用 native 协议，`117.50.163.43:8157` 可直接连接；无需额外安装依赖（Python 仅标准库，JDBC 无第三方依赖）。
- 演示服务为公网可访问，请勿存放敏感数据。
