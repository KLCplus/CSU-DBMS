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

#include <string>
#include <string_view>
#include <vector>

#include "sql/autocomplete/completion_scope.h"
#include "sql/autocomplete/completion_types.h"

class Db;

/**
 * @brief 基于真实 Catalog 的表 / 列补全
 * @ingroup SQLAutocomplete
 */
struct CatalogCompletionContext
{
  Db                        *db = nullptr;
  std::string_view           statement_prefix;  ///< 光标之前的当前语句
  CompletionScope            scope;
  std::vector<std::string>   expected_symbols;
  std::string                partial;  ///< 正在输入的前缀（可能含 "table."）
  size_t                     replace_begin = 0;
  size_t                     replace_end   = 0;
};

void complete_catalog(const CatalogCompletionContext &context, std::vector<CompletionItem> &out);
