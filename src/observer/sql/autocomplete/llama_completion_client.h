// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  llama_completion_client.h:44 ExtraContextFile
//  llama_completion_client.h:53 InfillRequest
//  llama_completion_client.h:65 InfillResult
//  llama_completion_client.h:71 LlamaCompletionClient
//  llama_completion_client.h:83 LlamaCompletionClient
//  llama_completion_client.h:92 health
//  llama_completion_client.h:102 infill


#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file llama_completion_client.h
 * @brief llama.cpp /infill 的极简 HTTP 客户端
 * @ingroup SQLAutocomplete
 *
 * 本文件是模型补全链路的最底层：以 FIM（Fill-In-the-Middle）方式向本地
 * llama.cpp server 的 /infill 接口发 HTTP 请求，并提供带缓存的健康检查。
 *
 * 核心原则：只负责 HTTP，不理解 SQL；连接失败/超时/非 200/解析失败一律返回
 * 空 optional，绝不抛异常到 SQL 主流程，由调用方静默降级。
 */

/**
 * @brief 附加到 FIM 请求的额外上下文文件
 */
struct ExtraContextFile
{
  std::string filename;  ///< 文件名（如 dialect.sql、schema.sql）
  std::string text;      ///< 文件内容
};

/**
 * @brief 一次 /infill 请求参数
 */
struct InfillRequest
{
  std::string                   prefix;       ///< 光标之前的文本
  std::string                   suffix;       ///< 光标之后的文本
  std::vector<ExtraContextFile> extra;        ///< 额外上下文文件
  int                           max_tokens  = 32;   ///< 生成 token 上限
  double                        temperature = 0.0;  ///< 采样温度
};

/**
 * @brief 一次 /infill 结果
 */
struct InfillResult
{
  std::string text;           ///< 模型返回的补全内容
  int64_t     elapsed_ms = 0;  ///< 请求耗时（毫秒）
};

class LlamaCompletionClient
{
public:
  /**
   * @brief 构造客户端
   * @param base_url 形如 host:port 的基础地址（可带 HTTP 前缀）
   * @param timeout_ms 连接与读写超时（毫秒）
   * @details 实现原理：保存 base_url 与 timeout，并用 parse_base_url 解析出 host/port。
   */
  LlamaCompletionClient(std::string base_url, int timeout_ms);

  /// 默认析构；无资源需要释放
  ~LlamaCompletionClient() = default;

  /**
   * @brief 健康检查（带 TTL 缓存）
   * @return true 表示 2 秒内检测过且服务可达，或本次探测可达
   * @details 实现原理：命中 2 秒缓存窗口时直接返回上次结果；否则发起 TCP 连接
   *          （非阻塞 connect + poll 超时），成功建立连接并发出 `GET /health` 且写满即视为健康。
   *          结果缓存到 last_health_ok_。
   */
  bool health();

  /**
   * @brief 调用 /infill 获取 FIM 补全
   * @param request 请求参数
   * @return 成功时返回 InfillResult；失败/超时返回 std::nullopt
   * @details 实现原理：先 health() 检查；再构造 JSON（input_prefix/input_suffix/
   *          可选 input_extra/n_predict/temperature 等），TCP 连接后发送 HTTP POST，
   *          在超时内读取响应，仅接受 HTTP 200 且 content 非空的结果。
   */
  std::optional<InfillResult> infill(const InfillRequest &request);

private:
  std::string base_url_;  ///< 原始基础地址
  std::string host_;      ///< 解析出的主机名
  int         port_       = 8012;  ///< 解析出的端口
  int         timeout_ms_ = 800;   ///< 连接/读写超时

  int64_t last_health_check_ms_ = 0;      ///< 上次健康检查时间戳（毫秒）
  bool    last_health_ok_       = false;  ///< 上次健康检查结果
};
