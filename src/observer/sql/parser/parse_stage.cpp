// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  parse_stage.cpp:59             处理一次解析请求：SQL 文本 -> ParsedSqlNode，并写入事件

/**
 * @file parse_stage.cpp
 * @brief 解析阶段（ParseStage）的实现
 * @details 本文件位于编译器流水线的“语法分析”环节：接收 SQL 文本，调用
 * parse() 驱动 Flex 词法 + Bison LALR 语法分析，得到 ParsedSqlNode AST，
 * 随后交给 ResolveStage 转换为 Stmt。
 * 核心实现原则：本阶段只做语法层面的成功/失败判定与结果搬运，不访问数据库元数据；
 * 任何语义问题都留给 ResolveStage 处理。
 */

#include <string.h>

#include "parse_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/parser/parse.h"

using namespace common;

/**
 * @brief 处理一次解析请求：SQL 文本 -> ParsedSqlNode，并写入事件
 * @param sql_event 承载 SQL 文本、会话与结果对象的阶段事件
 * @return RC::SUCCESS 解析成功并已设置 sql_node；
 *         RC::INTERNAL 表示没有解析出任何语句节点；
 *         RC::SQL_SYNTAX 表示解析结果中存在 SCF_ERROR 节点
 * @details 实现原理：
 *   1. 调用 parse() 让 Bison/Flex 完成词法与语法分析，结果汇集到
 *      ParsedSqlResult；
 *   2. 若结果为空，说明未产生任何语句，直接返回 INTERNAL（返回码语义上先被
 *      set_return_code 置为 SUCCESS，随后由调用方按 INTERNAL 处理）；
 *   3. 逐条扫描节点，只要出现 SCF_ERROR 就立即把语法错误消息反馈给用户并返回，
 *      这样即使多语句尾部存在非法内容也不会被忽略；
 *   4. 确认全部合法后，取第一个节点移入 event（当前实现仅处理一条语句）。
 */
RC ParseStage::handle_request(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  SqlResult         *sql_result = sql_event->session_event()->sql_result();
  const string &sql        = sql_event->sql();

  ParsedSqlResult parsed_sql_result;

  // 驱动 Flex + Bison 完成一次完整的词法/语法分析
  parse(sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().empty()) {
    // 空输入或未产生任何语句节点：不视为语法错误，但本阶段无节点可交付
    sql_result->set_return_code(RC::SUCCESS);
    sql_result->set_state_string("");
    return RC::INTERNAL;
  }

  if (parsed_sql_result.sql_nodes().size() > 1) {
    // 语法上允许多条语句连续出现，但执行层目前只处理第一条
    LOG_WARN("got multi sql commands but only 1 will be handled");
  }

  // 只要任意一条语句解析失败，就把该错误反馈给用户（避免尾部非法内容被忽略）
  for (const unique_ptr<ParsedSqlNode> &node : parsed_sql_result.sql_nodes()) {
    if (node->flag == SCF_ERROR) {
      rc = RC::SQL_SYNTAX;
      sql_result->set_return_code(rc);
      if (!node->error.error_msg.empty()) {
        // 优先使用 yyreport_syntax_error/yyerror 生成的带行列定位的详细消息
        sql_result->set_state_string(node->error.error_msg);
      } else {
        sql_result->set_state_string("Failed to parse sql");
      }
      return rc;
    }
  }

  // 校验通过：取首个语句节点转移所有权到事件，供 ResolveStage 使用
  unique_ptr<ParsedSqlNode> sql_node = std::move(parsed_sql_result.sql_nodes().front());
  sql_event->set_sql_node(std::move(sql_node));

  return RC::SUCCESS;
}
