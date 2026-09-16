// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  current_statement_extractor.cpp:44 find_statement_at_cursor
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

#include "sql/autocomplete/current_statement_extractor.h"

#include <vector>

#include "sql/autocomplete/sql_text_scanner.h"

/**
 * @file current_statement_extractor.cpp
 * @ingroup SQLAutocomplete
 * @brief 光标所在 SQL statement 的提取实现
 *
 * 本文件是补全请求进入解析器之前的定位步骤，保证后续语法/目录补全只作用于单条语句。
 * 核心原则：仅根据分词器判定为 Symbol 且文本为 ";" 的 token 切分，
 * 因此字符串内或注释内的分号不会造成错误切分。
 */

/**
 * @brief 定位光标所在的单条 SQL statement
 * @param sql 完整 SQL 文本
 * @param cursor 光标偏移；越界时会被截断到 sql.size()
 * @return 光标所在 statement 的 [begin, end) 范围，found 恒为 true
 * @details 实现原理：
 *          1. cursor 越界时钳制到 sql.size()；
 *          2. scan_sql_text 对全文分词，收集所有类型为 Symbol 且文本为 ";" 的 token 起点；
 *          3. 按顺序遍历分隔符，找到第一个 >= cursor 的位置作为 end，
 *             其前一个分隔符 +1 作为 begin；找到即返回；
 *          4. 若光标位于最后一个分号之后，则 begin 为最后一个分号 +1，end 为全文末尾。
 */
StatementSlice find_statement_at_cursor(std::string_view sql, size_t cursor)
{
  StatementSlice slice;
  if (cursor > sql.size()) {
    cursor = sql.size();
  }

  std::vector<SqlTextToken> tokens;
  scan_sql_text(sql, tokens);

  // 只在字符串/注释之外的 ';' 处切分语句
  std::vector<size_t> separators;
  for (const SqlTextToken &token : tokens) {
    if (token.kind == SqlTextToken::Kind::Symbol && token.text == ";") {
      separators.push_back(token.begin);
    }
  }

  size_t begin = 0;
  for (size_t separator : separators) {
    if (cursor <= separator) {
      slice.begin = begin;
      slice.end   = separator;
      slice.found = true;
      return slice;
    }
    begin = separator + 1;
  }

  slice.begin = begin;
  slice.end   = sql.size();
  slice.found = true;
  return slice;
}
