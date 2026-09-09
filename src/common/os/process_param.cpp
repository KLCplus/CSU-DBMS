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
// Created by Longda on 2010
//

#include "process_param.h"
#include <assert.h>
#include <cstdlib>
#include <filesystem>
namespace common {

namespace {

string stable_data_directory()
{
  const char *configured = std::getenv("CSUDB_DATA_DIR");
  if (configured != nullptr && configured[0] != '\0') {
    return configured;
  }

  const char *xdg_state_home = std::getenv("XDG_STATE_HOME");
  if (xdg_state_home != nullptr && xdg_state_home[0] != '\0') {
    return (std::filesystem::path(xdg_state_home) / "csudb").string();
  }

  const char *user_home = std::getenv("HOME");
  if (user_home != nullptr && user_home[0] != '\0') {
    return (std::filesystem::path(user_home) / ".local" / "state" / "csudb").string();
  }

  return std::filesystem::absolute("csudb_data").string();
}

} // namespace

//! Global process config
ProcessParam *&the_process_param()
{
  static ProcessParam *process_cfg = new ProcessParam();

  return process_cfg;
}

void ProcessParam::init_default(string &process_name)
{
  assert(process_name.empty() == false);
  this->process_name_ = process_name;
  if (std_out_.empty()) {
    std_out_ = "../log/" + process_name + ".out";
  }
  if (std_err_.empty()) {
    std_err_ = "../log/" + process_name + ".err";
  }
  if (conf.empty()) {
    conf = "../etc/" + process_name + ".ini";
  }
  if (data_dir_.empty()) {
    data_dir_ = stable_data_directory();
  }

  demon = false;
}

}  // namespace common
