// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  model_completion_provider.cpp:39 ModelCompletionProvider
//  model_completion_provider.cpp:52 available
//  model_completion_provider.cpp:69 complete
// ------------------------------------------------------------------------------------------------
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

/**
 * @file model_completion_provider.cpp
 * @ingroup SQLAutocomplete
 * @brief 模型补全 provider 的实现
 *
 * 本文件把「配置门禁 + 健康检查 + 上下文注入 + /infill 调用 + 输出校验 + 最长合法前缀」
 * 串成一条可失败、可降级的链路。核心原则：任何一步失败都返回空，不影响确定性补全。
 */

/**
 * @brief 构造 provider
 * @param client HTTP 客户端
 * @param config 补全配置
 * @details 实现原理：移动保存 client/config，并创建 builder 与 validator 两个 unique_ptr。
 */
ModelCompletionProvider::ModelCompletionProvider(std::shared_ptr<LlamaCompletionClient> client, SqlCompletionConfig config)
    : client_(std::move(client)), config_(std::move(config)), builder_(std::make_unique<SqlModelContextBuilder>()),
      validator_(std::make_unique<ModelCompletionValidator>())
{}

/// @details 实现原理：在 .cpp 中定义为 default，以在此处见到 builder_/validator_ 的完整类型。
ModelCompletionProvider::~ModelCompletionProvider() = default;

/**
 * @brief 判断模型路径当前是否可用
 * @return true 表示配置开启、client 非空且健康
 * @details 实现原理：短路求值依次检查 model_enabled、enabled、client_ 非空、client_->health()。
 */
bool ModelCompletionProvider::available() const
{
  return config_.model_enabled && config_.enabled && client_ != nullptr && client_->health();
}

/**
 * @brief 获取模型补全文本
 * @param context 补全上下文
 * @return 校验通过时返回补全文本；任何失败返回 std::nullopt
 * @details 实现原理：
 *          1. available() 不通过直接返回空；
 *          2. builder 构造 dialect 与 schema 文本；
 *          3. 组装 InfillRequest：prefix/suffix 取 statement_prefix/suffix，
 *             max_tokens/temperature 取配置，extra 注入 dialect.sql 与 schema.sql；
 *          4. client_->infill，返回空则返回空；
 *          5. validator_->validate，未接受则返回空，否则返回清洗后的文本。
 */
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
