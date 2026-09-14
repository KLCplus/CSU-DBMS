#include "common/terminal/terminal_ui.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace common::terminal {
namespace {

int environment_dimension(const char *override_name, const char *fallback_name, int fallback)
{
  const char *value = std::getenv(override_name);
  if (value == nullptr || *value == '\0') value = std::getenv(fallback_name);
  if (value == nullptr || *value == '\0') return fallback;
  char *end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  return end != value && *end == '\0' && parsed >= 20 && parsed <= 10000 ? static_cast<int>(parsed) : fallback;
}

bool utf8_locale()
{
  const char *locale = std::getenv("LC_ALL");
  if (locale == nullptr || *locale == '\0') locale = std::getenv("LC_CTYPE");
  if (locale == nullptr || *locale == '\0') locale = std::getenv("LANG");
  if (locale == nullptr) return false;
  std::string normalized(locale);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return normalized.find("utf-8") != std::string::npos || normalized.find("utf8") != std::string::npos;
}

}  // namespace

Capabilities Capabilities::detect(int fd, bool allow_color)
{
  Capabilities capabilities;
  capabilities.tty_ = ::isatty(fd) != 0;

  const char *width_override = std::getenv("CSUDB_TERM_WIDTH");
  const char *height_override = std::getenv("CSUDB_TERM_HEIGHT");
  winsize size{};
  if (capabilities.tty_ && ioctl(fd, TIOCGWINSZ, &size) == 0) {
    if (size.ws_col >= 20) capabilities.width_ = size.ws_col;
    if (size.ws_row >= 2) capabilities.height_ = size.ws_row;
  } else {
    capabilities.width_ = environment_dimension("COLUMNS", "COLUMNS", capabilities.width_);
    capabilities.height_ = environment_dimension("LINES", "LINES", capabilities.height_);
  }
  if (width_override != nullptr && *width_override != '\0') {
    capabilities.width_ = environment_dimension("CSUDB_TERM_WIDTH", "CSUDB_TERM_WIDTH", capabilities.width_);
  }
  if (height_override != nullptr && *height_override != '\0') {
    capabilities.height_ = environment_dimension("CSUDB_TERM_HEIGHT", "CSUDB_TERM_HEIGHT", capabilities.height_);
  }

  const char *term = std::getenv("TERM");
  const bool dumb_terminal = term != nullptr && std::string(term) == "dumb";
  capabilities.color_ = allow_color && capabilities.tty_ && std::getenv("NO_COLOR") == nullptr && !dumb_terminal;
  capabilities.unicode_ = utf8_locale();
  return capabilities;
}

LayoutMode Capabilities::layout() const
{
  if (width_ >= 120) return LayoutMode::FULL;
  if (width_ >= 80) return LayoutMode::NORMAL;
  if (width_ >= 60) return LayoutMode::COMPACT;
  return LayoutMode::MINIMAL;
}

int Capabilities::content_width(int maximum, int margin) const
{
  return std::max(20, std::min(maximum, width_ - std::max(0, margin) * 2));
}

std::string Style::paint(const char *code, std::string_view text) const
{
  if (!enabled_) return std::string(text);
  return std::string(code) + std::string(text) + "\033[0m";
}

std::string Style::bold(std::string_view text) const { return paint("\033[1m", text); }
std::string Style::cyan(std::string_view text) const { return paint("\033[38;2;73;218;235m", text); }
std::string Style::magenta(std::string_view text) const { return paint("\033[38;2;224;92;255m", text); }
std::string Style::purple(std::string_view text) const { return paint("\033[38;2;155;126;255m", text); }
std::string Style::green(std::string_view text) const { return paint("\033[38;2;82;214;134m", text); }
std::string Style::yellow(std::string_view text) const { return paint("\033[38;2;247;190;76m", text); }
std::string Style::red(std::string_view text) const { return paint("\033[38;2;255;102;117m", text); }
std::string Style::white(std::string_view text) const { return paint("\033[38;2;235;241;248m", text); }
std::string Style::dim(std::string_view text) const { return paint("\033[38;2;120;128;153m", text); }

std::string repeat(std::string_view value, int count)
{
  std::string result;
  if (count <= 0 || value.empty()) return result;
  result.reserve(value.size() * static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) result.append(value);
  return result;
}

int display_width(std::string_view value)
{
  int width = 0;
  for (unsigned char byte : value) {
    if ((byte & 0xc0) != 0x80) ++width;
  }
  return width;
}

std::string truncate_middle(std::string_view value, size_t maximum)
{
  if (value.size() <= maximum) return std::string(value);
  if (maximum <= 3) return std::string(maximum, '.');
  const size_t left = (maximum - 3 + 1) / 2;
  const size_t right = maximum - 3 - left;
  return std::string(value.substr(0, left)) + "..." + std::string(value.substr(value.size() - right));
}

std::string display_path(std::string_view path, size_t maximum)
{
  std::string result(path);
  const char *home = std::getenv("HOME");
  if (home != nullptr) {
    std::string prefix(home);
    if (result == prefix) result = "~";
    else if (result.size() > prefix.size() && result.compare(0, prefix.size(), prefix) == 0 && result[prefix.size()] == '/') {
      result.replace(0, prefix.size(), "~");
    }
  }
  return truncate_middle(result, maximum);
}

std::string current_time_hms()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&time, &local);
  std::ostringstream output;
  output << std::put_time(&local, "%H:%M:%S");
  return output.str();
}

}  // namespace common::terminal
