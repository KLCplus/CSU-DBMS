# CSUDB JDBC 驱动 · 接口文档

本模块提供一个**零第三方依赖**的 JDBC 4 驱动，直接对接 CSUDB 已认证的 Native 协议
（不使用实验性的 MySQL 兼容层）。

- 驱动类：`edu.csu.csudb.jdbc.CsuDbDriver`
- URL 前缀：`jdbc:csudb://`
- JDBC 版本：`getMajorVersion()=2026`、`getMinorVersion()=1`；`jdbcCompliant()=false`

## 1. 构建与引入

需要 JDK 17 或更高版本：

```bash
./sdk/java/build.sh
# 产物: sdk/java/build/csudb-jdbc-2026.1.0.jar
```

将 JAR 放入 classpath 即可。驱动同时通过 `META-INF/services/java.sql.Driver` 与静态注册块
自动注册，无需 `Class.forName(...)`。

## 2. 快速开始

```java
import java.sql.*;
import java.util.Properties;

Properties properties = new Properties();
properties.setProperty("user", "root");
properties.setProperty("password", "csudb1234");

Connection connection = DriverManager.getConnection(
    "jdbc:csudb://127.0.0.1:6789/sys", properties);

try (Statement statement = connection.createStatement();
     ResultSet rs = statement.executeQuery("SELECT id, name FROM student WHERE age >= 20")) {
  while (rs.next()) {
    System.out.println(rs.getString(1) + " " + rs.getString(2));
  }
}
connection.close();
```

## 3. JDBC URL 与属性

```text
jdbc:csudb://HOST:PORT/DATABASE
```

| 片段 | 默认值 | 说明 |
| --- | --- | --- |
| `HOST` | `127.0.0.1` | 服务端地址 |
| `PORT` | `6789` | native 协议端口 |
| `DATABASE` | `sys` | 初始数据库 |

连接属性（`Properties`，也可作为 URL 查询串 `?k=v&...`，同名时 `Properties` 优先）：

| 属性 | 默认值 | 说明 |
| --- | --- | --- |
| `user` | `root` | 账号 |
| `password` | 空 | 密码 |
| `connectTimeout` | `5000` | 连接/读取超时（毫秒） |

> 不要把密码写进 URL，URL 常被日志记录。

## 4. 支持的 JDBC 子集

`Driver`
- `connect`、`acceptsURL`、`getPropertyInfo`、版本查询。

`Connection`
- `createStatement`、`prepareStatement(String)`、`close`、`isClosed`、`isValid`
- `getAutoCommit` / `setAutoCommit`（切到手动时执行 `BEGIN;`，切回时执行 `COMMIT;`）
- `commit`、`rollback`
- `getCatalog` / `getSchema`（返回当前数据库）、`setCatalog` / `setSchema`（执行 `USE db;`，仅允许合法标识符）
- `getMetaData`、`setReadOnly` / `isReadOnly`、`setTransactionIsolation` / `getTransactionIsolation`
- `nativeSQL`、`setNetworkTimeout` / `getNetworkTimeout`、`abort`
- `getWarnings` / `clearWarnings`、`setClientInfo` / `getClientInfo`、`getTypeMap` / `setTypeMap`、`setHoldability` / `getHoldability`

`Statement` / `PreparedStatement`
- `execute`、`executeQuery`、`executeUpdate`、`executeLargeUpdate`
- `addBatch`、`clearBatch`、`executeBatch`、`executeLargeBatch`
- `getResultSet`、`getUpdateCount`、`getLargeUpdateCount`、`getMoreResults`、`getGeneratedKeys`（返回空结果集）
- `getConnection`、`close`、`isClosed`
- `setMaxRows` / `getMaxRows`（及 `Large` 版本）、`setFetchSize` / `getFetchSize`、`setQueryTimeout` / `getQueryTimeout`
- `getResultSetType()=TYPE_SCROLL_INSENSITIVE`、`getResultSetConcurrency()=CONCUR_READ_ONLY`、`getResultSetHoldability()=CLOSE_CURSORS_AT_COMMIT`
- `getFetchDirection` / `setFetchDirection`、`setMaxFieldSize` / `getMaxFieldSize`

`ResultSet`（可滚动、只读、物化）
- 定位：`next`、`previous`、`first`、`last`、`beforeFirst`、`afterLast`、`absolute`、`relative`、`getRow`
- 判断：`isBeforeFirst`、`isAfterLast`、`isFirst`、`isLast`、`wasNull`
- 取值：`getString`、`getObject`、`getInt`、`getLong`、`getFloat`、`getDouble`、`getBigDecimal`、`getBoolean`
- 元数据：`getMetaData`、`getStatement`、`findColumn`、`getType`、`getConcurrency`、`getHoldability`
- `close`、`isClosed`

`ResultSetMetaData` / `DatabaseMetaData`
- `ResultSetMetaData`：`getColumnCount`、`getColumnName` / `getColumnLabel`、`getColumnTypeName`、`getColumnType`、`getColumnClassName`、`getColumnDisplaySize`、`isNullable` 等。
- `DatabaseMetaData`：产品名/版本、驱动名/版本、`getURL`、`getUserName`、`getConnection`、`supportsTransactions`、`supportsResultSetType`、`supportsResultSetConcurrency`、`isReadOnly`。

> 上表以外的方法统一抛出 `SQLFeatureNotSupportedException`（SQLState `0A000`），不会静默忽略。

## 5. 参数绑定与类型映射

- `PreparedStatement` 支持 `?` 占位符与常用 setter：`setNull`、`setBoolean`、`setByte`、`setShort`、`setInt`、`setLong`、`setFloat`、`setDouble`、`setBigDecimal`、`setString`、`setNString`、`setBytes`、`setDate`、`setTime`、`setTimestamp`、`setObject`。
- 绑定在**驱动端**将值转义为 SQL 字面量（识别单/双引号上下文），**不是服务端 PreparedStatement**。
- 列类型名映射为 JDBC 类型：含 `INT→Types.INTEGER`，含 `FLOAT/DOUBLE/DECIMAL→Types.FLOAT`，`DATE→Types.DATE`，`BOOL→Types.BOOLEAN`，其余 `Types.VARCHAR`。
- 结果单元格以字符串形式返回，`getColumnClassName()` 为 `java.lang.String`。

## 6. 异常与 SQLState

服务端错误会映射为 `SQLException`，`getErrorCode()` 为服务端错误码。常见 SQLState：

| SQLState | 含义 |
| --- | --- |
| `08001` | 无法建立连接 / URL 或属性非法 |
| `08003` | 连接已关闭 |
| `08S01` | 网络或协议错误 |
| `0A000` | 不支持的 JDBC 特性 |
| `3D000` | 非法数据库名 |
| `42000` | 缺少 SQL 语句 |
| `07000` | 语句已关闭 |
| `07001` | 参数未设置或数量不符 |
| `07009` | 非法列索引 |
| `02000` | 语句未产生结果集 |
| `24000` | 游标无效或结果集已关闭 |
| `S0022` | 未知列名 |
| `HY000` | 其它服务端错误 |

## 7. 公网/远程示例

```java
Properties p = new Properties();
p.setProperty("user", "root");
p.setProperty("password", "csudb1234");

Connection c = DriverManager.getConnection("jdbc:csudb://117.50.163.43:8157/sys", p);
Statement s = c.createStatement();
ResultSet rs = s.executeQuery("SELECT id, name FROM student WHERE age >= 20");
while (rs.next()) System.out.println(rs.getString(1) + " " + rs.getString(2));
c.close();
```

## 8. 运行仓库示例

```bash
./sdk/java/build.sh
javac -cp sdk/java/build/csudb-jdbc-2026.1.0.jar \
  -d sdk/java/build/example examples/JdbcExample.java
java -cp sdk/java/build/csudb-jdbc-2026.1.0.jar:sdk/java/build/example \
  JdbcExample
```

## 9. 限制

- 结果集一次性物化，不支持流式读取。
- 驱动端参数转义，非服务端 PreparedStatement。
- 单元格当前以字符串传输。
- TLS、连接池、生成主键、CallableStatement 为后续工作。
