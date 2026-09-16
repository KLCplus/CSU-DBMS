// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_completion_config.h:33 SqlCompletionConfig
//  sql_completion_config.h:55 load


#pragma once

#include <cstdint>
#include <string>

/**
 * @file sql_completion_config.h
 * @brief SQL 补全配置
 * @ingroup SQLAutocomplete
 *
 * 本文件集中定义补全模块的可调参数（总开关、模型开关、HTTP 客户端参数、
 * 候选与 schema 上下文规模上限），并负责从 JSON 文件加载覆盖默认值。
 *
 * 核心原则：默认值集中在此，避免散落 magic number；加载失败时静默回退到默认值，
 * 保证补全功能在缺少配置文件时仍可工作。
 */
struct SqlCompletionConfig
{
  bool        enabled                = true;   ///< 补全总开关；false 时确定性补全也停用
  bool        model_enabled          = true;   ///< 模型补全开关；false 时完全不走模型路径
  std::string llama_base_url         = "http://127.0.0.1:8012";  ///< llama.cpp /infill 服务地址
  int         model_debounce_ms      = 150;    ///< 模型请求去抖（当前由 UI 侧使用）
  int         model_http_timeout_ms  = 800;    ///< HTTP 连接/读写超时，超时即丢弃模型结果
  int         model_max_tokens       = 32;     ///< 单次生成的 token 上限
  double      model_temperature      = 0.0;    ///< 采样温度；0 表示贪心
  int         max_completion_items   = 12;     ///< 返回候选条数上限
  int         max_schema_tables      = 8;      ///< 注入模型上下文的最大表数
  int         max_schema_columns     = 128;    ///< 注入模型上下文的最大列数
  int         max_context_tokens     = 4096;   ///< 模型上下文长度上限

  /**
   * @brief 从环境变量或默认路径加载配置
   * @return 加载后的配置；全部候选路径都失败时返回默认值
   * @details 实现原理：候选顺序为环境变量 CSUDB_SQL_COMPLETION_CONFIG（非空时优先）、
   *          config/sql_completion.json、etc/sql_completion.json 及若干相对路径；
   *          逐个尝试 load_from_file，首个成功的立即返回并记录日志。
   *          任何路径都不存在或解析失败时返回默认构造的 config。
   */
  static SqlCompletionConfig load();
};
