/* Copyright (c) 2026 CSU-DBMS contributors. */

#include <filesystem>
#include <memory>

#include "gtest/gtest.h"
#include "common/global_context.h"
#include "common/value.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "net/communicator.h"
#include "net/sql_task_handler.h"
#include "session/session.h"
#include "storage/catalog/schema_catalog.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/default/default_handler.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"

using namespace std;

namespace {
class TestCommunicator : public Communicator
{
public:
  RC read_event(SessionEvent *&) override { return RC::UNIMPLEMENTED; }
  RC write_result(SessionEvent *, bool &) override { return RC::UNIMPLEMENTED; }
};

RC run_sql(TestCommunicator &communicator, const string &sql, vector<string> *rows = nullptr)
{
  SessionEvent session_event(&communicator);
  SQLStageEvent sql_event(&session_event, sql);
  SqlTaskHandler task_handler;
  RC rc = task_handler.handle_sql(&sql_event);
  if (OB_FAIL(rc)) {
    return rc;
  }

  SqlResult *result = session_event.sql_result();
  if (!result->has_operator()) {
    return result->return_code();
  }
  rc = result->open();
  if (OB_FAIL(rc)) {
    return rc;
  }
  Tuple *tuple = nullptr;
  while (OB_SUCC(rc = result->next_tuple(tuple))) {
    if (rows != nullptr) {
      rows->push_back(tuple->to_string());
    }
  }
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  RC close_rc = result->close();
  return OB_FAIL(rc) ? rc : close_rc;
}
}  // namespace

TEST(SchemaCatalog, bootstrap_create_and_restart_without_legacy_meta)
{
  const filesystem::path directory("schema_catalog_test_db");
  filesystem::remove_all(directory);
  ASSERT_TRUE(filesystem::create_directories(directory));

  vector<AttrInfoSqlNode> attributes(2);
  attributes[0].name = "id";
  attributes[0].type = AttrType::INTS;
  attributes[0].length = sizeof(int);
  attributes[0].nullable = false;
  attributes[1].name = "name";
  attributes[1].type = AttrType::CHARS;
  attributes[1].length = 32;

  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("catalog_test", directory.c_str(), "vacuous", "disk"));
    ASSERT_NE(nullptr, db->find_table(SchemaCatalog::TABLE_NAME));
    vector<string> user_tables;
    db->all_tables(user_tables);
    EXPECT_TRUE(user_tables.empty());
    ASSERT_TRUE(filesystem::exists(table_data_file(directory.c_str(), SchemaCatalog::TABLE_NAME)));
    ASSERT_FALSE(filesystem::exists(table_meta_file(directory.c_str(), SchemaCatalog::TABLE_NAME)));
    EXPECT_EQ(RC::SCHEMA_TABLE_EXIST,
        db->create_table(SchemaCatalog::TABLE_NAME, attributes, {}));

    ASSERT_EQ(RC::SUCCESS, db->create_table("users", attributes, {"id"}));
    Table *users = db->find_table("users");
    ASSERT_NE(nullptr, users);
    ASSERT_EQ(RC::SUCCESS, users->create_index(nullptr, users->table_meta().field("id"), "idx_users_id"));

    Value values[] = {Value(7), Value("alice")};
    Record record;
    ASSERT_EQ(RC::SUCCESS, users->make_record(2, values, record));
    ASSERT_EQ(RC::SUCCESS, users->insert_record(record));
    ASSERT_EQ(RC::SUCCESS, db->sync());
  }

  // Prove that the ordinary table can now be reconstructed from catalog pages.
  ASSERT_TRUE(filesystem::remove(table_meta_file(directory.c_str(), "users")));

  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("catalog_test", directory.c_str(), "vacuous", "disk"));
    Table *users = db->find_table("users");
    ASSERT_NE(nullptr, users);
    EXPECT_EQ(2, users->table_meta().field_num());
    ASSERT_NE(nullptr, users->table_meta().field("name"));
    EXPECT_TRUE(users->table_meta().field("name")->nullable());
    ASSERT_NE(nullptr, users->table_meta().index("idx_users_id"));
    EXPECT_STREQ("id", users->table_meta().index("idx_users_id")->field());
    ASSERT_EQ(1, users->table_meta().primary_keys().size());
    EXPECT_EQ("id", users->table_meta().primary_keys()[0]);

    RecordScanner *scanner = nullptr;
    ASSERT_EQ(RC::SUCCESS, users->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY));
    Record record;
    ASSERT_EQ(RC::SUCCESS, scanner->next(record));
    int id = 0;
    memcpy(&id, record.data() + users->table_meta().field("id")->offset(), sizeof(id));
    EXPECT_EQ(7, id);
    EXPECT_EQ(RC::RECORD_EOF, scanner->next(record));
    EXPECT_EQ(RC::SUCCESS, scanner->close_scan());
    delete scanner;
  }

  filesystem::remove_all(directory);
}

TEST(SchemaCatalog, migrates_legacy_metadata_then_becomes_catalog_only)
{
  const filesystem::path directory("schema_catalog_migration_test_db");
  filesystem::remove_all(directory);
  ASSERT_TRUE(filesystem::create_directories(directory));

  AttrInfoSqlNode attribute;
  attribute.name = "id";
  attribute.type = AttrType::INTS;
  attribute.length = sizeof(int);
  attribute.nullable = false;

  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("migration_test", directory.c_str(), "vacuous", "disk"));
    ASSERT_EQ(RC::SUCCESS, db->create_table("legacy_table", span<const AttrInfoSqlNode>(&attribute, 1), {}));
    ASSERT_EQ(RC::SUCCESS, db->sync());
  }

  // Simulate a pre-catalog database: legacy metadata remains, catalog pages do not.
  ASSERT_TRUE(filesystem::remove(table_data_file(directory.c_str(), SchemaCatalog::TABLE_NAME)));
  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("migration_test", directory.c_str(), "vacuous", "disk"));
    ASSERT_NE(nullptr, db->find_table("legacy_table"));
    ASSERT_EQ(RC::SUCCESS, db->sync());
  }

  ASSERT_TRUE(filesystem::remove(table_meta_file(directory.c_str(), "legacy_table")));
  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("migration_test", directory.c_str(), "vacuous", "disk"));
    ASSERT_NE(nullptr, db->find_table("legacy_table"));
  }

  filesystem::remove_all(directory);
}

TEST(SchemaCatalog, preserves_mvcc_system_fields)
{
  const filesystem::path directory("schema_catalog_mvcc_test_db");
  filesystem::remove_all(directory);
  ASSERT_TRUE(filesystem::create_directories(directory));

  AttrInfoSqlNode attribute;
  attribute.name = "id";
  attribute.type = AttrType::INTS;
  attribute.length = sizeof(int);

  int system_field_count = 0;
  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("mvcc_catalog_test", directory.c_str(), "mvcc", "disk"));
    ASSERT_EQ(RC::SUCCESS, db->create_table("mvcc_table", span<const AttrInfoSqlNode>(&attribute, 1), {}));
    system_field_count = db->find_table("mvcc_table")->table_meta().sys_field_num();
    ASSERT_GT(system_field_count, 0);
    ASSERT_EQ(RC::SUCCESS, db->sync());
  }

  ASSERT_TRUE(filesystem::remove(table_meta_file(directory.c_str(), "mvcc_table")));
  {
    auto db = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db->init("mvcc_catalog_test", directory.c_str(), "mvcc", "disk"));
    const TableMeta &meta = db->find_table("mvcc_table")->table_meta();
    EXPECT_EQ(system_field_count, meta.sys_field_num());
    EXPECT_EQ(system_field_count + 1, meta.field_num());
    for (int i = 0; i < system_field_count; i++) {
      EXPECT_FALSE(meta.field(i)->visible());
    }
  }

  filesystem::remove_all(directory);
}

TEST(SchemaCatalog, sql_select_after_restart_without_table_file)
{
  const filesystem::path directory("schema_catalog_sql_restart_test_db");
  filesystem::remove_all(directory);
  ASSERT_TRUE(filesystem::create_directories(directory));

  {
    DefaultHandler handler;
    GCTX.handler_ = &handler;
    ASSERT_EQ(RC::SUCCESS, handler.init(directory.c_str(), "vacuous", "disk", "heap"));
    TestCommunicator communicator;
    ASSERT_EQ(RC::SUCCESS,
        communicator.init(-1, make_unique<Session>(Session::default_session()), "schema-catalog-test"));
    Session::set_current_session(communicator.session());
    ASSERT_EQ(RC::SUCCESS, run_sql(communicator, "create table catalog_sql_table(id int, name char(16));"));
    ASSERT_EQ(RC::SUCCESS, run_sql(communicator, "insert into catalog_sql_table values(9, 'persisted');"));
    ASSERT_EQ(RC::SUCCESS, handler.sync());
    Session::set_current_session(nullptr);
  }
  GCTX.handler_ = nullptr;

  const filesystem::path db_path = directory / "db" / "sys";
  ASSERT_TRUE(filesystem::remove(table_meta_file(db_path.c_str(), "catalog_sql_table")));

  {
    DefaultHandler handler;
    GCTX.handler_ = &handler;
    ASSERT_EQ(RC::SUCCESS, handler.init(directory.c_str(), "vacuous", "disk", "heap"));
    TestCommunicator communicator;
    ASSERT_EQ(RC::SUCCESS,
        communicator.init(-1, make_unique<Session>(Session::default_session()), "schema-catalog-test"));
    Session::set_current_session(communicator.session());
    vector<string> rows;
    ASSERT_EQ(RC::SUCCESS, run_sql(communicator, "select * from catalog_sql_table where id = 9;", &rows));
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ("9, persisted", rows[0]);
    Session::set_current_session(nullptr);
  }
  GCTX.handler_ = nullptr;

  filesystem::remove_all(directory);
}
