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
 * @brief 补全用轻量作用域
 * @ingroup SQLAutocomplete
 * @details 通过 token 扫描得到当前语句已引用的表（FROM / JOIN / INSERT INTO）。
 *          当前 grammar 不支持表别名，因此不维护 alias 绑定。
 */
struct TableBinding
{
  std::string table_name;
  std::string alias;  ///< 预留；当前为空
};

struct CompletionScope
{
  std::vector<TableBinding> tables;

  bool contains(const std::string &table_name) const;
  std::vector<std::string> table_names() const;
};

/// 从光标之前的语句前缀中保守提取作用域；无法确认时不添加
void build_completion_scope(std::string_view statement_prefix, CompletionScope &scope);
