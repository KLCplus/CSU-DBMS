// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  model_context_builder.h:39 ModelContext
//  model_context_builder.h:45 SqlModelContextBuilder
//  model_context_builder.h:57 build
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

#pragma once

#include <string>

#include "sql/autocomplete/model_completion_provider.h"
#include "sql/autocomplete/sql_completion_config.h"

/**
 * @file model_context_builder.h
 * @brief 为 FIM 模型构造受约束的上下文（dialect + 相关 schema）
 * @ingroup SQLAutocomplete
 *
 * 本文件把项目方言与当前语句相关的表结构整理成两段「类似 SQL 的说明文本」，
 * 作为 input_extra 注入 FIM 请求，降低模型生成未实现语法的概率。
 *
 * 核心原则：注入的内容受能力表与数量上限约束，只包含项目真实支持的语法与真实存在的表。
 */

/**
 * @brief 注入模型的上下文文本
 */
struct ModelContext
{
  std::string dialect;  ///< 方言说明（支持哪些语句/类型/布尔运算）
  std::string schema;   ///< 相关表的 CREATE TABLE 描述
};

class SqlModelContextBuilder
{
public:
  /**
   * @brief 构造模型上下文
   * @param context 补全上下文（提供语句、作用域与 Db）
   * @param config 配置（提供 max_schema_tables 等上限）
   * @return 包含 dialect 与 schema 两段文本的 ModelContext
   * @details 实现原理：dialect 依据能力表逐项拼接支持语句与类型；
   *          schema 优先取作用域内表，不足 max_schema_tables 时用 Catalog 中其余表补齐，
   *          再逐表渲染 CREATE TABLE 片段。
   */
  ModelContext build(const CompletionContext &context, const SqlCompletionConfig &config) const;
};
