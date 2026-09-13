#pragma once

#include <string>

struct WebConsoleOptions
{
  std::string database_host;
  int database_port = 6789;
  int web_port = 8765;
  bool open_browser = true;
};

/**
 * Start/inspect/stop the local-only CSUDB Web Console helper.
 *
 * The helper never receives database credentials on its command line. Login is
 * performed in the browser and retained in an in-memory, HttpOnly session.
 */
bool start_web_console(const WebConsoleOptions &options, std::string &message);
bool stop_web_console(std::string &message);
bool web_console_status(std::string &message);
