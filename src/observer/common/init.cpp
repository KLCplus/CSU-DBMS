/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/5/3.
//

#include "common/init.h"

#include "common/conf/ini.h"
#include "common/lang/string.h"
#include "common/lang/iostream.h"
#include "common/log/log.h"
#include "common/os/path.h"
#include "common/os/pidfile.h"
#include "common/os/process.h"
#include "common/os/signal.h"
#include "global_context.h"
#include "session/session.h"
#include "session/session_stage.h"
#include "sql/plan_cache/plan_cache_stage.h"
#include "storage/os/buffer/disk_buffer_pool.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"

#include <filesystem>

using namespace common;

bool *&_get_init()
{
  static bool  util_init   = false;
  static bool *util_init_p = &util_init;
  return util_init_p;
}

bool get_init() { return *_get_init(); }

void set_init(bool value) { *_get_init() = value; }

void sig_handler(int sig)
{
  // Signal handler will be add in the next step.
  //  Add action to shutdown

  LOG_INFO("Receive one signal of %d.", sig);
}

int init_log(ProcessParam *process_cfg, Ini &properties)
{
  const string &proc_name = process_cfg->get_process_name();
  try {
    // we had better alloc one lock to do so, but simplify the logic
    if (g_log) {
      return 0;
    }

    auto log_context_getter = []() { return reinterpret_cast<intptr_t>(Session::current_session()); };

    const string        log_section_name = "LOG";
    map<string, string> log_section      = properties.get(log_section_name);

    string log_file_name;

    // get log file name
    string key = "LOG_FILE_NAME";

    map<string, string>::iterator it = log_section.find(key);
    if (it == log_section.end()) {
      log_file_name = proc_name + ".log";
      cout << "Not set log file name, use default " << log_file_name << endl;
    } else {
      log_file_name = it->second;
    }

    if (!process_cfg->log_dir().empty()) {
      std::error_code ec;
      std::filesystem::create_directories(process_cfg->log_dir(), ec);
      if (ec) return ec.value();
      log_file_name = (std::filesystem::path(process_cfg->log_dir()) / (proc_name + ".log")).string();
    }

    log_file_name = getAboslutPath(log_file_name.c_str());

    LOG_LEVEL log_level = LOG_LEVEL_INFO;
    key                 = ("LOG_FILE_LEVEL");
    it                  = log_section.find(key);
    if (it != log_section.end()) {
      int log = (int)log_level;
      str_to_val(it->second, log);
      log_level = (LOG_LEVEL)log;
    }

    LOG_LEVEL console_level = LOG_LEVEL_INFO;
    key                     = ("LOG_CONSOLE_LEVEL");
    it                      = log_section.find(key);
    if (it != log_section.end()) {
      int log = (int)console_level;
      str_to_val(it->second, log);
      console_level = (LOG_LEVEL)log;
    }

    LoggerFactory::init_default(log_file_name, log_level, console_level);
    g_log->set_context_getter(log_context_getter);

    key = ("DefaultLogModules");
    it  = log_section.find(key);
    if (it != log_section.end()) {
      g_log->set_default_module(it->second);
    }

    if (process_cfg->is_demon()) {
      sys_log_redirect(log_file_name.c_str(), log_file_name.c_str());
    }

    return 0;
  } catch (exception &e) {
    cerr << "Failed to init log for " << proc_name << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
    return errno;
  }

  return 0;
}

void cleanup_log()
{

  if (g_log) {
    delete g_log;
    g_log = nullptr;
  }
}

int prepare_init_seda()
{
  return 0;
}

int init_global_objects(ProcessParam *process_param, Ini &properties)
{
  GCTX.handler_ = new DefaultHandler();

  int ret = 0;

  int buffer_pool_memory_size = process_param->buffer_pool_memory_size();
  string replacement_policy = process_param->buffer_pool_replacement_policy();
  string io_backend = process_param->page_io_backend();
  map<string, string> buffer_pool_section = properties.get("BUFFER_POOL");
  auto config = buffer_pool_section.find("BUFFER_SIZE");
  if (buffer_pool_memory_size <= 0 && config != buffer_pool_section.end()) {
    str_to_val(config->second, buffer_pool_memory_size);
  }
  config = buffer_pool_section.find("REPLACEMENT_POLICY");
  if (!process_param->buffer_pool_replacement_policy_specified() && config != buffer_pool_section.end()) {
    replacement_policy = config->second;
  }
  config = buffer_pool_section.find("IO_BACKEND");
  if (!process_param->page_io_backend_specified() && config != buffer_pool_section.end()) {
    io_backend = config->second;
  }

  RC rc = GCTX.handler_->init(process_param->data_dir().c_str(),
                              process_param->trx_kit_name().c_str(),
                              process_param->durability_mode().c_str(),
                              process_param->storage_engine().c_str(),
                              buffer_pool_memory_size,
                              replacement_policy.c_str(),
                              io_backend.c_str());
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to init handler. rc=%s", strrc(rc));
    return -1;
  }
  return ret;
}

int uninit_global_objects()
{
  delete GCTX.handler_;
  GCTX.handler_ = nullptr;

  return 0;
}

int init(ProcessParam *process_param)
{
  if (get_init()) {
    return 0;
  }

  set_init(true);

  // Run as daemon if daemonization requested
  int rc = STATUS_SUCCESS;
  if (process_param->is_demon()) {
    rc = daemonize_service(process_param->get_std_out().c_str(), process_param->get_std_err().c_str());
    if (rc != 0) {
      cerr << "Shutdown due to failed to daemon current process!" << endl;
      return rc;
    }
  }

  writePidFile(process_param->get_process_name().c_str());

  // Initialize global variables before enter multi-thread mode
  // to avoid race condition

  // Read Configuration files
  const string &configuration_file = process_param->get_conf();
  if (!configuration_file.empty() && std::filesystem::exists(configuration_file)) {
    rc = get_properties()->load(configuration_file);
    if (rc) {
      cerr << "Failed to load configuration file: " << configuration_file << endl;
      return rc;
    }
  } else if (process_param->conf_specified()) {
    cerr << "Configuration file does not exist: " << configuration_file << endl;
    return ENOENT;
  } else {
    cerr << "Warning: default configuration file is unavailable; using built-in defaults." << endl;
  }

  // Init tracer
  rc = init_log(process_param, *get_properties());
  if (rc) {
    cerr << "Failed to init Log" << endl;
    return rc;
  }

  string conf_data;
  get_properties()->to_string(conf_data);
  LOG_INFO("Output configuration \n%s", conf_data.c_str());

  rc = init_global_objects(process_param, *get_properties());
  if (rc != 0) {
    LOG_ERROR("failed to init global objects");
    return rc;
  }

  // Block interrupt signals before creating child threads.
  // setSignalHandler(sig_handler);
  // sigset_t newSigset, oset;
  // blockDefaultSignals(&newSigset, &oset);
  //  wait interrupt signals
  // startWaitForSignals(&newSigset);

  LOG_INFO("Successfully init utility");

  return STATUS_SUCCESS;
}

void cleanup_util()
{
  uninit_global_objects();

  if (nullptr != get_properties()) {
    delete get_properties();
    get_properties() = nullptr;
  }

  LOG_INFO("Shutdown Cleanly!");

  // Finalize tracer
  cleanup_log();

  set_init(false);
}

void cleanup() { cleanup_util(); }
