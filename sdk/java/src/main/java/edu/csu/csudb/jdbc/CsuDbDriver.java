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

/**
 * JDBC 4 驱动入口。
 *
 * <p>静态块把自己注册进 DriverManager，因此调用方只要把 jar 放进 classpath，
 * 无需 Class.forName 也能通过 jdbc:csudb:// 这样的 URL 自动发现本驱动。
 *
 * <p>URL 形式为 jdbc:csudb://host:port/database?user=&password=&connectTimeout=，
 * 缺省主机 127.0.0.1、端口 6789、库 sys、用户 root、超时 5000 毫秒。
 *
 * <p>核心原理：本类只负责「URL/属性解析」与「驱动注册」两件事，
 * 真正的协议通信委托给 NativeClient（Native 协议帧为 utf-8 JSON + NUL 结束符），
 * 而把 Connection/Statement/ResultSet 接口的适配交给 JdbcProxies 的动态代理。
 */
/** JDBC 4 driver backed by CSUDB's authenticated Native protocol. */
public final class CsuDbDriver implements Driver {
  public static final String URL_PREFIX = "jdbc:csudb://";

  /** 驱动加载时自行注册，实现 JDBC 4 的自动发现 */
  static {
    try {
      DriverManager.registerDriver(new CsuDbDriver());
    } catch (SQLException error) {
      throw new ExceptionInInitializerError(error);
    }
  }

  /**
   * 解析 URL 与属性并建立连接。
   *
   * <p>实现原理：先用 acceptsURL 过滤非本驱动的 URL；再从 URL 尾部按 URI 规则
   * 解析出 host/port/database，并用百分号解码读取查询串；显式传入的 Properties
   * 优先级高于查询串（putIfAbsent 只补缺省项）；最后交给 NativeClient 登录并由
   * JdbcProxies 生成 Connection 代理。
   *
   * @param url        形如 jdbc:csudb://host:port/database 的 JDBC URL
   * @param properties 连接属性（user/password/connectTimeout），可为 null
   * @return 已认证的连接代理；URL 前缀不匹配时返回 null，这是 JDBC 约定的
   *         「我不负责这个 URL」
   * @throws SQLException 端口或超时非法、无法建立连接等；解析类失败统一转成
   *         SQLNonTransientConnectionException（SQLState 08001）
   */
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

  /**
   * 判断 URL 是否归本驱动处理。
   *
   * @param url 待判断的 JDBC URL，可为 null
   * @return 当且仅当以 jdbc:csudb:// 开头时返回 true；null 返回 false
   */
  @Override
  public boolean acceptsURL(String url) {
    return url != null && url.startsWith(URL_PREFIX);
  }

  /**
   * 向调用方声明本驱动认识的三个连接属性。
   *
   * @param url        连接 URL，本实现不据其区分属性，可为 null
   * @param properties 已有的属性，用于给 user 提供当前缺省值，可为 null
   * @return 描述 user、password、connectTimeout 的 DriverPropertyInfo 数组
   */
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

  /** 主版本号，与产品版本保持一致。@return 2026 */
  @Override public int getMajorVersion() { return 2026; }
  /** 次版本号。@return 1 */
  @Override public int getMinorVersion() { return 1; }
  /**
   * 显式声明本驱动不是完全 JDBC 兼容实现。
   * @return 恒为 false，即未支持的能力会抛 SQLFeatureNotSupportedException
   */
  @Override public boolean jdbcCompliant() { return false; }
  /** 返回本驱动使用的日志器。@return 名为 edu.csu.csudb.jdbc 的 Logger */
  @Override public Logger getParentLogger() { return Logger.getLogger("edu.csu.csudb.jdbc"); }

  /**
   * URL 中的参数需要先做百分号解码。
   *
   * @param value 已按 URI 切分、可能含 %XX 转义的片段
   * @return 用 UTF-8 解码后的原文
   */
  private static String decode(String value) {
    return URLDecoder.decode(value, StandardCharsets.UTF_8);
  }
}
