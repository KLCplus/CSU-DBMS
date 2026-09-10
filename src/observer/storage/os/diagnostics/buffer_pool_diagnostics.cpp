#include "storage/os/diagnostics/buffer_pool_diagnostics.h"

#include <atomic>

uint64_t next_buffer_pool_event_sequence()
{
  static std::atomic<uint64_t> sequence{0};
  return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
