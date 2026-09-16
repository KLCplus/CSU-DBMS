/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/completion_scope.h"

#include <algorithm>
#include <cctype>

#include "sql/autocomplete/sql_text_scanner.h"

/**
 * @file completion_scope.cpp
 * @ingroup SQLAutocomplete
 * @brief 补全用轻量作用域的实现
 *
 * 本文件从语句前缀中保守地抽取 FROM/JOIN/INSERT INTO 引用的表，供列补全与模型上下文使用。
 * 核心原则：基于文本 token 而非 AST，无法确认时不添加；结果去重并保持首次出现顺序。
 */

namespace {

/**
 * @brief 将字符串逐字节转大写（ASCII）
 * @param text 输入字符串
 * @return 转换后的副本
 * @details 实现原理：复制后遍历每个 char，用 std::toupper（先转 unsigned char）转换。
 */
std::string upper(const std::string &text)
{
  std::string result = text;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

/**
 * @brief 判断大写单词是否为子句边界关键字
 * @param upper_word 已转大写的单词
 * @return true 表示表名收集应在此停止
 * @details 实现原理：与静态边界词表（WHERE/GROUP/ORDER/JOIN/INNER/ON/SET/VALUES/LIMIT）逐个比较。
 */
bool is_clause_boundary(const std::string &upper_word)
{
  static const char *boundaries[] = {"WHERE", "GROUP", "ORDER", "JOIN", "INNER", "ON", "SET", "VALUES", "LIMIT"};
  for (const char *b : boundaries) {
    if (upper_word == b) {
      return true;
    }
  }
  return false;
}

}  // namespace

/**
 * @brief 判断作用域中是否已包含某表
 * @param table_name 表名
 * @return true 表示包含
 * @details 实现原理：用 std::any_of 线性比对 tables 中每个 binding 的 table_name。
 */
bool CompletionScope::contains(const std::string &table_name) const
{
  return std::any_of(tables.begin(), tables.end(), [&](const TableBinding &binding) {
    return binding.table_name == table_name;
  });
}

/**
 * @brief 返回作用域内所有表名
 * @return 表名列表（保持 tables 中的顺序）
 * @details 实现原理：预分配后遍历 tables，依次取出 table_name 追加。
 */
std::vector<std::string> CompletionScope::table_names() const
{
  std::vector<std::string> names;
  names.reserve(tables.size());
  for (const TableBinding &binding : tables) {
    names.push_back(binding.table_name);
  }
  return names;
}

/**
 * @brief 从光标之前的语句前缀中保守提取作用域
 * @param statement_prefix 光标之前的当前语句文本
 * @param scope 输出参数；入口先 clear，结束时整体替换为去重结果
 * @return 无
 * @details 实现原理：
 *          1. 清空 scope，分词；
 *          2. 顺序遍历 token，只处理 Word：
 *             - INTO：取其后的第一个 Word 作为表，若没有 Word 则跳过；
 *             - FROM/JOIN：从其后的 token 收集表名，遇到子句边界词或非逗号符号即停止；
 *               FROM 允许逗号分隔多表，JOIN 只取一个；
 *          3. 最后对 scope.tables 去重（保持首次出现顺序）并 swap 回 scope。
 */
void build_completion_scope(std::string_view statement_prefix, CompletionScope &scope)
{
  scope.tables.clear();

  std::vector<SqlTextToken> tokens;
  scan_sql_text(statement_prefix, tokens);

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != SqlTextToken::Kind::Word) {
      continue;
    }
    const std::string word = upper(tokens[i].text);

    // INSERT INTO <table>
    if (word == "INTO") {
      if (i + 1 < tokens.size() && tokens[i + 1].kind == SqlTextToken::Kind::Word) {
        scope.tables.push_back({tokens[i + 1].text, ""});
      }
      continue;
    }

    if (word == "FROM" || word == "JOIN") {
      // FROM 后可能跟逗号分隔的多个表；JOIN 后只跟一个表
      for (size_t j = i + 1; j < tokens.size(); ++j) {
        if (tokens[j].kind == SqlTextToken::Kind::Word) {
          const std::string word_j = upper(tokens[j].text);
          if (is_clause_boundary(word_j)) {
            break;  // 进入下一个子句，停止收集
          }
          scope.tables.push_back({tokens[j].text, ""});
          if (word == "JOIN") {
            break;  // JOIN 只关联一个表
          }
        } else if (tokens[j].kind == SqlTextToken::Kind::Symbol) {
          if (tokens[j].text == ",") {
            continue;  // 逗号分隔的下一张表
          }
          break;
        }
      }
    }
  }

  // 去重并保持首次出现顺序
  std::vector<TableBinding> unique;
  for (const TableBinding &binding : scope.tables) {
    if (!std::any_of(unique.begin(), unique.end(), [&](const TableBinding &existing) {
          return existing.table_name == binding.table_name;
        })) {
      unique.push_back(binding);
    }
  }
  scope.tables.swap(unique);
}
