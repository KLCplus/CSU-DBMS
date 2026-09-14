/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstddef>
#include <string_view>

/**
 * @brief 取出光标所在的单条 SQL statement 的范围
 * @ingroup SQLAutocomplete
 */
struct StatementSlice
{
  size_t begin = 0;
  size_t end   = 0;
  bool   found = false;
};

StatementSlice find_statement_at_cursor(std::string_view sql, size_t cursor);
