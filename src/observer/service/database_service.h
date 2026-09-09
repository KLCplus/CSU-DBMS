#pragma once

#include <mutex>

#include "auth/system_catalog.h"
#include "common/lang/string.h"
#include "common/sys/rc.h"
#include "net/sql_task_handler.h"
#include "service/query_result.h"

class Communicator;
class Session;
class SessionEvent;

/**
 * External database boundary shared by native CLI and future adapters.
 * Core SQL always delegates to SqlTaskHandler.
 */
class DatabaseService
{
public:
  static RC initialize(const string &data_dir, string &temporary_root_password, bool &created);
  static RC initialize_new(const string &data_dir, const string &root_password, string &effective_root_password);
  static SystemCatalog &system_catalog();

  RC handle_event(Communicator *communicator);
  QueryResult execute(SessionEvent &event);

private:
  QueryResult login(Session &session, const string &username, const string &password, const string &database);
  QueryResult server_info(Session &session, bool include_frames, size_t limit);
  QueryResult execute_sql(SessionEvent &event);
  QueryResult materialize(SessionEvent &event, RC pipeline_rc);
  QueryResult execute_management(Session &session, const string &sql, bool &handled);
  bool authorize_sql(const Session &session, const string &sql, string &reason) const;
  bool can_admin(const Session &session, const string &privilege) const;

  SqlTaskHandler sql_task_handler_;
};
