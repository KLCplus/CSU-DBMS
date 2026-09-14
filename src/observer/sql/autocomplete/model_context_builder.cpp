/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/model_context_builder.h"

#include <sstream>

#include "common/type/attr_type.h"
#include "sql/autocomplete/sql_capabilities.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

void append_table_schema(Db *db, const std::string &table_name, std::ostringstream &out)
{
  if (db == nullptr) {
    return;
  }
  Table *table = db->find_table(table_name.c_str());
  if (table == nullptr) {
    return;
  }
  const TableMeta &table_meta = table->table_meta();
  out << "CREATE TABLE " << table->name() << " (";
  bool first = true;
  for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); ++i) {
    const FieldMeta *field = table_meta.field(i);
    if (!first) {
      out << ", ";
    }
    first = false;
    out << field->name() << ' ' << attr_type_to_sql_string(field->type());
  }
  out << ");\n";
}

ModelContext SqlModelContextBuilder::build(const CompletionContext &context, const SqlCompletionConfig &config) const
{
  ModelContext result;

  const SqlCapabilities &caps = SqlCapabilities::instance();
  std::ostringstream     dialect;
  dialect << "-- CSU_DBMS_SQL_DIALECT\n"
          << "-- Supported statements:\n"
          << "-- CREATE TABLE / INSERT INTO ... VALUES ... / SELECT ... FROM ... [WHERE ...] / DELETE FROM ... [WHERE ...]\n";
  if (caps.update) {
    dialect << "-- UPDATE ... SET ... [WHERE ...]\n";
  }
  if (caps.join) {
    dialect << "-- JOIN ... ON ...\n";
  }
  if (caps.group_by) {
    dialect << "-- GROUP BY ...\n";
  }
  if (caps.order_by) {
    dialect << "-- ORDER BY ...\n";
  }
  dialect << "-- Boolean: AND OR NOT\n-- Types:";
  for (const std::string &type : caps.types) {
    dialect << ' ' << type;
  }
  dialect << "\n-- Only use syntax listed above.\n";
  result.dialect = dialect.str();

  // schema：优先当前 scope 的表，其次 catalog 中的表（受数量限制）
  std::ostringstream schema;
  std::vector<std::string> selected;
  for (const TableBinding &binding : context.scope.tables) {
    if (static_cast<int>(selected.size()) >= config.max_schema_tables) {
      break;
    }
    selected.push_back(binding.table_name);
  }
  if (context.db != nullptr && static_cast<int>(selected.size()) < config.max_schema_tables) {
    std::vector<std::string> all;
    context.db->all_tables(all);
    for (const std::string &table : all) {
      if (static_cast<int>(selected.size()) >= config.max_schema_tables) {
        break;
      }
      bool already = false;
      for (const std::string &name : selected) {
        if (name == table) {
          already = true;
          break;
        }
      }
      if (!already) {
        selected.push_back(table);
      }
    }
  }

  for (const std::string &table : selected) {
    append_table_schema(context.db, table, schema);
  }
  result.schema = schema.str();
  return result;
}
