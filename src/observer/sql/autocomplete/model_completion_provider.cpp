/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/model_completion_provider.h"

#include "sql/autocomplete/llama_completion_client.h"
#include "sql/autocomplete/model_completion_validator.h"
#include "sql/autocomplete/model_context_builder.h"

ModelCompletionProvider::ModelCompletionProvider(std::shared_ptr<LlamaCompletionClient> client, SqlCompletionConfig config)
    : client_(std::move(client)), config_(std::move(config)), builder_(std::make_unique<SqlModelContextBuilder>()),
      validator_(std::make_unique<ModelCompletionValidator>())
{}

ModelCompletionProvider::~ModelCompletionProvider() = default;

bool ModelCompletionProvider::available() const
{
  return config_.model_enabled && config_.enabled && client_ != nullptr && client_->health();
}

std::optional<std::string> ModelCompletionProvider::complete(const CompletionContext &context)
{
  if (!available()) {
    return std::nullopt;
  }

  ModelContext model_context = builder_->build(context, config_);

  InfillRequest request;
  request.prefix      = context.statement_prefix;
  request.suffix      = context.statement_suffix;
  request.max_tokens  = config_.model_max_tokens;
  request.temperature = config_.model_temperature;
  request.extra.push_back({"dialect.sql", model_context.dialect});
  request.extra.push_back({"schema.sql", model_context.schema});

  std::optional<InfillResult> infill = client_->infill(request);
  if (!infill.has_value()) {
    return std::nullopt;
  }

  ValidationResult validation = validator_->validate(context, infill->text);
  if (!validation.accepted) {
    return std::nullopt;
  }
  return validation.text;
}
