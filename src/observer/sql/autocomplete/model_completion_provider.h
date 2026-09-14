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
#include <optional>
#include <string>

#include "sql/autocomplete/completion_scope.h"
#include "sql/autocomplete/sql_completion_config.h"

class Db;
class LlamaCompletionClient;
class SqlModelContextBuilder;
class ModelCompletionValidator;

/**
 * @brief 送往模型的补全上下文
 * @ingroup SQLAutocomplete
 */
struct CompletionContext
{
  std::string     statement_prefix;  ///< 光标之前的当前语句
  std::string     statement_suffix;  ///< 光标之后的当前语句
  CompletionScope scope;
  Db             *db = nullptr;
  std::string     partial;
};

/**
 * @brief 基于 Qwen2.5-Coder FIM 的补全 provider
 * @ingroup SQLAutocomplete
 * @details 模型不可用/超时/校验失败时一律返回空，绝不阻塞或影响确定性补全。
 */
class ModelCompletionProvider
{
public:
  ModelCompletionProvider(std::shared_ptr<LlamaCompletionClient> client, SqlCompletionConfig config);
  ~ModelCompletionProvider();

  bool available() const;

  std::optional<std::string> complete(const CompletionContext &context);

private:
  std::shared_ptr<LlamaCompletionClient> client_;
  SqlCompletionConfig                    config_;
  std::unique_ptr<SqlModelContextBuilder> builder_;
  std::unique_ptr<ModelCompletionValidator> validator_;
};
