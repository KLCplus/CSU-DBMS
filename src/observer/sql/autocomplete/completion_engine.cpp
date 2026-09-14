/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/completion_engine.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "sql/autocomplete/catalog_completion_provider.h"
#include "sql/autocomplete/completion_scope.h"
#include "sql/autocomplete/current_statement_extractor.h"
#include "sql/autocomplete/grammar_completion_provider.h"
#include "sql/autocomplete/sql_capabilities.h"
#include "sql/autocomplete/sql_text_scanner.h"
#include "sql/autocomplete/model_completion_provider.h"
#include "sql/parser/expected_tokens.h"

namespace {

inline bool is_partial_char(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
}

std::string to_upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

// 统计当前语句中关键字的大小写风格，决定补全 keyword 的大小写
bool prefer_lowercase(const std::string &statement)
{
  std::vector<SqlTextToken> tokens;
  scan_sql_text(statement, tokens);
  const SqlCapabilities &caps = SqlCapabilities::instance();
  int upper_count = 0;
  int lower_count = 0;
  for (const SqlTextToken &token : tokens) {
    if (token.kind != SqlTextToken::Kind::Word) {
      continue;
    }
    const std::string upper = to_upper(token.text);
    if (!caps.is_keyword(upper)) {
      continue;
    }
    bool all_upper = true;
    bool all_lower = true;
    for (char c : token.text) {
      if (std::islower(static_cast<unsigned char>(c))) {
        all_upper = false;
      } else if (std::isupper(static_cast<unsigned char>(c))) {
        all_lower = false;
      }
    }
    if (all_upper) {
      ++upper_count;
    } else if (all_lower) {
      ++lower_count;
    }
  }
  return lower_count > upper_count;
}

void compute_sentinel_location(std::string_view prefix, int &line, int &column)
{
  line   = 1;
  column = 1;
  for (char c : prefix) {
    if (c == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
}

// 判断 prefix 是否是一段合法前缀（末尾追加哨兵后，语法错误正好发生在哨兵处）
bool prefix_is_valid(const std::string &prefix)
{
  std::vector<std::string> tokens;
  int                      line = 0;
  int                      col  = 0;
  int                      sentinel_line = 0;
  int                      sentinel_col  = 0;
  compute_sentinel_location(prefix, sentinel_line, sentinel_col);
  std::string probe = prefix;
  probe.push_back('\x01');
  collect_expected_tokens(probe.c_str(), tokens, line, col);
  return line == sentinel_line && col == sentinel_col;
}

// 通过“试探”确认某一关键字在该位置是否合法（仍然是交给同一个 Parser 判断）
const char *const kStructuralKeywords[] = {
    "SELECT", "INSERT", "DELETE", "UPDATE", "CREATE", "DROP", "SHOW", "DESC", "EXPLAIN", "ANALYZE", "SYNC", "HELP",
    "EXIT", "CALC", "LOAD", "BEGIN", "COMMIT", "ROLLBACK", "FROM", "WHERE", "GROUP", "ORDER", "JOIN", "INNER", "SET",
    "VALUES", "INTO", "TABLE", "TABLES", "AND", "OR", "NOT", "BY", "ON", "IS", "NULL", "PRIMARY", "KEY", "ASC",
};

void sort_and_trim(std::vector<CompletionItem> &items, size_t max_items)
{
  std::stable_sort(items.begin(), items.end(), [](const CompletionItem &a, const CompletionItem &b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.insert_text < b.insert_text;
  });
  if (items.size() > max_items) {
    items.resize(max_items);
  }
}

}  // namespace

CompletionResponse CompletionEngine::complete(Db *db, const CompletionRequest &request) const
{
  CompletionResponse response;
  const std::string &sql = request.sql;
  size_t             cursor = std::min(request.cursor_offset, sql.size());

  // 注释 / 字符串中不补全（也不调用模型）
  if (cursor_in_string_or_comment(sql, cursor)) {
    return response;
  }

  StatementSlice slice = find_statement_at_cursor(sql, cursor);
  if (!slice.found) {
    return response;
  }

  // 计算光标处正在输入的半截 token
  size_t word_start = cursor;
  while (word_start > slice.begin && is_partial_char(sql[word_start - 1])) {
    --word_start;
  }
  const std::string partial = sql.substr(word_start, cursor - word_start);

  std::string statement_prefix = sql.substr(slice.begin, word_start - slice.begin);
  std::string statement_suffix = sql.substr(cursor, slice.end - cursor);
  std::string statement_full   = sql.substr(slice.begin, slice.end - slice.begin);
  const bool  prefer_lower     = prefer_lowercase(statement_full);

  // 复用 Parser 获取当前光标处期望的 terminal 集合
  std::vector<std::string> expected;
  int                      err_line = 0;
  int                      err_col  = 0;
  int sentinel_line = 0;
  int sentinel_col  = 0;
  compute_sentinel_location(statement_prefix, sentinel_line, sentinel_col);
  std::string probe = statement_prefix;
  probe.push_back('\x01');
  collect_expected_tokens(probe.c_str(), expected, err_line, err_col);

  const bool prefix_valid = (err_line == sentinel_line && err_col == sentinel_col);
  if (!prefix_valid) {
    // 光标之前存在确定语法错误，退化为不做补全，避免误导
    return response;
  }

  GrammarCompletionContext grammar_context;
  grammar_context.expected_symbols = expected;
  grammar_context.partial          = partial;
  grammar_context.prefer_lower     = prefer_lower;
  grammar_context.replace_begin    = word_start;
  grammar_context.replace_end      = cursor;
  // 用同一个 Parser 试探当前位置合法的结构关键字（覆盖语句已完整时的续写场景）
  for (const char *keyword : kStructuralKeywords) {
    if (!partial.empty()) {
      // 只试探可能匹配当前前缀的关键字，降低开销
      std::string keyword_lower;
      for (const char *p = keyword; *p; ++p) {
        keyword_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
      }
      std::string partial_lower = partial;
      std::transform(partial_lower.begin(), partial_lower.end(), partial_lower.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (keyword_lower.compare(0, std::min(partial_lower.size(), keyword_lower.size()), partial_lower) != 0) {
        continue;
      }
    }
    if (prefix_is_valid(statement_prefix + " " + keyword)) {
      grammar_context.extra_keywords.emplace_back(keyword);
    }
  }
  complete_grammar(grammar_context, response.items);

  CompletionScope scope;
  build_completion_scope(statement_full, scope);

  CatalogCompletionContext catalog_context;
  catalog_context.db                = db;
  catalog_context.statement_prefix  = statement_prefix;
  catalog_context.scope             = scope;
  catalog_context.expected_symbols  = expected;
  catalog_context.partial           = partial;
  catalog_context.replace_begin     = word_start;
  catalog_context.replace_end       = cursor;
  complete_catalog(catalog_context, response.items);

  sort_and_trim(response.items, request.max_items == 0 ? 12 : request.max_items);

  // 可选模型补全（增强项，失败不影响确定性结果）
  if (model_ != nullptr && request.want_model_completion && !statement_full.empty()) {
    CompletionContext model_context;
    model_context.statement_prefix = statement_prefix;
    model_context.statement_suffix = statement_suffix;
    model_context.scope            = scope;
    model_context.db               = db;
    model_context.partial          = partial;
    std::optional<std::string> ghost = model_->complete(model_context);
    if (ghost.has_value() && !ghost->empty()) {
      response.ghost_text = *ghost;
      response.model_used = true;
    }
  }

  return response;
}
