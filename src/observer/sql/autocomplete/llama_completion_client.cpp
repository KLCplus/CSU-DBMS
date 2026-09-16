// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  llama_completion_client.cpp:53 now_ms
//  llama_completion_client.cpp:68 parse_base_url
//  llama_completion_client.cpp:98 LlamaCompletionClient
//  llama_completion_client.cpp:111 health
//  llama_completion_client.cpp:190 infill


#include "sql/autocomplete/llama_completion_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <sstream>

#include "common/log/log.h"
#include "json/json.h"

/**
 * @file llama_completion_client.cpp
 * @ingroup SQLAutocomplete
 * @brief llama.cpp /infill HTTP 客户端的实现
 *
 * 本文件用裸 socket + poll 实现极简 HTTP，避免引入额外依赖，并保证所有失败路径
 * 都返回空而不是异常。核心原则：模型服务不可用时快速失败，绝不阻塞 SQL 主流程。
 */

namespace {

/**
 * @brief 取当前单调时钟毫秒数
 * @return 自 steady_clock 纪元起的毫秒数
 * @details 实现原理：用 steady_clock（不受系统时间调整影响）取 now 并转换为毫秒。
 */
int64_t now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/**
 * @brief 从基础 URL 解析 host 与 port
 * @param url 形如 HTTP 的 host:port[/path] 地址
 * @param host 输出参数：主机名
 * @param port 输出参数：端口；未显式给出时默认 80
 * @return true 表示解析出非空 host 且 port > 0
 * @details 实现原理：先剥离可选的 HTTP 协议前缀，再截断到第一个 '/' 之前；
 *          用 rfind(':') 分离 host 与 port（无冒号则默认 80），atoi 解析端口。
 */
bool parse_base_url(const std::string &url, std::string &host, int &port)
{
  std::string trimmed = url;
  const std::string scheme = "http://";
  if (trimmed.compare(0, scheme.size(), scheme) == 0) {
    trimmed = trimmed.substr(scheme.size());
  }
  const size_t slash = trimmed.find('/');
  if (slash != std::string::npos) {
    trimmed = trimmed.substr(0, slash);
  }
  const size_t colon = trimmed.rfind(':');
  if (colon == std::string::npos) {
    host = trimmed;
    port = 80;
    return !host.empty();
  }
  host = trimmed.substr(0, colon);
  port = atoi(trimmed.substr(colon + 1).c_str());
  return !host.empty() && port > 0;
}

}  // namespace

/**
 * @brief 构造客户端
 * @param base_url 形如 host:port 的基础地址（可带 HTTP 前缀）
 * @param timeout_ms 连接与读写超时（毫秒）
 * @details 实现原理：移动保存 base_url 与 timeout，并调用 parse_base_url 填充 host_/port_。
 */
LlamaCompletionClient::LlamaCompletionClient(std::string base_url, int timeout_ms)
    : base_url_(std::move(base_url)), timeout_ms_(timeout_ms)
{
  parse_base_url(base_url_, host_, port_);
}

/**
 * @brief 健康检查（带 TTL 缓存）
 * @return true 表示服务可达
 * @details 实现原理：2 秒内检测过则直接返回缓存；否则用 getaddrinfo 解析地址，
 *          对每个候选地址做非阻塞 connect + poll(timeout_ms_)；连接成功后再发送
 *          `GET /health`，整个请求写满即判定健康。结果写入上次检查时间与结果缓存。
 */
bool LlamaCompletionClient::health()
{
  const int64_t now = now_ms();
  // 2000ms TTL 缓存：避免每次补全都建立 TCP 连接
  if (now - last_health_check_ms_ < 2000) {
    return last_health_ok_;
  }
  last_health_check_ms_ = now;

  addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port_string = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port_string.c_str(), &hints, &addresses) != 0) {
    last_health_ok_ = false;
    return false;
  }

  int fd = -1;
  for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      continue;
    }
    // 非阻塞 connect + poll 超时
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc == 0) {
      break;
    }
    if (errno != EINPROGRESS) {
      close(fd);
      fd = -1;
      continue;
    }
    pollfd pfd{fd, POLLOUT, 0};
    rc = poll(&pfd, 1, timeout_ms_);
    if (rc > 0) {
      int error = 0;
      socklen_t len = sizeof(error);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
      if (error == 0) {
        break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);

  if (fd < 0) {
    last_health_ok_ = false;
    return false;
  }

  const std::string request = "GET /health HTTP/1.1\r\nHost: " + host_ + "\r\nConnection: close\r\n\r\n";
  ssize_t           written = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
  bool              ok      = written == static_cast<ssize_t>(request.size());
  close(fd);
  last_health_ok_ = ok;
  return ok;
}

/**
 * @brief 调用 /infill 获取 FIM 补全
 * @param request 请求参数（prefix/suffix/extra/max_tokens/temperature）
 * @return 成功时返回 InfillResult；任何失败返回 std::nullopt
 * @details 实现原理：
 *          1. 先 health()，不可达直接返回空；
 *          2. 组装 JSON：input_prefix/input_suffix、可选 input_extra 数组，
 *             以及 n_predict/temperature/top_k=1/cache_prompt/n_cache_reuse/t_max_predict_ms；
 *          3. 解析地址并建立非阻塞 TCP 连接（同 health）；
 *          4. 循环 poll 写超时内发送完整 POST /infill 请求；
 *          5. 循环 recv 读取响应，poll 超时或出错即停止；
 *          6. 解析 HTTP 头：必须有 "\r\n\r\n" 且状态为 200；
 *          7. 解析 JSON body 取 content，非空则返回，否则返回空。
 */
std::optional<InfillResult> LlamaCompletionClient::infill(const InfillRequest &request)
{
  if (!health()) {
    return std::nullopt;
  }

  Json::Value root;
  root["input_prefix"] = request.prefix;
  root["input_suffix"] = request.suffix;
  Json::Value extra(Json::arrayValue);
  for (const ExtraContextFile &file : request.extra) {
    Json::Value entry;
    entry["filename"] = file.filename;
    entry["text"]     = file.text;
    extra.append(entry);
  }
  if (!extra.empty()) {
    root["input_extra"] = extra;
  }
  root["n_predict"]        = request.max_tokens;
  root["temperature"]      = request.temperature;
  root["top_k"]            = 1;
  root["cache_prompt"]     = true;
  root["n_cache_reuse"]    = 64;
  root["t_max_predict_ms"] = 150;

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string body = Json::writeString(builder, root);

  addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port_string = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port_string.c_str(), &hints, &addresses) != 0) {
    return std::nullopt;
  }

  int fd = -1;
  for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      continue;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc == 0) {
      break;
    }
    if (errno != EINPROGRESS) {
      close(fd);
      fd = -1;
      continue;
    }
    pollfd pfd{fd, POLLOUT, 0};
    rc = poll(&pfd, 1, timeout_ms_);
    if (rc > 0) {
      int error = 0;
      socklen_t len = sizeof(error);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
      if (error == 0) {
        break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);

  if (fd < 0) {
    return std::nullopt;
  }

  const int64_t start = now_ms();

  std::ostringstream http;
  http << "POST /infill HTTP/1.1\r\n"
       << "Host: " << host_ << ':' << port_ << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  const std::string request_text = http.str();

  size_t written = 0;
  while (written < request_text.size()) {
    pollfd pfd{fd, POLLOUT, 0};
    if (poll(&pfd, 1, timeout_ms_) <= 0) {
      close(fd);
      return std::nullopt;
    }
    ssize_t n = ::send(fd, request_text.data() + written, request_text.size() - written, MSG_NOSIGNAL);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (n <= 0) {
      close(fd);
      return std::nullopt;
    }
    written += static_cast<size_t>(n);
  }

  std::string response;
  char        buffer[4096];
  while (true) {
    pollfd pfd{fd, POLLIN, 0};
    int    ready = poll(&pfd, 1, timeout_ms_);
    if (ready == 0) {
      break;  // 超时：丢弃
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    response.append(buffer, static_cast<size_t>(n));
  }
  close(fd);

  // 划分 HTTP 头与 body；仅接受状态码 200
  const size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return std::nullopt;
  }
  const std::string headers = response.substr(0, header_end);
  if (headers.find(" 200 ") == std::string::npos) {
    return std::nullopt;
  }
  const std::string payload = response.substr(header_end + 4);

  Json::Value json;
  Json::CharReaderBuilder reader_builder;
  std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
  std::string errors;
  if (!reader->parse(payload.data(), payload.data() + payload.size(), &json, &errors)) {
    return std::nullopt;
  }
  // llama.cpp /infill 在 "content" 字段返回补全文本
  std::string content = json.get("content", "").asString();
  if (content.empty()) {
    return std::nullopt;
  }

  InfillResult result;
  result.text       = std::move(content);
  result.elapsed_ms = now_ms() - start;
  return result;
}
