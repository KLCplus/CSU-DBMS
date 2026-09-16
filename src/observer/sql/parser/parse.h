// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  parse.h:49                     解析一段 SQL 文本，并把解析结果写入 ParsedSqlResult

/**
 * @file parse.h
 * @brief SQL 解析对外入口的声明
 * @ingroup SQLParser
 * @details 本文件位于编译器流水线的最前端，向 ParseStage 暴露唯一的入口函数
 * parse()，用于把 SQL 文本交给 Flex/Bison 生成的词法与语法分析器。
 * 整条流水线为：
 *   SQL 文本 -> Flex 切分 token -> Bison LALR 归约得到 ParsedSqlNode AST
 *   -> ParseStage -> ResolveStage -> Stmt。
 * 核心实现原则：解析器只负责语法结构，不访问数据库目录（不查询表/字段是否存在），
 * 这些语义校验推迟到 ResolveStage 完成，从而实现语法与语义的分层。
 */

#pragma once

#include "common/sys/rc.h"
#include "sql/parser/parse_defs.h"

/**
 * @brief 解析一段 SQL 文本，并把解析结果写入 ParsedSqlResult
 * @ingroup SQLParser
 * @param st         以 '\0' 结尾的 SQL 字符串，通常是用户输入的一条或一段命令
 * @param sql_result 输出参数；解析得到的每个 ParsedSqlNode 会依次追加到它的
 *                   sql_nodes_ 中，语法错误则以 SCF_ERROR 节点形式追加
 * @return 当前实现恒返回 RC::SUCCESS；真正的语法错误通过 sql_result 内的
 *         SCF_ERROR 节点表达，由 ParseStage 负责判定并转换为错误码
 * @details 实现原理：直接转调 yacc_sql.y 中由 Bison 生成的 sql_parse()，
 * 自身不做任何额外处理，因此其行为与语法分析器完全一致。
 */
RC parse(const char *st, ParsedSqlResult *sql_result);
