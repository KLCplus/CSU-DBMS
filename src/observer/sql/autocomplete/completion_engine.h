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

#include <memory>

#include "sql/autocomplete/completion_types.h"
#include "sql/autocomplete/sql_completion_config.h"

class Db;
class ModelCompletionProvider;

/**
 * @brief 统一补全入口
 * @ingroup SQLAutocomplete
 * @details 组合确定性 Grammar/Catalog 补全与可选的模型补全。UI 只依赖本接口。
 */
class CompletionEngine
{
public:
  CompletionEngine() = default;
  explicit CompletionEngine(std::shared_ptr<ModelCompletionProvider> model) : model_(std::move(model)) {}

  CompletionResponse complete(Db *db, const CompletionRequest &request) const;

private:
  std::shared_ptr<ModelCompletionProvider> model_;
};
