/* Copyright (c) 2026 CSU-DBMS contributors. */
#pragma once

#include "common/lang/vector.h"
#include "common/sys/rc.h"
#include "storage/table/table_meta.h"

class IndexMeta;
class Table;

/**
 * Page-backed schema data dictionary for one Db.
 *
 * The catalog is one reserved heap table. Its own TableMeta is compiled into
 * the binary so opening it never recursively depends on catalog contents.
 */
class SchemaCatalog
{
public:
  static constexpr const char *TABLE_NAME = "__csudb_catalog";
  static constexpr int32_t     TABLE_ID   = 0;

  explicit SchemaCatalog(Table *table) : table_(table) {}

  static RC bootstrap_meta(TableMeta &meta);
  static bool is_reserved_name(const char *name);

  RC register_table(const TableMeta &meta);
  RC register_index(const TableMeta &table_meta, const IndexMeta &index_meta);
  RC load_tables(vector<TableMeta> &tables) const;

private:
  enum class ObjectType
  {
    TABLE,
    COLUMN,
    INDEX
  };

  RC insert_row(ObjectType object_type, int32_t table_id, const char *table_name, int ordinal,
      const char *object_name, int attr_type, int field_offset, int field_length, int visible, int nullable,
      int field_id, int primary_ordinal, int storage_format, int storage_engine, const char *index_field);

private:
  Table *table_ = nullptr;
};
