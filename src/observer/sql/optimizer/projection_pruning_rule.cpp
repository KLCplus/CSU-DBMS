/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/projection_pruning_rule.h"

#include <map>
#include <set>
#include <vector>

#include "sql/expr/expression_iterator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/sort_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "storage/field/field.h"
#include "storage/table/table.h"

using namespace std;

namespace {

using FieldSet = set<const FieldMeta *>;
using FieldMap = map<const Table *, FieldSet>;

void collect_expr_fields(Expression &expr, FieldMap &fields)
{
  if (expr.type() == ExprType::FIELD) {
    FieldExpr   &field_expr = static_cast<FieldExpr &>(expr);
    const Field &field      = field_expr.field();
    fields[field.table()].insert(field.meta());
  }

  ExpressionIterator::iterate_child_expr(expr, [&fields](unique_ptr<Expression> &child) -> RC {
    if (child != nullptr) {
      collect_expr_fields(*child, fields);
    }
    return RC::SUCCESS;
  });
}

void collect_operator_fields(LogicalOperator &oper, FieldMap &fields)
{
  for (unique_ptr<Expression> &expr : oper.expressions()) {
    if (expr != nullptr) {
      collect_expr_fields(*expr, fields);
    }
  }

  switch (oper.type()) {
    case LogicalOperatorType::TABLE_GET: {
      TableGetLogicalOperator &table_get = static_cast<TableGetLogicalOperator &>(oper);
      for (unique_ptr<Expression> &expr : table_get.predicates()) {
        if (expr != nullptr) {
          collect_expr_fields(*expr, fields);
        }
      }
    } break;

    case LogicalOperatorType::SORT: {
      SortLogicalOperator &sort = static_cast<SortLogicalOperator &>(oper);
      for (OrderByUnit &unit : sort.order_by_units()) {
        if (unit.expression != nullptr) {
          collect_expr_fields(*unit.expression, fields);
        }
      }
    } break;

    case LogicalOperatorType::GROUP_BY: {
      GroupByLogicalOperator &group_by = static_cast<GroupByLogicalOperator &>(oper);
      for (unique_ptr<Expression> &expr : group_by.group_by_expressions()) {
        if (expr != nullptr) {
          collect_expr_fields(*expr, fields);
        }
      }
      for (Expression *expr : group_by.aggregate_expressions()) {
        if (expr != nullptr) {
          collect_expr_fields(*expr, fields);
        }
      }
    } break;

    case LogicalOperatorType::JOIN: {
      JoinLogicalOperator &join = static_cast<JoinLogicalOperator &>(oper);
      for (unique_ptr<Expression> &expr : join.get_join_predicates()) {
        if (expr != nullptr) {
          collect_expr_fields(*expr, fields);
        }
      }
      if (join.predicates() != nullptr) {
        collect_expr_fields(*join.predicates(), fields);
      }
    } break;

    default:
      break;
  }

  for (unique_ptr<LogicalOperator> &child : oper.children()) {
    if (child != nullptr) {
      collect_operator_fields(*child, fields);
    }
  }
}

bool apply_used_fields(LogicalOperator &oper, const FieldMap &fields)
{
  bool changed = false;

  if (oper.type() == LogicalOperatorType::TABLE_GET) {
    TableGetLogicalOperator &table_get = static_cast<TableGetLogicalOperator &>(oper);
    if (table_get.read_write_mode() == ReadWriteMode::READ_ONLY) {
      vector<const FieldMeta *> needed;
      auto                      iter = fields.find(table_get.table());
      if (iter != fields.end()) {
        const TableMeta &table_meta = table_get.table()->table_meta();
        // 按表定义顺序保留需要的列，保证输出稳定
        for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); i++) {
          const FieldMeta *field_meta = table_meta.field(i);
          if (iter->second.count(field_meta) != 0) {
            needed.push_back(field_meta);
          }
        }
      }

      if (!table_get.used_fields_valid() || table_get.used_fields() != needed) {
        table_get.set_used_fields(std::move(needed));
        changed = true;
      }
    }
  }

  for (unique_ptr<LogicalOperator> &child : oper.children()) {
    if (child != nullptr && apply_used_fields(*child, fields)) {
      changed = true;
    }
  }
  return changed;
}

}  // namespace

RC ProjectionPruningRule::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  if (oper->type() != LogicalOperatorType::PROJECTION) {
    return RC::SUCCESS;
  }

  FieldMap fields;
  collect_operator_fields(*oper, fields);
  change_made = apply_used_fields(*oper, fields);
  return RC::SUCCESS;
}
