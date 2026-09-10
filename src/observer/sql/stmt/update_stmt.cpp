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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/update_stmt.h"
#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/field/field_meta.h"

using namespace common;

UpdateStmt::UpdateStmt(Table *table, const FieldMeta *field_meta, Value value, FilterStmt *filter_stmt)
    : table_(table), field_meta_(field_meta), value_(value), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update_sql, Stmt *&stmt)
{
  const char *table_name = update_sql.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // check whether the field exists
  const TableMeta &table_meta = table->table_meta();
  const FieldMeta *field_meta = table_meta.field(update_sql.attribute_name.c_str());
  if (nullptr == field_meta) {
    LOG_WARN("no such field. table=%s, field=%s", table_name, update_sql.attribute_name.c_str());
    return RC::SCHEMA_FIELD_MISSING;
  }

  // 校验更新的值类型是否与字段类型一致（允许 INT/FLOAT 数值类型间隐式转换）
  Value    value      = update_sql.value;
  AttrType field_type = field_meta->type();
  AttrType value_type = value.attr_type();
  if (value_type != field_type) {
    bool both_numeric = is_numerical_type(value_type) && is_numerical_type(field_type);
    if (!both_numeric) {
      LOG_WARN("field %s expects type %s but value is %s",
          field_meta->name(), attr_type_to_string(field_type), attr_type_to_string(value_type));
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  // 数值类型统一转换成字段类型，便于执行期直接写入记录
  if (value.attr_type() != field_type) {
    Value real_value;
    RC    rc = Value::cast_to(value, field_type, real_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to cast value to field type. field=%s", field_meta->name());
      return rc;
    }
    value = real_value;
  }

  unordered_map<string, Table *> table_map;
  table_map.insert(pair<string, Table *>(string(table_name), table));

  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(
      db, table, &table_map, update_sql.conditions.data(), static_cast<int>(update_sql.conditions.size()), filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create filter statement. rc=%d:%s", rc, strrc(rc));
    return rc;
  }

  stmt = new UpdateStmt(table, field_meta, value, filter_stmt);
  return RC::SUCCESS;
}