#include "net/native_communicator.h"

#include <errno.h>
#include <cstring>
#include <unistd.h>

#include "event/session_event.h"
#include "json/json.h"
#include "net/buffered_writer.h"
#include "service/query_result.h"
#include "sql/executor/sql_result.h"

namespace {

constexpr size_t MAX_NATIVE_PACKET_SIZE = 1024 * 1024;

Json::Value to_json(const QueryResult &result)
{
  Json::Value root;
  root["success"] = result.success;
  root["error"]["code"] = result.error_code;
  root["error"]["name"] = result.error_name;
  root["error"]["message"] = result.success ? "" : result.message;
  root["message"] = result.message;
  root["affected_rows"] = result.affected_rows_known ? Json::Value(Json::UInt64(result.affected_rows)) : Json::Value();
  root["execution_time_us"] = Json::UInt64(result.execution_time_us);
  for (const QueryColumn &column : result.columns) {
    Json::Value value;
    value["name"] = column.name;
    value["type"] = column.type;
    root["columns"].append(value);
  }
  if (result.columns.empty()) {
    root["columns"] = Json::arrayValue;
  }
  for (const vector<string> &row : result.rows) {
    Json::Value value(Json::arrayValue);
    for (const string &cell : row) {
      value.append(cell);
    }
    root["rows"].append(value);
  }
  if (result.rows.empty()) {
    root["rows"] = Json::arrayValue;
  }
  for (const string &warning : result.warnings) {
    root["warnings"].append(warning);
  }
  if (result.warnings.empty()) {
    root["warnings"] = Json::arrayValue;
  }
  for (const auto &attribute : result.attributes) {
    root["attributes"][attribute.first] = attribute.second;
  }
  return root;
}

} // namespace

RC NativeCommunicator::read_packet(string &packet)
{
  packet.clear();
  char buffer[4096];
  while (packet.size() <= MAX_NATIVE_PACKET_SIZE) {
    ssize_t n = ::read(fd_, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return RC::IOERR_READ;
    }
    if (n == 0) {
      return RC::IOERR_CLOSE;
    }
    const char *end = static_cast<const char *>(memchr(buffer, '\0', static_cast<size_t>(n)));
    packet.append(buffer, end == nullptr ? static_cast<size_t>(n) : static_cast<size_t>(end - buffer));
    if (packet.size() > MAX_NATIVE_PACKET_SIZE) {
      return RC::IOERR_TOO_LONG;
    }
    if (end != nullptr) {
      return RC::SUCCESS;
    }
  }
  return RC::IOERR_TOO_LONG;
}

RC NativeCommunicator::read_event(SessionEvent *&event)
{
  event = nullptr;
  string packet;
  RC rc = read_packet(packet);
  if (OB_FAIL(rc)) {
    return rc;
  }

  event = new SessionEvent(this);
  Json::Value root;
  Json::CharReaderBuilder builder;
  string errors;
  unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(packet.data(), packet.data() + packet.size(), &root, &errors) || !root.isObject()) {
    event->set_request_type(ClientRequestType::PROTOCOL_ERROR);
    event->set_protocol_error("invalid native protocol request");
    return RC::SUCCESS;
  }

  string type = root.get("type", "query").asString();
  if (type == "login") {
    event->set_request_type(ClientRequestType::LOGIN);
    event->set_username(root.get("user", "").asString());
    event->set_password(root.get("password", "").asString());
    event->set_database(root.get("database", "").asString());
  } else if (type == "logout") {
    event->set_request_type(ClientRequestType::LOGOUT);
  } else if (type == "ping") {
    event->set_request_type(ClientRequestType::PING);
  } else if (type == "server_info") {
    event->set_request_type(ClientRequestType::SERVER_INFO);
  } else if (type == "buffer_snapshot") {
    event->set_request_type(ClientRequestType::BUFFER_SNAPSHOT);
    event->set_snapshot_limit(root.get("limit", 20).asUInt64());
  } else if (type == "query") {
    event->set_request_type(ClientRequestType::QUERY);
    event->set_query(root.get("sql", "").asString());
  } else {
    event->set_request_type(ClientRequestType::PROTOCOL_ERROR);
    event->set_protocol_error("unknown native request type");
  }
  return RC::SUCCESS;
}

RC NativeCommunicator::write_result(SessionEvent *event, bool &need_disconnect)
{
  QueryResult result = event->sql_result()->return_code() == RC::SUCCESS
      ? QueryResult::ok(event->sql_result()->state_string())
      : QueryResult::error(event->sql_result()->return_code(), event->sql_result()->state_string());
  return write_query_result(result, need_disconnect);
}

RC NativeCommunicator::write_query_result(const QueryResult &result, bool &need_disconnect)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  string packet = Json::writeString(builder, to_json(result));
  packet.push_back('\0');
  RC rc = writer_->writen(packet.data(), packet.size());
  if (OB_SUCC(rc)) {
    rc = writer_->flush();
  }
  need_disconnect = OB_FAIL(rc);
  return rc;
}
