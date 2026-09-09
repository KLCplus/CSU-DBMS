/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Longda on 2021/4/13.
//

#include "storage/default/default_handler.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/os/path.h"
#include "session/session.h"
#include "storage/common/condition_filter.h"
#include "storage/index/bplus_tree.h"
#include "storage/record/record_manager.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;

DefaultHandler::DefaultHandler() {}

DefaultHandler::~DefaultHandler() noexcept { destroy(); }

RC DefaultHandler::init(const char *base_dir, const char *trx_kit_name, const char *log_handler_name,
    const char *storage_engine, int buffer_pool_memory_size, const char *buffer_pool_replacement_policy,
    const char *page_io_backend)
{
  // 检查目录是否存在，或者创建
  filesystem::path db_dir(base_dir);
  db_dir /= "db";
  error_code ec;
  if (!filesystem::is_directory(db_dir) && !filesystem::create_directories(db_dir, ec)) {
    LOG_ERROR("Cannot access base dir: %s. msg=%d:%s", db_dir.c_str(), errno, strerror(errno));
    return RC::INTERNAL;
  }

  base_dir_ = base_dir;
  db_dir_   = db_dir;
  trx_kit_name_ = trx_kit_name;
  log_handler_name_ = log_handler_name;
  storage_engine_ = storage_engine;
  buffer_pool_memory_size_ = buffer_pool_memory_size;
  buffer_pool_replacement_policy_ = buffer_pool_replacement_policy;
  page_io_backend_ = page_io_backend;

  const char *sys_db = "sys";

  RC ret = create_db(sys_db);
  if (ret != RC::SUCCESS && ret != RC::SCHEMA_DB_EXIST) {
    LOG_ERROR("Failed to create system db");
    return ret;
  }

  ret = open_db(sys_db);
  if (ret != RC::SUCCESS) {
    LOG_ERROR("Failed to open system db. rc=%s", strrc(ret));
    return ret;
  }

  Session &default_session = Session::default_session();
  default_session.set_current_db(sys_db);

  LOG_INFO("Default handler init with %s success", base_dir);
  return RC::SUCCESS;
}

void DefaultHandler::destroy()
{
  sync();

  for (const auto &iter : opened_dbs_) {
    delete iter.second;
  }
  opened_dbs_.clear();
}

RC DefaultHandler::create_db(const char *dbname)
{
  if (nullptr == dbname || common::is_blank(dbname)) {
    LOG_WARN("Invalid db name");
    return RC::INVALID_ARGUMENT;
  }

  // 如果对应名录已经存在，返回错误
  filesystem::path dbpath = db_dir_ / dbname;
  if (filesystem::is_directory(dbpath)) {
    LOG_WARN("Db already exists: %s", dbname);
    return RC::SCHEMA_DB_EXIST;
  }

  error_code ec;
  if (!filesystem::create_directories(dbpath, ec)) {
    LOG_ERROR("Create db fail: %s. error=%s", dbpath.c_str(), strerror(errno));
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC DefaultHandler::drop_db(const char *dbname)
{
  if (nullptr == dbname || common::is_blank(dbname) || 0 == strcmp(dbname, "sys")) {
    return RC::INVALID_ARGUMENT;
  }
  filesystem::path dbpath = db_dir_ / dbname;
  if (!filesystem::is_directory(dbpath)) return RC::SCHEMA_DB_NOT_EXIST;
  RC rc = close_db(dbname);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_DB_NOT_OPENED) return rc;
  error_code ec;
  filesystem::remove_all(dbpath, ec);
  return ec ? RC::FILE_REMOVE : RC::SUCCESS;
}

RC DefaultHandler::open_db(const char *dbname)
{
  if (nullptr == dbname || common::is_blank(dbname)) {
    LOG_WARN("Invalid db name");
    return RC::INVALID_ARGUMENT;
  }

  if (opened_dbs_.find(dbname) != opened_dbs_.end()) {
    return RC::SUCCESS;
  }

  filesystem::path dbpath = db_dir_ / dbname;
  if (!filesystem::is_directory(dbpath)) {
    return RC::SCHEMA_DB_NOT_EXIST;
  }

  // open db
  Db *db  = new Db();
  RC  ret = RC::SUCCESS;
  if ((ret = db->init(dbname,
           dbpath.c_str(),
           trx_kit_name_.c_str(),
           log_handler_name_.c_str(),
           storage_engine_.c_str(),
           buffer_pool_memory_size_,
           buffer_pool_replacement_policy_.c_str(),
           page_io_backend_.c_str())) != RC::SUCCESS) {
    LOG_ERROR("Failed to open db: %s. error=%s", dbname, strrc(ret));
    delete db;
  } else {
    opened_dbs_[dbname] = db;
  }
  return ret;
}

RC DefaultHandler::close_db(const char *dbname)
{
  auto it = opened_dbs_.find(dbname == nullptr ? "" : dbname);
  if (it == opened_dbs_.end()) return RC::SCHEMA_DB_NOT_OPENED;
  RC rc = it->second->sync();
  if (OB_FAIL(rc)) return rc;
  delete it->second;
  opened_dbs_.erase(it);
  return RC::SUCCESS;
}

// TODO: remove DefaultHandler
RC DefaultHandler::create_table(const char *dbname, const char *relation_name, span<const AttrInfoSqlNode> attributes)
{
  Db *db = find_db(dbname);
  if (db == nullptr) {
    return RC::SCHEMA_DB_NOT_OPENED;
  }
  return db->create_table(relation_name, attributes, {});
}

RC DefaultHandler::drop_table(const char *dbname, const char *relation_name) { return RC::UNIMPLEMENTED; }

Db *DefaultHandler::find_db(const char *dbname) const
{
  map<string, Db *>::const_iterator iter = opened_dbs_.find(dbname);
  if (iter == opened_dbs_.end()) {
    return nullptr;
  }
  return iter->second;
}

Table *DefaultHandler::find_table(const char *dbname, const char *table_name) const
{
  if (dbname == nullptr || table_name == nullptr) {
    LOG_WARN("Invalid argument. dbname=%p, table_name=%p", dbname, table_name);
    return nullptr;
  }
  Db *db = find_db(dbname);
  if (nullptr == db) {
    return nullptr;
  }

  return db->find_table(table_name);
}

RC DefaultHandler::sync()
{
  RC rc = RC::SUCCESS;
  for (const auto &db_pair : opened_dbs_) {
    Db *db = db_pair.second;
    rc     = db->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to sync db. name=%s, rc=%d:%s", db->name(), rc, strrc(rc));
      return rc;
    }
  }
  return rc;
}

vector<string> DefaultHandler::opened_databases() const
{
  vector<string> result;
  for (const auto &entry : opened_dbs_) result.push_back(entry.first);
  return result;
}
