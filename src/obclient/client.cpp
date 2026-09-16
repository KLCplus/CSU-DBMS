/**
 * @file client.cpp
 * @brief CSUDB 原生协议交互式命令行客户端（csudb）
 * @details 这是与 CSUDB 服务端直接对话的原生 TCP 客户端：由 replxx 提供行编辑、
 *  Tab 补全与行内提示（ghost text），由服务端负责 SQL 语法分析并返回确定性补全
 *  候选与可选的 AI 提示。支持用 --url / -U 指定 host[:port]，既可连本地
 *  127.0.0.1，也可连线上地址；同时提供 -e/-f/--complete 等非交互模式，以及
 *  /web 子命令拉起本地 Web 控制台。
 * @note 核心原则：客户端不解析、不执行 SQL，只做连接、输入编辑与结果渲染；
 *  补全位置、语句完整性、执行语义全部交给服务端，使 CLI、Web 与 SDK 共用
 *  同一套协议语义。
 */
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
#include "common/terminal/terminal_ui.h"
#include "common/version.h"
#include "json/json.h"
#include "web_console_launcher.h"

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::optional;
using std::string;
using std::vector;

namespace {

namespace tui = common::terminal;

constexpr int DEFAULT_PORT = 6789;
constexpr size_t MAX_PACKET_SIZE = 16 * 1024 * 1024;

class Shell;
Shell *g_active_shell = nullptr;

struct CompletionEntry
{
  string insert_text;
  string display_text;
  string kind;
  string source;
  string detail;
  size_t replace_start = 0;
  size_t replace_end   = 0;
  double score         = 0.0;
};

struct ClientOptions
{
  string host = "127.0.0.1";
  int port = DEFAULT_PORT;
  string user = "root";
  string database;
  string url;
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
  string complete_sql;
};

struct CliOverrides
{
  optional<string> host;
  optional<int> port;
  optional<string> user;
  optional<string> database;
  optional<string> url;
};

/**
 * @brief 去掉字符串首尾的空白字符
 * @param value 待处理的字符串；按值传入，内部副本不影响调用方
 * @return 去掉首尾空白后的新字符串；若整串都是空白则返回空串
 * @details 用 find_if_not 从左右两侧分别定位第一个非空白字符，
 *  反向迭代器再通过 base() 换算回正向位置，最后截取两端之间的区间。
 */
string trim(string value)
{
  auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return begin < end ? string(begin, end) : string();
}

/**
 * @brief 转成大写，用于把用户输入的关键字做不区分大小写的比较
 * @param value 待转换的字符串；按值传入，返回的是副本
 * @return 全部转换为大写后的新字符串
 * @details 用 std::transform 对每个字符调用 std::toupper，
 *  形参转 unsigned char 以避免负值字符未定义行为。
 */
string upper(string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
  return value;
}

/**
 * @brief 返回用户主目录
 * @return HOME 环境变量的值；未设置时返回 "."（当前目录）
 * @details 读取 HOME 环境变量，空指针时退回当前目录，保证配置与历史文件总有落点。
 */
string home_directory()
{
  const char *home = getenv("HOME");
  return home == nullptr ? "." : home;
}

/**
 * @brief 打印命令行帮助，覆盖连接、执行与输出三类选项
 * @param program argv[0]，用于在 Usage 行显示当前程序名
 * @return 无返回值
 * @details 直接向标准输出拼出固定文本；--help 与参数解析失败时都会调用它。
 */
void usage(const char *program)
{
  cout << CSUDB_PRODUCT_NAME << " native client " << CSUDB_VERSION_STRING << "\n"
       << "Usage: " << program << " [options] [database]\n\n"
        << "Connection:\n"
        << "  -h, --host HOST          Server host (default 127.0.0.1)\n"
        << "  -P, --port PORT          Server port (default 6789)\n"
        << "  -U, --url URL            Server endpoint: host[:port], local or online\n"
        << "                           e.g. 127.0.0.1:6789 | 117.50.163.43:8157\n"
       << "  -u, --user USER          Login user (default root)\n"
       << "  -p, --password           Prompt for password (never accepts a value)\n"
       << "  -D, --database DB        Initial database\n"
       << "      --profile NAME       Profile in ~/.csudb/config.toml\n"
       << "      --config FILE        Alternate profile file\n\n"
       << "Execution:\n"
       << "  -e, --execute SQL        Execute SQL and exit\n"
        << "  -f, --file FILE          Execute SQL file and exit\n"
        << "      --complete \"SQL\"       Print SQL completion candidates and exit\n"
       << "      --batch              Tab-separated script output\n"
       << "      --silent             Suppress banner\n"
       << "      --no-color           Disable ANSI color\n"
       << "      --ping               Test server reachability and exit\n"
       << "      --table              Table output (default)\n"
       << "      --help               Show this help\n"
       << "      --version            Show version\n";
}

/**
 * @brief 去掉配置文件里值两侧的成对单引号或双引号
 * @param value 原始的配置值文本
 * @return 去掉外层成对引号后的字符串；没有成对引号时原样返回
 * @details 先 trim，再检查首尾是否为同种引号且长度至少为 2，满足才截取中间部分。
 */
string unquote(string value)
{
  value = trim(value);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
    return value.substr(1, value.size() - 2);
  return value;
}

using ConfigSections = map<string, map<string, string>>;

/**
 * @brief 读取极简 TOML 配置
 * @details 只支持 [节名] 与 key = value 两种行，够用即可，不追求完整 TOML 语法。
 *  以 std::ifstream 逐行读取，忽略空行与 # 注释；未进入任何节时归入 default 节。
 * @param path 配置文件路径；文件打不开时直接返回空表而不报错
 * @return 节名到键值对的映射；文件不存在时返回空表
 */
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

/**
 * @brief 把某个配置节里的 host、user、database、port 覆盖到选项上
 * @param options 输入输出参数，被选中的字段会被就地覆盖
 * @param section 已解析出的节键值对
 * @return 无返回值
 * @details 用局部 lambda 只在 key 存在时赋值，缺省项保持原值；
 *  port 为文本，用 std::atoi 转成整数。
 */
void apply_section(ClientOptions &options, const map<string, string> &section)
{
  auto assign = [&](const char *key, string &target) { auto it = section.find(key); if (it != section.end()) target = it->second; };
  assign("host", options.host);
  assign("user", options.user);
  assign("database", options.database);
  auto port = section.find("port");
  if (port != section.end()) options.port = std::atoi(port->second.c_str());
}

/**
 * @brief 用 CSUDB_* 环境变量覆盖选项
 * @param options 输入输出参数，命中的环境变量会覆盖对应字段
 * @return 无返回值
 * @details 只接受非空的环境变量值；CSUDB_PORT 用 std::atoi 转整数后覆盖端口。
 */
void apply_environment(ClientOptions &options)
{
  auto assign = [](const char *name, string &target) { const char *value = getenv(name); if (value != nullptr && *value != '\0') target = value; };
  assign("CSUDB_HOST", options.host);
  assign("CSUDB_USER", options.user);
  assign("CSUDB_DATABASE", options.database);
  assign("CSUDB_URL", options.url);
  const char *port = getenv("CSUDB_PORT");
  if (port != nullptr && *port != '\0') options.port = std::atoi(port);
}

/**
 * @brief 解析统一服务地址，支持 host、host:port 以及带协议前缀与路径的写法
 * @details 先去协议前缀与路径，再单独处理 IPv6 字面量 [::1]:6789，
 * 其余情况按最后一个冒号切分主机与端口；用 rfind 可正确保留主机内的冒号。
 * @param options 输入输出参数；url 为空时原样返回，否则写回 host 与 port
 * @return 无返回值（解析结果通过 options 引用回写）
 */
// 解析统一服务地址：支持 host、host:port，可带 native:// / http(s):// 前缀与路径。
// 例：127.0.0.1:6789（本地）、117.50.163.43:8157（线上）、http://host:port
void apply_url(ClientOptions &options)
{
  if (options.url.empty()) {
    return;
  }
  string value = options.url;
  auto   scheme = value.find("://");
  if (scheme != string::npos) {
    value = value.substr(scheme + 3);
  }
  auto slash = value.find('/');
  if (slash != string::npos) {
    value = value.substr(0, slash);
  }
  if (value.empty()) {
    return;
  }
  if (value.front() == '[') {  // IPv6 字面量 [::1]:6789
    auto close = value.find(']');
    if (close != string::npos) {
      options.host = value.substr(1, close - 1);
      if (close + 1 < value.size() && value[close + 1] == ':') {
        options.port = std::atoi(value.substr(close + 2).c_str());
      }
      return;
    }
  }
  auto colon = value.rfind(':');
  if (colon != string::npos && colon + 1 < value.size()) {
    options.host = value.substr(0, colon);
    options.port = std::atoi(value.substr(colon + 1).c_str());
  } else {
    options.host = value;
  }
}

/**
 * @brief 解析命令行参数并与配置合并
 * @details 优先级从低到高依次是：内置默认值、配置文件 default 节、指定 profile 节、
 * 环境变量、命令行参数。所以配置文件先合并进来，命令行覆盖放在最后，
 * 这样命令行永远拥有最高优先级。先用 getopt_long 收集命令行覆盖项，
 * 再逐层合并，最后统一校验端口范围。
 * @param argc 参数个数，来自 main
 * @param argv 参数数组，来自 main
 * @param options 输入输出参数，合并结果写入其中
 * @return 参数非法、profile 不存在或端口越界时返回 false，其余返回 true
 */
bool parse_options(int argc, char **argv, ClientOptions &options)
{
  CliOverrides overrides;
  const char *env_config = getenv("CSUDB_CONFIG");
  options.config_path = env_config == nullptr ? home_directory() + "/.csudb/config.toml" : env_config;
  const char *env_profile = getenv("CSUDB_PROFILE");
  if (env_profile != nullptr) options.profile = env_profile;

  enum { OPT_PROFILE = 1000, OPT_CONFIG, OPT_BATCH, OPT_SILENT, OPT_NO_COLOR, OPT_PING, OPT_TABLE, OPT_HELP, OPT_VERSION, OPT_COMPLETE, OPT_URL };
  static option long_options[] = {{"host", required_argument, nullptr, 'h'}, {"port", required_argument, nullptr, 'P'},
      {"user", required_argument, nullptr, 'u'}, {"password", no_argument, nullptr, 'p'},
      {"database", required_argument, nullptr, 'D'}, {"execute", required_argument, nullptr, 'e'},
      {"file", required_argument, nullptr, 'f'}, {"url", required_argument, nullptr, 'U'},
      {"profile", required_argument, nullptr, OPT_PROFILE},
      {"config", required_argument, nullptr, OPT_CONFIG}, {"batch", no_argument, nullptr, OPT_BATCH},
      {"silent", no_argument, nullptr, OPT_SILENT}, {"no-color", no_argument, nullptr, OPT_NO_COLOR},
      {"ping", no_argument, nullptr, OPT_PING}, {"table", no_argument, nullptr, OPT_TABLE},
      {"complete", required_argument, nullptr, OPT_COMPLETE},
      {"help", no_argument, nullptr, OPT_HELP}, {"version", no_argument, nullptr, OPT_VERSION}, {nullptr, 0, nullptr, 0}};

  // First pass records command-line values; profile configuration is merged afterward.
  int option;
  while ((option = getopt_long(argc, argv, "h:P:u:pD:e:f:U:", long_options, nullptr)) != -1) {
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
      case OPT_COMPLETE: options.complete_sql = optarg; options.silent = true; break;
      case 'U': overrides.url = optarg; break;
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
  if (overrides.url) options.url = *overrides.url;
  if (!options.url.empty()) apply_url(options);
  if (overrides.host) options.host = *overrides.host;
  if (overrides.port) options.port = *overrides.port;
  if (overrides.user) options.user = *overrides.user;
  if (overrides.database) options.database = *overrides.database;
  if (options.port <= 0 || options.port > 65535) { cerr << "ERROR: invalid port\n"; return false; }
  return true;
}

/**
 * @brief 以不回显的方式读取密码
 * @details 标准输入不是终端时改从 /dev/tty 读，避免密码混在管道重定向的内容里；
 * 读取期间关闭终端回显，读完立即恢复。密码从不作为命令行参数接收。
 * 实现上逐字节 ::read，遇 EINTR 重试，遇换行或文件结束即停止。
 * @return 读到的密码；没有可用终端时返回空串
 */
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

/**
 * @brief Native 协议的客户端连接
 * @details 只负责建立 TCP 连接与收发 JSON 包，不做任何 SQL 解析。
 * 连接不可复制、可以移动，析构时自动关闭描述符。
 * @note 唯一资源是文件描述符 fd_，采用“独占所有权”语义：
 *  移动后源对象置 -1，析构与 close() 都只在 fd_ >= 0 时关闭，避免二次 close。
 */
class NativeConnection
{
public:
  /** @brief 默认构造：不持有任何连接，fd_ 为 -1 */
  NativeConnection() = default;
  /** @brief 禁用拷贝构造：文件描述符不能被两个对象同时拥有 */
  NativeConnection(const NativeConnection &) = delete;
  /** @brief 禁用拷贝赋值：理由同拷贝构造 */
  NativeConnection &operator=(const NativeConnection &) = delete;
  /**
   * @brief 移动构造：接管 other 的描述符
   * @param other 被移动的连接；移动后其 fd_ 置为 -1，不再拥有资源
   * @return 无（构造函数）
   * @details 直接偷取描述符并把源置 -1，不产生任何系统调用。
   */
  NativeConnection(NativeConnection &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  /**
   * @brief 移动赋值：先释放自身连接，再接管 other
   * @param other 被移动的连接
   * @return 自身引用，支持链式赋值
   * @details 用 this != &other 防止自赋值；接管后把源置 -1。
   */
  NativeConnection &operator=(NativeConnection &&other) noexcept
  {
    if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
    return *this;
  }
  /** @brief 析构：自动关闭底层套接字，防止描述符泄漏 */
  ~NativeConnection() { close(); }
  /** @brief 关闭连接：描述符有效时关闭并置 -1，可安全重复调用 */
  void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

/**
 * @brief 建立到服务端的 TCP 连接
 * @details 用 getaddrinfo 解析地址，逐个尝试返回的候选地址，第一个连上的就采用。
 * @param host 主机名或 IP
 * @param port 端口
 * @param error 输出参数，失败时写入错误原因
 * @return 成功建立连接返回 true，全部候选地址都失败返回 false
 * @note 失败时保证 fd_ 处于关闭状态；errno 用 strerror 转成可读文本。
 */
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

/**
 * @brief 发一次请求并读回响应
 * @details 协议是「UTF-8 JSON 加一个 NUL 结尾」。发送侧循环处理短写；
 * 接收侧循环累积直到遇到 NUL，并以 16 MiB 作为安全上限，防止异常服务端把客户端撑爆。
 * 请求与响应都是 JSON，客户端与 CLI、Web、SDK 共用同一套语义。
 * @param request 请求 JSON
 * @param response 输出参数，响应 JSON
 * @param error 输出参数，失败原因
 * @return 发送、接收与 JSON 解析都成功返回 true，否则返回 false 并写入 error
 */
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

/**
 * @brief 把响应渲染成终端输出
 * @details 三种形态：失败时打印错误码与消息；无结果集时打印影响行数与可选耗时；
 * 有结果集时按 batch 开关选择制表符分隔或带边框的表格。
 * 表格里值为 NULL 的单元格高亮显示，列宽按内容自适应。
 * @param response 服务端响应
 * @param out 输出流，可能是标准输出也可能是被重定向的文件
 * @param batch 是否用制表符分隔的批处理格式
 * @param timing 是否显示执行耗时
 * @param style 终端配色
 * @return 无返回值；所有内容直接写入 out
 * @note 值等于 "NULL"（忽略大小写）的单元格用紫色高亮，列宽按内容取最大值。
 */
void render_table(
    const Json::Value &response, std::ostream &out, bool batch, bool timing, const tui::Style &style)
{
  if (!response.get("success", false).asBool()) {
    const Json::Value &error = response["error"];
    out << style.bold(style.red("ERROR " + std::to_string(error.get("code", 1).asInt()))) << ": "
        << error.get("message", "request failed").asString() << '\n';
    return;
  }
  const Json::Value &columns = response["columns"];
  const Json::Value &rows = response["rows"];
  if (columns.empty()) {
    out << style.green(response.get("message", "Query OK").asString());
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
    auto border = [&]() {
      std::ostringstream line;
      line << '+';
      for (size_t width : widths) line << string(width + 2, '-') << '+';
      out << style.cyan(line.str()) << '\n';
    };
    border();
    out << style.cyan("|");
    for (Json::ArrayIndex i = 0; i < columns.size(); ++i) {
      string heading = columns[i]["name"].asString();
      heading.append(widths[i] - heading.size(), ' ');
      out << ' ' << style.bold(style.cyan(heading)) << ' ' << style.cyan("|");
    }
    out << '\n';
    border();
    for (const Json::Value &row : rows) {
      out << style.cyan("|");
      for (Json::ArrayIndex i = 0; i < columns.size(); ++i) {
        string cell = i < row.size() ? row[i].asString() : "";
        cell.append(widths[i] - cell.size(), ' ');
        out << ' ' << (upper(trim(cell)) == "NULL" ? style.purple(cell) : cell) << ' ' << style.cyan("|");
      }
      out << '\n';
    }
    border();
    out << style.green(std::to_string(rows.size()) + (rows.size() == 1 ? " row" : " rows") + " in set");
    if (timing) out << " (" << std::fixed << std::setprecision(3) << response.get("execution_time_us", 0).asUInt64() / 1000000.0 << " sec)";
    out << '\n';
  }
}

/**
 * @brief 判断一段输入是不是一条完整可执行的 SQL
 * @details 逐字符跟踪单引号、双引号与反斜杠转义状态，只有出现在字符串之外的
 * 分号且其后只剩空白，或者整段以反斜杠 g 结尾，才算输入结束。
 * 这样才能支持跨行的字符串与多行语句。
 * @param sql 当前累积的完整输入缓冲（可能含换行）
 * @return 可以执行返回 true，还需要继续输入返回 false
 */
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

/**
 * @brief 把一段脚本按分号切成多条语句
 * @details 与 complete_sql 用同一套引号与转义状态跟踪，所以字符串里的分号不会被误切。
 *  分号本身保留在切出的语句里，末尾不足一条的残句也会作为最后一条返回。
 * @param script 待切分的完整脚本内容
 * @return 语句列表；每段都做了 trim，空白段被丢弃
 */
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

/**
 * @brief 判断语句是否包含敏感内容
 * @details 命中 PASSWORD 或 IDENTIFIED BY 的语句不写进历史记录文件，避免口令落盘。
 * @param sql 待检查的原始 SQL 文本
 * @return 含敏感关键字返回 true，否则返回 false
 */
bool sensitive_sql(const string &sql)
{
  string normalized = upper(sql);
  return normalized.find("IDENTIFIED BY") != string::npos || normalized.find("PASSWORD") != string::npos;
}

/**
 * @brief 打印 CLI 启动标志，按终端宽度选择完整标志、简版或纯文字
 * @param terminal 终端能力探测结果，用来判断布局模式与是否支持 Unicode
 * @param style 终端配色
 * @return 无返回值，直接写入标准输出
 * @details MINIMAL 或非 Unicode 终端只打一行文字，COMPACT 打两行，
 *  否则输出完整的 ASCII 艺术字 LOGO。
 */
void print_client_logo(const tui::Capabilities &terminal, const tui::Style &style)
{
  if (terminal.layout() == tui::LayoutMode::MINIMAL || !terminal.unicode()) {
    cout << style.bold(style.magenta("CSUDB 2026")) << '\n';
    return;
  }
  if (terminal.layout() == tui::LayoutMode::COMPACT) {
    cout << style.bold(style.magenta("CSUDB 2026")) << '\n'
         << style.dim("Compiler · Database · Operating Systems") << "\n";
    return;
  }
  static const char *logo[] = {
      "  ██████╗███████╗██╗   ██╗██████╗ ██████╗ ",
      " ██╔════╝██╔════╝██║   ██║██╔══██╗██╔══██╗",
      " ██║     ███████╗██║   ██║██║  ██║██████╔╝",
      " ██║     ╚════██║██║   ██║██║  ██║██╔══██╗",
      " ╚██████╗███████║╚██████╔╝██████╔╝██████╔╝",
      "  ╚═════╝╚══════╝ ╚═════╝ ╚═════╝ ╚═════╝ "};
  for (const char *line : logo) cout << style.magenta(line) << '\n';
  cout << "\n" << style.bold(style.cyan("CSUDB 2026")) << '\n'
       << style.dim("Compiler · Database · Operating Systems") << "\n";
}

/**
 * @brief 打印登录后的欢迎信息
 * @details 先画标志，再列出服务端版本、主机、用户与当前数据库，
 * 后面三行提示怎么输入语句、怎么补全、去哪看帮助。
 * @param options 已解析的客户端选项，提供主机、用户与库名
 * @param login 登录响应，从中读取服务端版本号
 * @return 无返回值；options.silent 为真时直接跳过
 */
void banner(const ClientOptions &options, const Json::Value &login)
{
  if (options.silent) return;
  const tui::Capabilities terminal = tui::Capabilities::detect(STDOUT_FILENO, !options.no_color);
  const tui::Style style(terminal.color());
  print_client_logo(terminal, style);
  const int rule_width = terminal.content_width(64, 0);
  cout << style.dim(tui::repeat(terminal.unicode() ? "─" : "-", rule_width)) << "\n\n";
  cout << style.cyan("Connection") << "\n\n";
  auto field = [&](const string &label, const string &value) {
    cout << "  " << style.cyan(label)
         << string(static_cast<size_t>(std::max(1, 12 - static_cast<int>(label.size()))), ' ')
         << style.white(tui::truncate_middle(value, static_cast<size_t>(std::max(12, rule_width - 16)))) << '\n';
  };
  field("Server", "CSUDB " + login["attributes"].get("version", CSUDB_VERSION_STRING).asString());
  field("Host", options.host + ':' + std::to_string(options.port));
  field("User", options.user);
  field("Database", options.database.empty() ? "(none)" : options.database);
  cout << "\n" << style.bold("Welcome to the CSUDB monitor.") << "\n\n"
       << style.dim("Commands end with ';' or '\\g'.") << '\n'
       << style.dim("Press Tab for SQL completion (keywords / tables / columns); pause for AI suggestion.") << '\n'
       << style.dim("Type '/help' for help. Use '/web' to open the Web Console.") << "\n\n";
}

const char *meta_help = R"(CSUDB shell commands:
  /help, /?                  Show this help
  /q, /quit, /exit           Quit
  /status                    Connection, session, and engine status
  /connect HOST PORT USER    Reconnect (password is prompted)
  /use DATABASE              Select database
  /database                  Show current database
  /complete SQL              Show SQL completion candidates for a prefix
  /timing [on|off]           Toggle execution timing
  /clear                     Clear an ANSI terminal
  /history                   Show session history
  /source FILE               Execute a SQL file
  /output [FILE]             Redirect or restore query output
  /buffer                    Show Buffer Pool summary
  /pages [N]                 Show at most N frame snapshots (default 20)
  /server                    Show server status
  /web [PORT]                Open the local Web Console (default 8765)
  /web status                Show Web Console process status
  /web stop                  Stop the local Web Console
  /pager                     Reserved; currently unsupported

Type '/' and press Tab to list commands. Partial names are completed and
mistyped commands show nearby candidates. Legacy backslash forms still work.
)";

/**
 * @brief 返回全部元命令名字，供补全与拼写纠错使用
 * @return 元命令名字列表的常量引用
 * @details 用函数内 static 局部变量只初始化一次，避免每次调用都重建列表。
 */
const vector<string> &meta_command_names()
{
  static const vector<string> names = {"/help", "/?", "/q", "/quit", "/exit", "/status", "/connect", "/use",
      "/database", "/timing", "/clear", "/history", "/source", "/output", "/buffer", "/pages", "/server",
      "/web", "/pager"};
  return names;
}

/**
 * @brief 计算两个字符串的编辑距离
 * @details 标准动态规划实现，只保留上一行与当前行两个数组，空间 O(n)。
 * 用于把拼错的元命令映射到相近候选。
 * @param left 第一个字符串
 * @param right 第二个字符串
 * @return 把 left 变成 right 所需的最少单字符增删改次数
 */
size_t edit_distance(const string &left, const string &right)
{
  vector<size_t> previous(right.size() + 1);
  vector<size_t> current(right.size() + 1);
  for (size_t column = 0; column <= right.size(); ++column) previous[column] = column;
  for (size_t row = 1; row <= left.size(); ++row) {
    current[0] = row;
    for (size_t column = 1; column <= right.size(); ++column) {
      const size_t substitution = previous[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1);
      current[column] = std::min({previous[column] + 1, current[column - 1] + 1, substitution});
    }
    previous.swap(current);
  }
  return previous[right.size()];
}

/**
 * @brief 判断 needle 是否为 candidate 的子序列，用于放宽拼错时的候选范围
 * @param needle 待查找的子序列
 * @param candidate 被扫描的候选字符串
 * @return needle 中字符按顺序都出现在 candidate 中返回 true
 * @details 单次线性扫描，用 position 指针推进，不要求字符连续。
 */
bool is_subsequence(const string &needle, const string &candidate)
{
  size_t position = 0;
  for (char character : candidate) {
    if (position < needle.size() && needle[position] == character) ++position;
  }
  return position == needle.size();
}

/**
 * @brief 统一元命令写法：转小写并把反斜杠前缀改成正斜杠，两种写法都接受
 * @param command 原始命令词，例如 /USE 或 \use
 * @return 归一化后的命令词；已有 / 前缀或非前缀字符时保持原样
 * @details 先整体转小写，若首字符是反斜杠则改成正斜杠，便于后续统一比较。
 */
string normalized_meta_command(string command)
{
  std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return std::tolower(c); });
  if (!command.empty() && command[0] == '\\') command[0] = '/';
  return command;
}

/**
 * @brief 按输入给出元命令候选
 * @details 先找前缀匹配；一个都没有时才做模糊匹配，条件是编辑距离不超过 2
 * 或者是输入的子序列，最后按距离排序并最多返回 4 个。
 * @param input 已经输入的部分命令
 * @param include_fuzzy 是否允许模糊匹配。补全列表允许，行内提示只给前缀匹配
 * @return 候选命令列表；有前缀匹配时只返回前缀匹配，否则返回最相近的最多 4 个
 */
vector<string> matching_meta_commands(const string &input, bool include_fuzzy)
{
  const string query = normalized_meta_command(input);
  vector<string> prefix_matches;
  vector<std::pair<size_t, string>> fuzzy_matches;
  for (const string &candidate : meta_command_names()) {
    if (candidate.compare(0, query.size(), query) == 0) {
      prefix_matches.push_back(candidate);
      continue;
    }
    if (!include_fuzzy || query == "/") continue;
    size_t distance = edit_distance(query, candidate);
    if (distance <= 2 || is_subsequence(query, candidate)) fuzzy_matches.emplace_back(distance, candidate);
  }
  if (!prefix_matches.empty()) return prefix_matches;
  std::sort(fuzzy_matches.begin(), fuzzy_matches.end());
  vector<string> result;
  for (const auto &match : fuzzy_matches) {
    result.push_back(match.second);
    if (result.size() == 4) break;
  }
  return result;
}

/**
 * @brief 元命令的 Tab 补全回调
 * @param input 当前整行输入
 * @param context_length 输出参数，需替换的上下文长度（此处为整行长度）
 * @return replxx 补全候选列表；输入含空格或非命令前缀时返回空
 * @details 只在以 / 或 \ 开头且尚未输入空白的整词上触发，走本地候选表，不访问服务端。
 */
replxx::Replxx::completions_t complete_meta_command(const string &input, int &context_length)
{
  replxx::Replxx::completions_t completions;
  if (input.empty() || (input[0] != '/' && input[0] != '\\') || input.find_first_of(" \t\n") != string::npos) return completions;
  context_length = static_cast<int>(input.size());
  for (const string &candidate : matching_meta_commands(input, true)) completions.emplace_back(candidate);
  return completions;
}

/**
 * @brief 元命令的行内提示回调，唯一候选时会高亮显示
 * @param input 当前整行输入
 * @param context_length 输出参数，需替换的上下文长度
 * @param color 输出参数，唯一候选时置为亮青色
 * @return 提示文本列表；非命令前缀或含空白时返回空
 * @details 只做前缀匹配（不允许模糊），因此比较保守，不会误导用户。
 */
replxx::Replxx::hints_t hint_meta_command(
    const string &input, int &context_length, replxx::Replxx::Color &color)
{
  replxx::Replxx::hints_t hints;
  if (input.empty() || (input[0] != '/' && input[0] != '\\') || input.find_first_of(" \t\n") != string::npos) return hints;
  context_length = static_cast<int>(input.size());
  for (const string &candidate : matching_meta_commands(input, false)) hints.push_back(candidate);
  if (hints.size() == 1) color = replxx::Replxx::Color::BRIGHTCYAN;
  return hints;
}

replxx::Replxx::completions_t complete_dispatch(const string &input, int &context_length);
replxx::Replxx::hints_t       hint_dispatch(const string &input, int &context_length, replxx::Replxx::Color &color);

/**
 * @brief 交互式外壳
 * @details 承担全部分布在客户端一侧的工作：连接与登录、提示符、多行缓冲、
 * 历史记录、补全与提示、元命令分发、结果渲染、输出重定向、Web 控制台启停。
 * 它不解析也不执行 SQL，所有语句都原样发给服务端。
 * @note 全局唯一实例由 g_active_shell 持有，供无捕获的自由函数回调转发到本类。
 */
class Shell
{
public:
  friend replxx::Replxx::completions_t complete_dispatch(const string &input, int &context_length);
  friend replxx::Replxx::hints_t       hint_dispatch(const string &input, int &context_length, replxx::Replxx::Color &color);

  /**
   * @brief 构造外壳并接管选项
   * @param options 已解析的客户端选项，按值传入后移动到成员
   * @return 无（构造函数）
   * @details 用 std::move 避免复制，声明为 explicit 防止隐式转换。
   */
  explicit Shell(ClientOptions options) : options_(std::move(options)) {}

/**
 * @brief 外壳主流程
 * @details 依次是：连服务端、处理 ping、取密码并登录、抹掉内存里的口令、
 * 按选项选择一次性执行或进入交互。返回码区分连接失败、认证失败与语句失败，
 * 便于脚本判断。
 * @return 0 成功；2 连接或登录请求失败；3 认证被拒；1 一次性语句执行失败
 */
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
    if (password.empty()) {
      const char *env_password = getenv("CSUDB_PASSWORD");
      if (env_password != nullptr) password = env_password;
    }
    Json::Value login_request, login_response;
    login_request["type"] = "login"; login_request["user"] = options_.user; login_request["password"] = password; login_request["database"] = options_.database;
    if (!connection_.request(login_request, login_response, error)) { cerr << "ERROR: login request failed: " << error << '\n'; return 2; }
    password.assign(password.size(), '\0'); password.clear(); login_request["password"] = "";
    if (!login_response.get("success", false).asBool()) {
      render_table(login_response, cerr, false, false, error_style());
      return 3;
    }
    if (options_.database.empty()) options_.database = login_response["attributes"].get("database", "sys").asString();
    banner(options_, login_response);

    if (!options_.execute_sql.empty()) return execute(options_.execute_sql) ? 0 : 1;
    if (!options_.file.empty()) return execute_file(options_.file) ? 0 : 1;
    if (!options_.complete_sql.empty()) return print_completion(options_.complete_sql) ? 0 : 1;
    if (!isatty(STDIN_FILENO)) { std::ostringstream input; input << std::cin.rdbuf(); return execute_script(input.str()) ? 0 : 1; }
    return interactive();
  }

private:
  /**
   * @brief 返回当前输出流
   * @return /output 重定向打开时返回文件流，否则返回标准输出
   * @details 所有查询结果与元命令输出都经此单一出口，方便随时重定向。
   */
  std::ostream &output() { return output_file_.is_open() ? output_file_ : cout; }
  /**
   * @brief 结果配色
   * @return 结果输出使用的样式对象
   * @details 输出被 /output 重定向到文件或处于批处理模式时强制关闭颜色，
   *  否则通过终端能力探测决定是否着色。
   */
  tui::Style result_style() const
  {
    if (output_file_.is_open() || options_.batch) return tui::Style(false);
    return tui::Style(tui::Capabilities::detect(STDOUT_FILENO, !options_.no_color).color());
  }
  /**
   * @brief 错误信息的配色
   * @return 写入标准错误所用的样式对象
   * @details 单独探测 STDERR_FILENO，避免标准输出被重定向时连错误一起失色。
   */
  tui::Style error_style() const
  {
    return tui::Style(tui::Capabilities::detect(STDERR_FILENO, !options_.no_color).color());
  }

  /**
   * @brief 发一次请求（外壳层薄封装）
   * @param request 输入输出参数，要发送的请求 JSON
   * @param response 输出参数，服务端响应 JSON
   * @return 连接与收发成功返回 true；失败时已向标准错误打印原因并返回 false
   * @details 只封装 NativeConnection::request 的错误提示，不解释响应内容。
   */
  bool request(Json::Value &request, Json::Value &response)
  {
    string error;
    if (!connection_.request(request, response, error)) { cerr << "ERROR: connection failed: " << error << '\n'; return false; }
    return true;
  }

/**
 * @brief 向服务端请求 SQL 补全候选
 * @details 把完整缓冲与光标位置一起发过去，由服务端做语法分析，客户端不自己猜。
 * 可选返回模型生成的 ghost text。响应是一个八列的结果集，
 * 依次是插入文本、显示文本、类型、来源、替换起止位置、分数与补充说明。
 * @param buffer 当前完整输入缓冲，多行时含换行
 * @param cursor 光标在缓冲中的位置
 * @param want_model 是否请求模型生成的提示
 * @param items 输出参数，补全候选
 * @param ghost 输出参数，模型生成的提示文本
 * @param model_used 输出参数，服务端是否真的用了模型
 * @return 请求成功且服务端返回 success 时返回 true，否则返回 false
 * @details 客户端只负责透传缓冲与光标，具体补全位置与候选由服务端语法分析决定。
 */
  // 从服务端获取 SQL 补全候选（确定性 + 可选模型 ghost text）
  bool fetch_completion(const string &buffer, size_t cursor, bool want_model, vector<CompletionEntry> &items, string &ghost, bool &model_used)
  {
    Json::Value request_value, response;
    request_value["type"]       = "complete";
    request_value["sql"]        = buffer;
    request_value["cursor"]     = Json::UInt64(cursor);
    request_value["max_items"]  = Json::UInt(12);
    request_value["want_model"] = want_model;
    if (!request(request_value, response)) return false;
    if (!response.get("success", false).asBool()) return false;

    const Json::Value &rows = response["rows"];
    for (const Json::Value &row : rows) {
      if (row.size() < 8) continue;
      CompletionEntry entry;
      entry.insert_text   = row[0].asString();
      entry.display_text  = row[1].asString();
      entry.kind          = row[2].asString();
      entry.source        = row[3].asString();
      entry.replace_start = static_cast<size_t>(std::strtoll(row[4].asString().c_str(), nullptr, 10));
      entry.replace_end   = static_cast<size_t>(std::strtoll(row[5].asString().c_str(), nullptr, 10));
      entry.score         = std::atof(row[6].asString().c_str());
      entry.detail        = row[7].asString();
      items.push_back(std::move(entry));
    }
    if (response["attributes"].isMember("ghost_text")) ghost = response["attributes"]["ghost_text"].asString();
    if (response["attributes"].isMember("model_used")) model_used = response["attributes"]["model_used"].asString() == "true";
    return true;
  }

  /**
   * @brief 打印某个前缀的补全候选，用于 --complete 与 /complete
   * @param sql 待补全的 SQL 前缀
   * @return 请求并打印成功返回 true，请求失败返回 false（已打印错误）
   * @details 以非模型模式请求候选，每行输出插入文本、类型、来源与说明，
   *  若有 ghost text 再额外打印一行。
   */
  bool print_completion(const string &sql)
  {
    vector<CompletionEntry> items;
    string                  ghost;
    bool                    model_used = false;
    if (!fetch_completion(sql, sql.size(), false, items, ghost, model_used)) {
      cerr << "ERROR: completion request failed\n";
      return false;
    }
    cout << "SQL> " << sql << '\n';
    for (const CompletionEntry &item : items) {
      cout << "  " << std::left << std::setw(18) << item.insert_text << "  " << item.kind << "  " << item.source;
      if (!item.detail.empty()) cout << "  " << item.detail;
      cout << '\n';
    }
    if (!ghost.empty()) cout << "ghost: " << ghost << '\n';
    return true;
  }

/**
 * @brief replxx 的补全回调
 * @details 元命令走本地补全；SQL 则把累积缓冲与当前行拼起来交给服务端，
 * 由服务端判断当前位置能不能补全。补全结果只取插入文本，
 * 并把替换范围换算成 replxx 需要的上下文长度。
 * @param input 当前正在编辑的行（不含已确认的多行缓冲）
 * @param context_length 输出参数，需替换的上下文长度
 * @return replxx 补全候选列表；请求失败或输入为空时返回空列表
 */
  // replxx 补全回调：SQL 语句补全（命令行/多行均可，使用 Shell 累积的 buffer）
  replxx::Replxx::completions_t complete_line(const string &input, int &context_length)
  {
    replxx::Replxx::completions_t completions;
    if (input.empty()) return completions;

    // 元命令仍走原有补全
    if (input[0] == '/' || input[0] == '\\') {
      return complete_meta_command(input, context_length);
    }

    // 禁止在字符串/注释中补全（服务端还会再做一次安全校验）
    string  full   = sql_buffer_.empty() ? input : sql_buffer_ + "\n" + input;
    size_t  cursor = full.size();
    vector<CompletionEntry> items;
    string                  ghost;
    bool                    model_used = false;
    if (!fetch_completion(full, cursor, false, items, ghost, model_used)) return completions;

    for (const CompletionEntry &item : items) completions.emplace_back(item.insert_text);
    if (!items.empty() && items.front().replace_end <= cursor) {
      context_length = static_cast<int>(cursor - items.front().replace_start);
    }
    return completions;
  }

/**
 * @brief replxx 的行内提示回调，只展示服务端返回的 ghost text
 * @param input 当前正在编辑的行
 * @param context_length 输出参数，置 0 表示提示不替换任何已有文本
 * @param color 输出参数，灰度色，表示这是补充建议而非补全
 * @return 提示列表；无 ghost text 或请求失败时为空
 * @details 以 want_model=true 请求服务端，是否真正生成由服务端决定。
 */
  // replxx ghost text 回调（模型未启用时不会返回内容）
  replxx::Replxx::hints_t hint_line(const string &input, int &context_length, replxx::Replxx::Color &color)
  {
    replxx::Replxx::hints_t hints;
    if (input.empty() || input[0] == '/' || input[0] == '\\') return hints;
    string full   = sql_buffer_.empty() ? input : sql_buffer_ + "\n" + input;
    size_t cursor = full.size();
    vector<CompletionEntry> items;
    string                  ghost;
    bool                    model_used = false;
    if (!fetch_completion(full, cursor, true, items, ghost, model_used)) return hints;
    if (!ghost.empty()) {
      hints.emplace_back(ghost);
      context_length = 0;
      color          = replxx::Replxx::Color::GRAY;
    }
    return hints;
  }

/**
 * @brief 执行一条 SQL 并渲染结果
 * @details 先去掉结尾的反斜杠 g，再包成 query 请求发出去。
 * 如果响应里带了当前数据库，就同步更新提示符上的库名，这样 USE 语句执行后提示符会自动跟着变。
 * @param sql 一条待执行的语句（可带结尾的 \g）
 * @return 响应 success 字段为真返回 true，连接失败或服务端报错返回 false
 */
  bool execute(string sql)
  {
    sql = trim(sql);
    if (sql.size() >= 2 && sql.substr(sql.size() - 2) == "\\g") sql = trim(sql.substr(0, sql.size() - 2));
    Json::Value request_value, response;
    request_value["type"] = "query"; request_value["sql"] = sql;
    if (!request(request_value, response)) return false;
    render_table(response, output(), options_.batch, options_.timing, result_style());
    if (response.get("success", false).asBool() && response["attributes"].isMember("database")) options_.database = response["attributes"]["database"].asString();
    return response.get("success", false).asBool();
  }

  /**
   * @brief 把一段脚本切成语句逐条执行
   * @param script 完整脚本内容
   * @return 全部语句都成功返回 true；任一条失败则整体为 false，但后续语句仍继续执行
   * @details 复用 split_sql_script 做切分，保证字符串内的分号不被误切。
   */
  bool execute_script(const string &script)
  {
    bool ok = true;
    for (const string &statement : split_sql_script(script)) if (!execute(statement)) ok = false;
    return ok;
  }

  /**
   * @brief 读取 SQL 文件并逐条执行
   * @param path SQL 文件路径
   * @return 文件全部语句执行成功返回 true；打不开或任一语句失败返回 false
   * @details 一次性读入整个文件再交给 execute_script，
   *  文件不存在时打印错误并立即返回。
   */
  bool execute_file(const string &path)
  {
    std::ifstream in(path);
    if (!in) { cerr << "ERROR: cannot open SQL file '" << path << "'\n"; return false; }
    std::ostringstream content; content << in.rdbuf(); return execute_script(content.str());
  }

/**
 * @brief 显示服务端信息或 Buffer Pool 快照
 * @details 两个命令共用一条路径，区别只在请求类型。先把 attributes 里的键值对
 * 按固定列宽打印出来，快照请求再额外把 rows 当作表格渲染。
 * 这正是 /status、/buffer 与 /pages 的实现。
 * @param pages 为真时请求 buffer_snapshot，为假时请求 server_info
 * @param limit 快照里最多返回多少个 Frame
 * @return 无返回值；请求或渲染失败时打印错误后返回
 */
  void show_status(bool pages, size_t limit)
  {
    Json::Value request_value, response;
    request_value["type"] = pages ? "buffer_snapshot" : "server_info";
    request_value["limit"] = Json::UInt64(limit);
    if (!request(request_value, response)) return;
    if (!response.get("success", false).asBool()) {
      render_table(response, cerr, false, false, error_style());
      return;
    }
    const tui::Style style = result_style();
    for (const string &name : response["attributes"].getMemberNames()) {
      string label = name;
      label.append(22 - std::min<size_t>(22, label.size()), ' ');
      output() << style.cyan(label) << ": " << response["attributes"][name].asString() << '\n';
    }
    if (pages) render_table(response, output(), options_.batch, false, style);
  }

/**
 * @brief 分发一条元命令
 * @details 命令名先归一化再比较，因此 /status 与 \status 等价。
 * 绝大多数命令在本地完成，只有 /use 会转成一条 USE 语句发给服务端。
 * 无法识别时用编辑距离给出相近候选。
 * @param line 整行输入
 * @param quit 输出参数，收到退出命令时置为 true
 * @return 恒为 true，表示这行输入已被消费
 * @note 用 std::istringstream 切出命令词与参数；未知命令用编辑距离给出相近候选。
 */
  bool dispatch_meta(const string &line, bool &quit)
  {
    std::istringstream stream(line);
    string command; stream >> command;
    string lower = normalized_meta_command(command);
    if (lower == "/q" || lower == "/quit" || lower == "/exit") { quit = true; return true; }
    if (lower == "/help" || lower == "/?") { output() << meta_help; return true; }
    if (lower == "/status" || lower == "/server") { show_status(false, 0); return true; }
    if (lower == "/buffer") { show_status(false, 0); return true; }
    if (lower == "/pages") { size_t limit = 20; stream >> limit; show_status(true, std::min<size_t>(limit, 200)); return true; }
    if (lower == "/database") { output() << (options_.database.empty() ? "(none)" : options_.database) << '\n'; return true; }
    if (lower == "/complete") {
      string rest;
      std::getline(stream, rest);
      print_completion(trim(rest));
      return true;
    }
    if (lower == "/use") { string database; stream >> database; if (database.empty()) cerr << "Usage: /use DATABASE\n"; else execute("USE " + database + ";"); return true; }
    if (lower == "/timing") {
      string setting; stream >> setting; setting = upper(setting);
      if (setting == "ON") options_.timing = true; else if (setting == "OFF") options_.timing = false; else if (setting.empty()) options_.timing = !options_.timing; else { cerr << "Usage: /timing [on|off]\n"; return true; }
      output() << "Timing is " << (options_.timing ? "on" : "off") << ".\n"; return true;
    }
    if (lower == "/clear") { if (isatty(STDOUT_FILENO)) cout << "\033[2J\033[H"; else cerr << "Unsupported: output is not a terminal.\n"; return true; }
    if (lower == "/history") { for (size_t i = 0; i < session_history_.size(); ++i) output() << i + 1 << "  " << session_history_[i] << '\n'; return true; }
    if (lower == "/source") { string path; stream >> path; if (path.empty()) cerr << "Usage: /source FILE\n"; else execute_file(path); return true; }
    if (lower == "/output") {
      string path; stream >> path;
      if (path.empty()) { output_file_.close(); cout << "Query output restored to stdout.\n"; }
      else { output_file_.close(); output_file_.open(path, std::ios::app); if (!output_file_) cerr << "ERROR: cannot open output file\n"; }
      return true;
    }
    if (lower == "/web") {
      string argument;
      stream >> argument;
      string message;
      if (argument == "status") {
        web_console_status(message);
        output() << message << '\n';
        return true;
      }
      if (argument == "stop") {
        stop_web_console(message);
        output() << message << '\n';
        return true;
      }

      int web_port = 8765;
      if (!argument.empty()) {
        char *end = nullptr;
        const long parsed = std::strtol(argument.c_str(), &end, 10);
        if (end == argument.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535) {
          cerr << "Usage: /web [PORT|status|stop]\n";
          return true;
        }
        web_port = static_cast<int>(parsed);
      }
      WebConsoleOptions web_options{options_.host, options_.port, web_port, true};
      const bool started = start_web_console(web_options, message);
      (started ? output() : cerr) << message << '\n';
      return true;
    }
    if (lower == "/pager") { cerr << "Unsupported: pager is reserved for a future release.\n"; return true; }
    if (lower == "/connect") {
      string host, user; int port = DEFAULT_PORT; stream >> host >> port >> user;
      if (host.empty() || user.empty()) { cerr << "Usage: /connect HOST PORT USER\n"; return true; }
      string password = read_password(), error;
      NativeConnection replacement;
      if (!replacement.connect_to(host, port, error)) { cerr << "ERROR: cannot connect: " << error << '\n'; return true; }
      Json::Value req, resp; req["type"] = "login"; req["user"] = user; req["password"] = password; req["database"] = "sys";
      if (!replacement.request(req, resp, error) || !resp.get("success", false).asBool()) { cerr << "ERROR: authentication failed\n"; return true; }
      connection_ = std::move(replacement); options_.host = host; options_.port = port; options_.user = user; options_.database = "sys"; return true;
    }
    cerr << "Unknown command '" << command << "'.";
    vector<string> suggestions = matching_meta_commands(lower, true);
    if (!suggestions.empty()) {
      cerr << " Did you mean ";
      for (size_t i = 0; i < suggestions.size(); ++i) cerr << (i == 0 ? "" : ", ") << suggestions[i];
      cerr << '?';
    }
    cerr << " Type /help.\n";
    return true;
  }

/**
 * @brief 交互式主循环
 * @details 每次循环先按终端能力重算提示符：一级提示符带库名，续行提示符缩进显示。
 * 多行 SQL 累积在 sql_buffer_ 里，直到 complete_sql 判定输入完整才执行。
 * 含密码的语句不进历史，Ctrl-C 清空缓冲但不退出。
 * @return 进程退出码（正常退出恒为 0）
 * @note 补全与提示回调通过 g_active_shell 指回本实例的 complete_line / hint_line。
 */
  int interactive()
  {
    string config_dir = home_directory() + "/.csudb";
    std::error_code ec; std::filesystem::create_directories(config_dir, ec); chmod(config_dir.c_str(), S_IRWXU);
    string history_path = config_dir + "/history";
    common::MiniobLineReader &line_reader = common::MiniobLineReader::instance();
    line_reader.init(history_path);
    g_active_shell = this;
    line_reader.set_completion_callback(complete_dispatch);
    line_reader.set_hint_callback(hint_dispatch);
    bool quit = false;
    sql_buffer_.clear();
    while (!quit) {
      const tui::Capabilities terminal = tui::Capabilities::detect(STDOUT_FILENO, !options_.no_color);
      const tui::Style style(terminal.color());
      const string database = options_.database.empty() ? "(none)" : options_.database;
      const string symbol = terminal.unicode() ? "❯" : ">";
      string prompt = sql_buffer_.empty()
          ? style.magenta("csudb") + " " + style.purple("[" + database + "]") + " " + style.cyan(symbol) + " "
          : style.dim("    ->") + " ";
      string line = line_reader.my_readline(prompt, false);
      if (line_reader.eof()) break;
      if (line == "interrupted") { sql_buffer_.clear(); cout << "^C\n"; continue; }
      if (sql_buffer_.empty() && !trim(line).empty() && (trim(line)[0] == '/' || trim(line)[0] == '\\')) {
        dispatch_meta(trim(line), quit);
        continue;
      }
      if (trim(line).empty()) continue;
      if (!sql_buffer_.empty()) sql_buffer_.push_back('\n');
      sql_buffer_ += line;
      if (!complete_sql(sql_buffer_)) continue;
      string history_entry = trim(sql_buffer_);
      if (!sensitive_sql(history_entry)) { line_reader.add_history(history_entry); session_history_.push_back(history_entry); }
      execute(sql_buffer_);
      sql_buffer_.clear();
    }
    g_active_shell = nullptr;
    return 0;
  }

/**
 * @brief 外壳持有的状态
 * @details output_file_ 是 /output 的目标文件，session_history_ 是本会话历史，
 * sql_buffer_ 是多行输入的累积缓冲。
 */
  ClientOptions options_;
  NativeConnection connection_;
  std::ofstream output_file_;
  vector<string> session_history_;
  string         sql_buffer_;
};

/**
 * @brief replxx 补全的统一入口
 * @param input 当前整行输入
 * @param context_length 输出参数，需替换的上下文长度
 * @return 补全候选列表
 * @details 存在活动 Shell 时转发到 Shell::complete_line（SQL 补全），
 *  否则退化为只补元命令。
 */
replxx::Replxx::completions_t complete_dispatch(const string &input, int &context_length)
{
  if (g_active_shell != nullptr) return g_active_shell->complete_line(input, context_length);
  return complete_meta_command(input, context_length);
}

/**
 * @brief replxx 行内提示的统一入口
 * @param input 当前整行输入
 * @param context_length 输出参数，需替换的上下文长度
 * @param color 输出参数，提示颜色
 * @return 提示文本列表
 * @details 存在活动 Shell 时转发到 Shell::hint_line（AI ghost text），
 *  否则退化为元命令前缀提示。
 */
replxx::Replxx::hints_t hint_dispatch(const string &input, int &context_length, replxx::Replxx::Color &color)
{
  if (g_active_shell != nullptr) return g_active_shell->hint_line(input, context_length, color);
  return hint_meta_command(input, context_length, color);
}

} // namespace

/**
 * @brief 程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 正常时返回 Shell::run 的退出码；参数非法时返回 64（EX_USAGE）
 * @details 先解析选项，失败则打印 usage；成功后把选项移交给 Shell 并运行完整流程。
 */
int main(int argc, char **argv)
{
  ClientOptions options;
  if (!parse_options(argc, argv, options)) { usage(argv[0]); return 64; }
  return Shell(std::move(options)).run();
}
