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

/**
 * @brief 复用现有 bison 语法分析器，返回给定 SQL 前缀在结尾处合法的 terminal 集合
 *
 * 实现方式：在输入末尾追加一个非法哨兵字符，让 Parser 在该位置产生语法错误，
 * 再通过 %define parse.error custom 的 yypcontext_expected_tokens 取出期望集合。
 * 这样自动补全与语法诊断使用同一份信息，不引入第二套 SQL parser。
 *
 * @param sql     待分析的 SQL 前缀
 * @param tokens  输出：期望的 terminal 符号名（如 SELECT / FROM / ID / LBRACE）
 * @param line    输出：语法错误发生行（哨兵所在行）
 * @param column  输出：语法错误发生列（哨兵所在列）
 * @return 期望符号个数；<0 表示失败
 */
int collect_expected_tokens(const char *sql, std::vector<std::string> &tokens, int &line, int &column);
