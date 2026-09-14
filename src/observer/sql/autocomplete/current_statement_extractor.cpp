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
