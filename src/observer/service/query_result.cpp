#include "service/query_result.h"

QueryResult QueryResult::ok(const string &message)
{
  QueryResult result;
  result.message = message;
  return result;
}

QueryResult QueryResult::error(RC rc, const string &message)
{
  QueryResult result;
  result.success = false;
  result.error_code = static_cast<int>(rc);
  result.error_name = strrc(rc);
  result.message = message.empty() ? strrc(rc) : message;
  return result;
}
