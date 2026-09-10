
#include "storage/os/diagnostics/buffer_pool_stats.h"

#include "common/lang/sstream.h"

namespace {

void update_max(std::atomic<uint64_t> &target, uint64_t value)
{
  uint64_t current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void decrement_if_positive(std::atomic<uint64_t> &counter)
{
  uint64_t current = counter.load(std::memory_order_relaxed);
  while (current > 0 &&
         !counter.compare_exchange_weak(current, current - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

}  // namespace

const char *flush_reason_name(FlushReason reason)
{
  switch (reason) {
    case FlushReason::EXPLICIT: return "EXPLICIT";
    case FlushReason::EVICTION: return "EVICTION";
    case FlushReason::SHUTDOWN: return "SHUTDOWN";
    case FlushReason::CHECKPOINT: return "CHECKPOINT";
    case FlushReason::OTHER: return "OTHER";
  }
  return "OTHER";
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
     << ",allocations=" << page_allocations << ",disposals=" << page_disposals
     << ",pin_requests=" << pin_requests << ",unpin_requests=" << unpin_requests
     << ",no_buffer_failures=" << no_buffer_failures
     << ",pinned_current=" << current_pinned_frames << ",pinned_peak=" << peak_pinned_frames
     << ",dirty_current=" << dirty_pages_current << ",dirty_peak=" << peak_dirty_pages
     << ",bytes_read=" << bytes_read << ",bytes_written=" << bytes_written
     << ",read_latency_ns_total=" << read_latency_ns_total
     << ",write_latency_ns_total=" << write_latency_ns_total
     << ",flush_latency_ns_total=" << flush_latency_ns_total
     << ",read_latency_max_ns=" << read_latency_max_ns
     << ",write_latency_max_ns=" << write_latency_max_ns
     << ",eviction_flushes=" << eviction_flushes
     << ",explicit_flushes=" << explicit_flushes
     << ",shutdown_flushes=" << shutdown_flushes;
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

void BufferPoolStats::record_disk_read(uint64_t bytes, uint64_t latency_ns)
{
  disk_reads_.fetch_add(1, std::memory_order_relaxed);
  bytes_read_.fetch_add(bytes, std::memory_order_relaxed);
  read_latency_ns_total_.fetch_add(latency_ns, std::memory_order_relaxed);
  update_max(read_latency_max_ns_, latency_ns);
}

void BufferPoolStats::record_disk_write(uint64_t bytes, uint64_t latency_ns)
{
  disk_writes_.fetch_add(1, std::memory_order_relaxed);
  bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
  write_latency_ns_total_.fetch_add(latency_ns, std::memory_order_relaxed);
  update_max(write_latency_max_ns_, latency_ns);
}

void BufferPoolStats::record_eviction(bool dirty)
{
  evictions_.fetch_add(1, std::memory_order_relaxed);
  if (dirty) {
    dirty_evictions_.fetch_add(1, std::memory_order_relaxed);
  }
}

void BufferPoolStats::record_flush(FlushReason reason, uint64_t latency_ns)
{
  flushes_.fetch_add(1, std::memory_order_relaxed);
  flush_latency_ns_total_.fetch_add(latency_ns, std::memory_order_relaxed);
  switch (reason) {
    case FlushReason::EVICTION: eviction_flushes_.fetch_add(1, std::memory_order_relaxed); break;
    case FlushReason::EXPLICIT: explicit_flushes_.fetch_add(1, std::memory_order_relaxed); break;
    case FlushReason::SHUTDOWN: shutdown_flushes_.fetch_add(1, std::memory_order_relaxed); break;
    case FlushReason::CHECKPOINT:
    case FlushReason::OTHER: break;
  }
}
void BufferPoolStats::record_page_allocation() { page_allocations_.fetch_add(1, std::memory_order_relaxed); }
void BufferPoolStats::record_page_disposal() { page_disposals_.fetch_add(1, std::memory_order_relaxed); }

void BufferPoolStats::record_pin(bool first_pin)
{
  pin_requests_.fetch_add(1, std::memory_order_relaxed);
  if (first_pin) {
    const uint64_t current = current_pinned_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    update_max(peak_pinned_frames_, current);
  }
}

void BufferPoolStats::record_unpin(bool last_unpin)
{
  unpin_requests_.fetch_add(1, std::memory_order_relaxed);
  if (last_unpin) {
    decrement_if_positive(current_pinned_frames_);
  }
}

void BufferPoolStats::record_no_buffer_failure()
{
  no_buffer_failures_.fetch_add(1, std::memory_order_relaxed);
}

void BufferPoolStats::record_dirty()
{
  const uint64_t current = dirty_pages_current_.fetch_add(1, std::memory_order_relaxed) + 1;
  update_max(peak_dirty_pages_, current);
}

void BufferPoolStats::record_clean() { decrement_if_positive(dirty_pages_current_); }

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
  result.pin_requests             = pin_requests_.load(std::memory_order_relaxed);
  result.unpin_requests           = unpin_requests_.load(std::memory_order_relaxed);
  result.no_buffer_failures       = no_buffer_failures_.load(std::memory_order_relaxed);
  result.current_pinned_frames    = current_pinned_frames_.load(std::memory_order_relaxed);
  result.peak_pinned_frames       = peak_pinned_frames_.load(std::memory_order_relaxed);
  result.dirty_pages_current      = dirty_pages_current_.load(std::memory_order_relaxed);
  result.peak_dirty_pages         = peak_dirty_pages_.load(std::memory_order_relaxed);
  result.bytes_read               = bytes_read_.load(std::memory_order_relaxed);
  result.bytes_written            = bytes_written_.load(std::memory_order_relaxed);
  result.read_latency_ns_total    = read_latency_ns_total_.load(std::memory_order_relaxed);
  result.write_latency_ns_total   = write_latency_ns_total_.load(std::memory_order_relaxed);
  result.flush_latency_ns_total   = flush_latency_ns_total_.load(std::memory_order_relaxed);
  result.read_latency_max_ns      = read_latency_max_ns_.load(std::memory_order_relaxed);
  result.write_latency_max_ns     = write_latency_max_ns_.load(std::memory_order_relaxed);
  result.eviction_flushes         = eviction_flushes_.load(std::memory_order_relaxed);
  result.explicit_flushes         = explicit_flushes_.load(std::memory_order_relaxed);
  result.shutdown_flushes         = shutdown_flushes_.load(std::memory_order_relaxed);
  return result;
}

void BufferPoolStats::reset()
{
  const uint64_t pinned_now = current_pinned_frames_.load(std::memory_order_relaxed);
  const uint64_t dirty_now = dirty_pages_current_.load(std::memory_order_relaxed);
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
  pin_requests_.store(0, std::memory_order_relaxed);
  unpin_requests_.store(0, std::memory_order_relaxed);
  no_buffer_failures_.store(0, std::memory_order_relaxed);
  peak_pinned_frames_.store(pinned_now, std::memory_order_relaxed);
  peak_dirty_pages_.store(dirty_now, std::memory_order_relaxed);
  bytes_read_.store(0, std::memory_order_relaxed);
  bytes_written_.store(0, std::memory_order_relaxed);
  read_latency_ns_total_.store(0, std::memory_order_relaxed);
  write_latency_ns_total_.store(0, std::memory_order_relaxed);
  flush_latency_ns_total_.store(0, std::memory_order_relaxed);
  read_latency_max_ns_.store(0, std::memory_order_relaxed);
  write_latency_max_ns_.store(0, std::memory_order_relaxed);
  eviction_flushes_.store(0, std::memory_order_relaxed);
  explicit_flushes_.store(0, std::memory_order_relaxed);
  shutdown_flushes_.store(0, std::memory_order_relaxed);
}
