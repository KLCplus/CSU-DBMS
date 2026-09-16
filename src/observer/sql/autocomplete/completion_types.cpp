/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/completion_types.h"

/**
 * @file completion_types.cpp
 * @ingroup SQLAutocomplete
 * @brief completion_types.h 中数据结构的字符串化实现
 *
 * 本文件只负责把枚举值映射为稳定的协议字符串，供 native 协议与 Web Console 消费。
 * 核心原则：映射必须一一对应且保持稳定，未知枚举值统一返回 "unknown" 而不是崩溃。
 */

/**
 * @brief 将候选种类转换为稳定的字符串名
 * @param kind 候选种类
 * @return 对应的小写字符串；未知取值返回 "unknown"
 * @details 实现原理：对 CompletionKind 做穷举 switch；
 *          所有枚举分支都返回后，末尾兜底返回 "unknown" 以防御非法取值。
 */
const char *completion_kind_name(CompletionKind kind)
{
  switch (kind) {
    case CompletionKind::Keyword: return "keyword";
    case CompletionKind::Table: return "table";
    case CompletionKind::Column: return "column";
    case CompletionKind::Alias: return "alias";
    case CompletionKind::Operator: return "operator";
    case CompletionKind::Type: return "type";
    case CompletionKind::Literal: return "literal";
    case CompletionKind::Snippet: return "snippet";
    case CompletionKind::Model: return "model";
  }
  return "unknown";
}

/**
 * @brief 将候选来源转换为稳定的字符串名
 * @param source 候选来源
 * @return 对应的小写字符串；未知取值返回 "unknown"
 * @details 实现原理：对 CompletionSource 做穷举 switch；
 *          所有枚举分支都返回后，末尾兜底返回 "unknown" 以防御非法取值。
 */
const char *completion_source_name(CompletionSource source)
{
  switch (source) {
    case CompletionSource::Grammar: return "grammar";
    case CompletionSource::Catalog: return "catalog";
    case CompletionSource::Semantic: return "semantic";
    case CompletionSource::Model: return "model";
  }
  return "unknown";
}
