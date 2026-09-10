/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC SumAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS;
  }
  if (!has_value_) {
    value_       = value;
    has_value_   = true;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  if (!has_value_) {
    result.set_null();
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}

RC CountAggregator::accumulate(const Value &value)
{
  if (!value.is_null()) {
    ++count_;
  }
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  result.set_int(count_);
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS;
  }
  sum_ += value.get_float();
  ++count_;
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  if (count_ == 0) {
    result.set_null();
  } else {
    result.set_float(sum_ / count_);
  }
  return RC::SUCCESS;
}

RC MinAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS;
  }
  if (!has_value_ || value.compare(value_) < 0) {
    value_     = value;
    has_value_ = true;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value &result)
{
  if (!has_value_) {
    result.set_null();
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS;
  }
  if (!has_value_ || value.compare(value_) > 0) {
    value_     = value;
    has_value_ = true;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value &result)
{
  if (!has_value_) {
    result.set_null();
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}
