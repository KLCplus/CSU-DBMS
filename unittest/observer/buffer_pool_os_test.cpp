/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <filesystem>

#include "gtest/gtest.h"

#include "storage/os/buffer/disk_buffer_pool.h"
#include "storage/os/buffer/double_write_buffer.h"
#include "storage/clog/vacuous_log_handler.h"

using namespace std;

namespace {

PageNum run_replacement_sequence(BufferPoolReplacementPolicy policy)
{
  BPFrameManager frame_manager("OSLab", policy);
  EXPECT_EQ(RC::SUCCESS, frame_manager.init(1, 3));

  for (PageNum page_num = 1; page_num <= 3; page_num++) {
    Frame *frame = frame_manager.alloc(1, page_num);
    EXPECT_NE(nullptr, frame);
    frame->unpin();
  }

  // LRU 会刷新页面 1 的新近性；FIFO 保持首次进入顺序；CLOCK 设置 reference bit。
  Frame *page_one = frame_manager.get(1, 1);
  EXPECT_NE(nullptr, page_one);
  page_one->unpin();

  PageNum victim = BP_INVALID_PAGE_NUM;
  EXPECT_EQ(1, frame_manager.purge_frames(1, [&victim](Frame *frame) {
    victim = frame->page_num();
    return RC::SUCCESS;
  }));
  EXPECT_EQ(1, frame_manager.stats().snapshot().evictions);

  frame_manager.purge_frames(3, [](Frame *) { return RC::SUCCESS; });
  EXPECT_EQ(RC::SUCCESS, frame_manager.cleanup());
  return victim;
}

}  // namespace

TEST(BufferPoolOS, lru_and_fifo_choose_different_victims)
{
  EXPECT_EQ(2, run_replacement_sequence(BufferPoolReplacementPolicy::LRU));
  EXPECT_EQ(1, run_replacement_sequence(BufferPoolReplacementPolicy::FIFO));
  EXPECT_EQ(1, run_replacement_sequence(BufferPoolReplacementPolicy::CLOCK));
}

TEST(BufferPoolOS, parses_replacement_policy)
{
  BufferPoolReplacementPolicy policy = BufferPoolReplacementPolicy::LRU;
  EXPECT_TRUE(parse_buffer_pool_replacement_policy("FIFO", policy));
  EXPECT_EQ(BufferPoolReplacementPolicy::FIFO, policy);
  EXPECT_TRUE(parse_buffer_pool_replacement_policy("lru", policy));
  EXPECT_EQ(BufferPoolReplacementPolicy::LRU, policy);
  EXPECT_TRUE(parse_buffer_pool_replacement_policy("ClOcK", policy));
  EXPECT_EQ(BufferPoolReplacementPolicy::CLOCK, policy);
}

TEST(BufferPoolOS, parses_page_io_backend)
{
  PageIOBackendType backend = PageIOBackendType::LEGACY;
  EXPECT_TRUE(parse_page_io_backend("POSITIONAL", backend));
  EXPECT_EQ(PageIOBackendType::POSITIONAL, backend);
  EXPECT_TRUE(parse_page_io_backend("legacy", backend));
  EXPECT_EQ(PageIOBackendType::LEGACY, backend);
  EXPECT_FALSE(parse_page_io_backend("unknown", backend));
}

TEST(BufferPoolOS, records_dirty_eviction)
{
  BPFrameManager frame_manager("OSLabDirty", BufferPoolReplacementPolicy::LRU);
  ASSERT_EQ(RC::SUCCESS, frame_manager.init(1, 1));

  Frame *frame = frame_manager.alloc(1, 1);
  ASSERT_NE(nullptr, frame);
  frame->mark_dirty();
  frame->unpin();

  ASSERT_EQ(1, frame_manager.purge_frames(1, [](Frame *victim) {
    victim->clear_dirty();
    return RC::SUCCESS;
  }));

  const BufferPoolStatsSnapshot stats = frame_manager.stats().snapshot();
  EXPECT_EQ(1, stats.evictions);
  EXPECT_EQ(1, stats.dirty_evictions);
  EXPECT_EQ(RC::SUCCESS, frame_manager.cleanup());
}

TEST(BufferPoolOS, records_hit_miss_and_disk_read)
{
  const filesystem::path directory("buffer_pool_os_stats");
  filesystem::remove_all(directory);
  filesystem::create_directories(directory);
  const filesystem::path filename = directory / "stats.bp";

  BufferPoolManager bpm(4 * BP_PAGE_SIZE, BufferPoolReplacementPolicy::LRU);
  ASSERT_EQ(RC::SUCCESS, bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
  VacuousLogHandler log_handler;
  ASSERT_EQ(RC::SUCCESS, bpm.create_file(filename.c_str()));

  DiskBufferPool *buffer_pool = nullptr;
  ASSERT_EQ(RC::SUCCESS, bpm.open_file(log_handler, filename.c_str(), buffer_pool));

  Frame *frame = nullptr;
  ASSERT_EQ(RC::SUCCESS, buffer_pool->allocate_page(&frame));
  const PageNum page_num = frame->page_num();
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->purge_page(page_num));

  bpm.reset_stats();

  ASSERT_EQ(RC::SUCCESS, buffer_pool->get_this_page(page_num, &frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->get_this_page(page_num, &frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));

  const BufferPoolStatsSnapshot stats = bpm.stats();
  EXPECT_EQ(2, stats.page_requests);
  EXPECT_EQ(1, stats.cache_hits);
  EXPECT_EQ(1, stats.cache_misses);
  EXPECT_EQ(1, stats.disk_reads);
  EXPECT_DOUBLE_EQ(0.5, stats.hit_rate());

  const BufferPoolSnapshot state = buffer_pool->snapshot();
  EXPECT_EQ(4, state.capacity);
  EXPECT_EQ("LRU", state.replacement_policy);
  EXPECT_EQ("legacy", state.io_backend);
  EXPECT_EQ(filename.string(), state.file_name);

  ASSERT_EQ(RC::SUCCESS, buffer_pool->get_this_page(page_num, &frame));
  frame->mark_dirty();
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));
  DirtyPageFlushResult flush_result;
  EXPECT_EQ(RC::SUCCESS, buffer_pool->flush_dirty_pages(1, flush_result));
  EXPECT_EQ(1, flush_result.flushed_count);
  EXPECT_EQ(0, flush_result.failed_count);

  EXPECT_EQ(RC::SUCCESS, bpm.close_file(filename.c_str()));
  filesystem::remove_all(directory);
}

TEST(BufferPoolOS, returns_no_buffer_when_every_frame_is_pinned)
{
  const filesystem::path directory("buffer_pool_os_pinned");
  filesystem::remove_all(directory);
  filesystem::create_directories(directory);
  const filesystem::path filename = directory / "pinned.bp";

  // 唯一的 Frame 被文件头页长期 pin，数据页分配应立即失败，不能无限循环。
  BufferPoolManager bpm(BP_PAGE_SIZE, BufferPoolReplacementPolicy::FIFO);
  ASSERT_EQ(RC::SUCCESS, bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
  VacuousLogHandler log_handler;
  ASSERT_EQ(RC::SUCCESS, bpm.create_file(filename.c_str()));

  DiskBufferPool *buffer_pool = nullptr;
  ASSERT_EQ(RC::SUCCESS, bpm.open_file(log_handler, filename.c_str(), buffer_pool));

  Frame *frame = nullptr;
  EXPECT_EQ(RC::BUFFERPOOL_NOBUF, buffer_pool->allocate_page(&frame));
  EXPECT_EQ(nullptr, frame);
  EXPECT_EQ(1, bpm.stats().no_buffer_failures);
  const BufferPoolSnapshot state = buffer_pool->snapshot();
  EXPECT_EQ(1, state.capacity);
  EXPECT_EQ(1, state.pinned_frames);

  EXPECT_EQ(RC::SUCCESS, bpm.close_file(filename.c_str()));
  filesystem::remove_all(directory);
}
