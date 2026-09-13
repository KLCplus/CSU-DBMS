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

#include "sql/operator/logical_operator.h"
#include "sql/parser/parse_defs.h"

/**
 * @brief 排序逻辑算子
 * @ingroup LogicalOperator
 * @details 根据 ORDER BY 表达式的值对子算子输出进行排序。
 */
class SortLogicalOperator : public LogicalOperator
{
public:
  SortLogicalOperator(vector<OrderByUnit> &&order_by_units);
  virtual ~SortLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::SORT; }
  OpType              get_op_type() const override { return OpType::LOGICALSORT; }

  auto &order_by_units() { return order_by_units_; }

private:
  // Logical Sort 保留每个表达式自己的方向，物理计划生成时整体移交所有权。
  vector<OrderByUnit> order_by_units_;
};
