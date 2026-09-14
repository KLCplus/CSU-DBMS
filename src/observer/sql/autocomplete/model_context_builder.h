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

#include "sql/autocomplete/model_completion_provider.h"
#include "sql/autocomplete/sql_completion_config.h"

/**
 * @brief 为 FIM 模型构造受约束的上下文（dialect + 相关 schema）
 * @ingroup SQLAutocomplete
 */
struct ModelContext
{
  std::string dialect;
  std::string schema;
};

class SqlModelContextBuilder
{
public:
  ModelContext build(const CompletionContext &context, const SqlCompletionConfig &config) const;
};
