// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_capabilities.cpp:43    build
//  sql_capabilities.cpp:79    instance
//  sql_capabilities.cpp:91    is_keyword
//  sql_capabilities.cpp:99    is_operator
//  sql_capabilities.cpp:107   is_type
//  sql_capabilities.cpp:116   is_forbidden


#include "sql/autocomplete/sql_capabilities.h"

#include <cctype>

/**
 * @file sql_capabilities.cpp
 * @ingroup SQLAutocomplete
 * @brief 运行时 SQL 能力表的构建与查询实现
 *
 * 本文件集中硬编码了当前 Parser/Executor 真实支持的关键字、运算符与类型，
 * 并提供禁用方言黑名单。它是 Grammar 补全与模型校验共同的方言门禁。
 * 核心原则：能力数据是静态常量，构建一次后只读；所有匹配都基于大写形式。
 */

namespace {

/**
 * @brief 构建并填充一张能力表
 * @return 已填充好 keywords/operators/types 的 SqlCapabilities
 * @details 实现原理：用三个栈上 C 字符串数组分别列出支持项，
 *          再逐一插入对应的 unordered_set；能力 bool 标志使用结构体默认值。
 */
SqlCapabilities build()
{
  SqlCapabilities caps;

  // 与 sql/parser/yacc_sql.y 中真实存在的 terminal 保持一致
  const char *keywords[] = {
      "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "INSERT", "INTO", "VALUES", "DELETE", "UPDATE", "SET",
      "CREATE", "DROP", "TABLE", "TABLES", "INDEX", "SHOW", "DESC", "SYNC", "GROUP", "ORDER", "BY", "JOIN",
      "INNER", "ON", "NULL", "IS", "PRIMARY", "KEY", "ANALYZE", "EXPLAIN", "LOAD", "DATA", "INFILE", "FIELDS",
      "TERMINATED", "ENCLOSED", "STORAGE", "FORMAT", "BEGIN", "COMMIT", "ROLLBACK", "CALC", "HELP", "EXIT",
      "ASC",
  };
  for (const char *keyword : keywords) {
    caps.keywords.insert(keyword);
  }

  const char *operators[] = {"=", "==", "!=", "<>", "<", "<=", ">", ">=", "+", "-", "*", "/", "(", ")", ",", ";"};
  for (const char *op : operators) {
    caps.operators.insert(op);
  }

  const char *types[] = {"INT", "CHAR", "FLOAT", "VECTOR", "DATE"};
  for (const char *type : types) {
    caps.types.insert(type);
  }

  return caps;
}

}  // namespace

/**
 * @brief 获取进程内唯一能力表实例
 * @return 能力表常量引用
 * @details 实现原理：静态局部变量在首次调用时执行 build()，C++11 保证初始化线程安全，后续直接返回引用。
 */
const SqlCapabilities &SqlCapabilities::instance()
{
  static SqlCapabilities caps = build();
  return caps;
}

/**
 * @brief 判断关键字是否受支持
 * @param upper_word 全大写关键字
 * @return true 表示在 keywords 集合中
 * @details 实现原理：直接在 keywords 集合中计数，非 0 即支持。
 */
bool SqlCapabilities::is_keyword(const std::string &upper_word) const { return keywords.count(upper_word) != 0; }

/**
 * @brief 判断运算符是否受支持
 * @param symbol 运算符字面量
 * @return true 表示在 operators 集合中
 * @details 实现原理：直接在 operators 集合中计数，非 0 即支持。
 */
bool SqlCapabilities::is_operator(const std::string &symbol) const { return operators.count(symbol) != 0; }

/**
 * @brief 判断数据类型是否受支持
 * @param upper_word 全大写类型名
 * @return true 表示在 types 集合中
 * @details 实现原理：直接在 types 集合中计数，非 0 即支持。
 */
bool SqlCapabilities::is_type(const std::string &upper_word) const { return types.count(upper_word) != 0; }

/**
 * @brief 判断单词是否属于被禁用的方言
 * @param upper_word 全大写单词
 * @return true 表示命中禁用黑名单
 * @details 实现原理：使用函数内静态 unordered_set 保存禁用词，直接计数判断；
 *          该集合覆盖本项目未实现的语法能力及未支持的列类型，用于模型输出截断。
 */
bool SqlCapabilities::is_forbidden(const std::string &upper_word) const
{
  static const std::unordered_set<std::string> forbidden = {
      "HAVING", "LIMIT", "OFFSET", "UNION", "INTERSECT", "EXCEPT", "ALTER", "TRUNCATE", "DISTINCT", "WITH",
      "WINDOW", "OVER", "PARTITION", "RECURSIVE", "CASE", "WHEN", "THEN", "ELSE", "END", "USING", "NATURAL",
      "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "EXISTS", "ANY", "ALL", "BETWEEN", "LIKE", "IN", "GRANT",
      "REVOKE", "VARCHAR", "BOOLEAN", "BOOL", "TEXT", "DECIMAL", "NUMERIC", "DOUBLE", "BIGINT", "SMALLINT",
      "TIMESTAMP", "DATETIME", "BLOB", "JSON", "AUTO_INCREMENT", "DEFAULT", "REFERENCES", "FOREIGN", "CHECK",
      "CONSTRAINT", "VIEW", "TRIGGER", "PROCEDURE", "FUNCTION", "DATABASE", "SCHEMA", "REPLACE", "MERGE",
  };
  return forbidden.count(upper_word) != 0;
}
