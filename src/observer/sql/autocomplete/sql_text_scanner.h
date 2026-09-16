// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_text_scanner.h:37      SqlTextToken
//  sql_text_scanner.h:66      scan_sql_text
//  sql_text_scanner.h:79      cursor_in_string_or_comment
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

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file sql_text_scanner.h
 * @brief 轻量 SQL 文本扫描器
 * @ingroup SQLAutocomplete
 *
 * 本文件位于补全架构的最底层，为 statement 提取、作用域扫描、上下文判定等
 * 提供与语法无关的文本切分能力；它也用于判断光标是否处于字符串/注释内。
 *
 * 核心原则：只做与补全相关的文本切分（单词 / 数字 / 字符串 / 符号 / 注释），
 * 严格处理单行注释、块注释、单/双引号字符串与 `''`、`\` 转义。
 * 它不是 SQL 语法分析器，语法合法性仍由现有 Parser（bison）判定。
 */
struct SqlTextToken
{
  /// token 种类
  enum class Kind
  {
    Word,     ///< 标识符或关键字
    Number,   ///< 数字字面量
    String,   ///< 带引号的字符串
    Symbol,   ///< 运算符或分隔符
    Comment,  ///< 注释
  };

  Kind        kind = Kind::Word;  ///< token 种类
  std::string text;               ///< token 原始文本
  size_t      begin = 0;  ///< 在原始文本中的起始偏移
  size_t      end   = 0;  ///< 结束偏移（不含）
};

/**
 * @brief 扫描整段文本，按出现顺序返回 token（包含注释与字符串）
 * @param sql 待扫描的 SQL 文本
 * @param tokens 输出参数；函数入口会先 clear，再依次追加 token
 * @return 无
 * @details 实现原理：单趟从左到右扫描，跳过空白后按优先级识别：
 *          单行注释 `--`、块注释（`/`+`*` 起、`*`+`/` 止）、引号字符串、数字、标识符/关键字、
 *          多字符运算符（>= <= != <> ==）与单字符符号。
 *          字符串支持反斜杠转义与连续引号转义；未闭合的字符串/块注释
 *          会一直消费到文本末尾，形成「未闭合 token」。
 */
void scan_sql_text(std::string_view sql, std::vector<SqlTextToken> &tokens);

/**
 * @brief 判断光标是否位于字符串或注释内部
 * @param sql 完整 SQL 文本
 * @param cursor 光标偏移；越界时会被截断到 sql.size()
 * @return true 表示光标在字符串或注释内（此时应禁用补全）
 * @details 实现原理：先扫描全部 token，再对每个 String/Comment token 判断：
 *          1. 通过首尾字符推断该 token 是否已闭合（字符串首尾同为引号；
 *             块注释以 `/`+`*` 开头且以 `*`+`/` 结尾；单行注释视为以行尾结束）；
 *          2. 光标严格落在 token 区间内（已闭合时 cursor < end，未闭合时 cursor <= end）即命中；
 *          3. 额外把「光标恰在注释 token 末尾」视为仍在注释内，覆盖 `-- FRO|` 的输入场景。
 */
bool cursor_in_string_or_comment(std::string_view sql, size_t cursor);
