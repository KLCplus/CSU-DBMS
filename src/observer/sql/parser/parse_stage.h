// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  parse_stage.h:44               解析SQL语句，解析后的结果可以参考parse_defs.h
//  parse_stage.h:55               处理一次解析请求
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

//
// Created by Longda on 2021/4/13.
//

/**
 * @file parse_stage.h
 * @brief 解析阶段（ParseStage）的声明
 * @ingroup SQLStage
 * @details 本文件是编译器流水线的第一个 Stage：位于 SQL 文本 -> Flex token
 * -> Bison LALR -> ParsedSqlNode AST 之后，负责把 AST 绑定到 SQLStageEvent，
 * 再交给 ResolveStage -> Stmt。
 * 核心实现原则：Stage 模式把“解析”封装为一个可组合的处理单元，ParseStage 只关心
 * 从 SQL 文本到 ParsedSqlNode 的产出及错误判定，不涉及语义绑定与执行。
 */

#pragma once

#include "common/sys/rc.h"

class SQLStageEvent;

/**
 * @brief 解析SQL语句，解析后的结果可以参考parse_defs.h
 * @ingroup SQLStage
 * @details 该 Stage 读取 SQLStageEvent 中的 SQL 文本，调用 parse() 得到
 * ParsedSqlResult，并从中取出唯一的 ParsedSqlNode 放入 event，供后续阶段使用。
 */
class ParseStage
{
public:
  /**
   * @brief 处理一次解析请求
   * @param sql_event 承载本次请求 SQL 文本、会话及结果对象的事件
   * @return RC::SUCCESS 表示解析成功且节点已写入 event；
   *         RC::INTERNAL 表示未解析出任何节点；RC::SQL_SYNTAX 表示存在语法错误节点
   * @details 实现原理：调用 parse() 生成 ParsedSqlResult，先检查是否为空，再扫描
   * 所有节点确认无 SCF_ERROR，最后取首个节点写入 event（详见 .cpp 中的实现）。
   */
  RC handle_request(SQLStageEvent *sql_event);
};
