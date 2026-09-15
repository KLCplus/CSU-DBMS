
/**
 * @file buffer_pool_stats.h
 * @brief Buffer Pool 的累计统计与刷盘原因
 * @ingroup BufferPool
 * @details 统计只做累加，不参与任何控制流程，因此可以随时清零重算而不影响运行。
 * 所有计数器都是原子变量，自增使用最宽松的内存序，既保证不丢更新又不引入锁开销。
 */
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

/** @brief 把刷盘原因转成名字，写进 Trace 与统计 */
const char *flush_reason_name(FlushReason reason);

/**
 * @brief 某一时刻的统计值副本
 * @ingroup BufferPool
 * @details 由 BufferPoolStats::snapshot 生成。之所以要这份副本而不是直接读计数器，
 * 是因为调用方需要一组彼此一致的数值，而不是被并发修改穿插过的读数。
 * 字段可分成六组：请求与命中、磁盘 I/O、淘汰与刷新、分配与释放、
 * pin 与脏页的当前值与峰值、延迟累计与最大值。
 */
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

/** @brief 命中率 = 命中次数 / 总请求次数；没有请求时返回 0 避免除零 */
  double hit_rate() const;
/** @brief 把全部指标拼成一行键值对，供进程退出时汇总输出 */
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
/** @brief 记录一次页请求，并计入命中或缺失。命中率就是由这两个计数算出来的 */
  void record_page_request(bool hit);
/** @brief 记录一次真实的读盘，同时累加字节数、总延迟并刷新读延迟峰值 */
  void record_disk_read(uint64_t bytes = 0, uint64_t latency_ns = 0);
/** @brief 记录一次真实的写盘，同时累加字节数、总延迟并刷新写延迟峰值 */
  void record_disk_write(uint64_t bytes = 0, uint64_t latency_ns = 0);
/** @brief 记录一次淘汰；脏页淘汰会额外计入 dirty_evictions */
  void record_eviction(bool dirty);
/** @brief 记录一次刷盘。除总数外还按原因分别计数，便于区分淘汰、显式与关闭三种来源 */
  void record_flush(FlushReason reason = FlushReason::OTHER, uint64_t latency_ns = 0);
/** @brief 记录一次页分配 */
  void record_page_allocation();
/** @brief 记录一次页释放 */
  void record_page_disposal();
/**
 * @brief 记录一次 pin
 * @param first_pin 是否是从 0 变成 1。当前被 pin 的计数单位是页而不是次数，
 * 同一个页被反复 pin 不应该重复累加，所以只有第一次才增加当前值与峰值
 */
  void record_pin(bool first_pin);
/** @brief 记录一次 unpin；last_unpin 为真时递减当前被 pin 的页数 */
  void record_unpin(bool last_unpin);
/** @brief 记录一次取不到 Frame 的失败，对应 BUFFERPOOL_NOBUF */
  void record_no_buffer_failure();
/** @brief 页由干净变为脏，递增脏页数并刷新脏页峰值 */
  void record_dirty();
/** @brief 页由脏变为干净，递减脏页数 */
  void record_clean();

/** @brief 取一份当前统计值的副本 */
  BufferPoolStatsSnapshot snapshot() const;
/** @brief 把累计计数清零，用于分段观测 */
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
