// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  model_completion_provider.h:48 CompletionContext
//  model_completion_provider.h:60 ModelCompletionProvider
//  model_completion_provider.h:72 ModelCompletionProvider
//  model_completion_provider.h:80 available
//  model_completion_provider.h:89 complete


#pragma once

#include <memory>
#include <optional>
#include <string>

#include "sql/autocomplete/completion_scope.h"
#include "sql/autocomplete/sql_completion_config.h"

class Db;
class LlamaCompletionClient;
class SqlModelContextBuilder;
class ModelCompletionValidator;

/**
 * @file model_completion_provider.h
 * @brief 基于 Qwen2.5-Coder FIM 的补全 provider
 * @ingroup SQLAutocomplete
 *
 * 本文件封装模型补全链路：可用性判断 -> 上下文构造 -> /infill 调用 -> 输出校验。
 * 它是确定性补全的增强项，不参与语法合法性判定。
 *
 * 核心原则：模型不可用/超时/校验失败时一律返回空 optional，绝不阻塞或影响确定性补全。
 */

/**
 * @brief 送往模型的补全上下文
 */
struct CompletionContext
{
  std::string     statement_prefix;  ///< 光标之前的当前语句
  std::string     statement_suffix;  ///< 光标之后的当前语句
  CompletionScope scope;             ///< 当前语句引用的表
  Db             *db = nullptr;      ///< 当前数据库；用于构造 schema 与 Catalog 校验
  std::string     partial;           ///< 光标处正在输入的前缀
};

/**
 * @brief 模型补全 provider
 */
class ModelCompletionProvider
{
public:
  /**
   * @brief 构造 provider
   * @param client HTTP 客户端（可为空，为空时 available() 返回 false）
   * @param config 补全配置
   * @details 实现原理：保存 client 与 config，并创建 context builder 与 validator。
   */
  ModelCompletionProvider(std::shared_ptr<LlamaCompletionClient> client, SqlCompletionConfig config);

  /// 析构；unique_ptr 成员在此处需要完整类型，故在 .cpp 中定义为 default
  ~ModelCompletionProvider();

  /**
   * @brief 判断模型路径当前是否可用
   * @return true 表示配置开启、client 非空且健康检查通过
   * @details 实现原理：短路与运算 config_.model_enabled && config_.enabled &&
   *          client_ != nullptr && client_->health()。
   */
  bool available() const;

  /**
   * @brief 获取模型补全文本
   * @param context 补全上下文
   * @return 校验通过时返回补全文本；任何环节失败返回 std::nullopt
   * @details 实现原理：available 检查 -> builder 构造 dialect/schema -> 组装 InfillRequest
   *          -> client->infill -> validator 校验；全链路任一失败返回空。
   */
  std::optional<std::string> complete(const CompletionContext &context);

private:
  std::shared_ptr<LlamaCompletionClient> client_;     ///< HTTP 客户端
  SqlCompletionConfig                    config_;     ///< 配置副本
  std::unique_ptr<SqlModelContextBuilder> builder_;   ///< 上下文构造器
  std::unique_ptr<ModelCompletionValidator> validator_;  ///< 输出校验器
};
