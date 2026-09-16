/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi
//

/**
 * @file parse.cpp
 * @brief SQL 解析入口与 ParsedSqlNode/ParsedSqlResult 的基础实现
 * @ingroup SQLParser
 * @details 本文件处于编译器流水线起始处：上层 ParseStage 调用 parse()，
 * parse() 再转发给 Bison 生成的 sql_parse()。整条流水线为：
 *   SQL 文本 -> Flex token -> Bison LALR -> ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage -> Stmt。
 * 核心实现原则：这里只做薄转发与容器的基本构造/追加，不包含词法、语法或语义逻辑，
 * 使语法实现细节（yacc_sql.y / lex_sql.l）与调用方解耦。
 */

#include "sql/parser/parse.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"

// 旧接口的前向声明（历史遗留，当前实现走 parse(const char*, ParsedSqlResult*)）
RC parse(char *st, ParsedSqlNode *sqln);

/**
 * @brief 默认构造函数：把语句类型初始化为 SCF_ERROR
 * @details 实现原理：默认即错误态是一种防御性设计，若解析过程中没有显式设置 flag，
 * 该节点会被 ParseStage 视为解析失败，避免出现“未分类语句”被误当作合法语句执行。
 */
ParsedSqlNode::ParsedSqlNode() : flag(SCF_ERROR) {}

/**
 * @brief 以指定语句类型构造 ParsedSqlNode
 * @param _flag SQL 命令类型（如 SCF_SELECT、SCF_INSERT 等）
 * @details 实现原理：Bison 语法动作在识别出具体语句后，用对应的 SqlCommandFlag
 * 构造节点，从而让后续 ParseStage/ResolveStage 通过 flag 分派处理逻辑。
 */
ParsedSqlNode::ParsedSqlNode(SqlCommandFlag _flag) : flag(_flag) {}

/**
 * @brief 向结果集中追加一条已解析的 SQL 语句
 * @param sql_node 待追加的语句节点（独占所有权，按值传入后转移）
 * @details 实现原理：使用 emplace_back + std::move 把 unique_ptr 直接移入 vector，
 * 全程不发生深拷贝，保证 AST 节点所有权的唯一性。
 */
void ParsedSqlResult::add_sql_node(unique_ptr<ParsedSqlNode> sql_node)
{
  sql_nodes_.emplace_back(std::move(sql_node));
}

////////////////////////////////////////////////////////////////////////////////

// 由 yacc_sql.y / Bison 生成的底层解析函数，parse() 仅作转发
int sql_parse(const char *st, ParsedSqlResult *sql_result);

/**
 * @brief 解析 SQL 文本的对外实现
 * @param st         以 '\0' 结尾的 SQL 字符串
 * @param sql_result 输出参数，收集解析得到的 ParsedSqlNode 列表
 * @return 恒为 RC::SUCCESS（错误经由 sql_result 中的 SCF_ERROR 节点表达）
 * @details 实现原理：转调 Bison 生成的 sql_parse()，把词法/语法分析完全委托给
 * 生成的解析器；解析器内部通过 yyparse 驱动 lex_sql 的 yylex 完成 LALR 归约。
 */
RC parse(const char *st, ParsedSqlResult *sql_result)
{
  sql_parse(st, sql_result);
  return RC::SUCCESS;
}
