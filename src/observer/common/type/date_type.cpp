/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstdio>

#include "common/lang/comparator.h"
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include "common/type/date_type.h"
#include "common/value.h"

int DateType::compare(const Value &left, const Value &right) const
{
  int left_val  = left.get_int();
  int right_val = right.get_int();
  return common::compare_int((void *)&left_val, (void *)&right_val);
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::DATES: {
      if (val.attr_type() == AttrType::DATES) {
        result = val;
        return RC::SUCCESS;
      }
      if (val.attr_type() == AttrType::CHARS) {
        return set_value_from_str(result, val.get_string());
      }
      if (val.attr_type() == AttrType::INTS) {
        result.set_int(val.get_int());
        result.set_type(AttrType::DATES);
        return RC::SUCCESS;
      }
    } break;
    default: {
      LOG_WARN("unsupported type %d", static_cast<int>(type));
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
  return RC::SCHEMA_FIELD_TYPE_MISMATCH;
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  // 期望格式 YYYY-MM-DD 或 YYYY-M-D，解析为整数 YYYYMMDD
  int year = 0, month = 0, day = 0;
  int used = 0;
  int n    = sscanf(data.c_str(), "%d-%d-%d%n", &year, &month, &day, &used);
  if (n != 3 || used != static_cast<int>(data.size()) || year < 0 || month < 1 || month > 12 || day < 1 || day > 31) {
    LOG_WARN("invalid date string '%s'", data.c_str());
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  val.set_int(year * 10000 + month * 100 + day);
  val.set_type(AttrType::DATES);
  return RC::SUCCESS;
}

RC DateType::to_string(const Value &val, string &result) const
{
  int date_int = val.get_int();
  int year     = date_int / 10000;
  int month    = (date_int / 100) % 100;
  int day      = date_int % 100;

  stringstream ss;
  ss << year << '-';
  if (month < 10) {
    ss << '0';
  }
  ss << month << '-';
  if (day < 10) {
    ss << '0';
  }
  ss << day;
  result = ss.str();
  return RC::SUCCESS;
}