#pragma once

#include <string>
#include <string_view>

namespace common::terminal {

enum class LayoutMode
{
  FULL,
  NORMAL,
  COMPACT,
  MINIMAL,
};

class Capabilities
{
public:
  static Capabilities detect(int fd, bool allow_color = true);

  int width() const { return width_; }
  int height() const { return height_; }
  bool is_tty() const { return tty_; }
  bool color() const { return color_; }
  bool unicode() const { return unicode_; }
  LayoutMode layout() const;
  int content_width(int maximum, int margin = 2) const;

private:
  int  width_ = 80;
  int  height_ = 24;
  bool tty_ = false;
  bool color_ = false;
  bool unicode_ = true;
};

class Style
{
public:
  explicit Style(bool enabled = false) : enabled_(enabled) {}

  bool enabled() const { return enabled_; }
  std::string bold(std::string_view text) const;
  std::string cyan(std::string_view text) const;
  std::string magenta(std::string_view text) const;
  std::string purple(std::string_view text) const;
  std::string green(std::string_view text) const;
  std::string yellow(std::string_view text) const;
  std::string red(std::string_view text) const;
  std::string white(std::string_view text) const;
  std::string dim(std::string_view text) const;

private:
  std::string paint(const char *code, std::string_view text) const;
  bool enabled_ = false;
};

std::string repeat(std::string_view value, int count);
int display_width(std::string_view value);
std::string truncate_middle(std::string_view value, size_t maximum);
std::string display_path(std::string_view path, size_t maximum);
std::string current_time_hms();

}  // namespace common::terminal
