package edu.csu.csudb.jdbc;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.Date;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.Statement;
import java.sql.Time;
import java.sql.Timestamp;
import java.sql.Types;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.Executor;

/** Small JDBC interface implementation using proxies to keep unsupported APIs explicit. */
/**
 * 用动态代理把 Native 协议包装成 JDBC 接口。
 *
 * <p>JDBC 的 Connection、Statement、ResultSet 等接口方法非常多，逐一手写实现
 * 会产生大量样板代码。这里改用 java.lang.reflect.Proxy：每个接口只需要一个
 * InvocationHandler，集中处理「哪些方法我们支持、哪些要抛
 * SQLFeatureNotSupportedException」，把不支持的方法显式暴露出来而不是静默失败。
 *
 * <p>参数绑定仍在客户端完成字面量转义，不是服务端 PreparedStatement。
 */
final class JdbcProxies {
  private JdbcProxies() {}

  /** 建立 JDBC Connection 代理 */
  static Connection connection(NativeClient client, String url, String user, String database) {
    ConnectionHandler handler = new ConnectionHandler(client, url, user, database);
    Connection connection = proxy(Connection.class, handler);
    handler.proxy = connection;
    return connection;
  }

  @SuppressWarnings("unchecked")
  /** 统一创建动态代理并附带可读描述，便于在异常信息里指明是哪个对象 */
  private static <T> T proxy(Class<T> type, InvocationHandler handler) {
    return (T) Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler);
  }

  /** 处理 equals、hashCode、toString 三个 Object 方法，其余返回 null 表示未处理 */
  private static Object objectMethod(Object proxy, Method method, Object[] args, String description) {
    return switch (method.getName()) {
      case "toString" -> description;
      case "hashCode" -> System.identityHashCode(proxy);
      case "equals" -> proxy == args[0];
      default -> null;
    };
  }

  /** 判断是否为 Object 自身声明的方法 */
  private static boolean isObjectMethod(Method method) {
    return method.getDeclaringClass() == Object.class;
  }

  /** 执行目标方法，把反射异常翻译成 SQLException */
  private static Object wrapper(Object proxy, Method method, Object[] args) throws SQLException {
    if (method.getName().equals("isWrapperFor")) return ((Class<?>) args[0]).isInstance(proxy);
    if (method.getName().equals("unwrap")) {
      Class<?> type = (Class<?>) args[0];
      if (type.isInstance(proxy)) return proxy;
      throw new SQLException("not a wrapper for " + type.getName());
    }
    return null;
  }

  /** 构造「该能力尚未支持」异常，附带方法名便于定位 */
  private static SQLFeatureNotSupportedException unsupported(Method method) {
    return new SQLFeatureNotSupportedException("CSUDB JDBC does not yet support " + method.getName(), "0A000");
  }

  /** 处理 Connection 接口上的全部方法 */
  private static final class ConnectionHandler implements InvocationHandler {
    private final NativeClient client;
    private final String url;
    private final String user;
    private String database;
    private boolean autoCommit = true;
    private boolean readOnly;
    private int isolation = Connection.TRANSACTION_READ_COMMITTED;
    private int networkTimeout;
    private Connection proxy;

    /**
     * 记录连接所需的全部状态。
     *
     * @param client   底层 Native 协议客户端
     * @param url      原始 JDBC URL，供 DatabaseMetaData 使用
     * @param user     登录账号
     * @param database 当前数据库名，可随后续 USE 变更
     */
    ConnectionHandler(NativeClient client, String url, String user, String database) {
      this.client = client;
      this.url = url;
      this.user = user;
      this.database = database;
    }

    /**
     * Connection 代理的统一入口。
     *
     * <p>实现原理：先处理 Object 三方法与 Wrapper 协议，再用 switch 按方法名分发；
     * 未列出的方法一律抛 SQLFeatureNotSupportedException，从而让不支持的能力显式失败。
     *
     * @param object 代理对象本身
     * @param method 被调用的接口方法
     * @param args   调用参数，可能为 null
     * @return 方法的返回值
     * @throws Throwable 透传底层 SQLException 或不支持异常
     */
    @Override
    public Object invoke(Object object, Method method, Object[] args) throws Throwable {
      if (isObjectMethod(method)) return objectMethod(object, method, args, "CSUDB Connection[" + url + "]");
      Object wrapped = wrapper(object, method, args);
      if (wrapped != null || method.getName().equals("isWrapperFor")) return wrapped;
      return switch (method.getName()) {
        case "createStatement" -> statement(this, null);
        case "prepareStatement" -> statement(this, (String) args[0]);
        case "close" -> { client.close(); yield null; }
        case "isClosed" -> client.isClosed();
        case "isValid" -> client.ping();
        case "nativeSQL" -> args[0];
        case "getAutoCommit" -> autoCommit;
        case "setAutoCommit" -> { setAutoCommit((Boolean) args[0]); yield null; }
        case "commit" -> { ensureOpen(); client.query("COMMIT;"); yield null; }
        case "rollback" -> { ensureOpen(); client.query("ROLLBACK;"); yield null; }
        case "getCatalog", "getSchema" -> database;
        case "setCatalog", "setSchema" -> { setDatabase((String) args[0]); yield null; }
        case "getMetaData" -> databaseMetaData(this);
        case "setReadOnly" -> { readOnly = (Boolean) args[0]; yield null; }
        case "isReadOnly" -> readOnly;
        case "setTransactionIsolation" -> { isolation = (Integer) args[0]; yield null; }
        case "getTransactionIsolation" -> isolation;
        case "getWarnings" -> null;
        case "clearWarnings" -> null;
        case "setNetworkTimeout" -> { networkTimeout = (Integer) args[1]; yield null; }
        case "getNetworkTimeout" -> networkTimeout;
        case "abort" -> { client.close(); yield null; }
        case "setClientInfo" -> null;
        case "getClientInfo" -> args == null || args.length == 0 ? new java.util.Properties() : null;
        case "getTypeMap" -> new LinkedHashMap<String, Class<?>>();
        case "setTypeMap", "setHoldability" -> null;
        case "getHoldability" -> ResultSet.CLOSE_CURSORS_AT_COMMIT;
        default -> throw unsupported(method);
      };
    }

    /**
     * 切换自动提交模式。
     *
     * <p>原理：服务端没有独立的提交开关，开启自动提交前先 COMMIT 结算此前事务，
     * 关闭则发 BEGIN 开启显式事务；值未变化时直接返回，避免多余往返。
     *
     * @param value 目标自动提交标志
     * @throws SQLException 连接已关闭或服务端返回错误
     */
    private void setAutoCommit(boolean value) throws SQLException {
      ensureOpen();
      if (autoCommit == value) return;
      client.query(value ? "COMMIT;" : "BEGIN;");
      autoCommit = value;
    }

    /**
     * 切换当前数据库。
     *
     * <p>由于库名会被直接拼进 SQL，先用正则白名单校验，防止注入；非法名抛 SQLState 3D000。
     *
     * @param value 目标数据库名
     * @throws SQLException 名称为 null/非法，或 USE 执行失败
     */
    private void setDatabase(String value) throws SQLException {
      if (value == null || !value.matches("[A-Za-z_][A-Za-z0-9_]*")) throw new SQLException("invalid database name", "3D000");
      client.query("USE " + value + ";");
      database = value;
    }

    /**
     * 确认连接仍可用。
     *
     * @throws SQLException 连接已关闭，SQLState 08003
     */
    private void ensureOpen() throws SQLException {
      if (client.isClosed()) throw new SQLException("CSUDB connection is closed", "08003");
    }
  }

  /** 创建一个未预编译的语句代理 */
  private static Statement statement(ConnectionHandler connection, String preparedSql) {
    StatementHandler handler = new StatementHandler(connection, preparedSql);
    Class<?> type = preparedSql == null ? Statement.class : PreparedStatement.class;
    Object statement = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler);
    handler.proxy = (Statement) statement;
    return handler.proxy;
  }

  /** 处理 Statement 与 PreparedStatement 上的全部方法 */
  private static final class StatementHandler implements InvocationHandler {
    private final ConnectionHandler connection;
    private final String preparedSql;
    private final TreeMap<Integer, Object> parameters = new TreeMap<>();
    private final List<String> batch = new ArrayList<>();
    private Statement proxy;
    private ResultSet resultSet;
    private int updateCount = -1;
    private int maxRows;
    private int fetchSize;
    private int queryTimeout;
    private boolean closed;

    /**
     * @param connection  所属连接处理器，用于取 NativeClient 与回传 Connection 代理
     * @param preparedSql 预编译 SQL 模板；null 表示普通 Statement（每次调用自带 SQL）
     */
    StatementHandler(ConnectionHandler connection, String preparedSql) {
      this.connection = connection;
      this.preparedSql = preparedSql;
    }

    /**
     * Statement/PreparedStatement 代理的统一入口。
     *
     * <p>实现原理：若是 PreparedStatement 且方法名属于参数设置，则把参数按下标
     * 存入 TreeMap 并直接返回（绑定推迟到执行时由 bind 完成字面量替换）；
     * 其余按方法名分发，未支持的方法抛 SQLFeatureNotSupportedException。
     *
     * @param object 代理对象本身
     * @param method 被调用的接口方法
     * @param args   调用参数，可能为 null
     * @return 方法的返回值
     * @throws Throwable 透传 SQLException 或不支持异常
     */
    @Override
    public Object invoke(Object object, Method method, Object[] args) throws Throwable {
      if (isObjectMethod(method)) return objectMethod(object, method, args, "CSUDB " + (preparedSql == null ? "Statement" : "PreparedStatement"));
      Object wrapped = wrapper(object, method, args);
      if (wrapped != null || method.getName().equals("isWrapperFor")) return wrapped;
      String name = method.getName();
      if (preparedSql != null && isParameterSetter(name) && args != null && args.length >= 2 && args[0] instanceof Integer index) {
        parameters.put(index, name.equals("setNull") ? null : args[1]);
        return null;
      }
      return switch (name) {
        case "clearParameters" -> { parameters.clear(); yield null; }
        case "executeQuery" -> { execute(resolveSql(args)); if (resultSet == null) throw new SQLException("statement did not produce a result set", "02000"); yield resultSet; }
        case "executeUpdate" -> { execute(resolveSql(args)); yield Math.max(updateCount, 0); }
        case "executeLargeUpdate" -> { execute(resolveSql(args)); yield (long) Math.max(updateCount, 0); }
        case "execute" -> execute(resolveSql(args));
        case "addBatch" -> { batch.add(resolveSql(args)); yield null; }
        case "clearBatch" -> { batch.clear(); yield null; }
        case "executeBatch" -> executeBatch(false);
        case "executeLargeBatch" -> executeBatch(true);
        case "getResultSet" -> resultSet;
        case "getUpdateCount" -> updateCount;
        case "getLargeUpdateCount" -> (long) updateCount;
        case "getMoreResults" -> { resultSet = null; updateCount = -1; yield false; }
        case "getGeneratedKeys" -> emptyResultSet(proxy);
        case "getConnection" -> connection.proxy;
        case "close" -> { closed = true; if (resultSet != null) resultSet.close(); yield null; }
        case "isClosed" -> closed;
        case "cancel", "clearWarnings", "closeOnCompletion", "setCursorName", "setEscapeProcessing", "setPoolable" -> null;
        case "getWarnings" -> null;
        case "isCloseOnCompletion", "isPoolable" -> false;
        case "setMaxRows" -> { maxRows = (Integer) args[0]; yield null; }
        case "getMaxRows" -> maxRows;
        case "setLargeMaxRows" -> { maxRows = Math.toIntExact((Long) args[0]); yield null; }
        case "getLargeMaxRows" -> (long) maxRows;
        case "setFetchSize" -> { fetchSize = (Integer) args[0]; yield null; }
        case "getFetchSize" -> fetchSize;
        case "setQueryTimeout" -> { queryTimeout = (Integer) args[0]; yield null; }
        case "getQueryTimeout" -> queryTimeout;
        case "getFetchDirection" -> ResultSet.FETCH_FORWARD;
        case "setFetchDirection" -> null;
        case "getResultSetType" -> ResultSet.TYPE_SCROLL_INSENSITIVE;
        case "getResultSetConcurrency" -> ResultSet.CONCUR_READ_ONLY;
        case "getResultSetHoldability" -> ResultSet.CLOSE_CURSORS_AT_COMMIT;
        case "getMaxFieldSize" -> 0;
        case "setMaxFieldSize" -> null;
        default -> throw unsupported(method);
      };
    }

    /**
     * 求出本次要执行的 SQL。
     *
     * <p>原理：PreparedStatement 走 bind 把占位符替换成字面量；普通 Statement 取
     * 第一个 String 参数，缺失时抛 SQLState 42000。
     *
     * @param args 方法参数
     * @return 已绑定的完整 SQL
     * @throws SQLException 未提供 SQL 文本
     */
    private String resolveSql(Object[] args) throws SQLException {
      if (preparedSql != null) return bind(preparedSql, parameters);
      if (args == null || args.length == 0 || !(args[0] instanceof String sql)) throw new SQLException("SQL is required", "42000");
      return sql;
    }

    /**
     * 真正执行一条 SQL 并把结果物化到本 handler。
     *
     * <p>原理：请求服务端后，若响应含列则构造 ResultSet 并令 updateCount=-1、返回 true；
     * 否则视为更新语句，从 affected_rows 取影响行数、清空结果集并返回 false。
     * maxRows 大于 0 时在本地截断行数。
     *
     * @param sql 已绑定的 SQL
     * @return 是否产生了结果集
     * @throws SQLException 语句已关闭或服务端执行失败
     */
    private boolean execute(String sql) throws SQLException {
      if (closed) throw new SQLException("statement is closed", "07000");
      Map<String, Object> response = connection.client.query(sql);
      List<Map<String, Object>> columns = columns(response);
      List<List<Object>> rows = rows(response);
      if (maxRows > 0 && rows.size() > maxRows) rows = new ArrayList<>(rows.subList(0, maxRows));
      if (!columns.isEmpty()) {
        resultSet = resultSet(columns, rows, proxy);
        updateCount = -1;
        return true;
      }
      resultSet = null;
      Object affected = response.get("affected_rows");
      updateCount = affected instanceof Number number ? number.intValue() : 0;
      return false;
    }

    /**
     * 逐条执行批处理。
     *
     * <p>原理：服务端没有批量接口，因此顺序执行每条 SQL，逐条记录影响行数；
     * 执行完清空批次。large 为 true 返回 long[]，否则收窄为 int[]（溢出即抛异常）。
     *
     * @param large 是否返回 long[] 版本
     * @return 每条语句的影响行数数组，负数归零
     * @throws SQLException 任一语句执行失败
     */
    private Object executeBatch(boolean large) throws SQLException {
      long[] counts = new long[batch.size()];
      for (int i = 0; i < batch.size(); i++) {
        execute(batch.get(i));
        counts[i] = Math.max(updateCount, 0);
      }
      batch.clear();
      if (large) return counts;
      int[] small = new int[counts.length];
      for (int i = 0; i < counts.length; i++) small[i] = Math.toIntExact(counts[i]);
      return small;
    }
  }

  /** 用服务端返回的列与行构造一个完整的 ResultSet */
  private static ResultSet resultSet(List<Map<String, Object>> columns, List<List<Object>> rows, Statement statement) {
    return proxy(ResultSet.class, new ResultSetHandler(columns, rows, statement));
  }

  /** 构造不含任何行的空结果集，用于无结果集的语句 */
  private static ResultSet emptyResultSet(Statement statement) {
    return resultSet(List.of(), List.of(), statement);
  }

  /** 处理 ResultSet 接口上的全部方法，行数据在创建时就已物化 */
  private static final class ResultSetHandler implements InvocationHandler {
    private final List<Map<String, Object>> columns;
    private final List<List<Object>> rows;
    private final Statement statement;
    private int position = -1;
    private boolean closed;
    private boolean wasNull;

    /**
     * 结果集在构造时就已把全部行物化进内存，position 初始为 -1（第一行之前）。
     *
     * @param columns   列元数据列表
     * @param rows      已物化的数据行
     * @param statement 产生本结果集的语句，用于 getStatement
     */
    ResultSetHandler(List<Map<String, Object>> columns, List<List<Object>> rows, Statement statement) {
      this.columns = columns;
      this.rows = rows;
      this.statement = statement;
    }

    /**
     * ResultSet 代理的统一入口。
     *
     * <p>实现原理：游标只是 rows 列表上的一个下标 position，next/previous/absolute
     * 等只移动下标不访问网络；取值方法统一走 value() 并按类型做字符串到数值的解析。
     *
     * @param object 代理对象本身
     * @param method 被调用的接口方法
     * @param args   调用参数，可能为 null
     * @return 方法的返回值
     * @throws Throwable 透传 SQLException 或不支持异常
     */
    @Override
    public Object invoke(Object object, Method method, Object[] args) throws Throwable {
      if (isObjectMethod(method)) return objectMethod(object, method, args, "CSUDB ResultSet[rows=" + rows.size() + "]");
      Object wrapped = wrapper(object, method, args);
      if (wrapped != null || method.getName().equals("isWrapperFor")) return wrapped;
      return switch (method.getName()) {
        case "next" -> { if (position < rows.size()) position++; yield position < rows.size(); }
        case "previous" -> { if (position >= 0) position--; yield position >= 0; }
        case "first" -> { position = rows.isEmpty() ? rows.size() : 0; yield !rows.isEmpty(); }
        case "last" -> { position = rows.isEmpty() ? rows.size() : rows.size() - 1; yield !rows.isEmpty(); }
        case "beforeFirst" -> { position = -1; yield null; }
        case "afterLast" -> { position = rows.size(); yield null; }
        case "absolute" -> moveAbsolute((Integer) args[0]);
        case "relative" -> moveAbsolute(position + 1 + (Integer) args[0]);
        case "getString" -> stringValue(value(args[0]));
        case "getObject" -> value(args[0]);
        case "getInt" -> integerValue(value(args[0]));
        case "getLong" -> longValue(value(args[0]));
        case "getFloat" -> (float) doubleValue(value(args[0]));
        case "getDouble" -> doubleValue(value(args[0]));
        case "getBigDecimal" -> new BigDecimal(stringValue(value(args[0])));
        case "getBoolean" -> booleanValue(value(args[0]));
        case "findColumn" -> columnIndex((String) args[0]) + 1;
        case "getMetaData" -> resultSetMetaData(columns);
        case "getStatement" -> statement;
        case "getRow" -> position >= 0 && position < rows.size() ? position + 1 : 0;
        case "isBeforeFirst" -> position < 0 && !rows.isEmpty();
        case "isAfterLast" -> position >= rows.size() && !rows.isEmpty();
        case "isFirst" -> position == 0 && !rows.isEmpty();
        case "isLast" -> position == rows.size() - 1 && !rows.isEmpty();
        case "wasNull" -> wasNull;
        case "close" -> { closed = true; yield null; }
        case "isClosed" -> closed;
        case "getType" -> ResultSet.TYPE_SCROLL_INSENSITIVE;
        case "getConcurrency" -> ResultSet.CONCUR_READ_ONLY;
        case "getFetchDirection" -> ResultSet.FETCH_FORWARD;
        case "setFetchDirection", "setFetchSize", "clearWarnings" -> null;
        case "getFetchSize" -> 0;
        case "getWarnings" -> null;
        case "getHoldability" -> ResultSet.CLOSE_CURSORS_AT_COMMIT;
        default -> throw unsupported(method);
      };
    }

    private Object value(Object key) throws SQLException {
      if (closed) throw new SQLException("result set is closed", "24000");
      if (position < 0 || position >= rows.size()) throw new SQLException("cursor is not on a row", "24000");
      int index = key instanceof Integer number ? number.intValue() - 1 : columnIndex(String.valueOf(key));
      if (index < 0 || index >= columns.size() || index >= rows.get(position).size()) throw new SQLException("invalid column", "07009");
      Object value = rows.get(position).get(index);
      wasNull = value == null;
      return value;
    }

    private int columnIndex(String label) throws SQLException {
      for (int i = 0; i < columns.size(); i++) {
        if (String.valueOf(columns.get(i).get("name")).equalsIgnoreCase(label)) return i;
      }
      throw new SQLException("unknown column '" + label + "'", "S0022");
    }

    private boolean moveAbsolute(int row) {
      int target = row > 0 ? row - 1 : rows.size() + row;
      position = Math.max(-1, Math.min(target, rows.size()));
      return position >= 0 && position < rows.size();
    }
  }

  /** 由列元数据构造 ResultSetMetaData 代理 */
  private static ResultSetMetaData resultSetMetaData(List<Map<String, Object>> columns) {
    return proxy(ResultSetMetaData.class, (object, method, args) -> {
      if (isObjectMethod(method)) return objectMethod(object, method, args, "CSUDB ResultSetMetaData");
      Object wrapped = wrapper(object, method, args);
      if (wrapped != null || method.getName().equals("isWrapperFor")) return wrapped;
      Map<String, Object> column = args != null && args.length > 0 && args[0] instanceof Integer index
          ? column(columns, index) : Map.of();
      String type = String.valueOf(column.getOrDefault("type", "VARCHAR"));
      return switch (method.getName()) {
        case "getColumnCount" -> columns.size();
        case "getColumnName", "getColumnLabel" -> String.valueOf(column.getOrDefault("name", ""));
        case "getColumnTypeName" -> type;
        case "getColumnType" -> sqlType(type);
        case "getColumnClassName" -> String.class.getName();
        case "getColumnDisplaySize" -> 255;
        case "getPrecision", "getScale" -> 0;
        case "isNullable" -> ResultSetMetaData.columnNullableUnknown;
        case "isAutoIncrement", "isCurrency", "isDefinitelyWritable", "isWritable" -> false;
        case "isCaseSensitive", "isSearchable", "isReadOnly" -> true;
        case "isSigned" -> sqlType(type) == Types.INTEGER || sqlType(type) == Types.FLOAT;
        case "getCatalogName", "getSchemaName", "getTableName" -> "";
        default -> throw unsupported(method);
      };
    });
  }

  /** 由连接信息构造 DatabaseMetaData 代理 */
  private static DatabaseMetaData databaseMetaData(ConnectionHandler connection) {
    return proxy(DatabaseMetaData.class, (object, method, args) -> {
      if (isObjectMethod(method)) return objectMethod(object, method, args, "CSUDB DatabaseMetaData");
      Object wrapped = wrapper(object, method, args);
      if (wrapped != null || method.getName().equals("isWrapperFor")) return wrapped;
      return switch (method.getName()) {
        case "getDatabaseProductName" -> "CSUDB 2026";
        case "getDatabaseProductVersion" -> "2026.1.0";
        case "getDriverName" -> "CSUDB Native JDBC Driver";
        case "getDriverVersion" -> "2026.1.0";
        case "getDriverMajorVersion", "getDatabaseMajorVersion" -> 2026;
        case "getDriverMinorVersion", "getDatabaseMinorVersion" -> 1;
        case "getURL" -> connection.url;
        case "getUserName" -> connection.user;
        case "getConnection" -> connection.proxy;
        case "supportsTransactions" -> true;
        case "supportsResultSetType" -> (Integer) args[0] == ResultSet.TYPE_FORWARD_ONLY || (Integer) args[0] == ResultSet.TYPE_SCROLL_INSENSITIVE;
        case "supportsResultSetConcurrency" -> (Integer) args[1] == ResultSet.CONCUR_READ_ONLY;
        case "isReadOnly" -> false;
        default -> throw unsupported(method);
      };
    });
  }

  @SuppressWarnings("unchecked")
  /** 从响应中取出列定义列表 */
  private static List<Map<String, Object>> columns(Map<String, Object> response) {
    Object value = response.get("columns");
    return value instanceof List<?> list ? (List<Map<String, Object>>) (List<?>) list : List.of();
  }

  @SuppressWarnings("unchecked")
  /** 从响应中取出数据行 */
  private static List<List<Object>> rows(Map<String, Object> response) {
    Object value = response.get("rows");
    return value instanceof List<?> list ? new ArrayList<>((List<List<Object>>) (List<?>) list) : new ArrayList<>();
  }

  /** 按 JDBC 的从 1 开始的列序号取列定义，越界时报错 */
  private static Map<String, Object> column(List<Map<String, Object>> columns, int jdbcIndex) throws SQLException {
    if (jdbcIndex <= 0 || jdbcIndex > columns.size()) throw new SQLException("invalid column index " + jdbcIndex, "07009");
    return columns.get(jdbcIndex - 1);
  }

  /** 把服务端的类型名映射成 java.sql.Types 里的整型常量 */
  private static int sqlType(String type) {
    String normalized = type.toUpperCase(Locale.ROOT);
    if (normalized.contains("INT")) return Types.INTEGER;
    if (normalized.contains("FLOAT") || normalized.contains("DOUBLE") || normalized.contains("DECIMAL")) return Types.FLOAT;
    if (normalized.contains("DATE")) return Types.DATE;
    if (normalized.contains("BOOL")) return Types.BOOLEAN;
    return Types.VARCHAR;
  }

  /**
   * 把预处理语句里的问号占位符替换成转义后的字面量。
   *
   * <p>按顺序扫描，遇到字符串字面量就整段跳过，避免把字符串里的问号误当占位符；
   * 问号个数与参数个数不一致时报错。
   */
  private static String bind(String sql, Map<Integer, Object> parameters) throws SQLException {
    StringBuilder output = new StringBuilder();
    boolean single = false;
    boolean quoted = false;
    int parameter = 1;
    for (int i = 0; i < sql.length(); i++) {
      char c = sql.charAt(i);
      if (c == '\'' && !quoted) single = !single;
      else if (c == '"' && !single) quoted = !quoted;
      if (c == '?' && !single && !quoted) {
        if (!parameters.containsKey(parameter)) throw new SQLException("parameter " + parameter + " is not set", "07001");
        output.append(literal(parameters.get(parameter++)));
      } else {
        output.append(c);
      }
    }
    if (parameters.size() >= parameter) throw new SQLException("too many parameters", "07001");
    return output.toString();
  }

  /** 判断某个方法名是否是设置参数的方法 */
  private static boolean isParameterSetter(String method) {
    return switch (method) {
      case "setNull", "setBoolean", "setByte", "setShort", "setInt", "setLong",
          "setFloat", "setDouble", "setBigDecimal", "setString", "setNString",
          "setBytes", "setDate", "setTime", "setTimestamp", "setObject" -> true;
      default -> false;
    };
  }

  /** 把 Java 值转成 SQL 字面量 */
  private static String literal(Object value) {
    if (value == null) return "NULL";
    if (value instanceof Boolean bool) return bool ? "1" : "0";
    if (value instanceof Number) return value.toString();
    if (value instanceof Date || value instanceof Time || value instanceof Timestamp) return quote(value.toString());
    if (value instanceof byte[] bytes) return quote(new String(bytes, StandardCharsets.UTF_8));
    return quote(String.valueOf(value));
  }

  /** 给字符串加引号并把内部单引号加倍 */
  private static String quote(String value) {
    return "'" + value.replace("'", "''") + "'";
  }

  /** 取字符串值，null 保持为 null */
  private static String stringValue(Object value) { return value == null ? null : String.valueOf(value); }
  /** 取整数值，null 视为 0 */
  private static int integerValue(Object value) { return value == null ? 0 : Integer.parseInt(String.valueOf(value)); }
  /** 取长整数值，null 视为 0 */
  private static long longValue(Object value) { return value == null ? 0L : Long.parseLong(String.valueOf(value)); }
  /** 取浮点值，null 视为 0 */
  private static double doubleValue(Object value) { return value == null ? 0.0 : Double.parseDouble(String.valueOf(value)); }
  /** 取布尔值，容忍字符串形式的真值 */
  private static boolean booleanValue(Object value) {
    if (value == null) return false;
    String text = String.valueOf(value);
    return text.equals("1") || text.equalsIgnoreCase("true") || text.equalsIgnoreCase("yes");
  }
}
