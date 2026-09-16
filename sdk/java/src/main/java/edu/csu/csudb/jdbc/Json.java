package edu.csu.csudb.jdbc;

import java.sql.SQLException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 极简 JSON 编解码，只覆盖本项目 Native 协议用到的类型。
 *
 * <p>刻意不引入第三方依赖，让 JDBC 驱动可以只靠 JDK 运行。
 * 支持对象、数组、字符串、数字、布尔与 null，不支持注释与尾随逗号。
 * 协议约定是「UTF-8 JSON 文本 + 一个 NUL 结尾」，本类只负责 JSON 本身。
 */
final class Json {
  /** 工具类，禁止实例化 */
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

  /**
   * 递归写出一个值。
   *
   * <p>实现原理：按运行时类型分派——String 做转义（含 Unicode 转义的控制字符）、
   * Number/Boolean 直接 append、Map 写对象、Iterable 写数组，其余一律降级为字符串。
   */
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

  /** 递归下降解析器：按当前字符决定进入哪种结构，position 为已消费偏移 */
  private static final class Parser {
    private final String text;
    private int position;

    /** 绑定待解析文本，position 从 0 开始 */
    Parser(String text) {
      this.text = text;
    }

    /**
     * 解析整个文本并返回顶层值。
     *
     * @return 顶层 JSON 值（Map / List / String / Long / Double / Boolean / null）
     * @throws SQLException 解析结束后仍有非空白字符（尾随数据）
     */
    Object parse() throws SQLException {
      Object value = value();
      whitespace();
      if (position != text.length()) fail("trailing data");
      return value;
    }

    /**
     * 读取一个值：先跳过空白，再按首字符分派到对象/数组/字符串/字面量/数字。
     *
     * @throws SQLException 输入意外结束或首字符无法识别
     */
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

    /**
     * 解析一个对象 `{...}`，用 LinkedHashMap 保留字段顺序。
     *
     * @throws SQLException 键不是字符串、缺少 ':' 或 ','、或对象未闭合
     */
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

    /**
     * 解析一个数组 `[...]`。
     *
     * @throws SQLException 缺少 ',' 或数组未闭合
     */
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

    /**
     * 解析字符串字面量，支持标准转义与 Unicode 转义。
     *
     * @throws SQLException 转义不完整、Unicode 转义后 4 位非十六进制、或字符串未闭合
     */
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

    /**
     * 解析数字：贪婪读取数字/符号/指数字符，含 '.'、'e'、'E' 时返回 Double，否则返回 Long。
     *
     * @throws SQLException 没有可解析的数字字符，或数值格式非法
     */
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

    /**
     * 匹配 true/false/null 字面量并返回对应值。
     *
     * @throws SQLException 当前位置不是期望的字面量
     */
    private Object literal(String token, Object value) throws SQLException {
      if (!text.startsWith(token, position)) return fail("invalid literal");
      position += token.length();
      return value;
    }

    /** 若当前位置字符等于 expected 则消费并返回 true，否则不动并返回 false */
    private boolean consume(char expected) {
      if (position < text.length() && text.charAt(position) == expected) {
        position++;
        return true;
      }
      return false;
    }

    /** 跳过连续的空白字符 */
    private void whitespace() {
      while (position < text.length() && Character.isWhitespace(text.charAt(position))) position++;
    }

    /**
     * 统一抛出带位置的解析错误。
     *
     * @return 永不返回；返回类型仅为便于在表达式中调用（如 `return fail(...)`）
     */
    private <T> T fail(String reason) throws SQLException {
      throw new SQLException("invalid CSUDB JSON at " + position + ": " + reason, "08S01");
    }
  }
}
