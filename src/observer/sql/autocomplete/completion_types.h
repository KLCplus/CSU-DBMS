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
#include <string>
#include <vector>

/**
 * @brief SQL 输入补全的通用数据结构（与 UI 无关）
 * @ingroup SQLAutocomplete
 */

enum class CompletionKind
{
  Keyword,
  Table,
  Column,
  Alias,
  Operator,
  Type,
  Literal,
  Snippet,
  Model
};

enum class CompletionSource
{
  Grammar,
  Catalog,
  Semantic,
  Model
};

const char *completion_kind_name(CompletionKind kind);
const char *completion_source_name(CompletionSource source);

struct CompletionItem
{
  std::string      insert_text;
  std::string      display_text;
  CompletionKind   kind   = CompletionKind::Keyword;
  CompletionSource source = CompletionSource::Grammar;

  // 替换用户当前半截 token 的范围（相对于整段 SQL buffer）
  size_t replace_start = 0;
  size_t replace_end   = 0;

  double      score = 0.0;  ///< 越高越靠前
  std::string detail;       ///< 例如 "student.name : VARCHAR"
};

struct CompletionRequest
{
  std::string sql;
  size_t      cursor_offset     = 0;
  size_t      max_items         = 12;
  bool        want_model_completion = true;
};

struct CompletionResponse
{
  std::vector<CompletionItem> items;
  std::string                 ghost_text;  ///< 可为空，用于 inline ghost text
  bool                        model_used = false;
};
