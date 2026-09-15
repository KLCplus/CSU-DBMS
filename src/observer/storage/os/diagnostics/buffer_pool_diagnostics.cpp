#include "storage/os/diagnostics/buffer_pool_diagnostics.h"

#include <atomic>

/**
 * @brief 取下一个事件序号
 * @details 进程内单调递增，只用于把同一段时间内的 Trace 事件排成顺序，
 * 不参与任何业务判断。
 * @return 从 1 开始递增的序号
 */
uint64_t next_buffer_pool_event_sequence()
{
  static std::atomic<uint64_t> sequence{0};
  return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
