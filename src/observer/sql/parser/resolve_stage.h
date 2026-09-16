// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  resolve_stage.h:44             执行Resolve，将解析后的SQL语句，转换成各种Stmt(Statement), 同时会做错误检查
//  resolve_stage.h:55             处理一次语义解析请求


/**
 * @file resolve_stage.h
 * @brief 语义解析阶段（ResolveStage）的声明
 * @ingroup SQLStage
 * @details 本文件是编译器流水线中 ParsedSqlNode AST 与 Stmt 之间的桥梁：
 *   SQL 文本 -> Flex token -> Bison LALR -> ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage -> Stmt。
 * 核心实现原则：语法分析只保证结构正确，本阶段才结合数据库元数据（表、字段、
 * 类型等）做语义校验，并把语法树转换成可直接执行的 Stmt 对象。
 */

#pragma once

#include "common/sys/rc.h"

class SQLStageEvent;

/**
 * @brief 执行Resolve，将解析后的SQL语句，转换成各种Stmt(Statement), 同时会做错误检查
 * @ingroup SQLStage
 * @details 该 Stage 依赖 Session 当前数据库，委托 Stmt::create_stmt() 完成
 * 具体的类型分派与表达式绑定（内部会使用 ExpressionBinder）。
 */
class ResolveStage
{
public:
  /**
   * @brief 处理一次语义解析请求
   * @param sql_event 承载 ParsedSqlNode、会话与结果对象的事件
   * @return RC::SUCCESS 表示已生成 Stmt；RC::SCHEMA_DB_NOT_EXIST 表示未选择数据库；
   *         其余为 Stmt/表达式绑定阶段返回的语义错误码
   * @details 实现原理：先校验当前会话已选择数据库，再调用 Stmt::create_stmt()，
   * 并在失败时把 ExpressionBinder 记录的行列级错误消息透出给用户（详见 .cpp）。
   */
  RC handle_request(SQLStageEvent *sql_event);
};
