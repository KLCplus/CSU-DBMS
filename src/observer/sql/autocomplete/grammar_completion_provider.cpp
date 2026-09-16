// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  grammar_completion_provider.cpp:41 DisplayToken
//  grammar_completion_provider.cpp:55 display_table
//  grammar_completion_provider.cpp:126 to_upper
//  grammar_completion_provider.cpp:141 to_lower
//  grammar_completion_provider.cpp:159 prefix_match
//  grammar_completion_provider.cpp:185 complete_grammar
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

#include "sql/autocomplete/grammar_completion_provider.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

/**
 * @file grammar_completion_provider.cpp
 * @ingroup SQLAutocomplete
 * @brief 基于 Parser 期望集合的语法补全实现
 *
 * 本文件把 bison 报告的期望 terminal 映射为候选，并叠加 Parser 试探确认的续写关键字。
 * 核心原则：不维护第二套语法；合法性由同一个 Parser 判定，本文件只做映射与过滤。
 */

namespace {

/**
 * @brief 面向用户展示的候选描述
 */
struct DisplayToken
{
  std::string    text;          ///< 用户可见文本（如 "SELECT"、"("）
  CompletionKind kind;          ///< 候选种类
  bool           keyword_like;  ///< 需要跟随大小写风格
};

/**
 * @brief 获取 bison 内部 terminal 名到用户可见文本的映射表
 * @return 静态映射表引用
 * @details 实现原理：首次调用时构建一个静态 unordered_map，键为 bison 内部符号名
 *          （如 NULL_T、INT_T、STRING_T、LBRACE），值为 DisplayToken；
 *          之后的调用直接返回该表，避免重复构建。
 */
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

/**
 * @brief 将字符串逐字节转大写（ASCII）
 * @param text 输入字符串
 * @return 转换后的副本
 * @details 实现原理：复制后遍历每个 char，用 std::toupper（先转 unsigned char）转换。
 */
std::string to_upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

/**
 * @brief 将字符串逐字节转小写（ASCII）
 * @param text 输入字符串
 * @return 转换后的副本
 * @details 实现原理：复制后遍历每个 char，用 std::tolower（先转 unsigned char）转换。
 */
std::string to_lower(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

/**
 * @brief 判断 candidate 是否以 partial 为前缀
 * @param candidate 候选完整文本
 * @param partial 用户已输入前缀
 * @param case_insensitive true 时忽略大小写比较
 * @return true 表示候选匹配当前前缀
 * @details 实现原理：partial 比 candidate 长时直接返回 false；
 *          忽略大小写时先各自 to_lower 再比较前 partial.size() 个字符，否则直接比较。
 */
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

/**
 * @brief 依据期望集合与试探关键字生成语法候选
 * @param context 语法补全上下文（期望符号、试探关键字、前缀、大小写风格、替换区间）
 * @param out 输出参数；候选追加到该 vector 末尾
 * @return 无
 * @details 实现原理：
 *          1. 遍历 bison 期望符号，经 display_table 映射为用户文本，无法映射的符号直接跳过；
 *          2. 对 Keyword/Type 做 capability 过滤（禁用词、类型白名单、关键字白名单，NULL 例外）；
 *          3. 对非运算符做大小写不敏感前缀匹配（运算符按原样匹配）；
 *          4. 关键字/类型按 prefer_lower 决定大小写，运算符不转换；
 *          5. 用 seen 去重后生成 score=10.0 的 Grammar 候选；
 *          6. 追加 extra_keywords（Parser 试探确认），过滤规则同上，score=12.0 以优先展示。
 */
void complete_grammar(const GrammarCompletionContext &context, std::vector<CompletionItem> &out)
{
  const SqlCapabilities &caps = SqlCapabilities::instance();
  const auto            &table = display_table();

  std::vector<std::string> seen;
  // 期望符号是 bison 在光标处报告的全部合法 terminal，逐个映射并过滤
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
      // NULL 特殊：display 名为 NULL，但不在关键字集合中，需单独放行
      if (token.kind == CompletionKind::Keyword && !caps.is_keyword(upper_text) && upper_text != "NULL") {
        continue;
      }
    }

    // 前缀过滤（关键字/类型忽略大小写，运算符精确匹配）
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
