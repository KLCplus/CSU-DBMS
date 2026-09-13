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

std::string home_directory()
{
  const char *home = std::getenv("HOME");
  return home == nullptr ? "." : home;
}

fs::path state_directory()
{
  return fs::path(home_directory()) / ".csudb";
}

fs::path pid_path()
{
  return state_directory() / "web.pid";
}

fs::path log_path()
{
  return state_directory() / "web.log";
}

bool read_pid(pid_t &pid)
{
  std::ifstream input(pid_path());
  long value = 0;
  if (!(input >> value) || value <= 1) return false;
  pid = static_cast<pid_t>(value);
  return true;
}

bool is_web_process(pid_t pid)
{
  if (kill(pid, 0) != 0) return false;
  std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  std::ostringstream command;
  command << input.rdbuf();
  const std::string value = command.str();
  return value.find("csudb-web") != std::string::npos || value.find("csudb_web.py") != std::string::npos;
}

fs::path executable_path()
{
  std::vector<char> buffer(4096);
  const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (count <= 0) return {};
  buffer[static_cast<size_t>(count)] = '\0';
  return fs::path(buffer.data());
}

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

void remove_stale_pid_file()
{
  std::error_code error;
  fs::remove(pid_path(), error);
}

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
