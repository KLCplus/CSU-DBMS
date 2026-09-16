// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  grammar_completion_provider.h:41 GrammarCompletionContext
//  grammar_completion_provider.h:64 complete_grammar
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
#include <vector>

#include "sql/autocomplete/completion_types.h"
#include "sql/autocomplete/sql_capabilities.h"

/**
 * @file grammar_completion_provider.h
 * @brief 基于 Parser 期望集合的语法补全
 * @ingroup SQLAutocomplete
 *
 * 本文件实现确定性补全中的「关键字/类型/运算符」来源：它不自己维护关键字表，
 * 而是复用 bison 在光标处报告出的期望 terminal 集合（collect_expected_tokens），
 * 再叠加通过 Parser 试探确认的续写关键字。
 *
 * 核心原则：合法性完全由现有 Parser 决定，本文件只做「期望集 -> 用户可见文本」的映射、
 * 大小写风格适配、前缀过滤与能力过滤。
 */

/**
 * @brief 语法补全的输入上下文
 */
struct GrammarCompletionContext
{
  std::vector<std::string> expected_symbols;  ///< 来自 collect_expected_tokens
  std::vector<std::string> extra_keywords;    ///< 通过试探确认的合法关键字（已是最终文本，如 FROM）
  std::string              partial;           ///< 光标处正在输入的前缀（可为空）
  bool                     prefer_lower = false;  ///< 是否将关键字转为小写以匹配用户风格
  size_t                   replace_begin = 0;  ///< 替换区间起点（半截 token 起点）
  size_t                   replace_end   = 0;  ///< 替换区间终点（通常为光标）
};

/**
 * @brief 依据期望集合与试探关键字生成语法候选
 * @param context 语法补全上下文（期望符号、试探关键字、前缀、大小写风格、替换区间）
 * @param out 输出参数；候选会追加到该 vector 末尾
 * @return 无
 * @details 实现原理：
 *          1. 将 bison 内部 terminal 名（如 SELECT、INT_T、LBRACE）映射为面向用户的文本与种类；
 *          2. 对 Keyword/Type 做能力过滤：命中禁用黑名单、类型不在能力表、关键字不在能力表（NULL 例外）则跳过；
 *          3. 若非空 partial，则做大小写不敏感（运算符除外）的前缀过滤；
 *          4. 按 prefer_lower 决定关键字/类型是否转小写，运算符不转；
 *          5. 对 extra_keywords 做同样的能力与前缀过滤，并赋予更高的 score（试探确认更可信）；
 *          6. 用 seen 去重，保证同一文本只出现一次。
 */
void complete_grammar(const GrammarCompletionContext &context, std::vector<CompletionItem> &out);
