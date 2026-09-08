/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <atomic>
#include <cstdint>

#include "common/lang/string.h"

enum class BufferPoolReplacementPolicy
{
  LRU,
  FIFO,
};

const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy);
bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy);

struct BufferPoolStatsSnapshot
{
  uint64_t page_requests    = 0;
  uint64_t cache_hits       = 0;
  uint64_t cache_misses     = 0;
  uint64_t disk_reads       = 0;
  uint64_t disk_writes      = 0;
  uint64_t evictions        = 0;
  uint64_t dirty_evictions  = 0;
  uint64_t flushes          = 0;
  uint64_t page_allocations = 0;
  uint64_t page_disposals   = 0;

  double hit_rate() const;
  string to_string() const;
};

/**
 * @brief Buffer Pool 的运行统计
 * @ingroup BufferPool
 * @details 计数器使用原子类型，供课程实验在不改变页面访问语义的前提下采集数据。
 */
class BufferPoolStats
{
public:
  void record_page_request(bool hit);
  void record_disk_read();
  void record_disk_write();
  void record_eviction(bool dirty);
  void record_flush();
  void record_page_allocation();
  void record_page_disposal();

  BufferPoolStatsSnapshot snapshot() const;
  void reset();

private:
  std::atomic<uint64_t> page_requests_{0};
  std::atomic<uint64_t> cache_hits_{0};
  std::atomic<uint64_t> cache_misses_{0};
  std::atomic<uint64_t> disk_reads_{0};
  std::atomic<uint64_t> disk_writes_{0};
  std::atomic<uint64_t> evictions_{0};
  std::atomic<uint64_t> dirty_evictions_{0};
  std::atomic<uint64_t> flushes_{0};
  std::atomic<uint64_t> page_allocations_{0};
  std::atomic<uint64_t> page_disposals_{0};
};
