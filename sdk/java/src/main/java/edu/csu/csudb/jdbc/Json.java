package edu.csu.csudb.jdbc;

import java.sql.SQLException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Minimal JSON codec for CSUDB's dependency-free Native protocol. */
/**
 * 极简 JSON 编解码，只覆盖本项目协议用到的类型。
 *
 * <p>刻意不引入第三方依赖，让 JDBC 驱动可以只靠 JDK 运行。
 * 支持对象、数组、字符串、数字、布尔与 null，不支持注释与尾随逗号。
 */
final class Json {
  private Json() {}

  /** 把 Map、List 与基本类型序列化成 JSON 文本 */
  static String stringify(Object value) {
    StringBuilder output = new StringBuilder();
    write(value, output);
    return output.toString();
  }

  /**
   * 把 JSON 文本解析成 Map。
   *
   * @throws SQLException 文本格式非法，或顶层不是对象
   */
  static Map<String, Object> parseObject(String text) throws SQLException {
    Object value = new Parser(text).parse();
    if (!(value instanceof Map<?, ?> map)) {
      throw new SQLException("invalid CSUDB response: expected JSON object", "08S01");
    }
    @SuppressWarnings("unchecked")
    Map<String, Object> result = (Map<String, Object>) map;
    return result;
  }

  /** 递归写出一个值 */
  private static void write(Object value, StringBuilder output) {
    if (value == null) {
      output.append("null");
    } else if (value instanceof String string) {
      output.append('"');
      for (int i = 0; i < string.length(); i++) {
        char c = string.charAt(i);
        switch (c) {
          case '"' -> output.append("\\\"");
          case '\\' -> output.append("\\\\");
          case '\b' -> output.append("\\b");
          case '\f' -> output.append("\\f");
          case '\n' -> output.append("\\n");
          case '\r' -> output.append("\\r");
          case '\t' -> output.append("\\t");
          default -> {
            if (c < 0x20) output.append(String.format("\\u%04x", (int) c));
            else output.append(c);
          }
        }
      }
      output.append('"');
    } else if (value instanceof Number || value instanceof Boolean) {
      output.append(value);
    } else if (value instanceof Map<?, ?> map) {
      output.append('{');
      boolean first = true;
      for (Map.Entry<?, ?> entry : map.entrySet()) {
        if (!first) output.append(',');
        first = false;
        write(String.valueOf(entry.getKey()), output);
        output.append(':');
        write(entry.getValue(), output);
      }
      output.append('}');
    } else if (value instanceof Iterable<?> values) {
      output.append('[');
      boolean first = true;
      for (Object item : values) {
        if (!first) output.append(',');
        first = false;
        write(item, output);
      }
      output.append(']');
    } else {
      write(String.valueOf(value), output);
    }
  }

  /** 递归下降解析器：按当前字符决定进入哪种结构 */
  private static final class Parser {
    private final String text;
    private int position;

    Parser(String text) {
      this.text = text;
    }

    Object parse() throws SQLException {
      Object value = value();
      whitespace();
      if (position != text.length()) fail("trailing data");
      return value;
    }

    private Object value() throws SQLException {
      whitespace();
      if (position >= text.length()) return fail("unexpected end of input");
      return switch (text.charAt(position)) {
        case '{' -> object();
        case '[' -> array();
        case '"' -> string();
        case 't' -> literal("true", Boolean.TRUE);
        case 'f' -> literal("false", Boolean.FALSE);
        case 'n' -> literal("null", null);
        default -> number();
      };
    }

    private Map<String, Object> object() throws SQLException {
      position++;
      Map<String, Object> map = new LinkedHashMap<>();
      whitespace();
      if (consume('}')) return map;
      while (true) {
        whitespace();
        if (position >= text.length() || text.charAt(position) != '"') fail("object key expected");
        String key = string();
        whitespace();
        if (!consume(':')) fail("':' expected");
        map.put(key, value());
        whitespace();
        if (consume('}')) return map;
        if (!consume(',')) fail("',' expected");
      }
    }

    private List<Object> array() throws SQLException {
      position++;
      List<Object> values = new ArrayList<>();
      whitespace();
      if (consume(']')) return values;
      while (true) {
        values.add(value());
        whitespace();
        if (consume(']')) return values;
        if (!consume(',')) fail("',' expected");
      }
    }

    private String string() throws SQLException {
      position++;
      StringBuilder output = new StringBuilder();
      while (position < text.length()) {
        char c = text.charAt(position++);
        if (c == '"') return output.toString();
        if (c != '\\') {
          output.append(c);
          continue;
        }
        if (position >= text.length()) fail("unfinished escape");
        char escaped = text.charAt(position++);
        switch (escaped) {
          case '"', '\\', '/' -> output.append(escaped);
          case 'b' -> output.append('\b');
          case 'f' -> output.append('\f');
          case 'n' -> output.append('\n');
          case 'r' -> output.append('\r');
          case 't' -> output.append('\t');
          case 'u' -> {
            if (position + 4 > text.length()) fail("invalid unicode escape");
            try {
              output.append((char) Integer.parseInt(text.substring(position, position + 4), 16));
            } catch (NumberFormatException error) {
              return fail("invalid unicode escape");
            }
            position += 4;
          }
          default -> fail("invalid escape");
        }
      }
      return fail("unterminated string");
    }

    private Object number() throws SQLException {
      int start = position;
      while (position < text.length() && "-+0123456789.eE".indexOf(text.charAt(position)) >= 0) position++;
      if (start == position) return fail("value expected");
      String number = text.substring(start, position);
      try {
        return number.indexOf('.') >= 0 || number.indexOf('e') >= 0 || number.indexOf('E') >= 0
            ? Double.parseDouble(number) : Long.parseLong(number);
      } catch (NumberFormatException error) {
        return fail("invalid number");
      }
    }

    private Object literal(String token, Object value) throws SQLException {
      if (!text.startsWith(token, position)) return fail("invalid literal");
      position += token.length();
      return value;
    }

    private boolean consume(char expected) {
      if (position < text.length() && text.charAt(position) == expected) {
        position++;
        return true;
      }
      return false;
    }

    private void whitespace() {
      while (position < text.length() && Character.isWhitespace(text.charAt(position))) position++;
    }

    private <T> T fail(String reason) throws SQLException {
      throw new SQLException("invalid CSUDB JSON at " + position + ": " + reason, "08S01");
    }
  }
}
