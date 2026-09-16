// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  NativeClient.java:35       NativeClient
//  NativeClient.java:76       query
//  NativeClient.java:84       ping
//  NativeClient.java:94       isClosed
//  NativeClient.java:100      close
//  NativeClient.java:114      request
//  NativeClient.java:141      checked
//  NativeClient.java:151      closeSocket
// ------------------------------------------------------------------------------------------------
package edu.csu.csudb.jdbc;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.sql.SQLException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Native 协议的 Java 传输层。
 *
 * <p>只做三件事：建连接、发送登录、按需收发 JSON 包。协议是
 * 「UTF-8 JSON 加一个 NUL 结尾」，与 CLI、Python SDK、Web 网关完全一致。
 *
 * <p>收发方法都加了 synchronized，因此一个连接上的请求天然串行，
 * 不会出现响应错配。出现网络错误时会主动关闭套接字，避免连接处于半开状态。
 */
final class NativeClient implements AutoCloseable {
  private static final int MAX_PACKET_SIZE = 16 * 1024 * 1024;
  private final Socket socket = new Socket();
  private final InputStream input;
  private final OutputStream output;
  private boolean closed;

  /**
   * 建立连接并立即完成登录。
   *
   * @param timeoutMs 连接与读取超时，单位毫秒
   * @param user 账号
   * @param password 口令
   * @param database 初始数据库
   * @throws SQLException 连接失败或认证失败；连接失败使用标准 SQLState 08001
   */
  NativeClient(String host, int port, int timeoutMs, String user, String password, String database)
      throws SQLException {
    try {
      socket.connect(new InetSocketAddress(host, port), timeoutMs);
      socket.setSoTimeout(timeoutMs);
      input = socket.getInputStream();
      output = socket.getOutputStream();
    } catch (IOException error) {
      try { socket.close(); } catch (IOException ignored) {}
      throw new SQLException("cannot connect to CSUDB at " + host + ":" + port + ": " + error.getMessage(), "08001", error);
    }
    try {
      Map<String, Object> login = new LinkedHashMap<>();
      login.put("type", "login");
      login.put("user", user);
      login.put("password", password);
      login.put("database", database);
      checked(request(login));
    } catch (SQLException error) {
      closeSocket();
      throw error;
    }
  }

  /** 发送一条 SQL 查询并返回响应；响应失败时抛出 SQLException */
  synchronized Map<String, Object> query(String sql) throws SQLException {
    Map<String, Object> request = new LinkedHashMap<>();
    request.put("type", "query");
    request.put("sql", sql);
    return checked(request(request));
  }

  /** 发送 ping 探测服务端是否可用，任何异常都视为不可用 */
  synchronized boolean ping() {
    if (closed) return false;
    try {
      return Boolean.TRUE.equals(checked(request(Map.of("type", "ping"))).get("success"));
    } catch (SQLException error) {
      return false;
    }
  }

  /** 连接是否已关闭 */
  boolean isClosed() {
    return closed;
  }

  /** 先尽力发送 logout，再关闭套接字；可重复调用 */
  @Override
  public synchronized void close() {
    if (closed) return;
    try { request(Map.of("type", "logout")); } catch (SQLException ignored) {}
    closeSocket();
  }

  /**
   * 发送一个请求并读回一个响应。
   *
   * <p>写入 JSON 字节后再写一个 0 字节作为结束标记；读取时逐字节累积直到遇到
   * 0 字节，并以 16 MiB 为上限防止异常响应耗尽内存。
   *
   * @throws SQLException 连接已关闭、网络出错或响应超限
   */
  private Map<String, Object> request(Map<String, Object> request) throws SQLException {
    if (closed) throw new SQLException("CSUDB connection is closed", "08003");
    byte[] bytes = Json.stringify(request).getBytes(StandardCharsets.UTF_8);
    try {
      output.write(bytes);
      output.write(0);
      output.flush();
      ByteArrayOutputStream packet = new ByteArrayOutputStream();
      while (packet.size() <= MAX_PACKET_SIZE) {
        int value = input.read();
        if (value < 0) throw new IOException("connection closed");
        if (value == 0) return Json.parseObject(packet.toString(StandardCharsets.UTF_8));
        packet.write(value);
      }
      throw new SQLException("CSUDB response exceeds safety limit", "08S01");
    } catch (IOException error) {
      closeSocket();
      throw new SQLException("CSUDB network error: " + error.getMessage(), "08S01", error);
    }
  }

  /**
   * 检查响应中的 success 字段，失败时把 error 对象翻译成 SQLException。
   *
   * <p>SQLState 固定用 HY000 表示一般错误，错误码取自服务端返回的 code。
   */
  @SuppressWarnings("unchecked")
  private static Map<String, Object> checked(Map<String, Object> response) throws SQLException {
    if (Boolean.TRUE.equals(response.get("success"))) return response;
    Object rawError = response.get("error");
    Map<String, Object> error = rawError instanceof Map<?, ?> ? (Map<String, Object>) rawError : Map.of();
    int code = error.get("code") instanceof Number number ? number.intValue() : 1;
    String message = String.valueOf(error.getOrDefault("message", response.getOrDefault("message", "CSUDB request failed")));
    throw new SQLException(message, "HY000", code);
  }

  /** 标记为已关闭并释放套接字 */
  private void closeSocket() {
    closed = true;
    try { socket.close(); } catch (IOException ignored) {}
  }
}
