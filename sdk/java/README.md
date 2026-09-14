# CSUDB JDBC Driver

This module provides a dependency-free JDBC 4 driver over CSUDB's authenticated
Native protocol. It does not use the experimental MySQL compatibility layer.

## Build

JDK 17 or newer is required:

```bash
./sdk/java/build.sh
```

Output:

```text
sdk/java/build/csudb-jdbc-2026.1.0.jar
```

## Connect

The driver is registered through the JDBC service-provider mechanism when the
JAR is on the classpath:

```java
Properties properties = new Properties();
properties.setProperty("user", "root");
properties.setProperty("password", password);

Connection connection = DriverManager.getConnection(
    "jdbc:csudb://127.0.0.1:6789/school",
    properties
);
```

URL format:

```text
jdbc:csudb://HOST:PORT/DATABASE
```

Supported properties are `user`, `password`, and `connectTimeout` in
milliseconds. Do not put passwords in a JDBC URL because URLs are commonly
logged.

## Current JDBC subset

- `DriverManager` automatic driver discovery;
- `Connection`, catalog/database selection, auto-commit, commit and rollback;
- `Statement.execute`, `executeQuery`, `executeUpdate`, and batch execution;
- `PreparedStatement` setters and `?` parameter binding;
- scrollable, read-only materialized `ResultSet`;
- column access by index or label;
- `ResultSetMetaData` and basic `DatabaseMetaData`;
- CSUDB error code mapping to `SQLException`.

Methods outside this subset throw `SQLFeatureNotSupportedException` instead of
silently doing nothing. Parameter binding currently escapes values in the
driver; it is not a server-side prepared statement. Result rows are materialized
and CSUDB currently transmits cell values as strings. TLS, streaming results,
connection pooling, generated keys, and callable statements are future work.

Run the repository example:

```bash
./sdk/java/build.sh
javac -cp sdk/java/build/csudb-jdbc-2026.1.0.jar \
  -d sdk/java/build/example examples/JdbcExample.java
java -cp sdk/java/build/csudb-jdbc-2026.1.0.jar:sdk/java/build/example \
  JdbcExample
```
