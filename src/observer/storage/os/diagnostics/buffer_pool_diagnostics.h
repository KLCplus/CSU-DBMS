#pragma once

#include <cstddef>
#include <cstdint>

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "storage/os/diagnostics/buffer_pool_stats.h"

struct FrameSnapshot
{
  string   frame_id;
  int32_t  buffer_pool_id = -1;
  int32_t  page_num = -1;
  int      pin_count = 0;
  bool     dirty = false;
  bool     replaceable = false;
  uint64_t last_access_ns = 0;
  string   policy_metadata;
};

struct BufferPoolSnapshot
{
  size_t capacity = 0;
  size_t used_frames = 0;
  size_t free_frames = 0;
  size_t pinned_frames = 0;
  size_t dirty_frames = 0;
  string replacement_policy;
  string io_backend;
  string file_name;
  int32_t buffer_pool_id = -1;
  BufferPoolStatsSnapshot stats;
  vector<FrameSnapshot> frames;
};

struct DirtyPageFlushResult
{
  size_t flushed_count = 0;
  size_t skipped_pinned_count = 0;
  size_t failed_count = 0;
};

/** 进程内单调递增，仅用于关联 BUFFER_POOL_TRACE 事件。 */
uint64_t next_buffer_pool_event_sequence();
