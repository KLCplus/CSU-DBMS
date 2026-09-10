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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const char *table_name = select_sql.relations[i].c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
  }

  // collect tables from explicit JOIN clauses（语义等价于加入 from 列表）
  for (size_t i = 0; i < select_sql.joins.size(); i++) {
    const char *table_name = select_sql.joins[i].relation_name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. join relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    RC rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> order_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.order_by) {
    RC rc = expression_binder.bind_expression(expression, order_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind order by expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(db,
      default_table,
      &table_map,
      select_sql.conditions.data(),
      static_cast<int>(select_sql.conditions.size()),
      filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // 绑定 WHERE 布尔表达式（若有），支持 AND/OR/NOT/括号/算术
  unique_ptr<Expression> where_expression;
  if (select_sql.where_expression != nullptr) {
    vector<unique_ptr<Expression>> bound_where;
    rc = expression_binder.bind_expression(select_sql.where_expression, bound_where);
    if (OB_FAIL(rc)) {
      LOG_WARN("bind where expression failed. rc=%s", strrc(rc));
      return rc;
    }
    if (bound_where.size() != 1) {
      LOG_WARN("invalid where expression result count. count=%d", static_cast<int>(bound_where.size()));
      return rc;
    }
    where_expression = std::move(bound_where[0]);
  }

  // 绑定 JOIN ON 条件，后续与 WHERE 条件用 AND 合并
  vector<unique_ptr<Expression>> join_conditions;
  for (size_t i = 0; i < select_sql.joins.size(); i++) {
    unique_ptr<Expression> &join_condition = select_sql.joins[i].condition;
    if (join_condition == nullptr) {
      continue;
    }
    vector<unique_ptr<Expression>> bound_cond;
    rc = expression_binder.bind_expression(join_condition, bound_cond);
    if (OB_FAIL(rc)) {
      LOG_WARN("bind join condition failed. rc=%s", strrc(rc));
      return rc;
    }
    if (bound_cond.size() != 1) {
      LOG_WARN("invalid join condition result count. count=%d", static_cast<int>(bound_cond.size()));
      return rc;
    }
    join_conditions.emplace_back(std::move(bound_cond[0]));
  }

  // 合并 WHERE 与 JOIN ON 条件（内连接 ON 等价于 WHERE）
  vector<unique_ptr<Expression>> all_where_conditions;
  if (where_expression != nullptr) {
    all_where_conditions.emplace_back(std::move(where_expression));
  }
  for (unique_ptr<Expression> &cond : join_conditions) {
    all_where_conditions.emplace_back(std::move(cond));
  }

  unique_ptr<Expression> final_where_expression;
  if (all_where_conditions.size() == 1) {
    final_where_expression = std::move(all_where_conditions[0]);
  } else if (all_where_conditions.size() > 1) {
    final_where_expression = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, all_where_conditions);
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_.swap(tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->where_expression_ = std::move(final_where_expression);
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_expressions);
  stmt                      = select_stmt;
  return RC::SUCCESS;
}
