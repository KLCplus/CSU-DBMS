// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  resolve_stage.cpp:59           处理一次语义解析请求：ParsedSqlNode -> Stmt
// ------------------------------------------------------------------------------------------------
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

/**
 * @file resolve_stage.cpp
 * @brief 语义解析阶段（ResolveStage）的实现
 * @details 本文件把 ParseStage 产出的 ParsedSqlNode AST 结合数据库元数据，
 * 转换为各语句对应的 Stmt（Statement），是编译器流水线中
 *   ParsedSqlNode AST -> ResolveStage -> Stmt 的关键一环。
 * 核心实现原则：语法与语义分离——语法分析阶段不访问元数据，真正的表/字段存在性、
 * 类型兼容等校验都在这里通过 Stmt::create_stmt() 与 ExpressionBinder 完成，
 * 并用线程本地的错误通道把带行列定位的语义错误回传给上层。
 */

#include <string.h>

#include "resolve_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/stmt.h"
#include "sql/parser/expression_binder.h"

using namespace common;

/**
 * @brief 处理一次语义解析请求：ParsedSqlNode -> Stmt
 * @param sql_event 承载 ParsedSqlNode、会话与结果对象的事件
 * @return RC::SUCCESS 已生成 Stmt；RC::SCHEMA_DB_NOT_EXIST 未选择数据库；
 *         其他为 Stmt 创建/表达式绑定阶段的语义错误码；
 *         注意 RC::UNIMPLEMENTED 也会写入 stmt 并原样返回，由上层判断
 * @details 实现原理：先取得当前数据库句柄（缺库直接报错），清空线程本地的
 * binder 错误槽位，然后调用 Stmt::create_stmt() 完成“AST -> Stmt”的转换。
 * create_stmt 内部会依据 ParsedSqlNode.flag 分派到不同语句的构造函数，并调用
 * ExpressionBinder 绑定表达式；绑定过程中记录的结构化错误（含行列号）在此处
 * 从线程本地通道取出并回填给用户，从而弥补 RC 码无法携带定位信息的不足。
 */
RC ResolveStage::handle_request(SQLStageEvent *sql_event)
{
  RC            rc            = RC::SUCCESS;
  SessionEvent *session_event = sql_event->session_event();
  SqlResult    *sql_result    = session_event->sql_result();

  Db *db = session_event->session()->get_current_db();
  if (nullptr == db) {
    // 语义解析必须依赖表/字段元数据，没有当前库无法继续
    LOG_ERROR("cannot find current db");
    rc = RC::SCHEMA_DB_NOT_EXIST;
    sql_result->set_return_code(rc);
    sql_result->set_state_string("no db selected");
    return rc;
  }

  ParsedSqlNode *sql_node = sql_event->sql_node().get();
  Stmt          *stmt     = nullptr;

  // 清空上一个语句遗留的语义错误消息（线程本地槽位）
  reset_binder_error_message();

  rc = Stmt::create_stmt(db, *sql_node, stmt);
  if (rc != RC::SUCCESS && rc != RC::UNIMPLEMENTED) {
    // UNIMPLEMENTED 视为“已识别但未实现”，仍保留 stmt 供上层给出提示
    LOG_WARN("failed to create stmt. rc=%d:%s", rc, strrc(rc));
    sql_result->set_return_code(rc);
    // 语义错误（字段/表不存在、类型不匹配等）若带有行列定位，则透出给用户
    const string &semantic_error = get_binder_error_message();
    if (!semantic_error.empty()) {
      sql_result->set_state_string(semantic_error);
    }
    return rc;
  }

  // 转换成功（或未实现）：把生成的 Stmt 交给事件，进入后续优化/执行阶段
  sql_event->set_stmt(stmt);

  return rc;
}
