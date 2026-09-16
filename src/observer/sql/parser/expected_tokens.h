// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  expected_tokens.h:47           复用现有 bison 语法分析器，返回给定 SQL 前缀在结尾处合法的 terminal 集合
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

/**
 * @file expected_tokens.h
 * @brief 自动补全：暴露 SQL 前缀处合法的终结符集合
 * @ingroup SQLParser
 * @details 本文件服务于编译器的“智能提示”能力，复用同一条流水线
 *   SQL 文本 -> Flex token -> Bison LALR -> ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage -> Stmt，
 * 直接向 Bison 的 LALR 状态机询问“下一个合法 token 有哪些”。
 * 核心实现原则：不另建语法分析器，直接复用同一份 Bison 文法；由调用方（补全层）在
 * 光标前缀末尾追加哨兵字符 '\x01' 触发语法错误，本函数再借 Bison
 * `%define parse.error custom` 的 yypcontext_expected_tokens 取出期望集合，
 * 从而让补全与语法诊断共享同一份语法信息。注意：本函数自身不追加哨兵。
 */

#include <string>
#include <vector>

/**
 * @brief 复用现有 bison 语法分析器，返回给定 SQL 前缀在结尾处合法的 terminal 集合
 *
 * 实现方式：调用方需先在 sql 末尾追加哨兵字符（补全层使用 '\x01'）再调用本函数，
 * Parser 会在该哨兵处产生语法错误，本函数通过 yypcontext_expected_tokens 收集期望集合。
 * 本函数自身不追加哨兵；这样自动补全与语法诊断使用同一份信息，不引入第二套 SQL parser。
 *
 * @param sql     待分析的 SQL 前缀
 * @param tokens  输出：期望的 terminal 符号名（如 SELECT / FROM / ID / LBRACE）
 * @param line    输出：语法错误发生行（哨兵所在行）
 * @param column  输出：语法错误发生列（哨兵所在列）
 * @return 期望符号个数；<0 表示失败
 */
int collect_expected_tokens(const char *sql, std::vector<std::string> &tokens, int &line, int &column);
