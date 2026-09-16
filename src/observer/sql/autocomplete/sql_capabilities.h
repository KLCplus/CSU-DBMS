// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_capabilities.h:38      SqlCapabilities
//  sql_capabilities.h:65      instance
//  sql_capabilities.h:72      is_keyword
//  sql_capabilities.h:79      is_operator
//  sql_capabilities.h:86      is_type
//  sql_capabilities.h:95      is_forbidden
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

#pragma once

#include <string>
#include <unordered_set>

/**
 * @file sql_capabilities.h
 * @brief 运行时 SQL 能力表（方言门禁）
 * @ingroup SQLAutocomplete
 *
 * 本文件是补全模块的「方言真值源」：只描述“当前 Parser/Executor 真实支持”的 SQL 子集。
 * Grammar 补全会按它过滤候选，模型输出校验也会用它做禁用关键字白名单截断，
 * 从而避免补全或模型扩展出项目尚未实现的方言。
 *
 * 核心原则：能力集合必须与 sql/parser/yacc_sql.y 中的真实 terminal 保持一致；
 * 能力表只做只读查询，进程内以单例复用。
 */
struct SqlCapabilities
{
  // 必备能力（requirements.md 必做项）
  bool create = true;
  bool insert = true;
  bool select = true;
  bool delete_stmt = true;
  bool where = true;

  // 进阶能力：本项目 Parser/Executor 已实现，故开启
  bool update = true;
  bool order_by = true;
  bool group_by = true;
  bool join = true;
  bool arithmetic = true;
  bool null_value = true;
  bool table_alias = false;  ///< 当前 grammar 不支持 FROM ... alias

  std::unordered_set<std::string> keywords;   ///< 全部大写的已支持关键字
  std::unordered_set<std::string> operators;  ///< 已支持运算符
  std::unordered_set<std::string> types;      ///< 已支持数据类型

  /**
   * @brief 获取进程内唯一的能力表实例
   * @return 能力表常量引用
   * @details 实现原理：函数内静态局部变量（C++11 起线程安全初始化）首次调用时构建一次，之后复用。
   */
  static const SqlCapabilities &instance();

  /**
   * @brief 查询关键字是否受支持
   * @param upper_word 全大写关键字（调用方负责转大写）
   * @return true 表示在 keywords 集合中
   */
  bool is_keyword(const std::string &upper_word) const;

  /**
   * @brief 查询运算符是否受支持
   * @param symbol 运算符字面量
   * @return true 表示在 operators 集合中
   */
  bool is_operator(const std::string &symbol) const;

  /**
   * @brief 查询数据类型是否受支持
   * @param upper_word 全大写类型名（调用方负责转大写）
   * @return true 表示在 types 集合中
   */
  bool is_type(const std::string &upper_word) const;

  /**
   * @brief 关键字/类型是否被禁止（模型输出过滤用）
   * @param upper_word 全大写单词
   * @return true 表示属于明确不支持的方言（如 HAVING/LIMIT/DISTINCT）
   * @details 与 is_keyword 不同，这里维护的是黑名单：即使某个单词在语法里可能作为
   *          普通标识符出现，只要它代表本项目未实现的语法能力，也应禁止模型推荐。
   */
  bool is_forbidden(const std::string &upper_word) const;
};
