/* Copyright (c) 2021OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/insert_stmt.h"
#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "sql/parser/expression_binder.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

#include <string>
#include <strings.h>

InsertStmt::InsertStmt(Table *table, const Value *values, int value_amount) : table_(table)
{
  values_.assign(values, values + value_amount);
}

RC InsertStmt::create(Db *db, const InsertSqlNode &inserts, Stmt *&stmt)
{
  const char *table_name = inserts.relation_name.c_str();
  if (nullptr == db || nullptr == table_name || inserts.values.empty()) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, value_num=%d",
        db, table_name, static_cast<int>(inserts.values.size()));
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta  = table->table_meta();
  const int        normal_start = table_meta.sys_field_num();
  const int        field_num    = table_meta.field_num() - normal_start;
  const int        value_num    = static_cast<int>(inserts.values.size());

  // 把待插入的值按“表字段顺序”对齐：没有指定列清单时按顺序匹配；
  // 指定列清单时，未出现的列填入 NULL，由 Table::make_record 负责非空约束检查。
  vector<Value> values;
  if (inserts.columns.empty()) {
    if (field_num != value_num) {
      LOG_WARN("schema mismatch. value num=%d, field num in schema=%d", value_num, field_num);
      return RC::SCHEMA_FIELD_MISSING;
    }
    values.assign(inserts.values.begin(), inserts.values.end());
  } else {
    if (static_cast<int>(inserts.columns.size()) != value_num) {
      LOG_WARN("insert column num=%d does not match value num=%d",
          static_cast<int>(inserts.columns.size()), value_num);
      return RC::SCHEMA_FIELD_MISSING;
    }

    values.resize(field_num);
    for (Value &value : values) {
      value.set_null();
    }

    for (int i = 0; i < value_num; i++) {
      const char *column_name = inserts.columns[i].c_str();
      int         field_index = -1;
      for (int j = 0; j < field_num; j++) {
        if (0 == strcasecmp(table_meta.field(normal_start + j)->name(), column_name)) {
          field_index = j;
          break;
        }
      }
      if (field_index < 0) {
        LOG_WARN("no such column. table=%s, column=%s", table->name(), column_name);
        set_binder_error_message(std::string("SemanticError: column '") + column_name + "' does not exist in table '" +
                                 table->name() + "'.");
        return RC::SCHEMA_FIELD_MISSING;
      }
      values[field_index] = inserts.values[i];
    }
  }

  // 逐列校验值类型是否与字段类型一致（允许 INT/FLOAT 数值类型间隐式转换）
  std::string mismatch_report;
  for (int i = 0; i < field_num; i++) {
    const FieldMeta *field_meta = table_meta.field(normal_start + i);
    AttrType         field_type = field_meta->type();
    AttrType         value_type = values[i].attr_type();
    if (values[i].is_null()) {
      continue;
    }
    if (value_type == field_type) {
      continue;
    }
    bool both_numeric = is_numerical_type(value_type) && is_numerical_type(field_type);
    if (both_numeric) {
      continue;
    }
    // 允许字符串/整数字面量写入 DATE 字段，由 Table::make_record 负责转换
    if (field_type == AttrType::DATES && (value_type == AttrType::CHARS || value_type == AttrType::INTS)) {
      continue;
    }
    LOG_WARN("field %s expects type %s but value[%d] is %s",
        field_meta->name(), attr_type_to_string(field_type), i, attr_type_to_string(value_type));
    mismatch_report += std::string(table->name()) + "." + field_meta->name() + " expects " +
                       attr_type_to_sql_string(field_type) + ", but " + attr_type_to_sql_string(value_type) + " found.\n";
  }

  if (!mismatch_report.empty()) {
    // 结构化报告每一个列的类型不匹配（需求：TypeMismatch + 逐列原因）
    set_binder_error_message("TypeMismatch:\n" + mismatch_report);
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  // everything alright
  stmt = new InsertStmt(table, values.data(), field_num);
  return RC::SUCCESS;
}
