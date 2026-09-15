/**
 * @file buffer_pool_diagnostics.h
 * @brief 只读诊断 DTO：Frame 快照、Buffer Pool 快照与批量刷盘结果
 * @ingroup BufferPool
 * @details 这些结构只携带值，不持有 Frame 或 Page 指针，也不暴露互斥量，
 * 因此可以安全地跨线程、跨进程传给 CLI 与 Web。
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "storage/os/diagnostics/buffer_pool_stats.h"

/**
 * @brief 单个 Frame 的只读快照
 * @ingroup BufferPool
 * @details policy_metadata 由当前淘汰策略生成，LRU 给出队列位置，
 * LRU-K 给出冷热分类，CLOCK 给出引用位与槽位。
 */
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

/**
 * @brief Buffer Pool 整体的只读快照
 * @ingroup BufferPool
 * @details 含容量、使用量、策略与后端名字，以及一份有限长度的 Frame 列表。
 * buffer_pool_id 为 -1 表示这是汇总全部文件的全局快照。
 */
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

/**
 * @brief 批量刷脏页的结果
 * @ingroup BufferPool
 * @details 分开统计跳过与失败，是为了让调用方区分「因为有业务在用所以没刷」
 * 和「刷了但出错」这两种完全不同的情况。
 */
struct DirtyPageFlushResult
{
  size_t flushed_count = 0;
  size_t skipped_pinned_count = 0;
  size_t failed_count = 0;
};

/** 进程内单调递增，仅用于关联 BUFFER_POOL_TRACE 事件。 */
uint64_t next_buffer_pool_event_sequence();
