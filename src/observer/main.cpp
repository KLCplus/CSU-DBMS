
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <termios.h>
#include <filesystem>

#include "common/ini_setting.h"
#include "common/init.h"
#include "common/lang/iostream.h"
#include "common/lang/string.h"
#include "common/lang/map.h"
#include "common/os/process.h"
#include "common/os/signal.h"
#include "common/log/log.h"
#include "common/terminal/terminal_ui.h"
#include "common/version.h"
#include "net/server.h"
#include "net/server_param.h"
#include "service/database_service.h"

using namespace common;

#define NET "NET"

static Server *g_server = nullptr;

bool read_secret(const char *prompt, string &secret)
{
  cerr << prompt << std::flush;
  int input_fd = STDIN_FILENO;
  bool close_input = false;
  if (!isatty(input_fd)) {
    input_fd = open("/dev/tty", O_RDONLY);
    close_input = input_fd >= 0;
  }
  if (input_fd < 0) {
    cerr << endl << "ERROR: no controlling terminal is available for secure password input." << endl;
    return false;
  }

  termios original{};
  bool echo_disabled = isatty(input_fd) && tcgetattr(input_fd, &original) == 0;
  if (echo_disabled) {
    termios protected_input = original;
    protected_input.c_lflag &= ~ECHO;
    if (tcsetattr(input_fd, TCSAFLUSH, &protected_input) != 0) echo_disabled = false;
  }

  secret.clear();
  bool completed = false;
  char character = '\0';
  while (true) {
    ssize_t count = ::read(input_fd, &character, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    if (character == '\n' || character == '\r') {
      completed = true;
      break;
    }
    secret.push_back(character);
  }

  if (echo_disabled) tcsetattr(input_fd, TCSAFLUSH, &original);
  if (close_input) ::close(input_fd);
  cerr << endl;
  return completed;
}

bool choose_initial_root_password(string &password)
{
  const char *password_env = getenv("CSUDB_INITIAL_ROOT_PASSWORD");
  if (password_env != nullptr && password_env[0] != '\0') {
    password = password_env;
    if (password.size() < 8) {
      cerr << "ERROR: CSUDB_INITIAL_ROOT_PASSWORD must contain at least 8 characters." << endl;
      return false;
    }
    return true;
  }

  cout << "Choose the password for the initial root account." << endl;
  for (int attempt = 0; attempt < 3; ++attempt) {
    string confirmation;
    if (!read_secret("Enter root password: ", password) ||
        !read_secret("Confirm root password: ", confirmation)) {
      password.assign(password.size(), '\0');
      confirmation.assign(confirmation.size(), '\0');
      return false;
    }
    if (password.size() < 8) {
      cerr << "Password must contain at least 8 characters." << endl;
    } else if (password != confirmation) {
      cerr << "Passwords do not match." << endl;
    } else {
      confirmation.assign(confirmation.size(), '\0');
      return true;
    }
    password.assign(password.size(), '\0');
    confirmation.assign(confirmation.size(), '\0');
  }
  cerr << "ERROR: password setup failed after 3 attempts." << endl;
  return false;
}

void usage(const char *program)
{
  cout << CSUDB_PRODUCT_NAME << " " << CSUDB_VERSION_STRING << endl;
  cout << "Usage: " << program << " [options]" << endl << endl;
  cout << "  -h, --help                 Show this help" << endl;
  cout << "  -v, --version              Show version" << endl;
  cout << "  -f, --config FILE          Configuration file" << endl;
  cout << "      --initialize           Initialize a new data directory and exit" << endl;
  cout << "      --host HOST            Bind address (default 127.0.0.1)" << endl;
  cout << "  -p, --port PORT            Listen port (default 6789)" << endl;
  cout << "      --data-dir DIR         Override the stable database data directory" << endl;
  cout << "      --log-dir DIR          Log directory" << endl;
  cout << "      --foreground           Run in foreground (default)" << endl;
  cout << "      --no-color             Disable ANSI color" << endl;
  cout << "  -P, --protocol MODE        native, plain, cli, or mysql" << endl;
  cout << "  -s, --socket PATH          Unix socket path" << endl;
  cout << "  -t, --transaction MODEL    vacuous or mvcc" << endl;
  cout << "  -T, --threads MODEL        one-thread-per-connection or java-thread-pool" << endl;
  cout << "  -n, --buffer-size BYTES    Buffer pool capacity" << endl;
  cout << "  -r, --replacement POLICY   lru, lru-k, fifo, or clock" << endl;
  cout << "      --io-backend BACKEND   legacy or positional" << endl;
  cout << "  -d, --durable              Enable disk durability" << endl;
  cout << "  -E, --engine ENGINE        heap" << endl;
  cout << endl;
  cout << "Default data directory: " << the_process_param()->data_dir() << endl;
  cout << "Override precedence: --data-dir > CSUDB_DATA_DIR > XDG_STATE_HOME > ~/.local/state/csudb" << endl;
}

void parse_parameter(int argc, char **argv)
{
  string process_name = get_process_name(argv[0]);

  ProcessParam *process_param = the_process_param();

  process_param->init_default(process_name);
  process_param->set_protocol("native");
#ifdef CSUDB_DEFAULT_CONFIG
  process_param->set_conf(CSUDB_DEFAULT_CONFIG);
#endif

  // Process args
  constexpr int OPT_IO_BACKEND = 1000;
  constexpr int OPT_HOST = 1001;
  constexpr int OPT_DATA_DIR = 1002;
  constexpr int OPT_LOG_DIR = 1003;
  constexpr int OPT_INITIALIZE = 1004;
  constexpr int OPT_FOREGROUND = 1005;
  constexpr int OPT_NO_COLOR = 1006;
  static option long_options[] = {{"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      {"config", required_argument, nullptr, 'f'},
      {"protocol", required_argument, nullptr, 'P'},
      {"port", required_argument, nullptr, 'p'},
      {"socket", required_argument, nullptr, 's'},
      {"transaction", required_argument, nullptr, 't'},
      {"threads", required_argument, nullptr, 'T'},
      {"buffer-size", required_argument, nullptr, 'n'},
      {"replacement", required_argument, nullptr, 'r'},
      {"io-backend", required_argument, nullptr, OPT_IO_BACKEND},
      {"host", required_argument, nullptr, OPT_HOST},
      {"data-dir", required_argument, nullptr, OPT_DATA_DIR},
      {"log-dir", required_argument, nullptr, OPT_LOG_DIR},
      {"initialize", no_argument, nullptr, OPT_INITIALIZE},
      {"foreground", no_argument, nullptr, OPT_FOREGROUND},
      {"no-color", no_argument, nullptr, OPT_NO_COLOR},
      {"durable", no_argument, nullptr, 'd'},
      {"engine", required_argument, nullptr, 'E'},
      {nullptr, 0, nullptr, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "dp:P:s:t:T:f:o:e:E:hvn:r:", long_options, nullptr)) > 0) {
    switch (opt) {
      case 's': process_param->set_unix_socket_path(optarg); break;
      case 'p': process_param->set_server_port(atoi(optarg)); break;
      case 'P': process_param->set_protocol(optarg); break;
      case 'f': process_param->set_conf_explicit(optarg); break;
      case 'o': process_param->set_std_out(optarg); break;
      case 'e': process_param->set_std_err(optarg); break;
      case 't': process_param->set_trx_kit_name(optarg); break;
      case 'E': process_param->set_storage_engine(optarg); break;
      case 'T': process_param->set_thread_handling_name(optarg); break;
      case 'n': process_param->set_buffer_pool_memory_size(atoi(optarg)); break;
      case 'r': process_param->set_buffer_pool_replacement_policy(optarg); break;
      case OPT_IO_BACKEND: process_param->set_page_io_backend(optarg); break;
      case OPT_HOST: process_param->set_listen_host(optarg); break;
      case OPT_DATA_DIR: process_param->set_data_dir(optarg); break;
      case OPT_LOG_DIR: process_param->set_log_dir(optarg); break;
      case OPT_INITIALIZE: process_param->set_initialize(true); break;
      case OPT_FOREGROUND: process_param->set_demon(false); break;
      case OPT_NO_COLOR: setenv("NO_COLOR", "1", 1); break;
      case 'd': process_param->set_durability_mode("disk"); break;
      case 'h':
        usage(argv[0]);
        exit(0);
        return;
      case 'v':
        cout << CSUDB_PRODUCT_NAME << " " << CSUDB_VERSION_STRING << endl;
        exit(0);
        return;
      default: cout << "Unknown option: " << static_cast<char>(opt) << ", ignored" << endl; break;
    }
  }
}

Server *init_server()
{
  map<string, string> net_section = get_properties()->get(NET);

  ProcessParam *process_param = the_process_param();

  long listen_addr        = ntohl(inet_addr("127.0.0.1"));
  long max_connection_num = MAX_CONNECTION_NUM_DEFAULT;
  int  port               = PORT_DEFAULT;

  map<string, string>::iterator it = net_section.find(CLIENT_ADDRESS);
  if (it != net_section.end()) {
    string str = it->second;
    str_to_val(str, listen_addr);
  }

  if (!process_param->listen_host().empty()) {
    in_addr parsed{};
    if (inet_pton(AF_INET, process_param->listen_host().c_str(), &parsed) != 1) {
      cerr << "Invalid IPv4 bind address: " << process_param->listen_host() << endl;
      return nullptr;
    }
    listen_addr = ntohl(parsed.s_addr);
  }

  it = net_section.find(MAX_CONNECTION_NUM);
  if (it != net_section.end()) {
    string str = it->second;
    str_to_val(str, max_connection_num);
  }

  if (process_param->get_server_port() > 0) {
    port = process_param->get_server_port();
    LOG_INFO("Use port config in command line: %d", port);
  } else {
    it = net_section.find(PORT);
    if (it != net_section.end()) {
      string str = it->second;
      str_to_val(str, port);
    }
  }

  ServerParam server_param;
  server_param.listen_addr        = listen_addr;
  server_param.max_connection_num = max_connection_num;
  server_param.port               = port;
  if (0 == strcasecmp(process_param->get_protocol().c_str(), "mysql")) {
    server_param.protocol = CommunicateProtocol::MYSQL;
  } else if (0 == strcasecmp(process_param->get_protocol().c_str(), "cli")) {
    server_param.use_std_io = true;
    server_param.protocol   = CommunicateProtocol::CLI;
  } else if (0 == strcasecmp(process_param->get_protocol().c_str(), "plain")) {
    server_param.protocol = CommunicateProtocol::PLAIN;
  } else {
    server_param.protocol = CommunicateProtocol::NATIVE;
  }

  if (process_param->get_unix_socket_path().size() > 0 && !server_param.use_std_io) {
    server_param.use_unix_socket  = true;
    server_param.unix_socket_path = process_param->get_unix_socket_path();
  }
  server_param.thread_handling = process_param->thread_handling_name();

  Server *server = nullptr;
  if (server_param.use_std_io) {
    server = new CliServer(server_param);
  } else {
    server = new NetServer(server_param);
  }

  return server;
}

/**
 * 如果收到terminal信号的时候，正在处理某些事情，比如打日志，并且拿着日志的锁
 * 那么直接在signal_handler里面处理的话，可能会导致死锁
 * 所以这里单独创建一个线程
 */
void *quit_thread_func(void *_signum)
{
  intptr_t signum = (intptr_t)_signum;
  LOG_INFO("Receive signal: %ld", signum);
  if (g_server) {
    g_server->shutdown();
  }
  return nullptr;
}
void quit_signal_handle(int signum)
{
  // 防止多次调用退出
  // 其实正确的处理是，应该全局性的控制来防止出现“多次”退出的状态，包括发起信号
  // 退出与进程主动退出
  set_signal_handler(nullptr);

  pthread_t tid;
  pthread_create(&tid, nullptr, quit_thread_func, (void *)(intptr_t)signum);
}

namespace tui = common::terminal;

void print_centered(const tui::Style &style, const string &line, int width, bool brand = false)
{
  const int padding = std::max(0, (width - tui::display_width(line)) / 2);
  cout << string(static_cast<size_t>(padding), ' ') << (brand ? style.magenta(line) : style.cyan(line)) << '\n';
}

void print_server_logo(const tui::Capabilities &terminal, const tui::Style &style)
{
  const int width = terminal.content_width(112, 1);
  if (terminal.layout() == tui::LayoutMode::MINIMAL || !terminal.unicode()) {
    cout << style.bold(style.magenta("CSUDB 2026")) << '\n' << style.dim("Database Server") << "\n\n";
    return;
  }
  if (terminal.layout() == tui::LayoutMode::COMPACT) {
    print_centered(style, "CSUDB 2026", width, true);
    print_centered(style, "Database Server", width);
    cout << '\n';
    return;
  }

  static const char *logo[] = {
      "  ██████╗███████╗██╗   ██╗██████╗ ██████╗ ",
      " ██╔════╝██╔════╝██║   ██║██╔══██╗██╔══██╗",
      " ██║     ███████╗██║   ██║██║  ██║██████╔╝",
      " ██║     ╚════██║██║   ██║██║  ██║██╔══██╗",
      " ╚██████╗███████║╚██████╔╝██████╔╝██████╔╝",
      "  ╚═════╝╚══════╝ ╚═════╝ ╚═════╝ ╚═════╝ "};
  for (const char *line : logo) print_centered(style, line, width, true);
  print_centered(style, "CSUDB 2026 · DATABASE SERVER", width);
  cout << '\n';
}

void print_runtime_panel(const tui::Capabilities &terminal, const tui::Style &style)
{
  ProcessParam *parameter = the_process_param();
  const int port = parameter->get_server_port() > 0 ? parameter->get_server_port() : PORT_DEFAULT;
  const int panel_width = terminal.content_width(76, 1);
  const int inner = panel_width - 2;
  const bool unicode = terminal.unicode();
  const string horizontal = tui::repeat(unicode ? "─" : "-", inner);
  const string top = unicode ? "┌" + horizontal + "┐" : "+" + horizontal + "+";
  const string bottom = unicode ? "└" + horizontal + "┘" : "+" + horizontal + "+";
  const string vertical = unicode ? "│" : "|";
  cout << style.cyan(top) << '\n';
  auto row = [&](const string &label, const string &value, bool starting = false) {
    const int value_width = std::max(1, inner - 18);
    const string rendered = tui::truncate_middle(value, static_cast<size_t>(value_width));
    cout << style.cyan(vertical) << ' ' << style.cyan(tui::truncate_middle(label, 15))
         << string(static_cast<size_t>(std::max(1, 16 - static_cast<int>(label.size()))), ' ');
    cout << (starting ? style.yellow(rendered) : style.white(rendered))
         << string(static_cast<size_t>(std::max(0, value_width - static_cast<int>(rendered.size()))), ' ')
         << ' ' << style.cyan(vertical) << '\n';
  };
  const string title = " Runtime Information ";
  cout << style.cyan(unicode ? "├─" : "+-") << style.bold(style.cyan(title))
       << style.cyan(tui::repeat(unicode ? "─" : "-", inner - static_cast<int>(title.size()) - 1))
       << style.cyan(unicode ? "┤" : "+") << '\n';
  row("Mode", parameter->initialize() ? "Data Directory Initialization" : "Database Server");
  row("Protocol", parameter->get_protocol());
  row("Listen", parameter->listen_host() + ":" + std::to_string(port));
  row("Data directory", tui::display_path(parameter->data_dir(), static_cast<size_t>(std::max(1, inner - 18))));
  row("Status", "STARTING", true);
  cout << style.cyan(bottom) << "\n\n";
}

void print_startup_step(const string &description)
{
  const tui::Capabilities terminal = tui::Capabilities::detect(STDOUT_FILENO);
  const tui::Style style(terminal.color());
  const string mark = terminal.unicode() ? "✓" : "[OK]";
  const string timestamp = "[" + tui::current_time_hms() + "]";
  const int available = std::max(8, terminal.width() - tui::display_width(timestamp) -
      tui::display_width(mark) - 3);
  cout << style.dim(timestamp) << "  " << style.green(mark) << ' '
       << style.white(tui::truncate_middle(description, static_cast<size_t>(available))) << '\n';
}

void print_startup_screen()
{
  const tui::Capabilities terminal = tui::Capabilities::detect(STDOUT_FILENO);
  const tui::Style style(terminal.color());
  const string dot = terminal.unicode() ? "●" : "*";
  const int width = terminal.content_width(112, 1);
  cout << style.green(dot) << ' ' << style.bold(style.magenta("CSUDB Server"));
  if (width >= 66) {
    cout << style.dim("  Database Engine Runtime")
         << string(static_cast<size_t>(std::max(1, width - 58)), ' ');
  }
  cout << ' ' << style.purple("v" CSUDB_VERSION_STRING) << "\n\n";
  print_server_logo(terminal, style);
  print_runtime_panel(terminal, style);
}

void print_ready_screen()
{
  ProcessParam *parameter = the_process_param();
  if (strcasecmp(parameter->get_protocol().c_str(), "cli") == 0) return;
  const tui::Capabilities terminal = tui::Capabilities::detect(STDOUT_FILENO);
  const tui::Style style(terminal.color());
  const string mark = terminal.unicode() ? "✓" : "[OK]";
  const int port = parameter->get_server_port() > 0 ? parameter->get_server_port() : PORT_DEFAULT;
  const string host = parameter->listen_host() == "0.0.0.0" ? "127.0.0.1" : parameter->listen_host();
  print_startup_step("TCP listener active on " + parameter->listen_host() + ':' + std::to_string(port));
  cout << '\n' << style.bold(style.green(mark + " Ready for client connections.")) << "\n\n";
  cout << style.cyan("Quick Connect") << "\n\n  "
       << style.magenta("csudb") << " -h " << style.cyan(host) << " -P " << style.cyan(std::to_string(port))
       << " -u root -p\n\n" << style.dim("Ctrl+C to shutdown.") << "\n\n" << std::flush;
}

int main(int argc, char **argv)
{
  int rc = STATUS_SUCCESS;

  set_signal_handler(quit_signal_handle);

  parse_parameter(argc, argv);

  print_startup_screen();

#ifdef CSUDB_PRODUCT_SERVER
  if (!the_process_param()->initialize()) {
    const std::filesystem::path catalog_path =
        std::filesystem::path(the_process_param()->data_dir()) / "system" / "catalog.json";
    if (!std::filesystem::exists(catalog_path)) {
      cerr << "ERROR: CSUDB data directory is not initialized." << endl
           << "Data directory: " << the_process_param()->data_dir() << endl
           << "Run 'csudbd --initialize' once, then start the server with 'csudbd'." << endl;
      return 1;
    }
  }
#endif

  rc = init(the_process_param());
  if (rc != STATUS_SUCCESS) {
    cerr << "Shutdown due to failed to init!" << endl;
    cleanup();
    return rc;
  }
  print_startup_step("Runtime and configuration initialized");

  string temporary_root_password;
  bool catalog_created = false;
  if (the_process_param()->initialize()) {
    string root_password;
    if (!choose_initial_root_password(root_password)) {
      cleanup();
      return 1;
    }
    string effective_password;
    RC init_rc = DatabaseService::initialize_new(the_process_param()->data_dir(), root_password, effective_password);
    root_password.assign(root_password.size(), '\0');
    if (init_rc == RC::FILE_EXIST) {
      cerr << "ERROR: CSUDB data directory is already initialized." << endl;
      cleanup();
      return 1;
    }
    if (OB_FAIL(init_rc)) {
      cerr << "ERROR: failed to initialize system catalog: " << strrc(init_rc) << endl;
      cleanup();
      return 1;
    }
    cout << "Initializing CSUDB data directory..." << endl
         << "Data directory : " << the_process_param()->data_dir() << endl
         << "System catalog : created" << endl
         << "Root user      : created" << endl
         << "Root password  : configured by user" << endl
         << "Initialization complete." << endl;
    effective_password.assign(effective_password.size(), '\0');
    cleanup();
    return 0;
  }

  RC service_rc = DatabaseService::initialize(the_process_param()->data_dir(), temporary_root_password, catalog_created);
  if (OB_FAIL(service_rc)) {
    cerr << "ERROR: failed to initialize DatabaseService: " << strrc(service_rc) << endl;
    cleanup();
    return 1;
  }
  if (catalog_created) {
    cerr << "ERROR: implicit catalog initialization is disabled; run 'csudbd --initialize'." << endl;
    cleanup();
    return 1;
  }
  print_startup_step("System catalog and storage engine loaded");

  g_server = init_server();
  if (g_server == nullptr) {
    cleanup();
    return 1;
  }
  print_startup_step("Network runtime initialized");
  g_server->set_ready_callback(print_ready_screen);
  g_server->serve();

  LOG_INFO("Server stopped");

  cleanup();

  delete g_server;
  return 0;
}
