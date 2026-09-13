/* Copyright (c) 2026 CSU-DBMS contributors. */

#include "storage/catalog/schema_catalog.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"
#include "common/value.h"
#include "storage/index/index_meta.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"

namespace {
constexpr int NAME_LENGTH = 128;

AttrInfoSqlNode attr(const char *name, AttrType type, size_t length)
{
  AttrInfoSqlNode result;
  result.name     = name;
  result.type     = type;
  result.length   = length;
  result.nullable = false;
  return result;
}

int read_int(const Record &record, const FieldMeta *field)
{
  int value = 0;
  memcpy(&value, record.data() + field->offset(), sizeof(value));
  return value;
}

string read_string(const Record &record, const FieldMeta *field)
{
  const char *data = record.data() + field->offset();
  return string(data, strnlen(data, field->len()));
}

struct PendingTable
{
  bool              has_table = false;
  string            name;
  StorageFormat     storage_format = StorageFormat::UNKNOWN_FORMAT;
  StorageEngine     storage_engine = StorageEngine::UNKNOWN_ENGINE;
  vector<pair<int, FieldMeta>> fields;
  vector<string>    primary_keys;
  vector<pair<string, string>> indexes;
};
}  // namespace

RC SchemaCatalog::bootstrap_meta(TableMeta &meta)
{
  vector<AttrInfoSqlNode> fields = {
      attr("object_type", AttrType::CHARS, 16),
      attr("table_id", AttrType::INTS, sizeof(int)),
      attr("table_name", AttrType::CHARS, NAME_LENGTH),
      attr("ordinal", AttrType::INTS, sizeof(int)),
      attr("object_name", AttrType::CHARS, NAME_LENGTH),
      attr("attr_type", AttrType::INTS, sizeof(int)),
      attr("field_offset", AttrType::INTS, sizeof(int)),
      attr("field_length", AttrType::INTS, sizeof(int)),
      attr("visible", AttrType::INTS, sizeof(int)),
      attr("nullable", AttrType::INTS, sizeof(int)),
      attr("field_id", AttrType::INTS, sizeof(int)),
      attr("primary_ordinal", AttrType::INTS, sizeof(int)),
      attr("storage_format", AttrType::INTS, sizeof(int)),
      attr("storage_engine", AttrType::INTS, sizeof(int)),
      attr("index_field", AttrType::CHARS, NAME_LENGTH),
  };
  return meta.init(TABLE_ID, TABLE_NAME, nullptr, fields, {}, StorageFormat::ROW_FORMAT, StorageEngine::HEAP);
}

bool SchemaCatalog::is_reserved_name(const char *name)
{
  return name != nullptr && strcmp(name, TABLE_NAME) == 0;
}

RC SchemaCatalog::insert_row(ObjectType object_type, int32_t table_id, const char *table_name, int ordinal,
    const char *object_name, int attr_type, int field_offset, int field_length, int visible, int nullable,
    int field_id, int primary_ordinal, int storage_format, int storage_engine, const char *index_field)
{
  const char *type_name = object_type == ObjectType::TABLE ? "TABLE" :
                          object_type == ObjectType::COLUMN ? "COLUMN" : "INDEX";
  vector<Value> values;
  values.emplace_back(type_name);
  values.emplace_back(table_id);
  values.emplace_back(table_name);
  values.emplace_back(ordinal);
  values.emplace_back(object_name);
  values.emplace_back(attr_type);
  values.emplace_back(field_offset);
  values.emplace_back(field_length);
  values.emplace_back(visible);
  values.emplace_back(nullable);
  values.emplace_back(field_id);
  values.emplace_back(primary_ordinal);
  values.emplace_back(storage_format);
  values.emplace_back(storage_engine);
  values.emplace_back(index_field);

  Record record;
  RC rc = table_->make_record(static_cast<int>(values.size()), values.data(), record);
  if (OB_SUCC(rc)) {
    rc = table_->insert_record(record);
  }
  return rc;
}

RC SchemaCatalog::register_table(const TableMeta &meta)
{
  RC rc = RC::SUCCESS;
  for (int i = 0; i < meta.field_num(); i++) {
    const FieldMeta *field = meta.field(i);
    int primary_ordinal = -1;
    for (size_t key_index = 0; key_index < meta.primary_keys().size(); key_index++) {
      if (meta.primary_keys()[key_index] == field->name()) {
        primary_ordinal = static_cast<int>(key_index);
        break;
      }
    }
    rc = insert_row(ObjectType::COLUMN, meta.table_id(), meta.name(), i, field->name(),
        static_cast<int>(field->type()), field->offset(), field->len(), field->visible(), field->nullable(),
        field->field_id(), primary_ordinal, 0, 0, "-");
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  for (int i = 0; i < meta.index_num(); i++) {
    rc = register_index(meta, *meta.index(i));
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  // The TABLE row is the commit marker. Rows left by a failed registration
  // have no effect because load_tables ignores objects without this row.
  return insert_row(ObjectType::TABLE, meta.table_id(), meta.name(), -1, "-", 0, 0, 0, 0, 0, -1, -1,
      static_cast<int>(meta.storage_format()), static_cast<int>(meta.storage_engine()), "-");
}

RC SchemaCatalog::register_index(const TableMeta &table_meta, const IndexMeta &index_meta)
{
  return insert_row(ObjectType::INDEX, table_meta.table_id(), table_meta.name(), table_meta.index_num(),
      index_meta.name(), 0, 0, 0, 0, 0, -1, 0, 0, 0, index_meta.field());
}

RC SchemaCatalog::load_tables(vector<TableMeta> &tables) const
{
  tables.clear();
  const TableMeta &catalog_meta = table_->table_meta();
  const FieldMeta *object_type = catalog_meta.field("object_type");
  const FieldMeta *table_id = catalog_meta.field("table_id");
  const FieldMeta *table_name = catalog_meta.field("table_name");
  const FieldMeta *ordinal = catalog_meta.field("ordinal");
  const FieldMeta *object_name = catalog_meta.field("object_name");
  const FieldMeta *attr_type = catalog_meta.field("attr_type");
  const FieldMeta *field_offset = catalog_meta.field("field_offset");
  const FieldMeta *field_length = catalog_meta.field("field_length");
  const FieldMeta *visible = catalog_meta.field("visible");
  const FieldMeta *nullable = catalog_meta.field("nullable");
  const FieldMeta *field_id = catalog_meta.field("field_id");
  const FieldMeta *primary_ordinal = catalog_meta.field("primary_ordinal");
  const FieldMeta *storage_format = catalog_meta.field("storage_format");
  const FieldMeta *storage_engine = catalog_meta.field("storage_engine");
  const FieldMeta *index_field = catalog_meta.field("index_field");

  unordered_map<int32_t, PendingTable> pending;
  RecordScanner *scanner = nullptr;
  RC rc = table_->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner->next(record))) {
    const int32_t id = read_int(record, table_id);
    PendingTable &item = pending[id];
    const string type = read_string(record, object_type);
    if (type == "TABLE") {
      item.has_table       = true;
      item.name            = read_string(record, table_name);
      item.storage_format  = static_cast<StorageFormat>(read_int(record, storage_format));
      item.storage_engine  = static_cast<StorageEngine>(read_int(record, storage_engine));
    } else if (type == "COLUMN") {
      FieldMeta field(read_string(record, object_name).c_str(),
          static_cast<AttrType>(read_int(record, attr_type)), read_int(record, field_offset),
          read_int(record, field_length), read_int(record, visible) != 0, read_int(record, field_id));
      field.set_nullable(read_int(record, nullable) != 0);
      item.fields.emplace_back(read_int(record, ordinal), field);
      const int key_ordinal = read_int(record, primary_ordinal);
      if (key_ordinal >= 0) {
        if (item.primary_keys.size() <= static_cast<size_t>(key_ordinal)) {
          item.primary_keys.resize(key_ordinal + 1);
        }
        item.primary_keys[key_ordinal] = field.name();
      }
    } else if (type == "INDEX") {
      item.indexes.emplace_back(read_string(record, object_name), read_string(record, index_field));
    }
  }
  RC close_rc = scanner->close_scan();
  delete scanner;
  if (rc != RC::RECORD_EOF) {
    return rc;
  }
  if (OB_FAIL(close_rc)) {
    return close_rc;
  }

  for (auto &entry : pending) {
    const int32_t id = entry.first;
    PendingTable &item = entry.second;
    if (!item.has_table || id == TABLE_ID) {
      continue;
    }
    sort(item.fields.begin(), item.fields.end(),
        [](const auto &left, const auto &right) { return left.first < right.first; });
    vector<FieldMeta> fields;
    fields.reserve(item.fields.size());
    for (const auto &field : item.fields) {
      fields.push_back(field.second);
    }

    TableMeta meta;
    rc = meta.init_from_catalog(id, item.name.c_str(), fields, item.primary_keys,
        item.storage_format, item.storage_engine);
    if (OB_FAIL(rc)) {
      return rc;
    }
    for (const auto &index : item.indexes) {
      const FieldMeta *field = meta.field(index.second.c_str());
      if (field == nullptr) {
        LOG_ERROR("Catalog index references missing field. table=%s index=%s field=%s",
            meta.name(), index.first.c_str(), index.second.c_str());
        return RC::INTERNAL;
      }
      IndexMeta index_meta;
      rc = index_meta.init(index.first.c_str(), *field);
      if (OB_FAIL(rc) || OB_FAIL(rc = meta.add_index(index_meta))) {
        return rc;
      }
    }
    tables.push_back(meta);
  }
  return RC::SUCCESS;
}
