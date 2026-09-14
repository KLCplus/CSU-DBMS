package edu.csu.csudb.jdbc;

import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.Driver;
import java.sql.DriverManager;
import java.sql.DriverPropertyInfo;
import java.sql.SQLException;
import java.sql.SQLNonTransientConnectionException;
import java.util.Properties;
import java.util.logging.Logger;

/** JDBC 4 driver backed by CSUDB's authenticated Native protocol. */
public final class CsuDbDriver implements Driver {
  public static final String URL_PREFIX = "jdbc:csudb://";

  static {
    try {
      DriverManager.registerDriver(new CsuDbDriver());
    } catch (SQLException error) {
      throw new ExceptionInInitializerError(error);
    }
  }

  @Override
  public Connection connect(String url, Properties properties) throws SQLException {
    if (!acceptsURL(url)) return null;
    try {
      URI uri = URI.create(url.substring("jdbc:".length()));
      Properties options = new Properties();
      if (properties != null) options.putAll(properties);
      if (uri.getRawQuery() != null) {
        for (String part : uri.getRawQuery().split("&")) {
          int equals = part.indexOf('=');
          if (equals > 0) {
            options.putIfAbsent(decode(part.substring(0, equals)), decode(part.substring(equals + 1)));
          }
        }
      }
      String host = uri.getHost() == null ? "127.0.0.1" : uri.getHost();
      int port = uri.getPort() < 0 ? 6789 : uri.getPort();
      String path = uri.getPath();
      String database = path == null || path.length() <= 1 ? "sys" : decode(path.substring(1));
      String user = options.getProperty("user", "root");
      String password = options.getProperty("password", "");
      int timeout = Integer.parseInt(options.getProperty("connectTimeout", "5000"));
      if (port <= 0 || port > 65535 || timeout <= 0) throw new IllegalArgumentException("invalid port or timeout");

      NativeClient client = new NativeClient(host, port, timeout, user, password, database);
      return JdbcProxies.connection(client, url, user, database);
    } catch (SQLException error) {
      throw error;
    } catch (RuntimeException error) {
      throw new SQLNonTransientConnectionException("invalid CSUDB JDBC URL or properties: " + error.getMessage(), "08001", error);
    }
  }

  @Override
  public boolean acceptsURL(String url) {
    return url != null && url.startsWith(URL_PREFIX);
  }

  @Override
  public DriverPropertyInfo[] getPropertyInfo(String url, Properties properties) {
    DriverPropertyInfo user = new DriverPropertyInfo("user", properties == null ? "root" : properties.getProperty("user", "root"));
    user.description = "CSUDB account name";
    DriverPropertyInfo password = new DriverPropertyInfo("password", null);
    password.description = "CSUDB account password";
    DriverPropertyInfo timeout = new DriverPropertyInfo("connectTimeout", "5000");
    timeout.description = "Socket connect/read timeout in milliseconds";
    return new DriverPropertyInfo[] {user, password, timeout};
  }

  @Override public int getMajorVersion() { return 2026; }
  @Override public int getMinorVersion() { return 1; }
  @Override public boolean jdbcCompliant() { return false; }
  @Override public Logger getParentLogger() { return Logger.getLogger("edu.csu.csudb.jdbc"); }

  private static String decode(String value) {
    return URLDecoder.decode(value, StandardCharsets.UTF_8);
  }
}
