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

#include "sql/autocomplete/model_completion_provider.h"

/**
 * @brief 模型输出校验：方言过滤 + Parser 校验 + Catalog 校验 + 最长合法前缀
 * @ingroup SQLAutocomplete
 */
struct ValidationResult
{
  bool        accepted = false;
  std::string text;    ///< 清洗/裁剪后的最终文本
  std::string reason;
};

class ModelCompletionValidator
{
public:
  ValidationResult validate(const CompletionContext &context, std::string_view model_text) const;
};
