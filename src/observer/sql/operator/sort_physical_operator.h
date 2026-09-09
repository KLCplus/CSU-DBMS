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

#include "sql/operator/physical_operator.h"
#include "common/value.h"

/**
 * @brief 排序物理算子
 * @ingroup PhysicalOperator
 * @details 一次性从子算子取出所有元组，计算 ORDER BY 表达式的值作为排序键，
 * 升序排列后再逐条输出。实现为内存排序（未做外排，适用于课程场景的数据量）。
 */
class SortPhysicalOperator : public PhysicalOperator
{
public:
  SortPhysicalOperator(vector<unique_ptr<Expression>> &&order_by_exprs);
  virtual ~SortPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::SORT; }

  string name() const override { return "SORT"; }
  string param() const override;

  RC  open(Trx *trx) override;
  RC  next() override;
  RC  close() override;

  Tuple *current_tuple() override { return current_tuple_; }

private:
  RC evaluate_order_by(const Tuple &tuple, vector<Value> &keys);

  vector<unique_ptr<Expression>> order_by_exprs_;

  vector<unique_ptr<ValueListTuple>> buffered_rows_;  ///< 缓冲的所有行（值的深拷贝）
  vector<vector<Value>>              keys_;           ///< 每行的排序键
  vector<size_t>                     sorted_order_;   ///< 按排序键升序后的行序号
  size_t                             cursor_ = 0;
  Tuple                             *current_tuple_ = nullptr;
};