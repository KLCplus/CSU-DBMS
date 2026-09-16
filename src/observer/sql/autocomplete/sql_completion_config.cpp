// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  sql_completion_config.cpp:46 load_from_file
//  sql_completion_config.cpp:85 load


#include "sql/autocomplete/sql_completion_config.h"

#include <fstream>
#include <vector>

#include "common/log/log.h"
#include "json/json.h"

/**
 * @file sql_completion_config.cpp
 * @ingroup SQLAutocomplete
 * @brief 补全配置的加载实现
 *
 * 本文件从 JSON 文件读取补全参数并覆盖默认值，找不到或解析失败时静默回退。
 * 核心原则：逐字段按存在性覆盖，未知字段忽略；配置错误绝不导致进程失败。
 */

namespace {

/**
 * @brief 尝试从单个 JSON 文件加载配置
 * @param path 文件路径
 * @param config 输入输出参数；成功时用文件中的字段覆盖对应默认值
 * @return true 表示文件存在、解析成功且为 JSON 对象
 * @details 实现原理：打开文件失败直接返回 false；用 Json::CharReader 解析整段内容，
 *          解析失败或根不是对象时记 WARN 并返回 false；
 *          否则对每个已知字段用 isMember 判断存在后覆盖，返回 true。
 *          未知字段被忽略，缺省字段保留原值。
 */
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

/**
 * @brief 从环境变量或默认路径加载配置
 * @return 加载后的配置；全部候选路径都失败时返回默认值
 * @details 实现原理：先放入环境变量指定路径（若非空），再依次追加若干默认相对路径；
 *          按顺序尝试 load_from_file，首个成功者记录 INFO 日志后返回；
 *          均失败则返回默认构造 config（不报错）。
 */
SqlCompletionConfig SqlCompletionConfig::load()
{
  SqlCompletionConfig config;

  // 候选路径优先级：环境变量 > 工作目录 config/ > etc/ 及其上两级的 etc/
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
