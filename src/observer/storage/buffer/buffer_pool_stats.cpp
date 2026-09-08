/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/buffer/buffer_pool_stats.h"

#include <strings.h>

#include "common/lang/sstream.h"

const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy)
{
  switch (policy) {
    case BufferPoolReplacementPolicy::LRU: return "LRU";
    case BufferPoolReplacementPolicy::FIFO: return "FIFO";
  }
  return "UNKNOWN";
}

bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy)
{
  if (0 == strcasecmp(name.c_str(), "lru")) {
    policy = BufferPoolReplacementPolicy::LRU;
    return true;
  }
  if (0 == strcasecmp(name.c_str(), "fifo")) {
    policy = BufferPoolReplacementPolicy::FIFO;
    return true;
  }
  return false;
}

double BufferPoolStatsSnapshot::hit_rate() const
{
  return page_requests == 0 ? 0.0 : static_cast<double>(cache_hits) / static_cast<double>(page_requests);
}

string BufferPoolStatsSnapshot::to_string() const
{
  stringstream ss;
  ss << "requests=" << page_requests << ",hits=" << cache_hits << ",misses=" << cache_misses
     << ",hit_rate=" << hit_rate() << ",disk_reads=" << disk_reads << ",disk_writes=" << disk_writes
     << ",evictions=" << evictions << ",dirty_evictions=" << dirty_evictions << ",flushes=" << flushes
     << ",allocations=" << page_allocations << ",disposals=" << page_disposals;
  return ss.str();
}

void BufferPoolStats::record_page_request(bool hit)
{
  page_requests_.fetch_add(1, std::memory_order_relaxed);
  if (hit) {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
  } else {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
  }
}

void BufferPoolStats::record_disk_read() { disk_reads_.fetch_add(1, std::memory_order_relaxed); }
void BufferPoolStats::record_disk_write() { disk_writes_.fetch_add(1, std::memory_order_relaxed); }

void BufferPoolStats::record_eviction(bool dirty)
{
  evictions_.fetch_add(1, std::memory_order_relaxed);
  if (dirty) {
    dirty_evictions_.fetch_add(1, std::memory_order_relaxed);
  }
}

void BufferPoolStats::record_flush() { flushes_.fetch_add(1, std::memory_order_relaxed); }
void BufferPoolStats::record_page_allocation() { page_allocations_.fetch_add(1, std::memory_order_relaxed); }
void BufferPoolStats::record_page_disposal() { page_disposals_.fetch_add(1, std::memory_order_relaxed); }

BufferPoolStatsSnapshot BufferPoolStats::snapshot() const
{
  BufferPoolStatsSnapshot result;
  result.page_requests    = page_requests_.load(std::memory_order_relaxed);
  result.cache_hits       = cache_hits_.load(std::memory_order_relaxed);
  result.cache_misses     = cache_misses_.load(std::memory_order_relaxed);
  result.disk_reads       = disk_reads_.load(std::memory_order_relaxed);
  result.disk_writes      = disk_writes_.load(std::memory_order_relaxed);
  result.evictions        = evictions_.load(std::memory_order_relaxed);
  result.dirty_evictions  = dirty_evictions_.load(std::memory_order_relaxed);
  result.flushes          = flushes_.load(std::memory_order_relaxed);
  result.page_allocations = page_allocations_.load(std::memory_order_relaxed);
  result.page_disposals   = page_disposals_.load(std::memory_order_relaxed);
  return result;
}

void BufferPoolStats::reset()
{
  page_requests_.store(0, std::memory_order_relaxed);
  cache_hits_.store(0, std::memory_order_relaxed);
  cache_misses_.store(0, std::memory_order_relaxed);
  disk_reads_.store(0, std::memory_order_relaxed);
  disk_writes_.store(0, std::memory_order_relaxed);
  evictions_.store(0, std::memory_order_relaxed);
  dirty_evictions_.store(0, std::memory_order_relaxed);
  flushes_.store(0, std::memory_order_relaxed);
  page_allocations_.store(0, std::memory_order_relaxed);
  page_disposals_.store(0, std::memory_order_relaxed);
}
