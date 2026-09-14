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
#include <vector>

#include "sql/autocomplete/completion_types.h"
#include "sql/autocomplete/sql_capabilities.h"

/**
 * @brief 基于 Parser 期望集合的语法补全
 * @ingroup SQLAutocomplete
 */
struct GrammarCompletionContext
{
  std::vector<std::string> expected_symbols;  ///< 来自 collect_expected_tokens
  std::vector<std::string> extra_keywords;    ///< 通过试探确认的合法关键字（已是最终文本，如 FROM）
  std::string              partial;           ///< 光标处正在输入的前缀（可为空）
  bool                     prefer_lower = false;
  size_t                   replace_begin = 0;
  size_t                   replace_end   = 0;
};

void complete_grammar(const GrammarCompletionContext &context, std::vector<CompletionItem> &out);
