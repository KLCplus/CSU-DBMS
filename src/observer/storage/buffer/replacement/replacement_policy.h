#pragma once

#include <functional>

#include "common/lang/memory.h"
#include "common/lang/string.h"
#include "storage/buffer/frame.h"

enum class BufferPoolReplacementPolicy
{
  LRU,
  FIFO,
  CLOCK,
};

const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy);
bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy);

/**
 * Frame 淘汰策略接口。调用方在持有 BPFrameManager 锁时调用这些方法。
 */
class ReplacementPolicy
{
public:
  virtual ~ReplacementPolicy() = default;

  virtual void on_insert(const FrameId &frame_id) = 0;
  virtual void on_access(const FrameId &frame_id) = 0;
  virtual void on_pin(const FrameId &frame_id) = 0;
  virtual void on_unpin(const FrameId &frame_id) = 0;
  virtual void on_remove(const FrameId &frame_id) = 0;

  virtual bool choose_victim(
      const std::function<bool(const FrameId &)> &is_replaceable, FrameId &victim) = 0;
  virtual string metadata(const FrameId &frame_id) const = 0;
  virtual const char *name() const = 0;
};

unique_ptr<ReplacementPolicy> create_replacement_policy(BufferPoolReplacementPolicy policy);
