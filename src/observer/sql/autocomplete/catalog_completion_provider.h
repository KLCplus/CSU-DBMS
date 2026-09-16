// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  catalog_completion_provider.h:43 CatalogCompletionContext
//  catalog_completion_provider.h:65 complete_catalog
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
#include <string_view>
#include <vector>

#include "sql/autocomplete/completion_scope.h"
#include "sql/autocomplete/completion_types.h"

class Db;

/**
 * @file catalog_completion_provider.h
 * @brief 基于真实 Catalog 的表 / 列补全
 * @ingroup SQLAutocomplete
 *
 * 本文件实现确定性补全中的「表名/列名/INSERT 值」来源：表与列全部来自真实
 * 存储层 Catalog（Db/Table/TableMeta），并结合轻量作用域与语句前缀判断光标意图。
 *
 * 核心原则：不猜测不存在的表/列；只有在语法上确实需要标识符（语法期望 ID）
 * 或用户已在输入 `table.` 限定名时才产出 Catalog 候选。
 */

/**
 * @brief Catalog 补全的输入上下文
 */
struct CatalogCompletionContext
{
  Db                        *db = nullptr;       ///< 当前数据库；为空时不做任何 Catalog 补全
  std::string_view           statement_prefix;  ///< 光标之前的当前语句
  CompletionScope            scope;             ///< 当前语句已引用的表作用域
  std::vector<std::string>   expected_symbols;  ///< bison 期望 terminal 集合，用于判断是否期待标识符
  std::string                partial;  ///< 正在输入的前缀（可能含 "table."）
  size_t                     replace_begin = 0;  ///< 替换区间起点
  size_t                     replace_end   = 0;  ///< 替换区间终点
};

/**
 * @brief 依据 Catalog 与光标意图生成表/列候选
 * @param context Catalog 补全上下文
 * @param out 输出参数；候选追加到该 vector 末尾
 * @return 无
 * @details 实现原理：
 *          1. 若语法期望集合中包含 ID，或 partial 形如 `table.`，才继续；
 *          2. resolve_context 判定光标处于表名/列名/INSERT 列清单/INSERT 值中的哪一种；
 *          3. 按种类分别调用表补全、指定表列补全、作用域内多表列补全或字面量补全；
 *          4. 所有候选经 add_item 去重后追加。
 */
void complete_catalog(const CatalogCompletionContext &context, std::vector<CompletionItem> &out);
