// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  expression_binder.cpp:72       把算术运算符枚举转换为可读符号
//  expression_binder.cpp:92       生成并记录“运算符不能作用于该类型”的语义错误消息
//  expression_binder.cpp:114      清空线程本地绑定错误消息
//  expression_binder.cpp:120      写入线程本地绑定错误消息
//  expression_binder.cpp:126      读取线程本地绑定错误消息
//  expression_binder.cpp:135      按表名在查询表集合中查找表（忽略大小写）
//  expression_binder.cpp:154      把一张表的用户字段全部展开为字段表达式
//  expression_binder.cpp:176      绑定表达式总入口：按表达式类型分派到具体绑定函数
//  expression_binder.cpp:240      绑定并展开星号表达式 `*` / `table.*`
//  expression_binder.cpp:288      绑定未解析的字段表达式（如 `a` 或 `t.a`）
//  expression_binder.cpp:356      绑定已解析的字段表达式
//  expression_binder.cpp:370      绑定常量值表达式
//  expression_binder.cpp:386      绑定 CAST 类型转换表达式
//  expression_binder.cpp:428      绑定比较表达式（= <> < <= > >= 及 IS [NOT] NULL）
//  expression_binder.cpp:487      绑定布尔连接表达式（AND / OR）
//  expression_binder.cpp:534      绑定算术表达式（+ - * / 以及一元负号）
//  expression_binder.cpp:609      校验聚合表达式是否合法
//  expression_binder.cpp:664      绑定聚合表达式（SUM/AVG/COUNT/MAX/MIN）
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
// Created by Wangyunlai on 2024/05/29.
//

/**
 * @file expression_binder.cpp
 * @brief 表达式绑定器（ExpressionBinder）的实现
 * @details 本文件处于编译器流水线的语义解析环节，衔接
 *   ParsedSqlNode AST -> ParseStage -> ResolveStage(Stmt::create_stmt) -> Stmt。
 * 语法阶段得到的字段/表达式只是文本描述，绑定阶段把它们与数据库元数据对应，
 * 生成可执行的 Expression，并完成表/字段存在性检查与类型检查。
 * 核心实现原则：
 *   1. 递归下降：bind_expression 按表达式类型分派到具体 bind_xxx；
 *   2. 用值时替换子节点所有权，避免深拷贝；
 *   3. 语义错误通过 thread_local 的 g_binder_error_message 携带行列信息回传，
 *      因为 RC 返回码本身无法表达定位。
 */

#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/lang/ranges.h"
#include "common/type/attr_type.h"
#include <cstdio>
#include "sql/parser/expression_binder.h"
#include "sql/expr/expression_iterator.h"

using namespace common;

namespace
{
// 线程本地的绑定错误消息槽位：一次语句处理一个线程，避免并发语句相互覆盖。
thread_local std::string g_binder_error_message;

/**
 * @brief 把算术运算符枚举转换为可读符号
 * @param type 算术表达式类型（ADD/SUB/MUL/DIV/NEGATIVE）
 * @return 对应的运算符字符串（如 "+"），未知类型返回 "?"
 * @details 用于生成面向用户的语义错误文本。
 */
const char *arithmetic_type_to_string(ArithmeticExpr::Type type)
{
  switch (type) {
    case ArithmeticExpr::Type::ADD: return "+";
    case ArithmeticExpr::Type::SUB: return "-";
    case ArithmeticExpr::Type::MUL: return "*";
    case ArithmeticExpr::Type::DIV: return "/";
    case ArithmeticExpr::Type::NEGATIVE: return "-";
  }
  return "?";
}

/**
 * @brief 生成并记录“运算符不能作用于该类型”的语义错误消息
 * @param expr       出错的一元/二元算术表达式（提供行列位置与运算符类型）
 * @param left_type  左操作数类型
 * @param right_type 右操作数类型；一元运算传 AttrType::UNDEFINED 以走单操作数文案
 * @details 实现原理：根据是否为一元运算拼出不同措辞，将运算符符号、行列号与类型名
 * 格式化为 "SemanticError at line ..., column ..." 文本，写入线程本地错误槽位。
 */
void set_arithmetic_type_error(const ArithmeticExpr &expr, AttrType left_type, AttrType right_type)
{
  char msg[512];
  if (right_type == AttrType::UNDEFINED) {
    snprintf(msg, sizeof(msg),
        "SemanticError at line %d, column %d\n\noperator '%s' cannot be applied to\n%s",
        expr.line(), expr.column(), arithmetic_type_to_string(expr.arithmetic_type()),
        attr_type_to_sql_string(left_type));
  } else {
    snprintf(msg, sizeof(msg),
        "SemanticError at line %d, column %d\n\noperator '%s' cannot be applied to\n%s and %s",
        expr.line(), expr.column(), arithmetic_type_to_string(expr.arithmetic_type()),
        attr_type_to_sql_string(left_type), attr_type_to_sql_string(right_type));
  }
  set_binder_error_message(msg);
}
}  // namespace

/**
 * @brief 清空线程本地绑定错误消息
 * @details 实现原理：直接 clear 槽位；通常在每条语句语义解析前调用。
 */
void reset_binder_error_message() { g_binder_error_message.clear(); }
/**
 * @brief 写入线程本地绑定错误消息
 * @param msg 已格式化的错误文本
 * @details 实现原理：整体赋值到 thread_local 槽位，供上层按线程安全方式读取。
 */
void set_binder_error_message(const std::string &msg) { g_binder_error_message = msg; }
/**
 * @brief 读取线程本地绑定错误消息
 * @return 槽位中当前消息的常量引用
 * @details 实现原理：返回引用以避免拷贝；无错误时为空串。
 */
const std::string &get_binder_error_message() { return g_binder_error_message; }

/**
 * @brief 按表名在查询表集合中查找表（忽略大小写）
 * @param table_name 待查找的表名
 * @return 命中的 Table 指针；未找到返回 nullptr
 * @details 实现原理：用 strcasecmp 构造谓词，借 ranges::find_if 做线性查找，
 * 与 SQL 标识符大小写不敏感语义一致。
 */
Table *BinderContext::find_table(const char *table_name) const
{
  auto pred = [table_name](Table *table) { return 0 == strcasecmp(table_name, table->name()); };
  auto iter = ranges::find_if(query_tables_, pred);
  if (iter == query_tables_.end()) {
    return nullptr;
  }
  return *iter;
}

////////////////////////////////////////////////////////////////////////////////
/**
 * @brief 把一张表的用户字段全部展开为字段表达式
 * @param table       目标表
 * @param expressions 输出：追加 table 的所有用户字段（不含系统字段）的 FieldExpr
 * @details 实现原理：从 sys_field_num() 开始遍历到 field_num()（跳过系统字段），
 * 为每个 FieldMeta 构造一个带表引用的 FieldExpr 并设置显示名。这是 `*` 展开的
 * 底层实现，`SELECT *`、`t.*` 均复用它。
 */
static void wildcard_fields(Table *table, vector<unique_ptr<Expression>> &expressions)
{
  const TableMeta &table_meta = table->table_meta();
  const int        field_num  = table_meta.field_num();
  for (int i = table_meta.sys_field_num(); i < field_num; i++) {
    Field      field(table, table_meta.field(i));
    FieldExpr *field_expr = new FieldExpr(field);
    field_expr->set_name(field.field_name());
    expressions.emplace_back(field_expr);
  }
}

/**
 * @brief 绑定表达式总入口：按表达式类型分派到具体绑定函数
 * @param expr               待绑定表达式（可能为 nullptr，此时视为成功）
 * @param bound_expressions  输出：绑定结果列表
 * @return 各具体绑定函数的返回码；未知类型返回 RC::INTERNAL；AGGREGATION 走到此处视为
 *         不应发生的内部错误
 * @details 实现原理：读取 expr->type() 做 switch 分派。STAR/UNBOUND_FIELD 可能产生
 * 多个结果，其余通常恰好一个。AGGREGATION 理论上在绑定后才会出现，若出现在未绑定
 * 阶段说明逻辑错误，故 ASSERT。
 */
RC ExpressionBinder::bind_expression(unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  switch (expr->type()) {
    case ExprType::STAR: {
      return bind_star_expression(expr, bound_expressions);
    } break;

    case ExprType::UNBOUND_FIELD: {
      return bind_unbound_field_expression(expr, bound_expressions);
    } break;

    case ExprType::UNBOUND_AGGREGATION: {
      return bind_aggregate_expression(expr, bound_expressions);
    } break;

    case ExprType::FIELD: {
      return bind_field_expression(expr, bound_expressions);
    } break;

    case ExprType::VALUE: {
      return bind_value_expression(expr, bound_expressions);
    } break;

    case ExprType::CAST: {
      return bind_cast_expression(expr, bound_expressions);
    } break;

    case ExprType::COMPARISON: {
      return bind_comparison_expression(expr, bound_expressions);
    } break;

    case ExprType::CONJUNCTION: {
      return bind_conjunction_expression(expr, bound_expressions);
    } break;

    case ExprType::ARITHMETIC: {
      return bind_arithmetic_expression(expr, bound_expressions);
    } break;

    case ExprType::AGGREGATION: {
      ASSERT(false, "shouldn't be here");
    } break;

    default: {
      LOG_WARN("unknown expression type: %d", static_cast<int>(expr->type()));
      return RC::INTERNAL;
    }
  }
  return RC::INTERNAL;
}

/**
 * @brief 绑定并展开星号表达式 `*` / `table.*`
 * @param expr               待绑定的 StarExpr
 * @param bound_expressions  输出：展开得到的各字段 FieldExpr
 * @return RC::SUCCESS 成功；RC::SCHEMA_TABLE_NOT_EXIST 指定表不在查询表集合中
 * @details 实现原理：若 StarExpr 指定了具体表名，则先在上下文中查找该表；否则把
 * FROM 中所有表都作为展开目标。对每个目标表调用 wildcard_fields 展开为用户字段。
 * 表不存在时写入带行列号的语义错误并返回错误码。
 */
RC ExpressionBinder::bind_star_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto star_expr = static_cast<StarExpr *>(expr.get());

  vector<Table *> tables_to_wildcard;

  const char *table_name = star_expr->table_name();
  if (!is_blank(table_name) && 0 != strcmp(table_name, "*")) {
    // 形如 `t.*`：只展开指定表
    Table *table = context_.find_table(table_name);
    if (nullptr == table) {
      LOG_INFO("no such table in from list: %s", table_name);
      char msg[256];
      snprintf(msg, sizeof(msg), "SemanticError at line %d, column %d: no such table '%s'",
          star_expr->line(), star_expr->column(), table_name);
      set_binder_error_message(msg);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    tables_to_wildcard.push_back(table);
  } else {
    // 裸 `*`：展开 FROM/JOIN 涉及的所有表
    const vector<Table *> &all_tables = context_.query_tables();
    tables_to_wildcard.insert(tables_to_wildcard.end(), all_tables.begin(), all_tables.end());
  }

  for (Table *table : tables_to_wildcard) {
    wildcard_fields(table, bound_expressions);
  }

  return RC::SUCCESS;
}

/**
 * @brief 绑定未解析的字段表达式（如 `a` 或 `t.a`）
 * @param expr               待绑定的 UnboundFieldExpr
 * @param bound_expressions  输出：绑定后的 FieldExpr（`*` 时可能多个）
 * @return RC::SUCCESS 成功；表不确定/不存在返回 RC::SCHEMA_TABLE_NOT_EXIST；
 *         字段不存在返回 RC::SCHEMA_FIELD_MISSING
 * @details 实现原理：先确定字段所属表——省略表名时要求查询中恰好只有一张表，否则
 * 无法判定；带表名时用 find_table 查找。若字段名为 `*` 则展开该表所有字段，否则通过
 * FieldMeta 查找并构造 FieldExpr；任何查找失败都会写入带行列的语义错误消息。
 */
RC ExpressionBinder::bind_unbound_field_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto unbound_field_expr = static_cast<UnboundFieldExpr *>(expr.get());

  const char *table_name = unbound_field_expr->table_name();
  const char *field_name = unbound_field_expr->field_name();

  Table *table = nullptr;
  if (is_blank(table_name)) {
    // 省略表名：仅当查询只涉及一张表时才能无歧义地确定归属
    if (context_.query_tables().size() != 1) {
      LOG_INFO("cannot determine table for field: %s", field_name);
      char msg[256];
      snprintf(msg, sizeof(msg), "SemanticError at line %d, column %d: cannot determine table for field '%s'",
          unbound_field_expr->line(), unbound_field_expr->column(), field_name);
      set_binder_error_message(msg);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    table = context_.query_tables()[0];
  } else {
    table = context_.find_table(table_name);
    if (nullptr == table) {
      LOG_INFO("no such table in from list: %s", table_name);
      char msg[256];
      snprintf(msg, sizeof(msg), "SemanticError at line %d, column %d: no such table '%s'",
          unbound_field_expr->line(), unbound_field_expr->column(), table_name);
      set_binder_error_message(msg);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  if (0 == strcmp(field_name, "*")) {
    // 已确定归属表的通配字段 `t.*`
    wildcard_fields(table, bound_expressions);
  } else {
    const FieldMeta *field_meta = table->table_meta().field(field_name);
    if (nullptr == field_meta) {
      LOG_INFO("no such field in table: %s.%s", table_name, field_name);
      char msg[256];
      snprintf(msg, sizeof(msg), "SemanticError at line %d, column %d: no such field '%s.%s'",
          unbound_field_expr->line(), unbound_field_expr->column(), table_name, field_name);
      set_binder_error_message(msg);
      return RC::SCHEMA_FIELD_MISSING;
    }

    // 元数据命中：生成带物理字段信息的可执行表达式
    Field      field(table, field_meta);
    FieldExpr *field_expr = new FieldExpr(field);
    field_expr->set_name(field_name);
    bound_expressions.emplace_back(field_expr);
  }

  return RC::SUCCESS;
}

/**
 * @brief 绑定已解析的字段表达式
 * @param field_expr         已携带 Field 的表达式
 * @param bound_expressions  输出：转移后的同一表达式
 * @return 恒为 RC::SUCCESS
 * @details 实现原理：无需额外处理，直接用 std::move 转移所有权到结果列表。
 */
RC ExpressionBinder::bind_field_expression(
    unique_ptr<Expression> &field_expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  bound_expressions.emplace_back(std::move(field_expr));
  return RC::SUCCESS;
}

/**
 * @brief 绑定常量值表达式
 * @param value_expr         常量表达式
 * @param bound_expressions  输出：转移后的同一表达式
 * @return 恒为 RC::SUCCESS
 * @details 实现原理：常量无需与元数据绑定，直接转移所有权。
 */
RC ExpressionBinder::bind_value_expression(
    unique_ptr<Expression> &value_expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  bound_expressions.emplace_back(std::move(value_expr));
  return RC::SUCCESS;
}

/**
 * @brief 绑定 CAST 类型转换表达式
 * @param expr               待绑定的 CastExpr
 * @param bound_expressions  输出：完成子表达式绑定后的 CastExpr
 * @return RC::SUCCESS 成功；子绑定失败则原样返回其错误码；
 *         子表达式数量不为 1 返回 RC::INVALID_ARGUMENT
 * @details 实现原理：递归绑定唯一的子表达式，若产生了新的子节点则原位替换后返回
 * 整个 CastExpr；若子节点指针未变化（无需替换）则提前返回成功（防御性分支）。
 */
RC ExpressionBinder::bind_cast_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto cast_expr = static_cast<CastExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &child_expr = cast_expr->child();

  RC rc = bind_expression(child_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid children number of cast expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &child = child_bound_expressions[0];
  if (child.get() == child_expr.get()) {
    // 子节点未被替换，无需回填
    return RC::SUCCESS;
  }

  child_expr.reset(child.release());
  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

/**
 * @brief 绑定比较表达式（= <> < <= > >= 及 IS [NOT] NULL）
 * @param expr               待绑定的 ComparisonExpr
 * @param bound_expressions  输出：左右子表达式均已绑定的 ComparisonExpr
 * @return RC::SUCCESS 成功；左右任一子绑定失败则返回其错误码；
 *         任一侧子表达式数量不为 1 返回 RC::INVALID_ARGUMENT
 * @details 实现原理：先绑定左操作数并原位替换，清空结果列表后再绑定右操作数并替换，
 * 最后把整个比较表达式放入结果。左右处理顺序保证了子表达式结果与目标槽位一一对应。
 */
RC ExpressionBinder::bind_comparison_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto comparison_expr = static_cast<ComparisonExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &left_expr  = comparison_expr->left();
  unique_ptr<Expression>        &right_expr = comparison_expr->right();

  RC rc = bind_expression(left_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid left children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  // 左操作数：绑定结果若为新对象则替换原节点
  unique_ptr<Expression> &left = child_bound_expressions[0];
  if (left.get() != left_expr.get()) {
    left_expr.reset(left.release());
  }

  child_bound_expressions.clear();
  rc = bind_expression(right_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid right children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  // 右操作数：同上，原位替换
  unique_ptr<Expression> &right = child_bound_expressions[0];
  if (right.get() != right_expr.get()) {
    right_expr.reset(right.release());
  }

  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

/**
 * @brief 绑定布尔连接表达式（AND / OR）
 * @param expr               待绑定的 ConjunctionExpr
 * @param bound_expressions  输出：各子表达式均已绑定的 ConjunctionExpr
 * @return RC::SUCCESS 成功；任一子绑定失败则返回其错误码；
 *         任一子表达式数量不为 1 返回 RC::INVALID_ARGUMENT
 * @details 实现原理：遍历 children，对每个子表达式单独绑定并要求恰好得到一个结果，
 * 若结果对象不同则原位替换，从而在保持 AND/OR 树结构不变的前提下完成绑定。
 */
RC ExpressionBinder::bind_conjunction_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());

  vector<unique_ptr<Expression>>  child_bound_expressions;
  vector<unique_ptr<Expression>> &children = conjunction_expr->children();

  for (unique_ptr<Expression> &child_expr : children) {
    // 每个子表达式复用同一结果列表，逐个子表达式清空以校验数量
    child_bound_expressions.clear();

    RC rc = bind_expression(child_expr, child_bound_expressions);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("invalid children number of conjunction expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> &child = child_bound_expressions[0];
    if (child.get() != child_expr.get()) {
      child_expr.reset(child.release());
    }
  }

  bound_expressions.emplace_back(std::move(expr));

  return RC::SUCCESS;
}

/**
 * @brief 绑定算术表达式（+ - * / 以及一元负号）
 * @param expr               待绑定的 ArithmeticExpr
 * @param bound_expressions  输出：操作数已绑定且类型校验通过的 ArithmeticExpr
 * @return RC::SUCCESS 成功；子绑定失败返回其错误码；子表达式数量异常返回
 *         RC::INVALID_ARGUMENT；操作数非数值类型返回 RC::SCHEMA_FIELD_TYPE_MISMATCH
 * @details 实现原理：先绑定左操作数，再（若存在）绑定右操作数并原位替换；随后做
 * 类型一致性校验——算术运算仅接受数值类型，类型不符时通过
 * set_arithmetic_type_error 生成带运算符位置的错误消息并返回类型不匹配。
 */
RC ExpressionBinder::bind_arithmetic_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto arithmetic_expr = static_cast<ArithmeticExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &left_expr  = arithmetic_expr->left();
  unique_ptr<Expression>        &right_expr = arithmetic_expr->right();

  RC rc = bind_expression(left_expr, child_bound_expressions);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid left children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &left = child_bound_expressions[0];
  if (left.get() != left_expr.get()) {
    left_expr.reset(left.release());
  }

  if (right_expr != nullptr) {
    child_bound_expressions.clear();
    rc = bind_expression(right_expr, child_bound_expressions);
    if (OB_FAIL(rc)) {
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("invalid right children number of comparison expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> &right = child_bound_expressions[0];
    if (right.get() != right_expr.get()) {
      right_expr.reset(right.release());
    }
  }

  // 类型一致性校验：算术运算只接受数值类型（简化类型系统集中在绑定阶段）
  //   INT + INT -> INT, INT + FLOAT -> FLOAT, INT + VARCHAR -> ERROR
  const AttrType left_value_type = left_expr->value_type();
  if (!is_numerical_type(left_value_type)) {
    set_arithmetic_type_error(*arithmetic_expr, left_value_type, AttrType::UNDEFINED);
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  if (right_expr != nullptr) {
    const AttrType right_value_type = right_expr->value_type();
    if (!is_numerical_type(right_value_type)) {
      set_arithmetic_type_error(*arithmetic_expr, left_value_type, right_value_type);
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

/**
 * @brief 校验聚合表达式是否合法
 * @param expression 待校验的 AggregateExpr
 * @return RC::SUCCESS 合法；RC::INVALID_ARGUMENT 子表达式为空、聚合类型与子类型
 *         不匹配，或子表达式中嵌套了聚合
 * @details 实现原理：
 *   1. 聚合必须有一个子表达式；
 *   2. SUM/AVG 仅接受数值类型，COUNT/MAX/MIN 接受任意类型；
 *   3. 通过 ExpressionIterator 递归遍历子表达式，禁止聚合嵌套聚合。
 */
RC check_aggregate_expression(AggregateExpr &expression)
{
  // 必须有一个子表达式
  Expression *child_expression = expression.child().get();
  if (nullptr == child_expression) {
    LOG_WARN("child expression of aggregate expression is null");
    return RC::INVALID_ARGUMENT;
  }

  // 校验数据类型与聚合类型是否匹配
  AggregateExpr::Type aggregate_type   = expression.aggregate_type();
  AttrType            child_value_type = child_expression->value_type();
  switch (aggregate_type) {
    case AggregateExpr::Type::SUM:
    case AggregateExpr::Type::AVG: {
      // 仅支持数值类型
      if (!is_numerical_type(child_value_type)) {
        LOG_WARN("invalid child value type for aggregate expression: %d", static_cast<int>(child_value_type));
        return RC::INVALID_ARGUMENT;
      }
    } break;

    case AggregateExpr::Type::COUNT:
    case AggregateExpr::Type::MAX:
    case AggregateExpr::Type::MIN: {
      // 任何类型都支持
    } break;
  }

  // 子表达式中不能再包含聚合表达式
  function<RC(unique_ptr<Expression>&)> check_aggregate_expr = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      LOG_WARN("aggregate expression cannot be nested");
      return RC::INVALID_ARGUMENT;
    }
    rc = ExpressionIterator::iterate_child_expr(*expr, check_aggregate_expr);
    return rc;
  };

  RC rc = ExpressionIterator::iterate_child_expr(expression, check_aggregate_expr);

  return rc;
}

/**
 * @brief 绑定聚合表达式（SUM/AVG/COUNT/MAX/MIN）
 * @param expr               待绑定的 UnboundAggregateExpr
 * @param bound_expressions  输出：绑定后的 AggregateExpr
 * @return RC::SUCCESS 成功；无效聚合名返回其错误码；子绑定失败返回其错误码；
 *         子表达式数量异常或聚合校验失败返回 RC::INVALID_ARGUMENT
 * @details 实现原理：先把聚合名解析为 AggregateExpr::Type；对 `COUNT(*)` 特化为常量
 * 1（因为 COUNT 只关心行数），其余情况递归绑定子表达式并原位替换；随后构造
 * AggregateExpr 并复制显示名，最后调用 check_aggregate_expression 做合法性校验。
 */
RC ExpressionBinder::bind_aggregate_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto unbound_aggregate_expr = static_cast<UnboundAggregateExpr *>(expr.get());
  const char *aggregate_name = unbound_aggregate_expr->aggregate_name();
  AggregateExpr::Type aggregate_type;
  RC rc = AggregateExpr::type_from_string(aggregate_name, aggregate_type);
  if (OB_FAIL(rc)) {
    LOG_WARN("invalid aggregate name: %s", aggregate_name);
    return rc;
  }

  unique_ptr<Expression>        &child_expr = unbound_aggregate_expr->child();
  vector<unique_ptr<Expression>> child_bound_expressions;

  if (child_expr->type() == ExprType::STAR && aggregate_type == AggregateExpr::Type::COUNT) {
    // COUNT(*) 统计行数，不依赖具体列，用常量 1 作为占位子表达式
    ValueExpr *value_expr = new ValueExpr(Value(1));
    child_expr.reset(value_expr);
  } else {
    rc = bind_expression(child_expr, child_bound_expressions);
    if (OB_FAIL(rc)) {
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("invalid children number of aggregate expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    if (child_bound_expressions[0].get() != child_expr.get()) {
      child_expr.reset(child_bound_expressions[0].release());
    }
  }

  auto aggregate_expr = make_unique<AggregateExpr>(aggregate_type, std::move(child_expr));
  aggregate_expr->set_name(unbound_aggregate_expr->name());
  rc = check_aggregate_expression(*aggregate_expr);
  if (OB_FAIL(rc)) {
    return rc;
  }

  bound_expressions.emplace_back(std::move(aggregate_expr));
  return RC::SUCCESS;
}
