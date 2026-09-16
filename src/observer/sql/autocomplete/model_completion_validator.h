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
 * @file model_completion_validator.h
 * @brief 模型输出校验：方言过滤 + Parser 校验 + Catalog 校验 + 最长合法前缀
 * @ingroup SQLAutocomplete
 *
 * 本文件是模型补全的安全闸门：模型原始输出必须先清洗，再依次通过方言黑名单、
 * Parser 语法与 Catalog 标识符校验；若整体不合法则退而求其次取最长合法前缀。
 *
 * 核心原则：绝不让模型决定 SQL 是否合法；非法内容一律拒绝或截断，校验失败返回空。
 */

/**
 * @brief 模型输出校验结果
 */
struct ValidationResult
{
  bool        accepted = false;  ///< 是否接受（最终文本非空即接受）
  std::string text;              ///< 清洗/裁剪后的最终文本
  std::string reason;            ///< 拒绝原因（便于调试）
};

class ModelCompletionValidator
{
public:
  /**
   * @brief 校验并清洗模型输出
   * @param context 补全上下文（提供 statement_prefix 与 Db）
   * @param model_text 模型原始输出
   * @return 校验结果；accepted=true 时 text 为可安全展示的补全内容
   * @details 实现原理：clean 清洗 -> 长度截断 -> 截到首个分号 -> 方言黑名单截断
   *          -> Parser 校验（失败则取最长合法前缀）-> Catalog 标识符校验（失败同样取最长合法前缀）；
   *          最终文本非空则接受。
   */
  ValidationResult validate(const CompletionContext &context, std::string_view model_text) const;
};
