// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  model_completion_validator.cpp:54 to_upper
//  model_completion_validator.cpp:69 trim
//  model_completion_validator.cpp:92 clean
//  model_completion_validator.cpp:152 prefix_is_valid
//  model_completion_validator.cpp:183 catalog_identifiers_valid
//  model_completion_validator.cpp:214 longest_valid_prefix
//  model_completion_validator.cpp:250 validate
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

#include "sql/autocomplete/model_completion_validator.h"

#include <algorithm>
#include <cctype>

#include "sql/autocomplete/sql_capabilities.h"
#include "sql/autocomplete/sql_text_scanner.h"
#include "sql/parser/expected_tokens.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

/**
 * @file model_completion_validator.cpp
 * @ingroup SQLAutocomplete
 * @brief 模型输出校验与最长合法前缀裁剪的实现
 *
 * 本文件对模型原始输出做清洗、门禁与语法/目录校验，必要时裁剪为最长合法前缀。
 * 核心原则：模型输出只是候选，必须通过同一套 Parser 与 Catalog 的检验才能进入 UI。
 */

namespace {

/// 模型输出允许保留的最大字符数，超出部分直接截断
constexpr size_t MAX_MODEL_CHARS = 256;

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
 * @brief 去除字符串首尾空白
 * @param text 输入字符串
 * @return 去掉首尾空白后的子串
 * @details 实现原理：双指针从两端跳过 isspace 字符，返回中间子串。
 */
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

/**
 * @brief 清洗模型原始输出
 * @param raw 模型返回的原始字符串
 * @return 清洗后的文本
 * @details 实现原理：
 *          1. 删除所有 `<|...|>` 形式的 FIM/特殊 token（找不到闭合时删除到末尾）；
 *          2. 若包含 ``` 围栏，则按行重排，丢弃所有含 ``` 的行；
 *          3. 若以小写 "here is " 开头，则丢弃第一行解释，保留其后内容；
 *          4. 最后 trim 首尾空白。
 */
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

/**
 * @brief 判断一段前缀是否语法合法
 * @param prefix 待测文本
 * @return true 表示语法错误只发生在末尾哨兵处（prefix 本身合法）
 * @details 实现原理：末尾追加哨兵 '\x01' 后调用 collect_expected_tokens，
 *          先按 prefix 计算哨兵应处的行列，再与 Parser 报告的错误行列比较，一致即合法。
 */
bool prefix_is_valid(const std::string &prefix)
{
  std::vector<std::string> tokens;
  int                      line = 0;
  int                      col  = 0;
  std::string              probe = prefix;
  probe.push_back('\x01');  // 哨兵：强制 Parser 在末尾报错，以提取期望集合

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

/**
 * @brief 校验文本中的 `表.列` 限定名是否都真实存在
 * @param context 补全上下文（提供 Db）
 * @param text 待校验文本
 * @return true 表示所有 `表.列` 组合都存在，或没有此类组合
 * @details 实现原理：分词后滑动扫描 Word('.' )Word 三元组；对每个限定名，
 *          先在 db 中查找表，再在表 meta 中查找列，任一缺失即返回 false。
 *          db 为空时直接返回 true（无从校验）。
 */
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

/**
 * @brief 求文本的最长合法前缀
 * @param context 补全上下文
 * @param text 待裁剪文本
 * @return 最长的、拼到 statement_prefix 后语法合法且 Catalog 标识符存在的子串；找不到返回空串
 * @details 实现原理：先分词，然后从 token 数量 count 由大到小（即从末尾逐 token 回退），
 *          取 text 到第 count 个 token 末尾的子串并 trim；依次通过 prefix_is_valid 与
 *          catalog_identifiers_valid 校验，首个通过者即返回；全部失败返回空串。
 */
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

/**
 * @brief 校验并清洗模型输出
 * @param context 补全上下文（statement_prefix 与 Db）
 * @param model_text 模型原始输出
 * @return 校验结果；accepted=true 时 text 为可安全展示的补全内容
 * @details 实现原理：
 *          1. clean 清洗，空则拒绝（reason=empty after cleaning）；
 *          2. 超过 MAX_MODEL_CHARS 截断；再截到第一个分号（只取一条语句）；
 *          3. 方言过滤：遇到禁用关键字则在它之前截断，空则拒绝（blocked by dialect filter）；
 *          4. Parser 校验：prefix + text 不合法时取最长合法前缀，空则拒绝（failed parser validation）；
 *          5. Catalog 校验：`表.列` 不存在时同样取最长合法前缀，空则拒绝（failed catalog validation）；
 *          6. 通过则 accepted=true 并返回最终 text。
 */
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

  // 整体不合法时退化为最长合法前缀（可能仍对用户有用）
  if (!prefix_is_valid(context.statement_prefix + text)) {
    text = longest_valid_prefix(context, text);
    if (text.empty()) {
      result.reason = "failed parser validation";
      return result;
    }
  }

  // 再确认所有 `表.列` 限定名真实存在
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
