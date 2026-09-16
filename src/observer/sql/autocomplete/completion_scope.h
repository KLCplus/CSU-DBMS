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
#include <string_view>
#include <vector>

/**
 * @file completion_scope.h
 * @brief 补全用轻量作用域
 * @ingroup SQLAutocomplete
 *
 * 本文件为 Catalog 列补全与模型上下文提供「当前语句引用了哪些表」的保守视图。
 * 它通过 token 扫描得到 FROM / JOIN / INSERT INTO 后的表名。
 *
 * 核心原则：只做保守提取——无法确认表名时不添加，宁少不误；
 * 当前 grammar 不支持表别名，因此不维护 alias 绑定。
 */

/**
 * @brief 表与别名的绑定关系
 */
struct TableBinding
{
  std::string table_name;  ///< 表名
  std::string alias;       ///< 预留；当前恒为空
};

/**
 * @brief 当前语句的轻量作用域
 */
struct CompletionScope
{
  std::vector<TableBinding> tables;  ///< 已引用的表（按首次出现顺序去重）

  /**
   * @brief 判断作用域中是否已包含某表
   * @param table_name 表名
   * @return true 表示包含
   */
  bool contains(const std::string &table_name) const;

  /**
   * @brief 返回作用域内所有表名
   * @return 表名列表（保持 tables 中的顺序）
   */
  std::vector<std::string> table_names() const;
};

/**
 * @brief 从光标之前的语句前缀中保守提取作用域
 * @param statement_prefix 光标之前的当前语句文本
 * @param scope 输出参数；入口会先清空再填充
 * @return 无
 * @details 实现原理：分词后遍历 Word：
 *          遇到 INTO 时取其后的第一个 Word 作为表；
 *          遇到 FROM/JOIN 时向后收集表名（FROM 支持逗号分隔多表，JOIN 只取一个），
 *          遇到子句边界关键字或非逗号符号即停止；
 *          最后按表名去重并保持首次出现顺序。无法确认表名时不添加。
 */
void build_completion_scope(std::string_view statement_prefix, CompletionScope &scope);
