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

final class NativeClient implements AutoCloseable {
  private static final int MAX_PACKET_SIZE = 16 * 1024 * 1024;
  private final Socket socket = new Socket();
  private final InputStream input;
  private final OutputStream output;
  private boolean closed;

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

  synchronized Map<String, Object> query(String sql) throws SQLException {
    Map<String, Object> request = new LinkedHashMap<>();
    request.put("type", "query");
    request.put("sql", sql);
    return checked(request(request));
  }

  synchronized boolean ping() {
    if (closed) return false;
    try {
      return Boolean.TRUE.equals(checked(request(Map.of("type", "ping"))).get("success"));
    } catch (SQLException error) {
      return false;
    }
  }

  boolean isClosed() {
    return closed;
  }

  @Override
  public synchronized void close() {
    if (closed) return;
    try { request(Map.of("type", "logout")); } catch (SQLException ignored) {}
    closeSocket();
  }

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

  @SuppressWarnings("unchecked")
  private static Map<String, Object> checked(Map<String, Object> response) throws SQLException {
    if (Boolean.TRUE.equals(response.get("success"))) return response;
    Object rawError = response.get("error");
    Map<String, Object> error = rawError instanceof Map<?, ?> ? (Map<String, Object>) rawError : Map.of();
    int code = error.get("code") instanceof Number number ? number.intValue() : 1;
    String message = String.valueOf(error.getOrDefault("message", response.getOrDefault("message", "CSUDB request failed")));
    throw new SQLException(message, "HY000", code);
  }

  private void closeSocket() {
    closed = true;
    try { socket.close(); } catch (IOException ignored) {}
  }
}
