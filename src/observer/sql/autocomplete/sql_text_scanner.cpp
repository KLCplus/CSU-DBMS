/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/sql_text_scanner.h"

#include <cctype>

namespace {

inline bool is_word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
inline bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

}  // namespace

void scan_sql_text(std::string_view sql, std::vector<SqlTextToken> &tokens)
{
  tokens.clear();
  const size_t n = sql.size();
  size_t       i = 0;

  while (i < n) {
    const char c = sql[i];

    if (is_space(c)) {
      ++i;
      continue;
    }

    // 单行注释：-- 到行尾
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      size_t start = i;
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::Comment;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      tokens.push_back(std::move(token));
      continue;
    }

    // 块注释
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      size_t start = i;
      i += 2;
      bool closed = false;
      while (i < n) {
        if (sql[i] == '*' && i + 1 < n && sql[i + 1] == '/') {
          i += 2;
          closed = true;
          break;
        }
        ++i;
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::Comment;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      (void)closed;
      tokens.push_back(std::move(token));
      continue;
    }

    // 字符串：单引号或双引号，支持 ''/"\"" 与反斜杠转义
    if (c == '\'' || c == '"') {
      const char quote = c;
      size_t     start = i;
      ++i;
      while (i < n) {
        if (sql[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        if (sql[i] == quote) {
          if (i + 1 < n && sql[i + 1] == quote) {  // 连续引号转义
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::String;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      tokens.push_back(std::move(token));
      continue;
    }

    // 数字
    if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t start = i;
      while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      if (i < n && sql[i] == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) {
          ++i;
        }
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::Number;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      tokens.push_back(std::move(token));
      continue;
    }

    // 标识符 / 关键字
    if (is_word_start(c)) {
      size_t start = i;
      while (i < n && is_word_char(sql[i])) {
        ++i;
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::Word;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      tokens.push_back(std::move(token));
      continue;
    }

    // 符号（优先多字符运算符）
    {
      size_t start = i;
      if (i + 1 < n) {
        const std::string_view two = sql.substr(i, 2);
        if (two == ">=" || two == "<=" || two == "!=" || two == "<>" || two == "==") {
          i += 2;
        } else {
          ++i;
        }
      } else {
        ++i;
      }
      SqlTextToken token;
      token.kind  = SqlTextToken::Kind::Symbol;
      token.begin = start;
      token.end   = i;
      token.text  = std::string(sql.substr(start, i - start));
      tokens.push_back(std::move(token));
      continue;
    }
  }
}

bool cursor_in_string_or_comment(std::string_view sql, size_t cursor)
{
  if (cursor > sql.size()) {
    cursor = sql.size();
  }
  std::vector<SqlTextToken> tokens;
  scan_sql_text(sql, tokens);
  for (const SqlTextToken &token : tokens) {
    if (token.kind != SqlTextToken::Kind::String && token.kind != SqlTextToken::Kind::Comment) {
      continue;
    }
    bool closed = false;
    if (token.kind == SqlTextToken::Kind::String) {
      closed = token.text.size() >= 2 && token.text.front() == token.text.back();
    } else {
      closed = token.text.size() >= 4 && token.text.compare(0, 2, "/*") == 0 &&
               token.text.compare(token.text.size() - 2, 2, "*/") == 0;
      if (!closed && token.text.size() >= 2 && token.text.compare(0, 2, "--") == 0) {
        closed = false;  // 单行注释以行尾结束，光标在末尾也算在注释内
      }
    }
    if (token.begin < cursor && (closed ? cursor < token.end : cursor <= token.end)) {
      return true;
    }
    // 光标正好在注释 token 末尾（例如 "-- FRO|"）也算在注释内
    if (token.kind == SqlTextToken::Kind::Comment && cursor == token.end && token.begin < cursor) {
      return true;
    }
  }
  return false;
}
