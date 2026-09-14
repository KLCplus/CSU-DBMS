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
 * @brief 运行时 SQL 能力表
 * @ingroup SQLAutocomplete
 *
 * 只描述“当前 Parser/Executor 真实支持”的 SQL 子集。
 * 补全器与模型输出校验都以它为唯一依据，避免补全/模型扩展出项目未实现的方言。
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

  static const SqlCapabilities &instance();

  bool is_keyword(const std::string &upper_word) const;
  bool is_operator(const std::string &symbol) const;
  bool is_type(const std::string &upper_word) const;

  /// 关键字/类型是否被禁止（模型输出过滤用）
  bool is_forbidden(const std::string &upper_word) const;
};
