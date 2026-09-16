// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  web_console_launcher.cpp:51 home_directory
//  web_console_launcher.cpp:62 state_directory
//  web_console_launcher.cpp:72 pid_path
//  web_console_launcher.cpp:82 log_path
//  web_console_launcher.cpp:93 read_pid
//  web_console_launcher.cpp:109 is_web_process
//  web_console_launcher.cpp:124 executable_path
//  web_console_launcher.cpp:139 find_web_script
//  web_console_launcher.cpp:161 remove_stale_pid_file
//  web_console_launcher.cpp:174 ensure_state_directory
//  web_console_launcher.cpp:193 open_browser
//  web_console_launcher.cpp:215 web_console_status
//  web_console_launcher.cpp:238 start_web_console
//  web_console_launcher.cpp:318 stop_web_console
// ------------------------------------------------------------------------------------------------
/**
 * @file web_console_launcher.cpp
 * @brief 从 CLI 启动与停止本地 Web 控制台进程
 * @details 负责把 csudb-web 这个 Python 助手拉起来、记录 pid、探活与终止。
 * 进程识别不只比对 pid，还要读 /proc 确认命令行，避免 pid 被复用后误杀无关进程。
 */
#include "web_console_launcher.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/**
 * @brief 返回用户主目录
 * @return HOME 环境变量的值；未设置时返回 "."（当前目录）
 * @details 读取 HOME，空指针时退回当前目录，保证状态目录总有可写落点。
 */
std::string home_directory()
{
  const char *home = std::getenv("HOME");
  return home == nullptr ? "." : home;
}

/**
 * @brief 返回状态目录，固定为 ~/.csudb
 * @return 主目录下的 .csudb 路径
 * @details 用 std::filesystem 拼接主目录与固定子目录名。
 */
fs::path state_directory()
{
  return fs::path(home_directory()) / ".csudb";
}

/**
 * @brief 返回记录 Web 进程号的 pid 文件路径
 * @return 状态目录下的 web.pid 路径
 * @details 由状态目录派生，随主目录变化自动定位。
 */
fs::path pid_path()
{
  return state_directory() / "web.pid";
}

/**
 * @brief 返回 Web 控制台的日志文件路径
 * @return 状态目录下的 web.log 路径
 * @details Web 子进程的标准输出与错误都会被重定向到这个文件。
 */
fs::path log_path()
{
  return state_directory() / "web.log";
}

/**
 * @brief 从 pid 文件读出进程号
 * @details 读不到或读到不大于 1 的值都视为无效，避免把 init 进程当成目标。
 * @param pid 输出参数，读取到的进程号
 * @return 读到一个大于 1 的合法 pid 返回 true，否则返回 false
 */
bool read_pid(pid_t &pid)
{
  std::ifstream input(pid_path());
  long value = 0;
  if (!(input >> value) || value <= 1) return false;
  pid = static_cast<pid_t>(value);
  return true;
}

/**
 * @brief 判断给定 pid 是否真的是 CSUDB Web 进程
 * @details 先确认进程存在，再读 /proc 下的命令行，只有包含 csudb-web 或
 * csudb_web.py 才算命中。系统会复用 pid，只比对号码可能误伤无关进程。
 * @param pid 待检查的进程号
 * @return 进程存在且命令行匹配 Web 助手返回 true，否则返回 false
 */
bool is_web_process(pid_t pid)
{
  if (kill(pid, 0) != 0) return false;
  std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  std::ostringstream command;
  command << input.rdbuf();
  const std::string value = command.str();
  return value.find("csudb-web") != std::string::npos || value.find("csudb_web.py") != std::string::npos;
}

/**
 * @brief 通过 /proc/self/exe 取得当前可执行文件的绝对路径
 * @return 可执行文件路径；readlink 失败时返回空路径
 * @details 读取符号链接内容并补 NUL 结尾，用于相对自身定位助手脚本。
 */
fs::path executable_path()
{
  std::vector<char> buffer(4096);
  const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (count <= 0) return {};
  buffer[static_cast<size_t>(count)] = '\0';
  return fs::path(buffer.data());
}

/**
 * @brief 定位 Web 助手脚本
 * @details 查找顺序是：环境变量显式指定、可执行文件同目录（开发构建）、
 * 上级 libexec 目录（安装后布局）。三处都没有就返回空路径。
 * @return 找到的助手脚本路径；都没命中时返回空路径
 */
fs::path find_web_script()
{
  const char *override_path = std::getenv("CSUDB_WEB_SCRIPT");
  if (override_path != nullptr && fs::is_regular_file(override_path)) return override_path;

  const fs::path executable = executable_path();
  if (!executable.empty()) {
    const fs::path build_script = executable.parent_path() / "csudb-web.py";
    if (fs::is_regular_file(build_script)) return build_script;

    const fs::path installed_script =
        executable.parent_path().parent_path() / "libexec" / "csudb" / "csudb-web";
    if (fs::is_regular_file(installed_script)) return installed_script;
  }
  return {};
}

/**
 * @brief 删除残留的 pid 文件
 * @return 无返回值
 * @details 使用带 error_code 的重载，不存在或失败都静默忽略。
 */
void remove_stale_pid_file()
{
  std::error_code error;
  fs::remove(pid_path(), error);
}

/**
 * @brief 确保状态目录存在并设为仅属主可访问
 * @param message 输出参数，失败时写入具体错误说明
 * @return 创建成功返回 true，失败返回 false
 * @details 用 create_directories 递归创建，再用 chmod S_IRWXU 收紧权限，
 *  避免 pid 与日志等状态被其他用户读取。
 */
bool ensure_state_directory(std::string &message)
{
  std::error_code error;
  fs::create_directories(state_directory(), error);
  if (error) {
    message = "cannot create " + state_directory().string() + ": " + error.message();
    return false;
  }
  chmod(state_directory().c_str(), S_IRWXU);
  return true;
}

/**
 * @brief 用系统默认浏览器打开地址
 * @details fork 出子进程后把标准输出与错误重定向到 /dev/null，
 * 再替换成 xdg-open，避免浏览器输出污染 CLI 的终端。
 * @param url 要打开的地址，例如 127.0.0.1:8765 的本地地址
 * @return 无返回值；父进程 fork 后立即返回，不等待浏览器
 */
void open_browser(const std::string &url)
{
  const pid_t pid = fork();
  if (pid != 0) return;
  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
  execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
  _exit(127);
}

} // namespace

/**
 * @brief 查询 Web 控制台是否在运行
 * @details 进程不存在或 pid 已对不上时顺手清掉残留的 pid 文件。
 * @param message 输出参数，运行时写入 pid 与日志路径，否则写入未运行提示
 * @return 正在运行返回 true，并把 pid 与日志路径写进 message
 */
bool web_console_status(std::string &message)
{
  pid_t pid = 0;
  if (!read_pid(pid) || !is_web_process(pid)) {
    remove_stale_pid_file();
    message = "CSUDB Web Console is not running.";
    return false;
  }
  message = "CSUDB Web Console is running (pid " + std::to_string(pid) +
            "). Log: " + log_path().string();
  return true;
}

/**
 * @brief 启动 Web 控制台
 * @details 先校验端口与状态目录，已经在跑就直接返回地址。否则 fork 出子进程，
 * 用 setsid 脱离终端，把标准输入接到 /dev/null、输出接到日志文件，
 * 再替换成 python3 运行 Web 助手，并把数据库地址与监听端口作为参数传过去。
 * 父进程轮询最多约一秒，确认 pid 文件出现且进程确实在跑才算启动成功。
 * @param options 数据库地址、Web 端口与是否打开浏览器
 * @param message 输出参数，成功时给出访问地址与日志路径
 * @return 启动成功（或本已在运行）返回 true，端口非法、目录/脚本缺失或启动超时返回 false
 */
bool start_web_console(const WebConsoleOptions &options, std::string &message)
{
  if (options.web_port <= 0 || options.web_port > 65535) {
    message = "web port must be between 1 and 65535";
    return false;
  }
  if (!ensure_state_directory(message)) return false;

  pid_t existing_pid = 0;
  if (read_pid(existing_pid) && is_web_process(existing_pid)) {
    const std::string url = "http://127.0.0.1:" + std::to_string(options.web_port) + "/";
    if (options.open_browser) open_browser(url);
    message = "CSUDB Web Console is already running. Open " + url;
    return true;
  }
  remove_stale_pid_file();

  const fs::path script = find_web_script();
  if (script.empty()) {
    message = "csudb-web helper was not found next to csudb or under libexec/csudb";
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    message = std::string("cannot start Web Console: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    if (setsid() < 0) _exit(126);
    const int log_fd = open(log_path().c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    const int null_fd = open("/dev/null", O_RDONLY);
    if (log_fd < 0 || null_fd < 0) _exit(126);
    dup2(null_fd, STDIN_FILENO);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(null_fd);
    close(log_fd);

    const std::string database_port = std::to_string(options.database_port);
    const std::string web_port = std::to_string(options.web_port);
    execlp("python3",
        "python3",
        script.c_str(),
        "--db-host",
        options.database_host.c_str(),
        "--db-port",
        database_port.c_str(),
        "--listen-host",
        "127.0.0.1",
        "--listen-port",
        web_port.c_str(),
        "--pid-file",
        pid_path().c_str(),
        static_cast<char *>(nullptr));
    _exit(127);
  }

  for (int attempt = 0; attempt < 25; ++attempt) {
    usleep(40000);
    pid_t running_pid = 0;
    if (read_pid(running_pid) && is_web_process(running_pid)) {
      const std::string url = "http://127.0.0.1:" + std::to_string(options.web_port) + "/";
      if (options.open_browser) open_browser(url);
      message = "CSUDB Web Console started at " + url + "\nLog: " + log_path().string();
      return true;
    }
    if (kill(pid, 0) != 0) break;
  }
  message = "Web Console did not start; inspect " + log_path().string();
  return false;
}

/**
 * @brief 停止 Web 控制台
 * @details 先确认 pid 对应的确实是本项目的 Web 进程，再发 SIGTERM，
 * 随后最多等约一秒确认退出，最后清掉 pid 文件。只终止经双重确认的进程。
 * @param message 输出参数，写入停止结果或未运行提示
 * @return 成功发出终止信号返回 true；未运行或 kill 失败返回 false
 */
bool stop_web_console(std::string &message)
{
  pid_t pid = 0;
  if (!read_pid(pid) || !is_web_process(pid)) {
    remove_stale_pid_file();
    message = "CSUDB Web Console is not running.";
    return false;
  }
  if (kill(pid, SIGTERM) != 0) {
    message = std::string("cannot stop Web Console: ") + std::strerror(errno);
    return false;
  }
  for (int attempt = 0; attempt < 25 && kill(pid, 0) == 0; ++attempt) usleep(40000);
  remove_stale_pid_file();
  message = "CSUDB Web Console stopped.";
  return true;
}
