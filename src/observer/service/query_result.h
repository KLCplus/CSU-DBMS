#pragma once

#include <cstdint>
#include <utility>

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "common/sys/rc.h"

struct QueryColumn
{
  string name;
  string type;
};

/**
 * Stable value-only response boundary for clients and future SDKs.
 * No executor, tuple, page, frame, or storage pointer crosses this boundary.
 */
class QueryResult
{
public:
  static QueryResult ok(const string &message = "Query OK");
  static QueryResult error(RC rc, const string &message = "");

  bool success = true;
  int error_code = 0;
  string error_name;
  string message;
  vector<QueryColumn> columns;
  vector<vector<string>> rows;
  uint64_t affected_rows = 0;
  bool affected_rows_known = false;
  uint64_t execution_time_us = 0;
  vector<string> warnings;

  // Stable diagnostic metadata used by login/status/buffer responses.
  vector<std::pair<string, string>> attributes;
};
