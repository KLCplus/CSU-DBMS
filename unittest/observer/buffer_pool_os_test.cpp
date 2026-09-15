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
#include <algorithm>
#include <iostream>

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

struct ReplacementWorkloadResult
{
  size_t accesses = 0;
  size_t hits = 0;
  size_t misses = 0;
  size_t evictions = 0;

  double hit_rate() const { return accesses == 0 ? 0.0 : static_cast<double>(hits) / accesses; }
};

ReplacementWorkloadResult run_scan_resistance_workload(BufferPoolReplacementPolicy policy)
{
  BPFrameManager frame_manager("ScanResistance", policy);
  EXPECT_EQ(RC::SUCCESS, frame_manager.init(1, 4));
  ReplacementWorkloadResult result;

  auto access = [&](PageNum page_num) {
    ++result.accesses;
    Frame *frame = frame_manager.get(1, page_num);
    if (frame != nullptr) {
      ++result.hits;
      frame->unpin();
      return;
    }

    ++result.misses;
    frame = frame_manager.alloc(1, page_num);
    if (frame == nullptr) {
      EXPECT_EQ(1, frame_manager.purge_frames(1, [](Frame *) { return RC::SUCCESS; }));
      frame = frame_manager.alloc(1, page_num);
    }
    ASSERT_NE(nullptr, frame);
    frame->unpin();
  };

  // Warm two genuinely hot pages, then repeatedly interleave one-pass scans.
  for (int repeat = 0; repeat < 3; ++repeat) {
    access(1);
    access(2);
  }
  for (int cycle = 0; cycle < 100; ++cycle) {
    for (int offset = 0; offset < 4; ++offset) {
      access(1000 + cycle * 4 + offset);
    }
    access(1);
    access(2);
  }

  result.evictions = frame_manager.stats().snapshot().evictions;
  frame_manager.purge_frames(4, [](Frame *) { return RC::SUCCESS; });
  EXPECT_EQ(RC::SUCCESS, frame_manager.cleanup());
  return result;
}

BufferPoolStatsSnapshot run_disk_scan_workload(BufferPoolReplacementPolicy policy, const filesystem::path &filename)
{
  constexpr int scan_cycles = 50;
  constexpr int highest_page = 2 + scan_cycles * 4;
  VacuousLogHandler log_handler;

  {
    BufferPoolManager setup_bpm((highest_page + 2) * BP_PAGE_SIZE, policy);
    EXPECT_EQ(RC::SUCCESS, setup_bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
    EXPECT_EQ(RC::SUCCESS, setup_bpm.create_file(filename.c_str()));
    DiskBufferPool *setup_pool = nullptr;
    EXPECT_EQ(RC::SUCCESS, setup_bpm.open_file(log_handler, filename.c_str(), setup_pool));
    for (int i = 1; i <= highest_page; ++i) {
      Frame *frame = nullptr;
      EXPECT_EQ(RC::SUCCESS, setup_pool->allocate_page(&frame));
      if (frame != nullptr) {
        EXPECT_EQ(RC::SUCCESS, setup_pool->unpin_page(frame));
      }
    }
    EXPECT_EQ(RC::SUCCESS, setup_bpm.close_file(filename.c_str()));
  }

  BufferPoolManager bpm(5 * BP_PAGE_SIZE, policy);  // header + four data frames
  EXPECT_EQ(RC::SUCCESS, bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
  DiskBufferPool *buffer_pool = nullptr;
  EXPECT_EQ(RC::SUCCESS, bpm.open_file(log_handler, filename.c_str(), buffer_pool));
  bpm.reset_stats();

  auto access = [&](PageNum page_num) {
    Frame *frame = nullptr;
    EXPECT_EQ(RC::SUCCESS, buffer_pool->get_this_page(page_num, &frame));
    if (frame != nullptr) {
      EXPECT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));
    }
  };

  for (int repeat = 0; repeat < 3; ++repeat) {
    access(1);
    access(2);
  }
  for (int cycle = 0; cycle < scan_cycles; ++cycle) {
    for (int offset = 0; offset < 4; ++offset) {
      access(3 + cycle * 4 + offset);
    }
    access(1);
    access(2);
  }

  const BufferPoolStatsSnapshot result = bpm.stats();
  EXPECT_EQ(RC::SUCCESS, bpm.close_file(filename.c_str()));
  return result;
}

}  // namespace

TEST(BufferPoolOS, lru_and_fifo_choose_different_victims)
{
  EXPECT_EQ(2, run_replacement_sequence(BufferPoolReplacementPolicy::LRU));
  EXPECT_EQ(1, run_replacement_sequence(BufferPoolReplacementPolicy::FIFO));
  EXPECT_EQ(1, run_replacement_sequence(BufferPoolReplacementPolicy::CLOCK));
  EXPECT_EQ(2, run_replacement_sequence(BufferPoolReplacementPolicy::LRU_K));
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
  EXPECT_TRUE(parse_buffer_pool_replacement_policy("LRU-K", policy));
  EXPECT_EQ(BufferPoolReplacementPolicy::LRU_K, policy);
  EXPECT_TRUE(parse_buffer_pool_replacement_policy("lruk", policy));
  EXPECT_EQ(BufferPoolReplacementPolicy::LRU_K, policy);
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

TEST(BufferPoolOS, writes_back_dirty_page_on_real_eviction)
{
  const filesystem::path directory("buffer_pool_os_dirty_eviction");
  filesystem::remove_all(directory);
  filesystem::create_directories(directory);
  const filesystem::path filename = directory / "dirty.bp";

  BufferPoolManager bpm(2 * BP_PAGE_SIZE, BufferPoolReplacementPolicy::LRU_K);
  ASSERT_EQ(RC::SUCCESS, bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
  VacuousLogHandler log_handler;
  ASSERT_EQ(RC::SUCCESS, bpm.create_file(filename.c_str()));

  DiskBufferPool *buffer_pool = nullptr;
  ASSERT_EQ(RC::SUCCESS, bpm.open_file(log_handler, filename.c_str(), buffer_pool));

  Frame *first = nullptr;
  ASSERT_EQ(RC::SUCCESS, buffer_pool->allocate_page(&first));
  const PageNum first_page_num = first->page_num();
  first->data()[0] = static_cast<char>(0x6B);
  first->mark_dirty();
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(first));

  Frame *second = nullptr;
  ASSERT_EQ(RC::SUCCESS, buffer_pool->allocate_page(&second));
  ASSERT_NE(first_page_num, second->page_num());
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(second));

  const BufferPoolStatsSnapshot after_eviction = bpm.stats();
  EXPECT_GE(after_eviction.dirty_evictions, 1);

  Frame *reloaded = nullptr;
  ASSERT_EQ(RC::SUCCESS, buffer_pool->get_this_page(first_page_num, &reloaded));
  EXPECT_EQ(static_cast<char>(0x6B), reloaded->data()[0]);
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(reloaded));

  EXPECT_EQ(RC::SUCCESS, bpm.close_file(filename.c_str()));
  filesystem::remove_all(directory);
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
TEST(BufferPoolOS, lru_k_resists_sequential_scan_pollution)
{
  const ReplacementWorkloadResult lru = run_scan_resistance_workload(BufferPoolReplacementPolicy::LRU);
  const ReplacementWorkloadResult lru_k = run_scan_resistance_workload(BufferPoolReplacementPolicy::LRU_K);

  std::cout << "[LRU_K_BENCHMARK] workload=hot2_scan4_cycles100 capacity=4 "
            << "lru_hits=" << lru.hits << " lru_misses=" << lru.misses
            << " lru_hit_rate=" << lru.hit_rate()
            << " lruk_hits=" << lru_k.hits << " lruk_misses=" << lru_k.misses
            << " lruk_hit_rate=" << lru_k.hit_rate() << std::endl;

  EXPECT_GT(lru_k.hits, lru.hits);
  EXPECT_LE(lru_k.misses + 150, lru.misses);
}

TEST(BufferPoolOS, rejects_invalid_pages_and_clears_reused_page)
{
  const filesystem::path directory("buffer_pool_os_page_validation");
  filesystem::remove_all(directory);
  filesystem::create_directories(directory);
  const filesystem::path filename = directory / "validation.bp";

  BufferPoolManager bpm(4 * BP_PAGE_SIZE, BufferPoolReplacementPolicy::LRU_K);
  ASSERT_EQ(RC::SUCCESS, bpm.init(make_unique<VacuousDoubleWriteBuffer>()));
  VacuousLogHandler log_handler;
  ASSERT_EQ(RC::SUCCESS, bpm.create_file(filename.c_str()));

  DiskBufferPool *buffer_pool = nullptr;
  ASSERT_EQ(RC::SUCCESS, bpm.open_file(log_handler, filename.c_str(), buffer_pool));

  Frame *frame = nullptr;
  ASSERT_EQ(RC::SUCCESS, buffer_pool->allocate_page(&frame));
  const PageNum page_num = frame->page_num();
  memset(frame->data(), 0x5A, BP_PAGE_DATA_SIZE);
  frame->mark_dirty();
  ASSERT_EQ(RC::SUCCESS, buffer_pool->flush_page(*frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->dispose_page(page_num));

  EXPECT_EQ(RC::BUFFERPOOL_INVALID_PAGE_NUM, buffer_pool->get_this_page(page_num, &frame));
  EXPECT_EQ(RC::BUFFERPOOL_INVALID_PAGE_NUM, buffer_pool->get_this_page(-1, &frame));
  EXPECT_EQ(RC::BUFFERPOOL_INVALID_PAGE_NUM, buffer_pool->get_this_page(page_num + 100, &frame));
  EXPECT_EQ(RC::BUFFERPOOL_INVALID_PAGE_NUM, buffer_pool->dispose_page(page_num));

  ASSERT_EQ(RC::SUCCESS, buffer_pool->allocate_page(&frame));
  ASSERT_EQ(page_num, frame->page_num());
  EXPECT_TRUE(std::all_of(frame->data(), frame->data() + BP_PAGE_DATA_SIZE, [](char value) { return value == 0; }));
  ASSERT_EQ(RC::SUCCESS, buffer_pool->unpin_page(frame));

  EXPECT_EQ(RC::SUCCESS, bpm.close_file(filename.c_str()));
  filesystem::remove_all(directory);
}

TEST(BufferPoolOS, lru_k_reduces_real_disk_reads_under_scan_pollution)
{
  const filesystem::path directory("buffer_pool_os_lruk_disk_benchmark");
  filesystem::remove_all(directory);
  filesystem::create_directories(directory);

  const BufferPoolStatsSnapshot lru =
      run_disk_scan_workload(BufferPoolReplacementPolicy::LRU, directory / "lru.bp");
  const BufferPoolStatsSnapshot lru_k =
      run_disk_scan_workload(BufferPoolReplacementPolicy::LRU_K, directory / "lruk.bp");

  const double read_reduction = lru.disk_reads == 0 ? 0.0 :
      1.0 - static_cast<double>(lru_k.disk_reads) / static_cast<double>(lru.disk_reads);
  std::cout << "[LRU_K_DISK_BENCHMARK] workload=hot2_scan4_cycles50 data_frames=4 "
            << "lru_disk_reads=" << lru.disk_reads << " lru_hit_rate=" << lru.hit_rate()
            << " lruk_disk_reads=" << lru_k.disk_reads << " lruk_hit_rate=" << lru_k.hit_rate()
            << " read_reduction=" << read_reduction << std::endl;

  EXPECT_LT(lru_k.disk_reads, lru.disk_reads);
  EXPECT_GT(read_reduction, 0.25);
  filesystem::remove_all(directory);
}
