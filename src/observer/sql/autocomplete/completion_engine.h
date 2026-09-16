// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  completion_engine.h:38     CompletionEngine
//  completion_engine.h:59     complete
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

#include <memory>

#include "sql/autocomplete/completion_types.h"
#include "sql/autocomplete/sql_completion_config.h"

class Db;
class ModelCompletionProvider;

/**
 * @file completion_engine.h
 * @brief 统一补全入口
 * @ingroup SQLAutocomplete
 *
 * 本文件是补全模块对外的总门面：把「确定性 Grammar/Catalog 补全」与「可选模型补全」
 * 组合成一次请求-响应。native 协议与 Web Console 只依赖本接口。
 *
 * 核心原则：模型是增强项。确定性补全始终先算完；模型失败/超时/校验失败时
 * 结果不受影响，不阻塞、不报错。
 */
class CompletionEngine
{
public:
  /// 默认构造：不持有模型 provider，仅提供确定性补全
  CompletionEngine() = default;

  /**
   * @brief 使用模型 provider 构造
   * @param model 模型补全 provider；可为空
   * @details 实现原理：移动构造保存 shared_ptr，生命周期由外部管理。
   */
  explicit CompletionEngine(std::shared_ptr<ModelCompletionProvider> model) : model_(std::move(model)) {}

  /**
   * @brief 执行一次补全
   * @param db 当前数据库；可为空（此时无 Catalog 候选）
   * @param request 补全请求（SQL 文本、光标、上限、是否要模型）
   * @return 补全响应（确定性候选 + 可选 ghost text）
   * @details 实现原理见 .cpp：定位 statement、复用 Parser 期望集合、试探结构关键字、
   *          叠加 Catalog 候选、排序裁剪，最后在满足条件时追加模型 ghost text。
   */
  CompletionResponse complete(Db *db, const CompletionRequest &request) const;

private:
  std::shared_ptr<ModelCompletionProvider> model_;  ///< 可选模型 provider；为空则跳过模型路径
};
