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
// Created by Longda on 2021/4/13.
//

#pragma once

#include "common/lang/string.h"
#include "event/sql_debug.h"
#include "sql/executor/sql_result.h"

class Session;
class Communicator;

enum class ClientRequestType
{
  PROTOCOL_ERROR,
  QUERY,
  LOGIN,
  LOGOUT,
  PING,
  SERVER_INFO,
  BUFFER_SNAPSHOT,
};

/**
 * @brief 表示一个SQL请求
 *
 */
class SessionEvent
{
public:
  SessionEvent(Communicator *client);
  virtual ~SessionEvent();

  Communicator *get_communicator() const;
  Session      *session() const;

  void set_query(const string &query) { query_ = query; }

  const string &query() const { return query_; }
  SqlResult    *sql_result() { return &sql_result_; }
  SqlDebug     &sql_debug() { return sql_debug_; }

  void set_request_type(ClientRequestType type) { request_type_ = type; }
  ClientRequestType request_type() const { return request_type_; }
  void set_username(const string &value) { username_ = value; }
  const string &username() const { return username_; }
  void set_password(const string &value) { password_ = value; }
  const string &password() const { return password_; }
  void clear_password() { password_.assign(password_.size(), '\0'); password_.clear(); }
  void set_database(const string &value) { database_ = value; }
  const string &database() const { return database_; }
  void set_snapshot_limit(size_t value) { snapshot_limit_ = value; }
  size_t snapshot_limit() const { return snapshot_limit_; }
  void set_protocol_error(const string &value) { protocol_error_ = value; }
  const string &protocol_error() const { return protocol_error_; }

private:
  Communicator *communicator_ = nullptr;  ///< 与客户端通讯的对象
  SqlResult     sql_result_;              ///< SQL执行结果
  SqlDebug      sql_debug_;               ///< SQL调试信息
  string        query_;                   ///< SQL语句
  ClientRequestType request_type_ = ClientRequestType::QUERY;
  string        username_;
  string        password_;
  string        database_;
  size_t        snapshot_limit_ = 20;
  string        protocol_error_;
};
