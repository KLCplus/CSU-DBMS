/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/grammar_completion_provider.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace {

struct DisplayToken
{
  std::string    text;
  CompletionKind kind;
  bool           keyword_like;  ///< 需要跟随大小写风格
};

// bison 内部 terminal 名 -> 面向用户的文本
const std::unordered_map<std::string, DisplayToken> &display_table()
{
  static const std::unordered_map<std::string, DisplayToken> table = {
      // 关键字
      {"SELECT", {"SELECT", CompletionKind::Keyword, true}},
      {"FROM", {"FROM", CompletionKind::Keyword, true}},
      {"WHERE", {"WHERE", CompletionKind::Keyword, true}},
      {"AND", {"AND", CompletionKind::Keyword, true}},
      {"OR", {"OR", CompletionKind::Keyword, true}},
      {"NOT", {"NOT", CompletionKind::Keyword, true}},
      {"INSERT", {"INSERT", CompletionKind::Keyword, true}},
      {"INTO", {"INTO", CompletionKind::Keyword, true}},
      {"VALUES", {"VALUES", CompletionKind::Keyword, true}},
      {"DELETE", {"DELETE", CompletionKind::Keyword, true}},
      {"UPDATE", {"UPDATE", CompletionKind::Keyword, true}},
      {"SET", {"SET", CompletionKind::Keyword, true}},
      {"CREATE", {"CREATE", CompletionKind::Keyword, true}},
      {"DROP", {"DROP", CompletionKind::Keyword, true}},
      {"TABLE", {"TABLE", CompletionKind::Keyword, true}},
      {"TABLES", {"TABLES", CompletionKind::Keyword, true}},
      {"INDEX", {"INDEX", CompletionKind::Keyword, true}},
      {"SHOW", {"SHOW", CompletionKind::Keyword, true}},
      {"DESC", {"DESC", CompletionKind::Keyword, true}},
      {"ASC", {"ASC", CompletionKind::Keyword, true}},
      {"SYNC", {"SYNC", CompletionKind::Keyword, true}},
      {"GROUP", {"GROUP", CompletionKind::Keyword, true}},
      {"ORDER", {"ORDER", CompletionKind::Keyword, true}},
      {"BY", {"BY", CompletionKind::Keyword, true}},
      {"JOIN", {"JOIN", CompletionKind::Keyword, true}},
      {"INNER", {"INNER", CompletionKind::Keyword, true}},
      {"ON", {"ON", CompletionKind::Keyword, true}},
      {"IS", {"IS", CompletionKind::Keyword, true}},
      {"NULL_T", {"NULL", CompletionKind::Keyword, true}},
      {"PRIMARY", {"PRIMARY", CompletionKind::Keyword, true}},
      {"KEY", {"KEY", CompletionKind::Keyword, true}},
      {"ANALYZE", {"ANALYZE", CompletionKind::Keyword, true}},
      {"EXPLAIN", {"EXPLAIN", CompletionKind::Keyword, true}},
      {"HELP", {"HELP", CompletionKind::Keyword, true}},
      {"EXIT", {"EXIT", CompletionKind::Keyword, true}},
      // 类型
      {"INT_T", {"INT", CompletionKind::Type, true}},
      {"STRING_T", {"CHAR", CompletionKind::Type, true}},
      {"FLOAT_T", {"FLOAT", CompletionKind::Type, true}},
      {"VECTOR_T", {"VECTOR", CompletionKind::Type, true}},
      {"DATE", {"DATE", CompletionKind::Type, true}},
      // 分隔符 / 运算符（不跟随大小写）
      {"LBRACE", {"(", CompletionKind::Operator, false}},
      {"RBRACE", {")", CompletionKind::Operator, false}},
      {"COMMA", {",", CompletionKind::Operator, false}},
      {"SEMICOLON", {";", CompletionKind::Operator, false}},
      {"DOT", {".", CompletionKind::Operator, false}},
      {"EQ", {"=", CompletionKind::Operator, false}},
      {"NE", {"!=", CompletionKind::Operator, false}},
      {"LT", {"<", CompletionKind::Operator, false}},
      {"GT", {">", CompletionKind::Operator, false}},
      {"LE", {"<=", CompletionKind::Operator, false}},
      {"GE", {">=", CompletionKind::Operator, false}},
      {"'+'", {"+", CompletionKind::Operator, false}},
      {"'-'", {"-", CompletionKind::Operator, false}},
      {"'*'", {"*", CompletionKind::Operator, false}},
      {"'/'", {"/", CompletionKind::Operator, false}},
  };
  return table;
}

std::string to_upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string to_lower(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

bool prefix_match(const std::string &candidate, const std::string &partial, bool case_insensitive)
{
  if (partial.size() > candidate.size()) {
    return false;
  }
  if (case_insensitive) {
    return to_lower(candidate).compare(0, partial.size(), to_lower(partial)) == 0;
  }
  return candidate.compare(0, partial.size(), partial) == 0;
}

}  // namespace

void complete_grammar(const GrammarCompletionContext &context, std::vector<CompletionItem> &out)
{
  const SqlCapabilities &caps = SqlCapabilities::instance();
  const auto            &table = display_table();

  std::vector<std::string> seen;
  for (const std::string &symbol : context.expected_symbols) {
    auto iter = table.find(symbol);
    if (iter == table.end()) {
      continue;
    }
    DisplayToken token = iter->second;

    // capability 过滤
    if (token.kind == CompletionKind::Keyword || token.kind == CompletionKind::Type) {
      const std::string upper_text = to_upper(token.text);
      if (caps.is_forbidden(upper_text)) {
        continue;
      }
      if (token.kind == CompletionKind::Type && !caps.is_type(upper_text)) {
        continue;
      }
      if (token.kind == CompletionKind::Keyword && !caps.is_keyword(upper_text) && upper_text != "NULL") {
        continue;
      }
    }

    // 前缀过滤
    if (!context.partial.empty() && !prefix_match(token.text, context.partial, token.kind != CompletionKind::Operator)) {
      continue;
    }

    std::string text = token.text;
    if (token.keyword_like && context.prefer_lower) {
      text = to_lower(text);
    }

    if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
      continue;
    }
    seen.push_back(text);

    CompletionItem item;
    item.insert_text  = text;
    item.display_text = text;
    item.kind         = token.kind;
    item.source       = CompletionSource::Grammar;
    item.replace_start = context.replace_begin;
    item.replace_end   = context.replace_end;
    item.score         = 10.0;
    if (token.kind == CompletionKind::Type) {
      item.detail = "data type";
    }
    out.push_back(std::move(item));
  }

  // 试探确认的额外关键字
  for (const std::string &keyword : context.extra_keywords) {
    const std::string upper_text = to_upper(keyword);
    if (caps.is_forbidden(upper_text) || !caps.is_keyword(upper_text)) {
      continue;
    }
    if (!context.partial.empty() && !prefix_match(keyword, context.partial, true)) {
      continue;
    }
    std::string text = context.prefer_lower ? to_lower(keyword) : keyword;
    if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
      continue;
    }
    seen.push_back(text);

    CompletionItem item;
    item.insert_text   = text;
    item.display_text  = text;
    item.kind          = CompletionKind::Keyword;
    item.source        = CompletionSource::Grammar;
    item.replace_start = context.replace_begin;
    item.replace_end   = context.replace_end;
    item.score         = 12.0;  // 经过 Parser 试探确认的续写关键字优先展示
    out.push_back(std::move(item));
  }
}
