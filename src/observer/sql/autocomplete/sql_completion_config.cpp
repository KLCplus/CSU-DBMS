/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/autocomplete/sql_completion_config.h"

#include <fstream>
#include <vector>

#include "common/log/log.h"
#include "json/json.h"

namespace {

bool load_from_file(const std::string &path, SqlCompletionConfig &config)
{
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!reader->parse(content.data(), content.data() + content.size(), &root, &errors) || !root.isObject()) {
    LOG_WARN("failed to parse sql completion config: %s (%s)", path.c_str(), errors.c_str());
    return false;
  }

  if (root.isMember("enabled")) config.enabled = root["enabled"].asBool();
  if (root.isMember("model_enabled")) config.model_enabled = root["model_enabled"].asBool();
  if (root.isMember("llama_base_url")) config.llama_base_url = root["llama_base_url"].asString();
  if (root.isMember("model_debounce_ms")) config.model_debounce_ms = root["model_debounce_ms"].asInt();
  if (root.isMember("model_http_timeout_ms")) config.model_http_timeout_ms = root["model_http_timeout_ms"].asInt();
  if (root.isMember("model_max_tokens")) config.model_max_tokens = root["model_max_tokens"].asInt();
  if (root.isMember("model_temperature")) config.model_temperature = root["model_temperature"].asDouble();
  if (root.isMember("max_completion_items")) config.max_completion_items = root["max_completion_items"].asInt();
  if (root.isMember("max_schema_tables")) config.max_schema_tables = root["max_schema_tables"].asInt();
  if (root.isMember("max_schema_columns")) config.max_schema_columns = root["max_schema_columns"].asInt();
  if (root.isMember("max_context_tokens")) config.max_context_tokens = root["max_context_tokens"].asInt();
  return true;
}

}  // namespace

SqlCompletionConfig SqlCompletionConfig::load()
{
  SqlCompletionConfig config;

  std::vector<std::string> candidates;
  const char              *env = getenv("CSUDB_SQL_COMPLETION_CONFIG");
  if (env != nullptr && env[0] != '\0') {
    candidates.emplace_back(env);
  }
  candidates.emplace_back("config/sql_completion.json");
  candidates.emplace_back("etc/sql_completion.json");
  candidates.emplace_back("../etc/sql_completion.json");
  candidates.emplace_back("../../etc/sql_completion.json");

  for (const std::string &path : candidates) {
    if (load_from_file(path, config)) {
      LOG_INFO("loaded sql completion config from %s", path.c_str());
      return config;
    }
  }
  return config;
}
