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

/**
 * @file completion_engine.cpp
 * @ingroup SQLAutocomplete
 * @brief 统一补全入口的实现：确定性补全 + 可选模型增强
 *
 * 本文件串联整个补全流水线：光标/字符串注释判定 -> statement 提取 -> 复用 Parser
 * 期望集合与试探结构关键字 -> Grammar 候选 -> Catalog 候选 -> 排序裁剪 -> 模型 ghost。
 * 核心原则：语法合法性与表/列真实性分别由现有 Parser 与 Catalog 保证，模型只是增强项。
 */

namespace {

/**
 * @brief 判断字符是否属于光标处半截 token
 * @param c 待判断字符
 * @return true 表示字母数字、下划线或点
 * @details 实现原理：用 std::isalnum（先转 unsigned char）判断字母数字，
 *          再额外接受 '_' 与 '.'（'.' 用于 `table.` 限定名）。
 */
inline bool is_partial_char(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
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
 * @brief 统计当前语句中关键字的大小写风格，决定补全 keyword 的大小写
 * @param statement 当前完整 statement 文本
 * @return true 表示应以小写补全（小写关键字多于大写）
 * @details 实现原理：分词后只统计属于能力表的关键字；对每个关键字分别判断
 *          是否全大写/全小写并累计；最终比较 lower_count > upper_count。
 *          混合大小写或非关键字的 Word 不计入。
 */
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

/**
 * @brief 计算给定文本末尾的下一个行列位置（1-based）
 * @param prefix 待测文本
 * @param line 输出参数：行号，从 1 开始
 * @param column 输出参数：列号，从 1 开始
 * @return 无
 * @details 实现原理：初始 (1,1)，逐字符扫描；遇 '\n' 行号+1、列号重置为 1，否则列号+1。
 *          结果用于判断 Parser 报错位置是否正好落在追加的哨兵字符处。
 */
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

/**
 * @brief 判断 prefix 是否是一段合法前缀
 * @param prefix 待测语句前缀
 * @return true 表示 prefix 本身语法合法（语法错误只发生在末尾哨兵处）
 * @details 实现原理：在 prefix 末尾追加一个不可见哨兵字符 '\x01'，交给
 *          collect_expected_tokens 解析；若 Parser 报告的错误行/列正好等于哨兵位置，
 *          说明哨兵之前的内容全部合法；否则 prefix 内部已存在确定语法错误。
 */
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

// 通过“试探”确认某一关键字在该位置是否合法（仍然是交给同一个 Parser 判断）。
// 候选集覆盖语句级关键字与常见子句关键字；完整语句之后继续续写时，
// bison 期望集合可能为空，这些试探关键字用于补上合法续写。
const char *const kStructuralKeywords[] = {
    "SELECT", "INSERT", "DELETE", "UPDATE", "CREATE", "DROP", "SHOW", "DESC", "EXPLAIN", "ANALYZE", "SYNC", "HELP",
    "EXIT", "CALC", "LOAD", "BEGIN", "COMMIT", "ROLLBACK", "FROM", "WHERE", "GROUP", "ORDER", "JOIN", "INNER", "SET",
    "VALUES", "INTO", "TABLE", "TABLES", "AND", "OR", "NOT", "BY", "ON", "IS", "NULL", "PRIMARY", "KEY", "ASC",
};

/**
 * @brief 对候选排序并裁剪到上限
 * @param items 待处理的候选列表（原地修改）
 * @param max_items 保留的最大条数
 * @return 无
 * @details 实现原理：用 stable_sort，先按 score 降序，score 相同再按 insert_text 字典序升序；
 *          若仍超过 max_items 则 resize 截断。stable 保证同分候选的稳定性。
 */
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

/**
 * @brief 执行一次补全
 * @param db 当前数据库；可为空（此时无 Catalog 候选）
 * @param request 补全请求
 * @return 补全响应；确定性候选 + 可选模型 ghost text
 * @details 实现原理：
 *          1. 光标越界钳制；若光标在字符串/注释内，直接返回空（也不调用模型）；
 *          2. find_statement_at_cursor 取出光标所在语句；
 *          3. 从光标向左回退 is_partial_char 字符，得到半截 token 作为 partial，
 *             并切出 statement_prefix（光标前半截之前）、statement_suffix、statement_full；
 *          4. 在 statement_prefix 后追加哨兵 '\x01'，用 collect_expected_tokens 取期望集合；
 *             若报错位置不在哨兵处，说明前缀已有语法错误，直接返回空；
 *          5. 组装 GrammarCompletionContext，并对 kStructuralKeywords 逐个试探，
 *             把能续写成功的关键字放入 extra_keywords；
 *          6. 构建作用域，组装 CatalogCompletionContext，叠加表/列候选；
 *          7. sort_and_trim 排序裁剪；
 *          8. 在允许且非“唯一关键字强确定性”时调用模型，成功则填 ghost_text。
 */
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
  probe.push_back('\x01');  // 追加哨兵，让 Parser 在光标处产生语法错误
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
  // 用同一个 Parser 试探当前位置合法的结构关键字（覆盖语句已完整时的续写场景）。
  // 注意：bison 期望集合通常包含关键字，但完整语句后可能为空，需靠试探补齐。
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

  // 作用域基于完整语句（而非前缀）计算，保证光标在 FROM 之后时也能拿到表
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
  // 仅当确定性结果恰好是唯一一个关键字时（如 SEL -> SELECT、FR -> FROM）才跳过模型，
  // 其余情况（列/表候选、运算符上下文等）都允许模型给出 ghost。
  bool deterministic_strong = false;
  if (!partial.empty() && response.items.size() == 1 && response.items.front().kind == CompletionKind::Keyword) {
    deterministic_strong = true;
  }

  if (model_ != nullptr && request.want_model_completion && !statement_full.empty() && !deterministic_strong) {
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
