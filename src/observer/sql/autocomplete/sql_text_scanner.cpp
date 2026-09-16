// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_text_scanner.cpp:42    is_word_start
//  sql_text_scanner.cpp:50    is_word_char
//  sql_text_scanner.cpp:58    is_space
//  sql_text_scanner.cpp:76    scan_sql_text
//  sql_text_scanner.cpp:229   cursor_in_string_or_comment
// ------------------------------------------------------------------------------------------------
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

/**
 * @file sql_text_scanner.cpp
 * @ingroup SQLAutocomplete
 * @brief 轻量 SQL 文本扫描器实现
 *
 * 本文件为补全模块提供无语法的分词能力，是 statement 提取、作用域扫描等步骤的基础。
 * 核心原则：单趟扫描、不建 AST、不做语义判断；词法边界（注释/字符串/转义）必须准确，
 * 因为补全是否可用、是否切分语句都依赖这些边界。
 */

namespace {

/**
 * @brief 判断字符是否可作为标识符/关键字的起始字符
 * @param c 待判断字符
 * @return true 表示字母或下划线
 * @details 实现原理：std::isalpha（先转 unsigned char）为真，或字符为 '_'。
 */
inline bool is_word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }

/**
 * @brief 判断字符是否可作为标识符/关键字的后续字符
 * @param c 待判断字符
 * @return true 表示字母、数字或下划线
 * @details 实现原理：std::isalnum（先转 unsigned char）为真，或字符为 '_'。
 */
inline bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

/**
 * @brief 判断字符是否为空白
 * @param c 待判断字符
 * @return true 表示空白字符
 * @details 实现原理：std::isspace（先转 unsigned char）非 0 即为空白。
 */
inline bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

}  // namespace

/**
 * @brief 扫描整段文本，按出现顺序返回 token（包含注释与字符串）
 * @param sql 待扫描的 SQL 文本
 * @param tokens 输出参数；入口先 clear，再依次追加 token
 * @return 无
 * @details 实现原理：单趟 while 扫描，每轮先跳过空白；随后按下述优先级分支处理并 continue：
 *          1. `--` 单行注释：消费到 `\n` 之前（不含换行）；
 *          2. 块注释（`/`+`*` 起、`*`+`/` 止）：消费到结束标记（未闭合则到文本末尾）；
 *          3. 单/双引号字符串：支持 `\` 转义与连续同引号转义，未闭合则到文本末尾；
 *          4. 数字：连续数字，允许一个小数点后跟数字；
 *          5. 标识符/关键字：由 is_word_start 起，连续 is_word_char；
 *          6. 符号：优先匹配多字符运算符（>= <= != <> ==），否则单字符。
 *          每个 token 都记录 [begin, end) 偏移与原始文本。
 */
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

/**
 * @brief 判断光标是否位于字符串或注释内部
 * @param sql 完整 SQL 文本
 * @param cursor 光标偏移；越界时会被截断到 sql.size()
 * @return true 表示光标在字符串或注释内
 * @details 实现原理：先整体分词，再逐一检查 String/Comment token：
 *          - 闭合判定：字符串要求首尾同为引号；块注释要求以 `/`+`*` 开头且以 `*`+`/` 结尾；
 *            单行注释以行尾结束（不视为需要结尾符的闭合区间）；
 *          - 命中判定：光标严格位于 token 内，且已闭合时用 cursor < end、未闭合时用 cursor <= end；
 *          - 额外对「光标恰好在注释 token 末尾且 token 起点在光标之前」的情形返回 true，
 *            以覆盖 `-- FRO|` 这类输入尚未敲下换行的场景。
 */
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
    // 推断该 token 是否已闭合：未闭合区间在光标位于末尾时也应视为“在其中”
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
