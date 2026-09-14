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
 * @brief 轻量 SQL 文本扫描器
 * @ingroup SQLAutocomplete
 *
 * 只做与补全相关的文本切分（单词 / 数字 / 字符串 / 符号 / 注释），
 * 严格处理单行注释、块注释、单/双引号字符串与 `''`、`\` 转义。
 * 它不是 SQL 语法分析器，语法合法性仍由现有 Parser 判定。
 */
struct SqlTextToken
{
  enum class Kind
  {
    Word,     ///< 标识符或关键字
    Number,   ///< 数字字面量
    String,   ///< 带引号的字符串
    Symbol,   ///< 运算符或分隔符
    Comment,  ///< 注释
  };

  Kind        kind = Kind::Word;
  std::string text;
  size_t      begin = 0;  ///< 在原始文本中的起始偏移
  size_t      end   = 0;  ///< 结束偏移（不含）
};

/// 扫描整段文本，按出现顺序返回 token（包含注释与字符串）
void scan_sql_text(std::string_view sql, std::vector<SqlTextToken> &tokens);

/// 光标是否位于字符串或注释内部
bool cursor_in_string_or_comment(std::string_view sql, size_t cursor);
