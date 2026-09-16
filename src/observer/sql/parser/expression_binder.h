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
 * @file expression_binder.h
 * @brief 表达式绑定器（ExpressionBinder）的声明
 * @ingroup SQLParser
 * @details 本文件属于编译器流水线中 ResolveStage -> Stmt 的语义解析环节：
 *   SQL 文本 -> Flex token -> Bison LALR -> ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage(Stmt::create_stmt) -> ExpressionBinder -> Stmt。
 * 核心实现原则：语法树中出现的字段、通配符、聚合、比较、布尔与算术表达式在解析
 * 阶段只是“文本描述”；绑定阶段把它们与数据库元数据（表、字段、类型）对应起来，
 * 生成可执行的具体表达式节点，并在此期间完成类型检查与错误定位。
 */

#pragma once

#include "common/lang/string.h"
#include "sql/expr/expression.h"

/**
 * @brief 表达式绑定上下文：提供查询涉及的物理表集合
 * @details 绑定字段时需要知道 FROM/JOIN 子句引入了哪些表，才能解析表名、并区分
 * 单表列引用与多表需限定表名的列引用。
 */
class BinderContext
{
public:
  /** @brief 默认构造：查询表集合为空 */
  BinderContext()          = default;
  /** @brief 虚析构（默认实现），保证可作为多态基类安全使用 */
  virtual ~BinderContext() = default;

  /**
   * @brief 注册一张参与查询的表
   * @param table 表对象指针（不接管所有权，生命周期由外部保证）
   * @details 实现原理：把表指针追加到 query_tables_，供后续 find_table 查找。
   */
  void add_table(Table *table) { query_tables_.push_back(table); }

  /**
   * @brief 按表名查找查询涉及的表（大小写不敏感）
   * @param table_name 表名
   * @return 找到则返回表指针，否则返回 nullptr
   * @details 实现原理：对 query_tables_ 做一次线性查找，用 strcasecmp 忽略大小写，
   * 与 SQL 标识符大小写不敏感语义保持一致。
   */
  Table *find_table(const char *table_name) const;

  /**
   * @brief 获取全部参与查询的表
   * @return 表指针列表的常量引用
   * @details 用于 `SELECT *` 展开以及字段绑定时判断当前查询涉及多少张表。
   */
  const vector<Table *> &query_tables() const { return query_tables_; }

private:
  vector<Table *> query_tables_;
};

/**
 * @brief 绑定表达式
 * @details 绑定表达式，就是在SQL解析后，得到文本描述的表达式，将表达式解析为具体的数据库对象
 * @ingroup SQLParser
 */
class ExpressionBinder
{
public:
  /**
   * @brief 构造绑定器
   * @param context 绑定上下文，提供查询涉及的表集合（引用，生命周期由调用方保证）
   */
  ExpressionBinder(BinderContext &context) : context_(context) {}
  /** @brief 虚析构（默认实现），便于将来扩展为多态绑定器 */
  virtual ~ExpressionBinder() = default;

  /**
   * @brief 绑定一个表达式（递归入口/分派器）
   * @param expr               输入输出参数，待绑定的表达式；绑定后其子节点可能被替换为已绑定节点
   * @param bound_expressions  输出参数，绑定结果（本实现要求恰好产生一个表达式）
   * @return RC::SUCCESS 绑定成功；否则为字段/表不存在、类型不匹配等语义错误码
   * @details 实现原理：按 expr->type() 分派到对应的 bind_xxx_expression；其中星号、
   * 未绑定字段可能展开为多个表达式，其余节点递归绑定子节点后原地替换。
   */
  RC bind_expression(unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions);

private:
  /**
   * @brief 绑定星号表达式（SELECT * / SELECT t.*）
   * @param star_expr         待绑定的 StarExpr
   * @param bound_expressions 输出：展开后的各字段表达式
   * @return RC::SUCCESS 成功；RC::SCHEMA_TABLE_NOT_EXIST 指定的表不存在
   * @details 作用：把 `*` 展开为具体字段列表；这里先收集目标表（显式指定表名或
   * FROM 中全部表），再逐表展开（详见 .cpp）。
   */
  RC bind_star_expression(unique_ptr<Expression> &star_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定未解析字段表达式（字段名在语法阶段尚未与具体表关联）
   * @param unbound_field_expr 待绑定的 UnboundFieldExpr
   * @param bound_expressions  输出：绑定后的字段表达式（`t.*` 时可能多个）
   * @return RC::SUCCESS 成功；表不确定/不存在、字段不存在时返回相应错误码
   * @details 作用：根据上下文确定字段所属表并查找 FieldMeta；单表时可省略表名前缀。
   */
  RC bind_unbound_field_expression(
      unique_ptr<Expression> &unbound_field_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定已解析字段表达式
   * @param field_expr        已经携带具体 Field 的表达式
   * @param bound_expressions 输出：原样转移该表达式
   * @return 恒为 RC::SUCCESS
   * @details 作用：无需再解析，仅完成所有权转移。
   */
  RC bind_field_expression(unique_ptr<Expression> &field_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定值表达式（常量）
   * @param value_expr        常量表达式
   * @param bound_expressions 输出：原样转移该表达式
   * @return 恒为 RC::SUCCESS
   * @details 作用：常量无需绑定，仅做所有权转移。
   */
  RC bind_value_expression(unique_ptr<Expression> &value_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定 CAST 类型转换表达式
   * @param cast_expr         待绑定的 CastExpr
   * @param bound_expressions 输出：绑定后的表达式
   * @return RC::SUCCESS 成功；RC::INVALID_ARGUMENT 子表达式数量异常；子绑定错误码
   * @details 作用：递归绑定唯一的子表达式并用结果原地替换（详见 .cpp）。
   */
  RC bind_cast_expression(unique_ptr<Expression> &cast_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定比较表达式
   * @param comparison_expr   待绑定的 ComparisonExpr
   * @param bound_expressions 输出：绑定后的表达式
   * @return RC::SUCCESS 成功；RC::INVALID_ARGUMENT 左右子表达式数量异常；子绑定错误码
   * @details 作用：递归绑定左右操作数，分别原位替换后返回原表达式。
   */
  RC bind_comparison_expression(
      unique_ptr<Expression> &comparison_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定布尔连接表达式（AND / OR）
   * @param conjunction_expr   待绑定的 ConjunctionExpr
   * @param bound_expressions  输出：绑定后的表达式
   * @return RC::SUCCESS 成功；RC::INVALID_ARGUMENT 子表达式数量异常；子绑定错误码
   * @details 作用：逐个绑定 children 并原位替换，保持原有布尔结构。
   */
  RC bind_conjunction_expression(
      unique_ptr<Expression> &conjunction_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定算术表达式（+ - * / 及一元负号）
   * @param arithmetic_expr   待绑定的 ArithmeticExpr
   * @param bound_expressions 输出：绑定后的表达式
   * @return RC::SUCCESS 成功；RC::SCHEMA_FIELD_TYPE_MISMATCH 操作数非数值类型；
   *         RC::INVALID_ARGUMENT 子表达式数量异常
   * @details 作用：递归绑定操作数并做类型一致性校验，只允许数值类型参与运算。
   */
  RC bind_arithmetic_expression(
      unique_ptr<Expression> &arithmetic_expr, vector<unique_ptr<Expression>> &bound_expressions);
  /**
   * @brief 绑定聚合表达式（SUM/AVG/COUNT/MAX/MIN）
   * @param aggregate_expr    待绑定的 UnboundAggregateExpr
   * @param bound_expressions 输出：绑定后的 AggregateExpr
   * @return RC::SUCCESS 成功；无效聚合名/嵌套聚合/类型不匹配等错误码
   * @details 作用：解析聚合名与子表达式，`COUNT(*)` 特化为常量 1，再做聚合合法性校验。
   */
  RC bind_aggregate_expression(
      unique_ptr<Expression> &aggregate_expr, vector<unique_ptr<Expression>> &bound_expressions);

private:
  BinderContext &context_;
};

/**
 * @brief 语义错误消息（含行列定位）的线程本地跨层通道
 * @details 语义错误在表达式绑定阶段被检测到，但此时返回的只有 RC 码。为了让上层
 * （如 resolve_stage）能够把带行列位置的结构化错误消息透出给用户，这里用一个
 * 线程独立的槽位来传递消息。每次语句处理开始前需调用 reset_binder_error_message()。
 */

/**
 * @brief 清空线程本地的绑定错误消息
 * @details 实现原理：置空 thread_local 字符串。每条语句开始语义解析前调用，
 * 防止上一条语句的错误消息被误用于当前语句。
 */
void reset_binder_error_message();
/**
 * @brief 写入线程本地的绑定错误消息
 * @param msg 已格式化的错误文本（通常含行号、列号）
 * @details 绑定器在检出错误时调用，把无法放在 RC 码里的定位信息暂存下来。
 */
void set_binder_error_message(const std::string &msg);
/**
 * @brief 读取线程本地的绑定错误消息
 * @return 当前错误消息的常量引用；无错误时为空串
 * @details ResolveStage 在创建 Stmt 失败后调用，取出并透出给用户。
 */
const std::string &get_binder_error_message();
