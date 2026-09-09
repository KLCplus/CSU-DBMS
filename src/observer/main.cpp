
#include <netinet/in.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>

#include "common/ini_setting.h"
#include "common/init.h"
#include "common/lang/iostream.h"
#include "common/lang/string.h"
#include "common/lang/map.h"
#include "common/os/process.h"
#include "common/os/signal.h"
#include "common/log/log.h"
#include "common/version.h"
#include "net/server.h"
#include "net/server_param.h"
#include "service/database_service.h"

using namespace common;

#define NET "NET"

static Server *g_server = nullptr;

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
  cout << "      --data-dir DIR         Database data directory" << endl;
  cout << "      --log-dir DIR          Log directory" << endl;
  cout << "      --foreground           Run in foreground (default)" << endl;
  cout << "  -P, --protocol MODE        native, plain, cli, or mysql" << endl;
  cout << "  -s, --socket PATH          Unix socket path" << endl;
  cout << "  -t, --transaction MODEL    vacuous or mvcc" << endl;
  cout << "  -T, --threads MODEL        one-thread-per-connection or java-thread-pool" << endl;
  cout << "  -n, --buffer-size BYTES    Buffer pool capacity" << endl;
  cout << "  -r, --replacement POLICY   lru, fifo, or clock" << endl;
  cout << "      --io-backend BACKEND   legacy or positional" << endl;
  cout << "  -d, --durable              Enable disk durability" << endl;
  cout << "  -E, --engine ENGINE        heap or lsm" << endl;
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

void print_startup_screen()
{
  cout << R"(
╭──────────────────────────────────────────────────────────╮
│                       CSUDB 2026                         │
│       Compiler × Database × Operating Systems            │
╰──────────────────────────────────────────────────────────╯
)";

  if (strcasecmp(the_process_param()->get_protocol().c_str(), "cli") == 0) {
    cout << "Ready. Enter SQL directly (a trailing ';' is recommended)." << endl;
    cout << "Type 'help;' for SQL examples; type 'exit' or '\\q' to leave." << endl;
    cout << "Data directory: ./csudb_data/db/sys" << endl << endl;
  } else {
    cout << "CSUDB server is starting. Press Ctrl+C to stop." << endl << endl;
  }
}

int main(int argc, char **argv)
{
  int rc = STATUS_SUCCESS;

  set_signal_handler(quit_signal_handle);

  parse_parameter(argc, argv);

  print_startup_screen();

  rc = init(the_process_param());
  if (rc != STATUS_SUCCESS) {
    cerr << "Shutdown due to failed to init!" << endl;
    cleanup();
    return rc;
  }

  string temporary_root_password;
  bool catalog_created = false;
  if (the_process_param()->initialize()) {
    const char *password_env = getenv("CSUDB_INITIAL_ROOT_PASSWORD");
    string effective_password;
    RC init_rc = DatabaseService::initialize_new(the_process_param()->data_dir(), password_env == nullptr ? "" : password_env, effective_password);
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
         << "Temporary root password:" << endl << effective_password << endl
         << "Initialization complete." << endl;
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
    cout << "CSUDB system catalog was initialized." << endl
         << "Temporary root password: " << temporary_root_password << endl
         << "Change it after the first login." << endl;
  }

  g_server = init_server();
  if (g_server == nullptr) {
    cleanup();
    return 1;
  }
  g_server->serve();

  LOG_INFO("Server stopped");

  cleanup();

  delete g_server;
  return 0;
}
