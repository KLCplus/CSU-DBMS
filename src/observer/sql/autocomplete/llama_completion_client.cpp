/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

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

namespace {

int64_t now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

LlamaCompletionClient::LlamaCompletionClient(std::string base_url, int timeout_ms)
    : base_url_(std::move(base_url)), timeout_ms_(timeout_ms)
{
  parse_base_url(base_url_, host_, port_);
}

bool LlamaCompletionClient::health()
{
  const int64_t now = now_ms();
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
  std::string content = json.get("content", "").asString();
  if (content.empty()) {
    return std::nullopt;
  }

  InfillResult result;
  result.text       = std::move(content);
  result.elapsed_ms = now_ms() - start;
  return result;
}
