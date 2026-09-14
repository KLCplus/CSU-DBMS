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

#include "sql/optimizer/rewrite_rule.h"

/**
 * @brief 投影裁剪（Projection Pruning）
 * @ingroup Rewriter
 * @details 收集整个查询真正引用到的列，只让底层的 TableGet 读取这些列，
 *          避免读取未被使用的字段。
 */
class ProjectionPruningRule : public RewriteRule
{
public:
  ProjectionPruningRule()          = default;
  virtual ~ProjectionPruningRule() = default;

  RC rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made) override;
};
