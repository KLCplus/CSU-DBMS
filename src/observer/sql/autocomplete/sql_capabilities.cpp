/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/sql_capabilities.h"

#include <cctype>

namespace {

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

const SqlCapabilities &SqlCapabilities::instance()
{
  static SqlCapabilities caps = build();
  return caps;
}

bool SqlCapabilities::is_keyword(const std::string &upper_word) const { return keywords.count(upper_word) != 0; }

bool SqlCapabilities::is_operator(const std::string &symbol) const { return operators.count(symbol) != 0; }

bool SqlCapabilities::is_type(const std::string &upper_word) const { return types.count(upper_word) != 0; }

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
