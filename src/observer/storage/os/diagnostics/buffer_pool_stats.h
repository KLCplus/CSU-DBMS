
#pragma once

#include <atomic>
#include <cstdint>

#include "common/lang/string.h"

enum class FlushReason
{
  EXPLICIT,
  EVICTION,
  SHUTDOWN,
  CHECKPOINT,
  OTHER,
};

const char *flush_reason_name(FlushReason reason);

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
  uint64_t pin_requests             = 0;
  uint64_t unpin_requests           = 0;
  uint64_t no_buffer_failures       = 0;
  uint64_t current_pinned_frames    = 0;
  uint64_t peak_pinned_frames       = 0;
  uint64_t dirty_pages_current      = 0;
  uint64_t peak_dirty_pages         = 0;
  uint64_t bytes_read               = 0;
  uint64_t bytes_written            = 0;
  uint64_t read_latency_ns_total    = 0;
  uint64_t write_latency_ns_total   = 0;
  uint64_t flush_latency_ns_total   = 0;
  uint64_t read_latency_max_ns      = 0;
  uint64_t write_latency_max_ns     = 0;
  uint64_t eviction_flushes         = 0;
  uint64_t explicit_flushes         = 0;
  uint64_t shutdown_flushes         = 0;

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
  void record_disk_read(uint64_t bytes = 0, uint64_t latency_ns = 0);
  void record_disk_write(uint64_t bytes = 0, uint64_t latency_ns = 0);
  void record_eviction(bool dirty);
  void record_flush(FlushReason reason = FlushReason::OTHER, uint64_t latency_ns = 0);
  void record_page_allocation();
  void record_page_disposal();
  void record_pin(bool first_pin);
  void record_unpin(bool last_unpin);
  void record_no_buffer_failure();
  void record_dirty();
  void record_clean();

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
  std::atomic<uint64_t> pin_requests_{0};
  std::atomic<uint64_t> unpin_requests_{0};
  std::atomic<uint64_t> no_buffer_failures_{0};
  std::atomic<uint64_t> current_pinned_frames_{0};
  std::atomic<uint64_t> peak_pinned_frames_{0};
  std::atomic<uint64_t> dirty_pages_current_{0};
  std::atomic<uint64_t> peak_dirty_pages_{0};
  std::atomic<uint64_t> bytes_read_{0};
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> read_latency_ns_total_{0};
  std::atomic<uint64_t> write_latency_ns_total_{0};
  std::atomic<uint64_t> flush_latency_ns_total_{0};
  std::atomic<uint64_t> read_latency_max_ns_{0};
  std::atomic<uint64_t> write_latency_max_ns_{0};
  std::atomic<uint64_t> eviction_flushes_{0};
  std::atomic<uint64_t> explicit_flushes_{0};
  std::atomic<uint64_t> shutdown_flushes_{0};
};
