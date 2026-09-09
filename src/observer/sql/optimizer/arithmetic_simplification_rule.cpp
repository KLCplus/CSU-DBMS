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
// Created by CSUDB on 2026/09/09.
//

#include "sql/optimizer/arithmetic_simplification_rule.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"

RC ArithmeticSimplificationRule::rewrite(unique_ptr<Expression> &expr, bool &change_made)
{
  RC rc = RC::SUCCESS;

  change_made = false;
  if (expr->type() == ExprType::ARITHMETIC) {
    auto arithmetic_expr = static_cast<ArithmeticExpr *>(expr.get());

    // 仅当左右两侧都是常量时才做折叠，避免对一元运算做过早处理
    unique_ptr<Expression> &left  = arithmetic_expr->left();
    unique_ptr<Expression> &right = arithmetic_expr->right();
    if (left == nullptr || right == nullptr) {
      return rc;
    }
    if (left->type() != ExprType::VALUE || right->type() != ExprType::VALUE) {
      return rc;
    }

    Value value;
    rc = arithmetic_expr->try_get_value(value);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("arithmetic expression cannot be folded. rc=%s", strrc(rc));
      return rc;
    }

    unique_ptr<Expression> new_expr(new ValueExpr(value));
    expr.swap(new_expr);
    change_made = true;
    LOG_TRACE("arithmetic expression is simplified: %s = %s",
        expr->name(), value.to_string().c_str());
  }
  return rc;
}