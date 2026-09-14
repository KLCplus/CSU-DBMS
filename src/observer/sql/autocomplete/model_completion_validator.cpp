/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/model_completion_validator.h"

#include <algorithm>
#include <cctype>

#include "sql/autocomplete/sql_capabilities.h"
#include "sql/autocomplete/sql_text_scanner.h"
#include "sql/parser/expected_tokens.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

namespace {

constexpr size_t MAX_MODEL_CHARS = 256;

std::string to_upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string trim(const std::string &text)
{
  size_t begin = 0;
  size_t end   = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// 去掉 Markdown 代码围栏、解释性前缀、FIM special token
std::string clean(const std::string &raw)
{
  std::string text = raw;

  // 去除 <|fim_*|> 等特殊 token
  size_t pos = 0;
  while ((pos = text.find("<|", pos)) != std::string::npos) {
    size_t close = text.find("|>", pos);
    if (close == std::string::npos) {
      text.erase(pos);
      break;
    }
    text.erase(pos, close + 2 - pos);
  }

  // 去掉 ```sql ... ``` 围栏
  if (text.find("```") != std::string::npos) {
    std::vector<std::string> lines;
    std::string              current;
    for (char c : text) {
      if (c == '\n') {
        lines.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) {
      lines.push_back(current);
    }
    std::string rebuilt;
    for (const std::string &line : lines) {
      if (line.find("```") != std::string::npos) {
        continue;
      }
      rebuilt += line;
      rebuilt.push_back('\n');
    }
    text = rebuilt;
  }

  // 去掉 "Here is ..." 之类解释
  std::string lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered.compare(0, 8, "here is ") == 0) {
    size_t newline = text.find('\n');
    text = newline == std::string::npos ? std::string() : text.substr(newline + 1);
  }

  return trim(text);
}

bool prefix_is_valid(const std::string &prefix)
{
  std::vector<std::string> tokens;
  int                      line = 0;
  int                      col  = 0;
  std::string              probe = prefix;
  probe.push_back('\x01');

  int sentinel_line = 1;
  int sentinel_col  = 1;
  for (char c : prefix) {
    if (c == '\n') {
      ++sentinel_line;
      sentinel_col = 1;
    } else {
      ++sentinel_col;
    }
  }
  collect_expected_tokens(probe.c_str(), tokens, line, col);
  return line == sentinel_line && col == sentinel_col;
}

bool catalog_identifiers_valid(const CompletionContext &context, const std::string &text)
{
  if (context.db == nullptr) {
    return true;
  }
  std::vector<SqlTextToken> tokens;
  scan_sql_text(text, tokens);
  for (size_t i = 0; i + 2 < tokens.size(); ++i) {
    if (tokens[i].kind == SqlTextToken::Kind::Word && tokens[i + 1].kind == SqlTextToken::Kind::Symbol &&
        tokens[i + 1].text == "." && tokens[i + 2].kind == SqlTextToken::Kind::Word) {
      Table *table = context.db->find_table(tokens[i].text.c_str());
      if (table == nullptr) {
        return false;
      }
      if (table->table_meta().field(tokens[i + 2].text.c_str()) == nullptr) {
        return false;
      }
    }
  }
  return true;
}

// 逐 token 从末尾裁剪，返回最长合法前缀
std::string longest_valid_prefix(const CompletionContext &context, const std::string &text)
{
  std::vector<SqlTextToken> tokens;
  scan_sql_text(text, tokens);
  for (size_t count = tokens.size(); count > 0; --count) {
    std::string candidate = text.substr(0, tokens[count - 1].end);
    candidate             = trim(candidate);
    if (candidate.empty()) {
      break;
    }
    if (!prefix_is_valid(context.statement_prefix + candidate)) {
      continue;
    }
    if (!catalog_identifiers_valid(context, candidate)) {
      continue;
    }
    return candidate;
  }
  return std::string();
}

}  // namespace

ValidationResult ModelCompletionValidator::validate(const CompletionContext &context, std::string_view model_text) const
{
  ValidationResult result;

  std::string text = clean(std::string(model_text));
  if (text.empty()) {
    result.reason = "empty after cleaning";
    return result;
  }

  if (text.size() > MAX_MODEL_CHARS) {
    text.resize(MAX_MODEL_CHARS);
  }
  // 到第一条语句边界为止
  const size_t semicolon = text.find(';');
  if (semicolon != std::string::npos) {
    text = text.substr(0, semicolon);
  }
  text = trim(text);

  // 方言白名单：出现禁用 keyword 则在之前截断
  const SqlCapabilities              &caps = SqlCapabilities::instance();
  std::vector<SqlTextToken>           tokens;
  scan_sql_text(text, tokens);
  for (const SqlTextToken &token : tokens) {
    if (token.kind == SqlTextToken::Kind::Word && caps.is_forbidden(to_upper(token.text))) {
      text = trim(text.substr(0, token.begin));
      break;
    }
  }
  if (text.empty()) {
    result.reason = "blocked by dialect filter";
    return result;
  }

  if (!prefix_is_valid(context.statement_prefix + text)) {
    text = longest_valid_prefix(context, text);
    if (text.empty()) {
      result.reason = "failed parser validation";
      return result;
    }
  }

  if (!catalog_identifiers_valid(context, text)) {
    text = longest_valid_prefix(context, text);
    if (text.empty()) {
      result.reason = "failed catalog validation";
      return result;
    }
  }

  result.accepted = true;
  result.text     = text;
  return result;
}
