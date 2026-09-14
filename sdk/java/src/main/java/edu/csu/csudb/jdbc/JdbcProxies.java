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
final class JdbcProxies {
  private JdbcProxies() {}

  static Connection connection(NativeClient client, String url, String user, String database) {
    ConnectionHandler handler = new ConnectionHandler(client, url, user, database);
    Connection connection = proxy(Connection.class, handler);
    handler.proxy = connection;
    return connection;
  }

  @SuppressWarnings("unchecked")
  private static <T> T proxy(Class<T> type, InvocationHandler handler) {
    return (T) Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler);
  }

  private static Object objectMethod(Object proxy, Method method, Object[] args, String description) {
    return switch (method.getName()) {
      case "toString" -> description;
      case "hashCode" -> System.identityHashCode(proxy);
      case "equals" -> proxy == args[0];
      default -> null;
    };
  }

  private static boolean isObjectMethod(Method method) {
    return method.getDeclaringClass() == Object.class;
  }

  private static Object wrapper(Object proxy, Method method, Object[] args) throws SQLException {
    if (method.getName().equals("isWrapperFor")) return ((Class<?>) args[0]).isInstance(proxy);
    if (method.getName().equals("unwrap")) {
      Class<?> type = (Class<?>) args[0];
      if (type.isInstance(proxy)) return proxy;
      throw new SQLException("not a wrapper for " + type.getName());
    }
    return null;
  }

  private static SQLFeatureNotSupportedException unsupported(Method method) {
    return new SQLFeatureNotSupportedException("CSUDB JDBC does not yet support " + method.getName(), "0A000");
  }

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

    ConnectionHandler(NativeClient client, String url, String user, String database) {
      this.client = client;
      this.url = url;
      this.user = user;
      this.database = database;
    }

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

    private void setAutoCommit(boolean value) throws SQLException {
      ensureOpen();
      if (autoCommit == value) return;
      client.query(value ? "COMMIT;" : "BEGIN;");
      autoCommit = value;
    }

    private void setDatabase(String value) throws SQLException {
      if (value == null || !value.matches("[A-Za-z_][A-Za-z0-9_]*")) throw new SQLException("invalid database name", "3D000");
      client.query("USE " + value + ";");
      database = value;
    }

    private void ensureOpen() throws SQLException {
      if (client.isClosed()) throw new SQLException("CSUDB connection is closed", "08003");
    }
  }

  private static Statement statement(ConnectionHandler connection, String preparedSql) {
    StatementHandler handler = new StatementHandler(connection, preparedSql);
    Class<?> type = preparedSql == null ? Statement.class : PreparedStatement.class;
    Object statement = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler);
    handler.proxy = (Statement) statement;
    return handler.proxy;
  }

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

    StatementHandler(ConnectionHandler connection, String preparedSql) {
      this.connection = connection;
      this.preparedSql = preparedSql;
    }

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

    private String resolveSql(Object[] args) throws SQLException {
      if (preparedSql != null) return bind(preparedSql, parameters);
      if (args == null || args.length == 0 || !(args[0] instanceof String sql)) throw new SQLException("SQL is required", "42000");
      return sql;
    }

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

  private static ResultSet resultSet(List<Map<String, Object>> columns, List<List<Object>> rows, Statement statement) {
    return proxy(ResultSet.class, new ResultSetHandler(columns, rows, statement));
  }

  private static ResultSet emptyResultSet(Statement statement) {
    return resultSet(List.of(), List.of(), statement);
  }

  private static final class ResultSetHandler implements InvocationHandler {
    private final List<Map<String, Object>> columns;
    private final List<List<Object>> rows;
    private final Statement statement;
    private int position = -1;
    private boolean closed;
    private boolean wasNull;

    ResultSetHandler(List<Map<String, Object>> columns, List<List<Object>> rows, Statement statement) {
      this.columns = columns;
      this.rows = rows;
      this.statement = statement;
    }

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
  private static List<Map<String, Object>> columns(Map<String, Object> response) {
    Object value = response.get("columns");
    return value instanceof List<?> list ? (List<Map<String, Object>>) (List<?>) list : List.of();
  }

  @SuppressWarnings("unchecked")
  private static List<List<Object>> rows(Map<String, Object> response) {
    Object value = response.get("rows");
    return value instanceof List<?> list ? new ArrayList<>((List<List<Object>>) (List<?>) list) : new ArrayList<>();
  }

  private static Map<String, Object> column(List<Map<String, Object>> columns, int jdbcIndex) throws SQLException {
    if (jdbcIndex <= 0 || jdbcIndex > columns.size()) throw new SQLException("invalid column index " + jdbcIndex, "07009");
    return columns.get(jdbcIndex - 1);
  }

  private static int sqlType(String type) {
    String normalized = type.toUpperCase(Locale.ROOT);
    if (normalized.contains("INT")) return Types.INTEGER;
    if (normalized.contains("FLOAT") || normalized.contains("DOUBLE") || normalized.contains("DECIMAL")) return Types.FLOAT;
    if (normalized.contains("DATE")) return Types.DATE;
    if (normalized.contains("BOOL")) return Types.BOOLEAN;
    return Types.VARCHAR;
  }

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

  private static boolean isParameterSetter(String method) {
    return switch (method) {
      case "setNull", "setBoolean", "setByte", "setShort", "setInt", "setLong",
          "setFloat", "setDouble", "setBigDecimal", "setString", "setNString",
          "setBytes", "setDate", "setTime", "setTimestamp", "setObject" -> true;
      default -> false;
    };
  }

  private static String literal(Object value) {
    if (value == null) return "NULL";
    if (value instanceof Boolean bool) return bool ? "1" : "0";
    if (value instanceof Number) return value.toString();
    if (value instanceof Date || value instanceof Time || value instanceof Timestamp) return quote(value.toString());
    if (value instanceof byte[] bytes) return quote(new String(bytes, StandardCharsets.UTF_8));
    return quote(String.valueOf(value));
  }

  private static String quote(String value) {
    return "'" + value.replace("'", "''") + "'";
  }

  private static String stringValue(Object value) { return value == null ? null : String.valueOf(value); }
  private static int integerValue(Object value) { return value == null ? 0 : Integer.parseInt(String.valueOf(value)); }
  private static long longValue(Object value) { return value == null ? 0L : Long.parseLong(String.valueOf(value)); }
  private static double doubleValue(Object value) { return value == null ? 0.0 : Double.parseDouble(String.valueOf(value)); }
  private static boolean booleanValue(Object value) {
    if (value == null) return false;
    String text = String.valueOf(value);
    return text.equals("1") || text.equalsIgnoreCase("true") || text.equalsIgnoreCase("yes");
  }
}
