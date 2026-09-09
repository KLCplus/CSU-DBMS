#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/linereader/line_reader.h"
#include "common/version.h"
#include "json/json.h"

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::optional;
using std::string;
using std::vector;

namespace {

constexpr int DEFAULT_PORT = 6789;
constexpr size_t MAX_PACKET_SIZE = 16 * 1024 * 1024;

struct ClientOptions
{
  string host = "127.0.0.1";
  int port = DEFAULT_PORT;
  string user = "root";
  string database;
  string profile;
  string config_path;
  bool prompt_password = false;
  bool no_color = false;
  bool batch = false;
  bool silent = false;
  bool ping = false;
  bool timing = false;
  string execute_sql;
  string file;
};

struct CliOverrides
{
  optional<string> host;
  optional<int> port;
  optional<string> user;
  optional<string> database;
};

string trim(string value)
{
  auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return begin < end ? string(begin, end) : string();
}

string upper(string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}

string home_directory()
{
  const char *home = getenv("HOME");
  return home == nullptr ? "." : home;
}

void usage(const char *program)
{
  cout << CSUDB_PRODUCT_NAME << " native client " << CSUDB_VERSION_STRING << "\n"
       << "Usage: " << program << " [options] [database]\n\n"
       << "Connection:\n"
       << "  -h, --host HOST          Server host (default 127.0.0.1)\n"
       << "  -P, --port PORT          Server port (default 6789)\n"
       << "  -u, --user USER          Login user (default root)\n"
       << "  -p, --password           Prompt for password (never accepts a value)\n"
       << "  -D, --database DB        Initial database\n"
       << "      --profile NAME       Profile in ~/.csudb/config.toml\n"
       << "      --config FILE        Alternate profile file\n\n"
       << "Execution:\n"
       << "  -e, --execute SQL        Execute SQL and exit\n"
       << "  -f, --file FILE          Execute SQL file and exit\n"
       << "      --batch              Tab-separated script output\n"
       << "      --silent             Suppress banner\n"
       << "      --no-color           Disable ANSI color\n"
       << "      --ping               Test server reachability and exit\n"
       << "      --table              Table output (default)\n"
       << "      --help               Show this help\n"
       << "      --version            Show version\n";
}

string unquote(string value)
{
  value = trim(value);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
    return value.substr(1, value.size() - 2);
  return value;
}

using ConfigSections = map<string, map<string, string>>;

ConfigSections load_toml(const string &path)
{
  ConfigSections sections;
  std::ifstream in(path);
  if (!in) return sections;
  string section = "default";
  string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
    size_t equals = line.find('=');
    if (equals == string::npos) continue;
    sections[section][trim(line.substr(0, equals))] = unquote(line.substr(equals + 1));
  }
  return sections;
}

void apply_section(ClientOptions &options, const map<string, string> &section)
{
  auto assign = [&](const char *key, string &target) { auto it = section.find(key); if (it != section.end()) target = it->second; };
  assign("host", options.host);
  assign("user", options.user);
  assign("database", options.database);
  auto port = section.find("port");
  if (port != section.end()) options.port = std::atoi(port->second.c_str());
}

void apply_environment(ClientOptions &options)
{
  auto assign = [](const char *name, string &target) { const char *value = getenv(name); if (value != nullptr && *value != '\0') target = value; };
  assign("CSUDB_HOST", options.host);
  assign("CSUDB_USER", options.user);
  assign("CSUDB_DATABASE", options.database);
  const char *port = getenv("CSUDB_PORT");
  if (port != nullptr && *port != '\0') options.port = std::atoi(port);
}

bool parse_options(int argc, char **argv, ClientOptions &options)
{
  CliOverrides overrides;
  const char *env_config = getenv("CSUDB_CONFIG");
  options.config_path = env_config == nullptr ? home_directory() + "/.csudb/config.toml" : env_config;
  const char *env_profile = getenv("CSUDB_PROFILE");
  if (env_profile != nullptr) options.profile = env_profile;

  enum { OPT_PROFILE = 1000, OPT_CONFIG, OPT_BATCH, OPT_SILENT, OPT_NO_COLOR, OPT_PING, OPT_TABLE, OPT_HELP, OPT_VERSION };
  static option long_options[] = {{"host", required_argument, nullptr, 'h'}, {"port", required_argument, nullptr, 'P'},
      {"user", required_argument, nullptr, 'u'}, {"password", no_argument, nullptr, 'p'},
      {"database", required_argument, nullptr, 'D'}, {"execute", required_argument, nullptr, 'e'},
      {"file", required_argument, nullptr, 'f'}, {"profile", required_argument, nullptr, OPT_PROFILE},
      {"config", required_argument, nullptr, OPT_CONFIG}, {"batch", no_argument, nullptr, OPT_BATCH},
      {"silent", no_argument, nullptr, OPT_SILENT}, {"no-color", no_argument, nullptr, OPT_NO_COLOR},
      {"ping", no_argument, nullptr, OPT_PING}, {"table", no_argument, nullptr, OPT_TABLE},
      {"help", no_argument, nullptr, OPT_HELP}, {"version", no_argument, nullptr, OPT_VERSION}, {nullptr, 0, nullptr, 0}};

  // First pass records command-line values; profile configuration is merged afterward.
  int option;
  while ((option = getopt_long(argc, argv, "h:P:u:pD:e:f:", long_options, nullptr)) != -1) {
    switch (option) {
      case 'h': overrides.host = optarg; break;
      case 'P': overrides.port = std::atoi(optarg); break;
      case 'u': overrides.user = optarg; break;
      case 'p': options.prompt_password = true; break;
      case 'D': overrides.database = optarg; break;
      case 'e': options.execute_sql = optarg; break;
      case 'f': options.file = optarg; break;
      case OPT_PROFILE: options.profile = optarg; break;
      case OPT_CONFIG: options.config_path = optarg; break;
      case OPT_BATCH: options.batch = true; options.silent = true; break;
      case OPT_SILENT: options.silent = true; break;
      case OPT_NO_COLOR: options.no_color = true; break;
      case OPT_PING: options.ping = true; options.silent = true; break;
      case OPT_TABLE: options.batch = false; break;
      case OPT_HELP: usage(argv[0]); std::exit(0);
      case OPT_VERSION: cout << CSUDB_PRODUCT_NAME << " " << CSUDB_VERSION_STRING << endl; std::exit(0);
      default: return false;
    }
  }
  if (optind < argc) overrides.database = argv[optind++];
  if (optind < argc) { cerr << "ERROR: unexpected argument '" << argv[optind] << "'\n"; return false; }

  ConfigSections sections = load_toml(options.config_path);
  auto defaults = sections.find("default");
  if (defaults != sections.end()) apply_section(options, defaults->second);
  if (!options.profile.empty()) {
    auto profile = sections.find("profile." + options.profile);
    if (profile == sections.end()) { cerr << "ERROR: profile '" << options.profile << "' was not found\n"; return false; }
    apply_section(options, profile->second);
  }
  apply_environment(options);
  if (overrides.host) options.host = *overrides.host;
  if (overrides.port) options.port = *overrides.port;
  if (overrides.user) options.user = *overrides.user;
  if (overrides.database) options.database = *overrides.database;
  if (options.port <= 0 || options.port > 65535) { cerr << "ERROR: invalid port\n"; return false; }
  return true;
}

string read_password()
{
  cerr << "Enter password: ";
  int input_fd = STDIN_FILENO;
  bool close_input = false;
  if (!isatty(input_fd)) {
    input_fd = open("/dev/tty", O_RDONLY);
    close_input = input_fd >= 0;
  }
  if (input_fd < 0) {
    cerr << "\nERROR: no controlling terminal is available for secure password input\n";
    return "";
  }
  termios old_state{};
  bool changed = isatty(input_fd) && tcgetattr(input_fd, &old_state) == 0;
  if (changed) { termios state = old_state; state.c_lflag &= ~ECHO; tcsetattr(input_fd, TCSAFLUSH, &state); }
  string password;
  char character = '\0';
  while (true) {
    ssize_t count = ::read(input_fd, &character, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0 || character == '\n' || character == '\r') break;
    password.push_back(character);
  }
  if (changed) tcsetattr(input_fd, TCSAFLUSH, &old_state);
  if (close_input) ::close(input_fd);
  cerr << '\n';
  return password;
}

class NativeConnection
{
public:
  NativeConnection() = default;
  NativeConnection(const NativeConnection &) = delete;
  NativeConnection &operator=(const NativeConnection &) = delete;
  NativeConnection(NativeConnection &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  NativeConnection &operator=(NativeConnection &&other) noexcept
  {
    if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
    return *this;
  }
  ~NativeConnection() { close(); }
  void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

  bool connect_to(const string &host, int port, string &error)
  {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    string port_string = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), port_string.c_str(), &hints, &addresses);
    if (rc != 0) { error = gai_strerror(rc); return false; }
    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
      fd_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd_ >= 0 && ::connect(fd_, address->ai_addr, address->ai_addrlen) == 0) break;
      close();
    }
    freeaddrinfo(addresses);
    if (fd_ < 0) { error = strerror(errno); return false; }
    return true;
  }

  bool request(const Json::Value &request, Json::Value &response, string &error)
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    string packet = Json::writeString(builder, request);
    packet.push_back('\0');
    size_t written = 0;
    while (written < packet.size()) {
      ssize_t n = ::send(fd_, packet.data() + written, packet.size() - written, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { error = strerror(errno); return false; }
      written += static_cast<size_t>(n);
    }
    string input;
    char buffer[4096];
    while (input.size() <= MAX_PACKET_SIZE) {
      ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { error = n == 0 ? "connection closed" : strerror(errno); return false; }
      const char *end = static_cast<const char *>(memchr(buffer, '\0', static_cast<size_t>(n)));
      input.append(buffer, end == nullptr ? static_cast<size_t>(n) : static_cast<size_t>(end - buffer));
      if (input.size() > MAX_PACKET_SIZE) { error = "response exceeds safety limit"; return false; }
      if (end != nullptr) break;
    }
    Json::CharReaderBuilder reader_builder;
    std::istringstream stream(input);
    return Json::parseFromStream(reader_builder, stream, &response, &error);
  }

private:
  int fd_ = -1;
};

void render_table(const Json::Value &response, std::ostream &out, bool batch, bool timing)
{
  if (!response.get("success", false).asBool()) {
    const Json::Value &error = response["error"];
    out << "ERROR " << error.get("code", 1).asInt() << ": " << error.get("message", "request failed").asString() << '\n';
    return;
  }
  const Json::Value &columns = response["columns"];
  const Json::Value &rows = response["rows"];
  if (columns.empty()) {
    out << response.get("message", "Query OK").asString();
    if (!response["affected_rows"].isNull()) out << ", " << response["affected_rows"].asUInt64() << " rows affected";
    if (timing) out << " (" << std::fixed << std::setprecision(3) << response.get("execution_time_us", 0).asUInt64() / 1000000.0 << " sec)";
    out << '\n';
    return;
  }
  if (batch) {
    for (Json::ArrayIndex i = 0; i < columns.size(); ++i) { if (i) out << '\t'; out << columns[i]["name"].asString(); }
    out << '\n';
    for (const Json::Value &row : rows) { for (Json::ArrayIndex i = 0; i < row.size(); ++i) { if (i) out << '\t'; out << row[i].asString(); } out << '\n'; }
  } else {
    vector<size_t> widths(columns.size(), 0);
    for (Json::ArrayIndex i = 0; i < columns.size(); ++i) widths[i] = columns[i]["name"].asString().size();
    for (const Json::Value &row : rows) for (Json::ArrayIndex i = 0; i < row.size() && i < widths.size(); ++i) widths[i] = std::max(widths[i], row[i].asString().size());
    auto border = [&]() { out << '+'; for (size_t width : widths) out << string(width + 2, '-') << '+'; out << '\n'; };
    border(); out << '|';
    for (Json::ArrayIndex i = 0; i < columns.size(); ++i) out << ' ' << std::left << std::setw(widths[i]) << columns[i]["name"].asString() << " |";
    out << '\n'; border();
    for (const Json::Value &row : rows) { out << '|'; for (Json::ArrayIndex i = 0; i < columns.size(); ++i) out << ' ' << std::left << std::setw(widths[i]) << (i < row.size() ? row[i].asString() : "") << " |"; out << '\n'; }
    border();
    out << rows.size() << (rows.size() == 1 ? " row" : " rows") << " in set";
    if (timing) out << " (" << std::fixed << std::setprecision(3) << response.get("execution_time_us", 0).asUInt64() / 1000000.0 << " sec)";
    out << '\n';
  }
}

bool complete_sql(const string &sql)
{
  bool single = false, quoted = false, escape = false;
  for (size_t i = 0; i < sql.size(); ++i) {
    char c = sql[i];
    if (escape) { escape = false; continue; }
    if (c == '\\') { escape = true; continue; }
    if (c == '\'' && !quoted) single = !single;
    else if (c == '"' && !single) quoted = !quoted;
    else if (c == ';' && !single && !quoted && trim(sql.substr(i + 1)).empty()) return true;
  }
  string stripped = trim(sql);
  return !single && !quoted && stripped.size() >= 2 && stripped.substr(stripped.size() - 2) == "\\g";
}

vector<string> split_sql_script(const string &script)
{
  vector<string> statements;
  string current;
  bool single = false, quoted = false, escape = false;
  for (char c : script) {
    current.push_back(c);
    if (escape) { escape = false; continue; }
    if (c == '\\') { escape = true; continue; }
    if (c == '\'' && !quoted) single = !single;
    else if (c == '"' && !single) quoted = !quoted;
    else if (c == ';' && !single && !quoted) { if (!trim(current).empty()) statements.push_back(trim(current)); current.clear(); }
  }
  if (!trim(current).empty()) statements.push_back(trim(current));
  return statements;
}

bool sensitive_sql(const string &sql)
{
  string normalized = upper(sql);
  return normalized.find("IDENTIFIED BY") != string::npos || normalized.find("PASSWORD") != string::npos;
}

void banner(const ClientOptions &options, const Json::Value &login)
{
  if (options.silent) return;
  cout << R"(   ██████╗███████╗██╗   ██╗██████╗ ██████╗
  ██╔════╝██╔════╝██║   ██║██╔══██╗██╔══██╗
  ██║     ███████╗██║   ██║██║  ██║██████╔╝
  ██║     ╚════██║██║   ██║██║  ██║██╔══██╗
  ╚██████╗███████║╚██████╔╝██████╔╝██████╔╝
   ╚═════╝╚══════╝ ╚═════╝ ╚═════╝ ╚═════╝

                     CSUDB 2026

         Compiler · Database · Operating System

────────────────────────────────────────────────────
)";
  cout << " Server Version : CSUDB " << login["attributes"].get("version", CSUDB_VERSION_STRING).asString() << '\n'
       << " Connection     : " << options.host << ':' << options.port << '\n'
       << " User           : " << options.user << '\n'
       << " Database       : " << (options.database.empty() ? "(none)" : options.database) << R"(

 SQL Engine     : READY
 Storage Engine : READY
 Buffer Pool    : READY

────────────────────────────────────────────────────

 Welcome to the CSUDB monitor.

 Commands end with ';' or '\g'.
 Type '\help' for help. Type '\q' to quit.

)";
}

const char *meta_help = R"(CSUDB shell commands:
  \help, \?                 Show this help
  \q, \quit, \exit          Quit
  \status                    Connection, session, and engine status
  \connect HOST PORT USER    Reconnect (password is prompted)
  \use DATABASE              Select database
  \database                  Show current database
  \timing [on|off]           Toggle execution timing
  \clear                     Clear an ANSI terminal
  \history                   Show session history
  \source FILE               Execute a SQL file
  \output [FILE]             Redirect or restore query output
  \buffer                    Show Buffer Pool summary
  \pages [N]                 Show at most N frame snapshots (default 20)
  \server                    Show server status
  \pager                     Reserved; currently unsupported
)";

class Shell
{
public:
  explicit Shell(ClientOptions options) : options_(std::move(options)) {}

  int run()
  {
    string error;
    if (!connection_.connect_to(options_.host, options_.port, error)) {
      cerr << "ERROR: cannot connect to CSUDB server at " << options_.host << ':' << options_.port << " (" << error << ")\n";
      return 2;
    }
    if (options_.ping) {
      Json::Value request, response; request["type"] = "ping";
      if (!connection_.request(request, response, error) || !response.get("success", false).asBool()) { cerr << "ERROR: ping failed: " << error << '\n'; return 2; }
      cout << "CSUDB server is alive.\n"; return 0;
    }
    bool interactive_login = isatty(STDIN_FILENO) && options_.execute_sql.empty() && options_.file.empty();
    string password = (options_.prompt_password || interactive_login) ? read_password() : "";
    Json::Value login_request, login_response;
    login_request["type"] = "login"; login_request["user"] = options_.user; login_request["password"] = password; login_request["database"] = options_.database;
    if (!connection_.request(login_request, login_response, error)) { cerr << "ERROR: login request failed: " << error << '\n'; return 2; }
    password.assign(password.size(), '\0'); password.clear(); login_request["password"] = "";
    if (!login_response.get("success", false).asBool()) { render_table(login_response, cerr, false, false); return 3; }
    if (options_.database.empty()) options_.database = login_response["attributes"].get("database", "sys").asString();
    banner(options_, login_response);

    if (!options_.execute_sql.empty()) return execute(options_.execute_sql) ? 0 : 1;
    if (!options_.file.empty()) return execute_file(options_.file) ? 0 : 1;
    if (!isatty(STDIN_FILENO)) { std::ostringstream input; input << std::cin.rdbuf(); return execute_script(input.str()) ? 0 : 1; }
    return interactive();
  }

private:
  std::ostream &output() { return output_file_.is_open() ? output_file_ : cout; }

  bool request(Json::Value &request, Json::Value &response)
  {
    string error;
    if (!connection_.request(request, response, error)) { cerr << "ERROR: connection failed: " << error << '\n'; return false; }
    return true;
  }

  bool execute(string sql)
  {
    sql = trim(sql);
    if (sql.size() >= 2 && sql.substr(sql.size() - 2) == "\\g") sql = trim(sql.substr(0, sql.size() - 2));
    Json::Value request_value, response;
    request_value["type"] = "query"; request_value["sql"] = sql;
    if (!request(request_value, response)) return false;
    render_table(response, output(), options_.batch, options_.timing);
    if (response.get("success", false).asBool() && response["attributes"].isMember("database")) options_.database = response["attributes"]["database"].asString();
    return response.get("success", false).asBool();
  }

  bool execute_script(const string &script)
  {
    bool ok = true;
    for (const string &statement : split_sql_script(script)) if (!execute(statement)) ok = false;
    return ok;
  }

  bool execute_file(const string &path)
  {
    std::ifstream in(path);
    if (!in) { cerr << "ERROR: cannot open SQL file '" << path << "'\n"; return false; }
    std::ostringstream content; content << in.rdbuf(); return execute_script(content.str());
  }

  void show_status(bool pages, size_t limit)
  {
    Json::Value request_value, response;
    request_value["type"] = pages ? "buffer_snapshot" : "server_info";
    request_value["limit"] = Json::UInt64(limit);
    if (!request(request_value, response)) return;
    if (!response.get("success", false).asBool()) { render_table(response, cerr, false, false); return; }
    for (const string &name : response["attributes"].getMemberNames()) output() << std::left << std::setw(22) << name << ": " << response["attributes"][name].asString() << '\n';
    if (pages) render_table(response, output(), options_.batch, false);
  }

  bool dispatch_meta(const string &line, bool &quit)
  {
    std::istringstream stream(line);
    string command; stream >> command;
    string lower = command; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower == "\\q" || lower == "\\quit" || lower == "\\exit") { quit = true; return true; }
    if (lower == "\\help" || lower == "\\?") { output() << meta_help; return true; }
    if (lower == "\\status" || lower == "\\server") { show_status(false, 0); return true; }
    if (lower == "\\buffer") { show_status(false, 0); return true; }
    if (lower == "\\pages") { size_t limit = 20; stream >> limit; show_status(true, std::min<size_t>(limit, 200)); return true; }
    if (lower == "\\database") { output() << (options_.database.empty() ? "(none)" : options_.database) << '\n'; return true; }
    if (lower == "\\use") { string database; stream >> database; if (database.empty()) cerr << "Usage: \\use DATABASE\n"; else execute("USE " + database + ";"); return true; }
    if (lower == "\\timing") {
      string setting; stream >> setting; setting = upper(setting);
      if (setting == "ON") options_.timing = true; else if (setting == "OFF") options_.timing = false; else if (setting.empty()) options_.timing = !options_.timing; else { cerr << "Usage: \\timing [on|off]\n"; return true; }
      output() << "Timing is " << (options_.timing ? "on" : "off") << ".\n"; return true;
    }
    if (lower == "\\clear") { if (isatty(STDOUT_FILENO)) cout << "\033[2J\033[H"; else cerr << "Unsupported: output is not a terminal.\n"; return true; }
    if (lower == "\\history") { for (size_t i = 0; i < session_history_.size(); ++i) output() << i + 1 << "  " << session_history_[i] << '\n'; return true; }
    if (lower == "\\source") { string path; stream >> path; if (path.empty()) cerr << "Usage: \\source FILE\n"; else execute_file(path); return true; }
    if (lower == "\\output") {
      string path; stream >> path;
      if (path.empty()) { output_file_.close(); cout << "Query output restored to stdout.\n"; }
      else { output_file_.close(); output_file_.open(path, std::ios::app); if (!output_file_) cerr << "ERROR: cannot open output file\n"; }
      return true;
    }
    if (lower == "\\pager") { cerr << "Unsupported: pager is reserved for a future release.\n"; return true; }
    if (lower == "\\connect") {
      string host, user; int port = DEFAULT_PORT; stream >> host >> port >> user;
      if (host.empty() || user.empty()) { cerr << "Usage: \\connect HOST PORT USER\n"; return true; }
      string password = read_password(), error;
      NativeConnection replacement;
      if (!replacement.connect_to(host, port, error)) { cerr << "ERROR: cannot connect: " << error << '\n'; return true; }
      Json::Value req, resp; req["type"] = "login"; req["user"] = user; req["password"] = password; req["database"] = "sys";
      if (!replacement.request(req, resp, error) || !resp.get("success", false).asBool()) { cerr << "ERROR: authentication failed\n"; return true; }
      connection_ = std::move(replacement); options_.host = host; options_.port = port; options_.user = user; options_.database = "sys"; return true;
    }
    cerr << "Unknown command '" << command << "'. Type \\help.\n";
    return true;
  }

  int interactive()
  {
    string config_dir = home_directory() + "/.csudb";
    std::error_code ec; std::filesystem::create_directories(config_dir, ec); chmod(config_dir.c_str(), S_IRWXU);
    string history_path = config_dir + "/history";
    common::MiniobLineReader::instance().init(history_path);
    bool quit = false;
    string sql;
    while (!quit) {
      string prompt = sql.empty() ? "csudb [" + (options_.database.empty() ? "(none)" : options_.database) + "]> " : "    -> ";
      string line = common::MiniobLineReader::instance().my_readline(prompt, false);
      if (common::MiniobLineReader::instance().eof()) break;
      if (line == "interrupted") { sql.clear(); cout << "^C\n"; continue; }
      if (sql.empty() && !trim(line).empty() && trim(line)[0] == '\\') { dispatch_meta(trim(line), quit); continue; }
      if (trim(line).empty()) continue;
      if (!sql.empty()) sql.push_back('\n');
      sql += line;
      if (!complete_sql(sql)) continue;
      string history_entry = trim(sql);
      if (!sensitive_sql(history_entry)) { common::MiniobLineReader::instance().add_history(history_entry); session_history_.push_back(history_entry); }
      execute(sql);
      sql.clear();
    }
    return 0;
  }

  ClientOptions options_;
  NativeConnection connection_;
  std::ofstream output_file_;
  vector<string> session_history_;
};

} // namespace

int main(int argc, char **argv)
{
  ClientOptions options;
  if (!parse_options(argc, argv, options)) { usage(argv[0]); return 64; }
  return Shell(std::move(options)).run();
}
