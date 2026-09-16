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
#include <string_view>

/**
 * @file current_statement_extractor.h
 * @brief 取出光标所在的单条 SQL statement 的范围
 * @ingroup SQLAutocomplete
 *
 * 本文件是补全链路的入口预处理之一：在可能包含多条语句的 SQL buffer 中，
 * 定位光标所在的那一条 statement，后续的 Parser 期望集合、Catalog 作用域等
 * 都只针对这条 statement 计算。
 *
 * 核心原则：切分只依据分词结果中「字符串/注释之外」的分号，绝不因为
 * 注释或字符串里的分号而误切；若光标越界则按 buffer 末尾处理。
 */

/**
 * @brief 单条 statement 在原始 SQL buffer 中的 [begin, end) 范围
 */
struct StatementSlice
{
  size_t begin = 0;      ///< statement 起始偏移（含）
  size_t end   = 0;      ///< statement 结束偏移（不含，通常为分号位置或 buffer 末尾）
  bool   found = false;  ///< 是否成功定位（当前实现始终为 true）
};

/**
 * @brief 定位光标所在的单条 SQL statement
 * @param sql 完整 SQL 文本
 * @param cursor 光标偏移；越界时会被截断到 sql.size()
 * @return 光标所在 statement 的 [begin, end) 范围
 * @details 实现原理：
 *          1. 用 scan_sql_text 对整段文本分词，得到字符串/注释等 token；
 *          2. 收集所有位于字符串/注释之外的分号位置作为语句分隔符；
 *          3. 从左到右扫描分隔符：第一个位置 >= cursor 的分号即为本条语句的结束，
 *             其前一个分隔符之后为起始；若都未命中，则取最后一个分隔符之后到 buffer 末尾。
 */
StatementSlice find_statement_at_cursor(std::string_view sql, size_t cursor);
