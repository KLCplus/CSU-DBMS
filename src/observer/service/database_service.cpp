#include "service/database_service.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>

#include "common/global_context.h"
#include "common/os/process_param.h"
#include "common/type/attr_type.h"
#include "common/value.h"
#include "common/version.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "net/communicator.h"
#include "session/session.h"
#include "storage/os/buffer/disk_buffer_pool.h"
#include "storage/db/db.h"
#include "storage/default/default_handler.h"

namespace {

SystemCatalog product_catalog;
std::mutex database_admin_mutex;

string trim(string value)
{
  auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return first < last ? string(first, last) : string();
}

string upper(string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}

vector<string> split_privileges(const string &text)
{
  vector<string> result;
  std::stringstream stream(text);
  string item;
  while (std::getline(stream, item, ',')) {
    item = upper(trim(item));
    if (!item.empty()) result.push_back(item);
  }
  return result;
}

bool valid_privilege(const string &privilege)
{
  static const vector<string> supported = {
      "ALL", "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "CREATE_USER", "GRANT"};
  return std::find(supported.begin(), supported.end(), privilege) != supported.end();
}

QueryResult rows_result(const vector<string> &column_names, const vector<vector<string>> &rows)
{
  QueryResult result = QueryResult::ok();
  for (const string &name : column_names) result.columns.push_back({name, "VARCHAR"});
  result.rows = rows;
  return result;
}

string join(const vector<string> &items, const char *separator)
{
  string result;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != 0) result += separator;
    result += items[i];
  }
  return result;
}

bool extract_table(const string &sql, const string &verb, string &table)
{
  std::smatch match;
  string pattern;
  if (verb == "SELECT" || verb == "DELETE") pattern = R"(\bFROM\s+([A-Za-z_][A-Za-z0-9_\.]*))";
  else if (verb == "INSERT") pattern = R"(\bINTO\s+([A-Za-z_][A-Za-z0-9_\.]*))";
  else if (verb == "UPDATE") pattern = R"(^\s*UPDATE\s+([A-Za-z_][A-Za-z0-9_\.]*))";
  else if (verb == "DESC" || verb == "DESCRIBE") pattern = R"(^\s*(?:DESC|DESCRIBE)\s+([A-Za-z_][A-Za-z0-9_\.]*))";
  else pattern = R"(\b(?:TABLE|ON)\s+([A-Za-z_][A-Za-z0-9_\.]*))";
  if (!std::regex_search(sql, match, std::regex(pattern, std::regex::icase))) return false;
  table = match[1].str();
  return true;
}

void split_resource(string resource, const string &current_database, string &database, string &table)
{
  size_t dot = resource.find('.');
  if (dot == string::npos) {
    database = current_database;
    table = resource;
  } else {
    database = resource.substr(0, dot);
    table = resource.substr(dot + 1);
  }
}

} // namespace

RC DatabaseService::initialize(const string &data_dir, string &temporary_root_password, bool &created)
{
  RC rc = product_catalog.open_or_initialize(data_dir, temporary_root_password, created);
  if (OB_FAIL(rc)) return rc;
  for (const string &database : product_catalog.databases()) {
    if (database == "sys") continue;
    rc = GCTX.handler_->open_db(database.c_str());
    if (OB_FAIL(rc)) return rc;
  }
  return RC::SUCCESS;
}

RC DatabaseService::initialize_new(const string &data_dir, const string &root_password, string &effective_root_password)
{
  return product_catalog.initialize_new(data_dir, root_password, effective_root_password);
}

SystemCatalog &DatabaseService::system_catalog() { return product_catalog; }

QueryResult DatabaseService::login(Session &session, const string &username, const string &password, const string &database)
{
  UserInfo user;
  RC rc = product_catalog.authenticate(username, password, user);
  if (OB_FAIL(rc)) return QueryResult::error(RC::AUTHENTICATION_FAILED, "authentication failed");
  string target = database.empty() ? "sys" : database;
  if (!product_catalog.database_exists(target) || GCTX.handler_->find_db(target.c_str()) == nullptr) {
    return QueryResult::error(RC::SCHEMA_DB_NOT_EXIST, "database '" + target + "' does not exist");
  }
  if (username != "root" && !product_catalog.allowed(username, "CONNECT", target)) {
    return QueryResult::error(RC::AUTHORIZATION_DENIED, "access denied for database '" + target + "'");
  }
  session.set_identity(user.id, user.username);
  session.set_current_db(target);
  QueryResult result = QueryResult::ok("authentication successful");
  result.attributes.push_back({"session_id", std::to_string(session.session_id())});
  result.attributes.push_back({"user", username});
  result.attributes.push_back({"database", target});
  result.attributes.push_back({"version", CSUDB_VERSION_STRING});
  return result;
}

bool DatabaseService::can_admin(const Session &session, const string &privilege) const
{
  return session.username() == "root" || product_catalog.allowed(session.username(), privilege, "");
}

QueryResult DatabaseService::execute_management(Session &session, const string &input, bool &handled)
{
  handled = true;
  string sql = trim(input);
  std::smatch match;
  const auto icase = std::regex::icase;

  if (std::regex_match(sql, match, std::regex(R"(^SHOW\s+DATABASES\s*;?$)", icase))) {
    vector<vector<string>> rows;
    for (const string &database : product_catalog.databases()) rows.push_back({database});
    return rows_result({"Database"}, rows);
  }
  if (std::regex_match(sql, match, std::regex(R"(^USE\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?$)", icase))) {
    string database = match[1].str();
    if (!product_catalog.database_exists(database) || GCTX.handler_->find_db(database.c_str()) == nullptr)
      return QueryResult::error(RC::SCHEMA_DB_NOT_EXIST, "database '" + database + "' does not exist");
    if (session.username() != "root" && !product_catalog.allowed(session.username(), "CONNECT", database))
      return QueryResult::error(RC::AUTHORIZATION_DENIED, "access denied for database '" + database + "'");
    session.destroy_trx();
    session.set_current_db(database);
    QueryResult result = QueryResult::ok("Database changed");
    result.attributes.push_back({"database", database});
    return result;
  }
  if (std::regex_match(sql, match, std::regex(R"(^CREATE\s+DATABASE\s+(IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*;?$)", icase))) {
    if (!can_admin(session, "CREATE")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "CREATE DATABASE denied");
    string database = match[2].str();
    bool if_not_exists = match[1].matched;
    std::lock_guard<std::mutex> guard(database_admin_mutex);
    if (product_catalog.database_exists(database))
      return if_not_exists ? QueryResult::ok("Database already exists") : QueryResult::error(RC::SCHEMA_DB_EXIST, "database already exists");
    RC rc = GCTX.handler_->create_db(database.c_str());
    if (OB_SUCC(rc)) rc = GCTX.handler_->open_db(database.c_str());
    if (OB_SUCC(rc)) rc = product_catalog.add_database(database);
    return OB_SUCC(rc) ? QueryResult::ok("Database created") : QueryResult::error(rc);
  }
  if (std::regex_match(sql, match, std::regex(R"(^DROP\s+DATABASE\s+(IF\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*;?$)", icase))) {
    if (!can_admin(session, "DROP")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "DROP DATABASE denied");
    string database = match[2].str();
    bool if_exists = match[1].matched;
    std::lock_guard<std::mutex> guard(database_admin_mutex);
    if (!product_catalog.database_exists(database))
      return if_exists ? QueryResult::ok("Database did not exist") : QueryResult::error(RC::SCHEMA_DB_NOT_EXIST, "database does not exist");
    if (session.get_current_db_name() == database) session.set_current_db("sys");
    RC rc = GCTX.handler_->drop_db(database.c_str());
    if (OB_SUCC(rc)) rc = product_catalog.remove_database(database);
    return OB_SUCC(rc) ? QueryResult::ok("Database dropped") : QueryResult::error(rc);
  }
  if (std::regex_match(sql, match, std::regex(R"(^CREATE\s+USER\s+'([^']+)'\s+IDENTIFIED\s+BY\s+'([^']+)'\s*;?$)", icase))) {
    if (!can_admin(session, "CREATE_USER")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "CREATE USER denied");
    RC rc = product_catalog.create_user(match[1].str(), match[2].str());
    return OB_SUCC(rc) ? QueryResult::ok("User created") : QueryResult::error(rc, rc == RC::INVALID_ARGUMENT ? "username invalid or password shorter than 8 characters" : "");
  }
  if (std::regex_match(sql, match, std::regex(R"(^ALTER\s+USER\s+'([^']+)'\s+IDENTIFIED\s+BY\s+'([^']+)'\s*;?$)", icase))) {
    string username = match[1].str();
    if (session.username() != username && !can_admin(session, "CREATE_USER")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "ALTER USER denied");
    RC rc = product_catalog.alter_user(username, match[2].str());
    return OB_SUCC(rc) ? QueryResult::ok("User altered") : QueryResult::error(rc);
  }
  if (std::regex_match(sql, match, std::regex(R"(^DROP\s+USER\s+'([^']+)'\s*;?$)", icase))) {
    if (!can_admin(session, "CREATE_USER")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "DROP USER denied");
    RC rc = product_catalog.drop_user(match[1].str());
    return OB_SUCC(rc) ? QueryResult::ok("User dropped") : QueryResult::error(rc);
  }
  if (std::regex_match(sql, match, std::regex(R"(^SHOW\s+USERS\s*;?$)", icase))) {
    if (!can_admin(session, "CREATE_USER")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "SHOW USERS denied");
    vector<vector<string>> rows;
    for (const UserInfo &user : product_catalog.users()) rows.push_back({std::to_string(user.id), user.username});
    return rows_result({"User ID", "User"}, rows);
  }
  if (std::regex_match(sql, match, std::regex(R"(^SHOW\s+GRANTS\s+FOR\s+'([^']+)'\s*;?$)", icase))) {
    string username = match[1].str();
    if (session.username() != username && !can_admin(session, "GRANT")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "SHOW GRANTS denied");
    vector<vector<string>> rows;
    for (const PrivilegeGrant &grant : product_catalog.grants_for(username)) {
      string resource = grant.scope == "GLOBAL" ? "*.*" : grant.database + (grant.scope == "TABLE" ? "." + grant.table : "");
      rows.push_back({join(grant.privileges, ", "), grant.scope, resource});
    }
    return rows_result({"Privileges", "Scope", "Resource"}, rows);
  }

  std::regex privilege_regex(R"(^(GRANT|REVOKE)\s+(.+?)\s+ON\s+(?:(DATABASE)\s+)?([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?|\*\.\*)\s+(TO|FROM)\s+'([^']+)'\s*;?$)", icase);
  if (std::regex_match(sql, match, privilege_regex)) {
    if (!can_admin(session, "GRANT")) return QueryResult::error(RC::AUTHORIZATION_DENIED, "GRANT/REVOKE denied");
    bool is_grant = upper(match[1].str()) == "GRANT";
    string direction = upper(match[5].str());
    if ((is_grant && direction != "TO") || (!is_grant && direction != "FROM"))
      return QueryResult::error(RC::INVALID_ARGUMENT, is_grant ? "GRANT requires TO" : "REVOKE requires FROM");
    PrivilegeGrant grant;
    grant.username = match[6].str();
    grant.privileges = split_privileges(match[2].str());
    if (grant.privileges.empty() ||
        !std::all_of(grant.privileges.begin(), grant.privileges.end(), valid_privilege))
      return QueryResult::error(RC::INVALID_ARGUMENT, "unknown or empty privilege list");
    string resource = match[4].str();
    if (resource == "*.*") grant.scope = "GLOBAL";
    else if (match[3].matched) { grant.scope = "DATABASE"; grant.database = resource; }
    else {
      size_t dot = resource.find('.');
      if (dot == string::npos) { grant.scope = "DATABASE"; grant.database = resource; }
      else { grant.scope = "TABLE"; grant.database = resource.substr(0, dot); grant.table = resource.substr(dot + 1); }
    }
    if (grant.scope != "GLOBAL") {
      Db *database = GCTX.handler_->find_db(grant.database.c_str());
      if (!product_catalog.database_exists(grant.database) || database == nullptr)
        return QueryResult::error(RC::SCHEMA_DB_NOT_EXIST, "database '" + grant.database + "' does not exist");
      if (grant.scope == "TABLE" && database->find_table(grant.table.c_str()) == nullptr)
        return QueryResult::error(RC::SCHEMA_TABLE_NOT_EXIST, "table '" + grant.database + "." + grant.table + "' does not exist");
    }
    if (is_grant && session.username() != "root") {
      for (const string &privilege : grant.privileges) {
        if (privilege == "ALL" ||
            !product_catalog.allowed(session.username(), privilege, grant.database, grant.table))
          return QueryResult::error(RC::AUTHORIZATION_DENIED, "cannot grant privileges not held by current user");
      }
    }
    RC rc = is_grant ? product_catalog.grant(grant) : product_catalog.revoke(grant);
    return OB_SUCC(rc) ? QueryResult::ok(is_grant ? "Privileges granted" : "Privileges revoked") : QueryResult::error(rc);
  }

  handled = false;
  return QueryResult::ok();
}

bool DatabaseService::authorize_sql(const Session &session, const string &sql, string &reason) const
{
  if (session.username() == "root") return true;
  std::smatch match;
  if (!std::regex_search(sql, match, std::regex(R"(^\s*([A-Za-z]+))"))) { reason = "empty or invalid SQL"; return false; }
  string verb = upper(match[1].str());
  if (verb == "BEGIN" || verb == "COMMIT" || verb == "ROLLBACK" || verb == "HELP" || verb == "EXIT" || verb == "CALC") return true;
  string privilege = verb;
  if (verb == "SHOW" || verb == "DESC" || verb == "DESCRIBE" || verb == "EXPLAIN") privilege = "SELECT";
  if (verb != "SELECT" && verb != "INSERT" && verb != "UPDATE" && verb != "DELETE" && verb != "CREATE" && verb != "DROP" && privilege != "SELECT") {
    reason = "command requires root until an explicit privilege mapping is defined";
    return false;
  }
  string database = session.get_current_db_name();
  string table;
  string resource;
  if (extract_table(sql, verb, resource)) split_resource(resource, database, database, table);
  if (product_catalog.allowed(session.username(), privilege, database, table)) return true;
  reason = privilege + " denied on " + database + (table.empty() ? "" : "." + table);
  return false;
}

QueryResult DatabaseService::materialize(SessionEvent &event, RC pipeline_rc)
{
  SqlResult *sql_result = event.sql_result();
  if (OB_FAIL(pipeline_rc) || OB_FAIL(sql_result->return_code())) {
    RC rc = OB_FAIL(pipeline_rc) ? pipeline_rc : sql_result->return_code();
    return QueryResult::error(rc, sql_result->state_string());
  }
  QueryResult result = QueryResult::ok(sql_result->state_string().empty() ? "Query OK" : sql_result->state_string());
  if (!sql_result->has_operator()) return result;
  RC rc = sql_result->open();
  if (OB_FAIL(rc)) return QueryResult::error(rc);
  const TupleSchema &schema = sql_result->tuple_schema();
  for (int i = 0; i < schema.cell_num(); ++i) {
    const TupleCellSpec &spec = schema.cell_at(i);
    string name = spec.alias();
    if (name.empty()) name = spec.field_name();
    result.columns.push_back({name, "UNKNOWN"});
  }
  Tuple *tuple = nullptr;
  while ((rc = sql_result->next_tuple(tuple)) == RC::SUCCESS) {
    vector<string> row;
    for (int i = 0; i < tuple->cell_num(); ++i) {
      Value value;
      RC cell_rc = tuple->cell_at(i, value);
      if (OB_FAIL(cell_rc)) { sql_result->close(); return QueryResult::error(cell_rc); }
      row.push_back(value.to_string());
      if (i < static_cast<int>(result.columns.size()) && result.columns[i].type == "UNKNOWN")
        result.columns[i].type = attr_type_to_string(value.attr_type());
    }
    result.rows.push_back(std::move(row));
  }
  RC close_rc = sql_result->close();
  if (rc == RC::RECORD_EOF) rc = RC::SUCCESS;
  if (OB_FAIL(rc)) return QueryResult::error(rc);
  if (OB_FAIL(close_rc)) return QueryResult::error(close_rc);
  return result;
}

QueryResult DatabaseService::execute_sql(SessionEvent &event)
{
  bool handled = false;
  QueryResult management = execute_management(*event.session(), event.query(), handled);
  if (handled) return management;
  string reason;
  if (!authorize_sql(*event.session(), event.query(), reason)) return QueryResult::error(RC::AUTHORIZATION_DENIED, reason);
  SQLStageEvent sql_event(&event, event.query());
  RC rc = sql_task_handler_.handle_sql(&sql_event);
  if (OB_FAIL(rc)) event.sql_result()->set_return_code(rc);
  QueryResult result = materialize(event, rc);
  if (result.success && result.columns.empty()) {
    string normalized = upper(trim(event.query()));
    if (normalized.rfind("INSERT", 0) == 0) {
      result.affected_rows = 1;
      result.affected_rows_known = true;
    } else if (normalized.rfind("CREATE", 0) == 0 || normalized.rfind("DROP", 0) == 0 ||
               normalized.rfind("BEGIN", 0) == 0 || normalized.rfind("COMMIT", 0) == 0 ||
               normalized.rfind("ROLLBACK", 0) == 0) {
      result.affected_rows = 0;
      result.affected_rows_known = true;
    }
  }
  return result;
}

QueryResult DatabaseService::server_info(Session &session, bool include_frames, size_t limit)
{
  QueryResult result = QueryResult::ok("Server is ready");
  result.attributes = {{"product", CSUDB_PRODUCT_NAME}, {"version", CSUDB_VERSION_STRING},
      {"session_id", std::to_string(session.session_id())}, {"user", session.username()},
      {"database", session.get_current_db_name()}, {"client_address", session.client_address()},
      {"autocommit", session.autocommit() ? "ON" : "OFF"}, {"page_size", std::to_string(BP_PAGE_SIZE)}};
  Db *db = session.get_current_db();
  if (db == nullptr) return result;
  BufferPoolSnapshot snapshot = db->buffer_pool_manager().snapshot();
  result.attributes.push_back({"buffer_pool_frames", std::to_string(snapshot.capacity)});
  result.attributes.push_back({"buffer_pool_bytes", std::to_string(snapshot.capacity * BP_PAGE_SIZE)});
  result.attributes.push_back({"buffer_pool_used", std::to_string(snapshot.used_frames)});
  result.attributes.push_back({"buffer_pool_pinned", std::to_string(snapshot.pinned_frames)});
  result.attributes.push_back({"buffer_pool_dirty", std::to_string(snapshot.dirty_frames)});
  result.attributes.push_back({"replacement_policy", snapshot.replacement_policy});
  result.attributes.push_back({"io_backend", snapshot.io_backend});
  result.attributes.push_back({"requests", std::to_string(snapshot.stats.page_requests)});
  result.attributes.push_back({"hits", std::to_string(snapshot.stats.cache_hits)});
  result.attributes.push_back({"misses", std::to_string(snapshot.stats.cache_misses)});
  double hit_rate = snapshot.stats.page_requests == 0 ? 0.0 :
      100.0 * static_cast<double>(snapshot.stats.cache_hits) / snapshot.stats.page_requests;
  std::ostringstream hit_rate_text;
  hit_rate_text << std::fixed << std::setprecision(2) << hit_rate << '%';
  result.attributes.push_back({"hit_rate", hit_rate_text.str()});
  if (include_frames) {
    result.columns = {{"Frame", "VARCHAR"}, {"Pool", "INT"}, {"Page", "INT"}, {"Pins", "INT"}, {"Dirty", "BOOLEAN"}, {"Replaceable", "BOOLEAN"}};
    size_t count = std::min(limit, snapshot.frames.size());
    for (size_t i = 0; i < count; ++i) {
      const FrameSnapshot &frame = snapshot.frames[i];
      result.rows.push_back({frame.frame_id, std::to_string(frame.buffer_pool_id), std::to_string(frame.page_num),
          std::to_string(frame.pin_count), frame.dirty ? "yes" : "no", frame.replaceable ? "yes" : "no"});
    }
  }
  return result;
}

QueryResult DatabaseService::execute(SessionEvent &event)
{
  auto start = std::chrono::steady_clock::now();
  QueryResult result;
  Session &session = *event.session();
  switch (event.request_type()) {
    case ClientRequestType::PROTOCOL_ERROR: result = QueryResult::error(RC::INVALID_ARGUMENT, event.protocol_error()); break;
    case ClientRequestType::LOGIN:
      result = login(session, event.username(), event.password(), event.database());
      event.clear_password();
      break;
    case ClientRequestType::PING: result = QueryResult::ok("CSUDB server is alive."); break;
    case ClientRequestType::LOGOUT: session.set_authenticated(false); result = QueryResult::ok("Logged out"); break;
    case ClientRequestType::SERVER_INFO:
      result = session.authenticated() ? server_info(session, false, 0) : QueryResult::error(RC::AUTHENTICATION_FAILED, "authentication required");
      break;
    case ClientRequestType::BUFFER_SNAPSHOT:
      result = session.authenticated() ? server_info(session, true, event.snapshot_limit()) : QueryResult::error(RC::AUTHENTICATION_FAILED, "authentication required");
      break;
    case ClientRequestType::QUERY:
      result = session.authenticated() ? execute_sql(event) : QueryResult::error(RC::AUTHENTICATION_FAILED, "authentication required");
      break;
  }
  result.execution_time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  return result;
}

RC DatabaseService::handle_event(Communicator *communicator)
{
  SessionEvent *event = nullptr;
  RC rc = communicator->read_event(event);
  if (OB_FAIL(rc)) return rc;
  if (event == nullptr) return RC::SUCCESS;

  Session *session = event->session();
  if (!communicator->structured_protocol() && !session->authenticated()) {
    session->set_identity(1, "root"); // compatibility protocols are trusted-local only
    if (session->get_current_db() == nullptr) session->set_current_db("sys");
  }
  Session::set_current_session(session);
  session->set_current_request(event);
  bool need_disconnect = false;

  if (communicator->structured_protocol()) {
    QueryResult result = execute(*event);
    rc = communicator->write_query_result(result, need_disconnect);
  } else {
    bool handled = false;
    QueryResult result = execute_management(*session, event->query(), handled);
    if (handled) {
      event->sql_result()->set_return_code(result.success ? RC::SUCCESS : static_cast<RC>(result.error_code));
      event->sql_result()->set_state_string(result.message);
    } else {
      string reason;
      if (!authorize_sql(*session, event->query(), reason)) {
        event->sql_result()->set_return_code(RC::AUTHORIZATION_DENIED);
        event->sql_result()->set_state_string(reason);
      } else {
        SQLStageEvent sql_event(event, event->query());
        RC pipeline_rc = sql_task_handler_.handle_sql(&sql_event);
        if (OB_FAIL(pipeline_rc)) event->sql_result()->set_return_code(pipeline_rc);
      }
    }
    rc = communicator->write_result(event, need_disconnect);
  }

  session->set_current_request(nullptr);
  Session::set_current_session(nullptr);
  delete event;
  return need_disconnect ? RC::INTERNAL : rc;
}
