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
#include <optional>
#include <string>
#include <vector>

/**
 * @brief llama.cpp /infill 的极简 HTTP 客户端
 * @ingroup SQLAutocomplete
 * @details 只负责 HTTP，不理解 SQL。连接失败/超时一律返回空，不抛异常到 SQL 主流程。
 */
struct ExtraContextFile
{
  std::string filename;
  std::string text;
};

struct InfillRequest
{
  std::string                   prefix;
  std::string                   suffix;
  std::vector<ExtraContextFile> extra;
  int                           max_tokens  = 32;
  double                        temperature = 0.0;
};

struct InfillResult
{
  std::string text;
  int64_t     elapsed_ms = 0;
};

class LlamaCompletionClient
{
public:
  LlamaCompletionClient(std::string base_url, int timeout_ms);
  ~LlamaCompletionClient() = default;

  /// 带 TTL 缓存的健康检查
  bool health();

  /// 返回空表示失败/超时，调用方应静默降级
  std::optional<InfillResult> infill(const InfillRequest &request);

private:
  std::string base_url_;
  std::string host_;
  int         port_       = 8012;
  int         timeout_ms_ = 800;

  int64_t last_health_check_ms_ = 0;
  bool    last_health_ok_       = false;
};
