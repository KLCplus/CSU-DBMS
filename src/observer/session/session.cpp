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
// Created by Wangyunlai on 2021/5/12.
//

#include "session/session.h"
#include "common/global_context.h"
#include "storage/db/db.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"

#include <atomic>
#include <ctime>

namespace {
std::atomic<uint64_t> next_session_id{1};
}

Session::Session()
    : session_id_(next_session_id.fetch_add(1)), connected_at_epoch_seconds_(static_cast<uint64_t>(time(nullptr)))
{}

Session &Session::default_session()
{
  static Session session;
  return session;
}

Session::Session(const Session &other)
    : session_id_(next_session_id.fetch_add(1)), username_(other.username_),
      connected_at_epoch_seconds_(static_cast<uint64_t>(time(nullptr))), authenticated_(other.authenticated_),
      db_(other.db_), sql_debug_(other.sql_debug_), hash_join_(other.hash_join_), use_cascade_(other.use_cascade_),
      execution_mode_(other.execution_mode_)
{}

Session::~Session()
{
  if (nullptr != trx_) {
    db_->trx_kit().destroy_trx(trx_);
    trx_ = nullptr;
  }
}

const char *Session::get_current_db_name() const
{
  if (db_ != nullptr)
    return db_->name();
  else
    return "";
}

Db *Session::get_current_db() const { return db_; }

void Session::set_current_db(const string &dbname)
{
  DefaultHandler &handler = *GCTX.handler_;
  Db             *db      = handler.find_db(dbname.c_str());
  if (db == nullptr) {
    LOG_WARN("no such database: %s", dbname.c_str());
    return;
  }

  LOG_TRACE("change db to %s", dbname.c_str());
  db_ = db;
}

void Session::set_trx_multi_operation_mode(bool multi_operation_mode)
{
  trx_multi_operation_mode_ = multi_operation_mode;
}

bool Session::is_trx_multi_operation_mode() const { return trx_multi_operation_mode_; }

Trx *Session::current_trx()
{
  /*
  当前把事务与数据库绑定到了一起。这样虽然不合理，但是处理起来也简单。
  我们在测试过程中，也不需要多个数据库之间做关联。
  */
  if (trx_ == nullptr) {
    trx_ = db_->trx_kit().create_trx(db_->log_handler());
  }
  return trx_;
}

void Session::destroy_trx()
  {
    if (trx_ != nullptr) {
      db_->trx_kit().destroy_trx(trx_);
      trx_ = nullptr;
    }
  }

thread_local Session *thread_session = nullptr;

void Session::set_current_session(Session *session) { thread_session = session; }

Session *Session::current_session() { return thread_session; }

void Session::set_current_request(SessionEvent *request) { current_request_ = request; }

SessionEvent *Session::current_request() const { return current_request_; }
