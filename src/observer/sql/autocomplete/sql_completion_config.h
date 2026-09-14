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

#include <cstdint>
#include <string>

/**
 * @brief SQL 补全配置
 * @ingroup SQLAutocomplete
 * @details 默认值集中在这里，避免散落 magic number；可由 config/sql_completion.json 覆盖。
 */
struct SqlCompletionConfig
{
  bool        enabled                = true;
  bool        model_enabled          = true;
  std::string llama_base_url         = "http://127.0.0.1:8012";
  int         model_debounce_ms      = 150;
  int         model_http_timeout_ms  = 800;
  int         model_max_tokens       = 32;
  double      model_temperature      = 0.0;
  int         max_completion_items   = 12;
  int         max_schema_tables      = 8;
  int         max_schema_columns     = 128;
  int         max_context_tokens     = 4096;

  /// 从环境变量 CSUDB_SQL_COMPLETION_CONFIG 或若干默认路径加载，失败则使用默认值
  static SqlCompletionConfig load();
};
