/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <algorithm>

#include "common/log/log.h"
#include "sql/operator/sort_physical_operator.h"
#include "sql/expr/expression.h"

using namespace std;
using namespace common;

SortPhysicalOperator::SortPhysicalOperator(vector<OrderByUnit> &&order_by_units)
    : order_by_units_(std::move(order_by_units))
{}

string SortPhysicalOperator::param() const
{
  return "";
}

RC SortPhysicalOperator::open(Trx *trx)
{
  ASSERT(children_.size() == 1, "sort operator only support one child, but got %d", children_.size());

  PhysicalOperator &child = *children_[0];
  RC                rc    = child.open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  // 一次性取出子算子的所有元组，并计算每行的排序键
  while (OB_SUCC(rc = child.next())) {
    Tuple *tuple = child.current_tuple();
    if (tuple == nullptr) {
      LOG_WARN("failed to get tuple from child operator");
      return RC::INTERNAL;
    }

    auto buffered_row = make_unique<ValueListTuple>();
    rc                = ValueListTuple::make(*tuple, *buffered_row);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to make value list tuple. rc=%s", strrc(rc));
      return rc;
    }

    vector<Value> keys;
    rc = evaluate_order_by(*tuple, keys);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to evaluate order by expressions. rc=%s", strrc(rc));
      return rc;
    }

    buffered_rows_.emplace_back(std::move(buffered_row));
    keys_.emplace_back(std::move(keys));
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to get next tuple. rc=%s", strrc(rc));
    return rc;
  }

  // 对整个结果按各排序键指定的方向执行稳定排序。
  sorted_order_.resize(keys_.size());
  for (size_t i = 0; i < sorted_order_.size(); i++) {
    sorted_order_[i] = i;
  }

  std::stable_sort(sorted_order_.begin(), sorted_order_.end(), [this](size_t a, size_t b) {
    const vector<Value> &key_a = keys_[a];
    const vector<Value> &key_b = keys_[b];
    for (size_t i = 0; i < key_a.size(); i++) {
      int cmp = key_a[i].compare(key_b[i]);
      if (cmp != 0) {
        // ORDER BY 的方向属于每个排序键，而不是整个结果集；当前键不等时立即按
        // 该键的 ASC/DESC 决定顺序，只有相等时才继续比较下一个键。
        // 不能统一 reverse，因为多列 ORDER BY 的每个键可以具有不同方向。
        return order_by_units_[i].direction == OrderDirection::ASC ? cmp < 0 : cmp > 0;
      }
    }
    return false;
  });

  cursor_        = 0;
  current_tuple_ = nullptr;

  return RC::SUCCESS;
}

RC SortPhysicalOperator::next()
{
  if (cursor_ >= sorted_order_.size()) {
    current_tuple_ = nullptr;
    return RC::RECORD_EOF;
  }

  current_tuple_ = buffered_rows_[sorted_order_[cursor_]].get();
  cursor_++;
  return RC::SUCCESS;
}

RC SortPhysicalOperator::close()
{
  // 子算子可能已经由 open 阶段开启，在 close 阶段统一关闭
  if (!children_.empty()) {
    children_[0]->close();
  }

  buffered_rows_.clear();
  keys_.clear();
  sorted_order_.clear();
  cursor_        = 0;
  current_tuple_ = nullptr;
  return RC::SUCCESS;
}

RC SortPhysicalOperator::evaluate_order_by(const Tuple &tuple, vector<Value> &keys)
{
  for (auto &order_by_unit : order_by_units_) {
    Value value;
    RC    rc = order_by_unit.expression->get_value(tuple, value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get sort key value. rc=%s", strrc(rc));
      return rc;
    }
    keys.push_back(value);
  }
  return RC::SUCCESS;
}
