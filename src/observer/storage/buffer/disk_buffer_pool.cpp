
#include <errno.h>
#include <string.h>
#include <chrono>

#include "common/io/io.h"
#include "common/lang/mutex.h"
#include "common/lang/algorithm.h"
#include "common/log/log.h"
#include "common/math/crc.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/buffer/buffer_pool_log.h"
#include "storage/db/db.h"

using namespace common;

static const int MEM_POOL_ITEM_NUM = 20;

////////////////////////////////////////////////////////////////////////////////

string BPFileHeader::to_string() const
{
  stringstream ss;
  ss << "pageCount:" << page_count << ", allocatedCount:" << allocated_pages;
  return ss.str();
}

////////////////////////////////////////////////////////////////////////////////

BPFrameManager::BPFrameManager(const char *name, BufferPoolReplacementPolicy replacement_policy)
    : allocator_(name),
      replacement_policy_type_(replacement_policy),
      replacement_policy_(create_replacement_policy(replacement_policy))
{}

RC BPFrameManager::init(int pool_num, int item_num_per_pool)
{
  int ret = allocator_.init(false, pool_num, item_num_per_pool);
  if (ret == 0) {
    capacity_ = static_cast<size_t>(pool_num) * static_cast<size_t>(item_num_per_pool);
    return RC::SUCCESS;
  }
  return RC::NOMEM;
}

RC BPFrameManager::cleanup()
{
  if (frames_.count() > 0) {
    return RC::INTERNAL;
  }

  frames_.destroy();
  return RC::SUCCESS;
}

int BPFrameManager::purge_frames(int count, function<RC(Frame *frame)> purger)
{
  lock_guard<mutex> lock_guard(lock_);

  vector<Frame *> frames_can_purge;
  if (count <= 0) {
    count = 1;
  }
  frames_can_purge.reserve(count);

  auto is_replaceable = [this](const FrameId &frame_id) {
    Frame *frame = nullptr;
    return frames_.peek(frame_id, frame) && frame != nullptr && frame->can_purge();
  };

  while (frames_can_purge.size() < static_cast<size_t>(count)) {
    FrameId victim;
    if (!replacement_policy_->choose_victim(is_replaceable, victim)) {
      break;
    }
    Frame *frame = nullptr;
    if (!frames_.peek(victim, frame) || frame == nullptr || !frame->can_purge()) {
      break;
    }
    frame->pin();
    replacement_policy_->on_pin(victim);
    frames_can_purge.push_back(frame);
  }
  LOG_INFO("purge frames find %ld pages total", frames_can_purge.size());

  /// 当前还在frameManager的锁内，而 purger 是一个非常耗时的操作
  /// 他需要把脏页数据刷新到磁盘上去，所以这里会极大地降低并发度
  int freed_count = 0;
  for (Frame *frame : frames_can_purge) {
    const bool dirty = frame->dirty();
    RC rc = purger(frame);
    if (RC::SUCCESS == rc) {
      stats_.record_eviction(dirty);
      LOG_TRACE("[BUFFER_POOL_TRACE] event=EVICT event_seq=%llu policy=%s frame_id=%s buffer_pool_id=%d page_num=%d dirty=%d",
          static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
          replacement_policy_->name(), frame->frame_id().to_string().c_str(),
          frame->buffer_pool_id(),
          frame->page_num(),
          dirty);
      free_internal(frame->frame_id(), frame);
      freed_count++;
    } else {
      frame->unpin();
      replacement_policy_->on_unpin(frame->frame_id());
      LOG_WARN("failed to purge frame. frame_id=%s, rc=%s", 
               frame->frame_id().to_string().c_str(), strrc(rc));
    }
  }
  LOG_INFO("purge frame done. number=%d", freed_count);
  return freed_count;
}

Frame *BPFrameManager::get(int buffer_pool_id, PageNum page_num)
{
  FrameId                     frame_id(buffer_pool_id, page_num);

  lock_guard<mutex> lock_guard(lock_);
  return get_internal(frame_id);
}

Frame *BPFrameManager::get_internal(const FrameId &frame_id)
{
  Frame *frame = nullptr;
  (void)frames_.peek(frame_id, frame);
  if (frame != nullptr) {
    replacement_policy_->on_access(frame_id);
    frame->pin();
    replacement_policy_->on_pin(frame_id);
    LOG_DEBUG("got a frame. frame=%s", frame->to_string().c_str());
  }
  return frame;
}

Frame *BPFrameManager::alloc(int buffer_pool_id, PageNum page_num)
{
  FrameId frame_id(buffer_pool_id, page_num);

  lock_guard<mutex> lock_guard(lock_);

  Frame                      *frame = get_internal(frame_id);
  if (frame != nullptr) {
    return frame;
  }

  frame = allocator_.alloc();
  if (frame != nullptr) {
    ASSERT(frame->pin_count() == 0, "got an invalid frame that pin count is not 0. frame=%s", 
           frame->to_string().c_str());
    frame->set_buffer_pool_id(buffer_pool_id);
    frame->set_page_num(page_num);
    frame->set_stats(&stats_);
    frame->pin();
    frames_.put(frame_id, frame);
    replacement_policy_->on_insert(frame_id);
    replacement_policy_->on_pin(frame_id);
    LOG_DEBUG("allocate a new frame. frame=%s", frame->to_string().c_str());
  }
  return frame;
}

RC BPFrameManager::free(int buffer_pool_id, PageNum page_num, Frame *frame)
{
  FrameId frame_id(buffer_pool_id, page_num);

  lock_guard<mutex> lock_guard(lock_);
  return free_internal(frame_id, frame);
}

RC BPFrameManager::free_internal(const FrameId &frame_id, Frame *frame)
{
  Frame                *frame_source = nullptr;
  [[maybe_unused]] bool found        = frames_.peek(frame_id, frame_source);
  ASSERT(found && frame == frame_source && frame->pin_count() == 1,
      "failed to free frame. found=%d, frameId=%s, frame_source=%p, frame=%p, pinCount=%d, lbt=%s",
      found, frame_id.to_string().c_str(), frame_source, frame, frame->pin_count(), lbt());

  frame->clear_dirty();
  frame->set_page_num(-1);
  frame->unpin();
  replacement_policy_->on_unpin(frame_id);
  replacement_policy_->on_remove(frame_id);
  frames_.remove(frame_id);
  frame->set_stats(nullptr);
  allocator_.free(frame);
  return RC::SUCCESS;
}

list<Frame *> BPFrameManager::find_list(int buffer_pool_id)
{
  lock_guard<mutex> lock_guard(lock_);

  list<Frame *> frames;
  auto               fetcher = [this, &frames, buffer_pool_id](const FrameId &frame_id, Frame *const frame) -> bool {
    if (buffer_pool_id == frame_id.buffer_pool_id()) {
      frame->pin();
      replacement_policy_->on_pin(frame_id);
      frames.push_back(frame);
    }
    return true;
  };
  frames_.foreach (fetcher);
  return frames;
}

BufferPoolSnapshot BPFrameManager::snapshot(int buffer_pool_id) const
{
  lock_guard<mutex> lock_guard(const_cast<mutex &>(lock_));

  BufferPoolSnapshot result;
  result.capacity = capacity_;
  result.free_frames = capacity_ > frames_.count() ? capacity_ - frames_.count() : 0;
  result.replacement_policy = replacement_policy_->name();

  auto collector = [this, buffer_pool_id, &result](const FrameId &frame_id, Frame *const frame) {
    if (buffer_pool_id >= 0 && frame_id.buffer_pool_id() != buffer_pool_id) {
      return true;
    }
    FrameSnapshot item;
    item.frame_id = frame_id.to_string();
    item.buffer_pool_id = frame_id.buffer_pool_id();
    item.page_num = frame_id.page_num();
    item.pin_count = frame->pin_count();
    item.dirty = frame->dirty();
    item.replaceable = frame->can_purge();
    item.last_access_ns = frame->last_access_ns();
    item.policy_metadata = replacement_policy_->metadata(frame_id);
    result.frames.push_back(std::move(item));
    ++result.used_frames;
    result.pinned_frames += frame->pin_count() > 0 ? 1 : 0;
    result.dirty_frames += frame->dirty() ? 1 : 0;
    return true;
  };
  const_cast<FrameLruCache &>(frames_).foreach(collector);
  result.stats = stats_.snapshot();
  return result;
}

////////////////////////////////////////////////////////////////////////////////
BufferPoolIterator::BufferPoolIterator() {}
BufferPoolIterator::~BufferPoolIterator() {}
RC BufferPoolIterator::init(DiskBufferPool &bp, PageNum start_page /* = 0 */)
{
  bitmap_.init(bp.file_header_->bitmap, bp.file_header_->page_count);
  if (start_page <= 0) {
    current_page_num_ = -1;
  } else {
    current_page_num_ = start_page - 1;
  }
  return RC::SUCCESS;
}

bool BufferPoolIterator::has_next() { return bitmap_.next_setted_bit(current_page_num_ + 1) != -1; }

PageNum BufferPoolIterator::next()
{
  PageNum next_page = bitmap_.next_setted_bit(current_page_num_ + 1);
  if (next_page != -1) {
    current_page_num_ = next_page;
  }
  return next_page;
}

RC BufferPoolIterator::reset()
{
  current_page_num_ = 0;
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
DiskBufferPool::DiskBufferPool(
    BufferPoolManager &bp_manager, BPFrameManager &frame_manager, DoubleWriteBuffer &dblwr_manager,
    LogHandler &log_handler, PageIOBackendType io_backend_type)
    : bp_manager_(bp_manager),
      frame_manager_(frame_manager),
      dblwr_manager_(dblwr_manager),
      log_handler_(*this, log_handler),
      io_backend_(create_page_io_backend(io_backend_type))
{}

DiskBufferPool::~DiskBufferPool()
{
  close_file();
  LOG_INFO("disk buffer pool exit");
}

RC DiskBufferPool::open_file(const char *file_name)
{
  int fd = open(file_name, O_RDWR);
  if (fd < 0) {
    LOG_ERROR("Failed to open file %s, because %s.", file_name, strerror(errno));
    return RC::IOERR_ACCESS;
  }
  LOG_INFO("Successfully open buffer pool file %s.", file_name);

  file_name_ = file_name;
  file_desc_ = fd;

  Page header_page;
  const auto read_start = std::chrono::steady_clock::now();
  RC rc = io_backend_->read_page(file_desc_, file_name_.c_str(), BP_HEADER_PAGE, header_page);
  const uint64_t read_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - read_start).count();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to read first page of %s, rc=%s.", file_name, strrc(rc));
    close(fd);
    file_desc_ = -1;
    return rc;
  }
  frame_manager_.stats().record_disk_read(BP_PAGE_SIZE, read_ns);
  stats_.record_disk_read(BP_PAGE_SIZE, read_ns);

  BPFileHeader *tmp_file_header = reinterpret_cast<BPFileHeader *>(header_page.data);
  buffer_pool_id_ = tmp_file_header->buffer_pool_id;

  rc = allocate_frame(BP_HEADER_PAGE, &hdr_frame_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to allocate frame for header. file name %s", file_name_.c_str());
    close(fd);
    file_desc_ = -1;
    return rc;
  }

  hdr_frame_->set_buffer_pool_id(id());
  hdr_frame_->access();

  if ((rc = load_page(BP_HEADER_PAGE, hdr_frame_)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load first page of %s, due to %s.", file_name, strerror(errno));
    purge_frame(BP_HEADER_PAGE, hdr_frame_, FlushReason::OTHER);
    close(fd);
    file_desc_ = -1;
    return rc;
  }

  file_header_ = (BPFileHeader *)hdr_frame_->data();

  LOG_INFO("Successfully open %s. file_desc=%d, hdr_frame=%p, file header=%s",
           file_name, file_desc_, hdr_frame_, file_header_->to_string().c_str());
  return RC::SUCCESS;
}

RC DiskBufferPool::close_file()
{
  RC rc = RC::SUCCESS;
  if (file_desc_ < 0) {
    return rc;
  }

  unpin_page(hdr_frame_);
  log_pinned_frames("shutdown", true);

  // TODO: 理论上是在回放时回滚未提交事务，但目前没有undo log，因此不下刷数据page，只通过redo log回放
  rc = purge_all_pages(FlushReason::SHUTDOWN);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to close %s, due to failed to purge pages. rc=%s", file_name_.c_str(), strrc(rc));
    return rc;
  }

  rc = dblwr_manager_.clear_pages(this);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to clear pages in double write buffer. filename=%s, rc=%s", file_name_.c_str(), strrc(rc));
    return rc;
  }

  disposed_pages_.clear();

  if (close(file_desc_) < 0) {
    LOG_ERROR("Failed to close fileId:%d, fileName:%s, error:%s", file_desc_, file_name_.c_str(), strerror(errno));
    return RC::IOERR_CLOSE;
  }
  LOG_INFO("Successfully close file %d:%s.", file_desc_, file_name_.c_str());
  file_desc_ = -1;

  bp_manager_.close_file(file_name_.c_str());
  return RC::SUCCESS;
}

RC DiskBufferPool::get_this_page(PageNum page_num, Frame **frame)
{
  RC rc  = RC::SUCCESS;
  *frame = nullptr;

  Frame *used_match_frame = frame_manager_.get(id(), page_num);
  if (used_match_frame != nullptr) {
    frame_manager_.stats().record_page_request(true);
    stats_.record_page_request(true);
    stats_.record_pin(used_match_frame->pin_count() == 1);
    LOG_TRACE("[BUFFER_POOL_TRACE] event=HIT event_seq=%llu policy=%s io_backend=%s frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
        static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
        frame_manager_.replacement_policy().name(), page_io_backend_name(io_backend_type()),
        used_match_frame->frame_id().to_string().c_str(), id(), page_num, used_match_frame->pin_count());
    LOG_TRACE("[BUFFER_POOL_TRACE] event=PIN event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
        static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
        used_match_frame->frame_id().to_string().c_str(), id(), page_num, used_match_frame->pin_count());
    used_match_frame->access();
    *frame = used_match_frame;
    return RC::SUCCESS;
  }

  frame_manager_.stats().record_page_request(false);
  stats_.record_page_request(false);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=MISS event_seq=%llu policy=%s io_backend=%s buffer_pool_id=%d page_num=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
      frame_manager_.replacement_policy().name(), page_io_backend_name(io_backend_type()), id(), page_num);

  scoped_lock lock_guard(lock_);  // 直接加了一把大锁，其实可以根据访问的页面来细化提高并行度

  // Allocate one page and load the data into this page
  Frame *allocated_frame = nullptr;

  rc = allocate_frame(page_num, &allocated_frame);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to alloc frame %s:%d, due to failed to alloc page.", file_name_.c_str(), page_num);
    return rc;
  }

  allocated_frame->set_buffer_pool_id(id());
  // allocated_frame->pin(); // pined in manager::get
  allocated_frame->access();

  if ((rc = load_page(page_num, allocated_frame)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load page %s:%d", file_name_.c_str(), page_num);
    purge_frame(page_num, allocated_frame, FlushReason::OTHER);
    return rc;
  }

  *frame = allocated_frame;
  stats_.record_pin(allocated_frame->pin_count() == 1);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=PIN event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
      allocated_frame->frame_id().to_string().c_str(), id(), page_num, allocated_frame->pin_count());
  return RC::SUCCESS;
}

RC DiskBufferPool::allocate_page(Frame **frame)
{
  RC rc = RC::SUCCESS;

  lock_.lock();

  int byte = 0, bit = 0;
  if ((file_header_->allocated_pages) < (file_header_->page_count)) {
    // There is one free page
    for (int i = 0; i < file_header_->page_count; i++) {
      byte = i / 8;
      bit  = i % 8;
      if (((file_header_->bitmap[byte]) & (1 << bit)) == 0) {
        (file_header_->allocated_pages)++;
        file_header_->bitmap[byte] |= (1 << bit);
        // TODO,  do we need clean the loaded page's data?
        hdr_frame_->mark_dirty();
        LSN lsn = 0;
        rc = log_handler_.allocate_page(i, lsn);
        if (OB_FAIL(rc)) {
          LOG_ERROR("Failed to log allocate page %d, rc=%s", i, strrc(rc));
          // 忽略了错误
        }

        hdr_frame_->set_lsn(lsn);

        frame_manager_.stats().record_page_allocation();
        stats_.record_page_allocation();
        LOG_TRACE("[BUFFER_POOL_TRACE] event=ALLOCATE event_seq=%llu buffer_pool_id=%d page_num=%d reused=1",
            static_cast<unsigned long long>(next_buffer_pool_event_sequence()), id(), i);

        LOG_DEBUG("allocate a new page without extend buffer pool. page num=%d, buffer pool=%d", i, id());

        lock_.unlock();
        return get_this_page(i, frame);
      }
    }
  }

  if (file_header_->page_count >= BPFileHeader::MAX_PAGE_NUM) {
    LOG_WARN("file buffer pool is full. page count %d, max page count %d",
        file_header_->page_count, BPFileHeader::MAX_PAGE_NUM);
    lock_.unlock();
    return RC::BUFFERPOOL_NOBUF;
  }

  LSN lsn = 0;
  rc = log_handler_.allocate_page(file_header_->page_count, lsn);
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to log allocate page %d, rc=%s", file_header_->page_count, strrc(rc));
    // 忽略了错误
  }
  hdr_frame_->set_lsn(lsn);

  PageNum page_num        = file_header_->page_count;
  Frame  *allocated_frame = nullptr;
  if ((rc = allocate_frame(page_num, &allocated_frame)) != RC::SUCCESS) {
    LOG_ERROR("Failed to allocate frame %s, due to no free page.", file_name_.c_str());
    lock_.unlock();
    return rc;
  }

  LOG_INFO("allocate new page by extending bufferpool. buffer_pool_id=%d, pageNum=%d, pin=%d",
           id(), page_num, allocated_frame->pin_count());

  file_header_->allocated_pages++;
  file_header_->page_count++;

  byte = page_num / 8;
  bit  = page_num % 8;
  file_header_->bitmap[byte] |= (1 << bit);
  hdr_frame_->mark_dirty();

  allocated_frame->set_buffer_pool_id(id());
  allocated_frame->access();
  allocated_frame->clear_page();
  allocated_frame->set_page_num(file_header_->page_count - 1);

  // Use flush operation to extension file
  if ((rc = flush_page_internal(*allocated_frame, FlushReason::OTHER)) != RC::SUCCESS) {
    LOG_WARN("Failed to alloc page %s , due to failed to extend one page.", file_name_.c_str());
    // skip return false, delay flush the extended page
    // return tmp;
  }

  lock_.unlock();

  *frame = allocated_frame;
  stats_.record_pin(allocated_frame->pin_count() == 1);
  frame_manager_.stats().record_page_allocation();
  stats_.record_page_allocation();
  LOG_TRACE("[BUFFER_POOL_TRACE] event=ALLOCATE event_seq=%llu buffer_pool_id=%d page_num=%d reused=0",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()), id(), page_num);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=PIN event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
      allocated_frame->frame_id().to_string().c_str(), id(), page_num, allocated_frame->pin_count());
  return RC::SUCCESS;
}

RC DiskBufferPool::dispose_page(PageNum page_num)
{
  if (page_num == 0) {
    LOG_ERROR("Failed to dispose page %d, because it is the first page. filename=%s", page_num, file_name_.c_str());
    return RC::INTERNAL;
  }
  
  scoped_lock lock_guard(lock_);
  Frame           *used_frame = frame_manager_.get(id(), page_num);
  if (used_frame != nullptr) {
    ASSERT("the page try to dispose is in use. frame:%s", used_frame->to_string().c_str());
    frame_manager_.free(id(), page_num, used_frame);
  } else {
    LOG_DEBUG("page not found in memory while disposing it. pageNum=%d", page_num);
  }

  LSN lsn = 0;
  RC rc = log_handler_.deallocate_page(page_num, lsn);
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to log deallocate page %d, rc=%s", page_num, strrc(rc));
    // ignore error handle
  }

  hdr_frame_->set_lsn(lsn);
  hdr_frame_->mark_dirty();
  file_header_->allocated_pages--;
  char tmp = 1 << (page_num % 8);
  file_header_->bitmap[page_num / 8] &= ~tmp;
  frame_manager_.stats().record_page_disposal();
  stats_.record_page_disposal();
  LOG_TRACE("[BUFFER_POOL_TRACE] event=DISPOSE event_seq=%llu buffer_pool_id=%d page_num=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()), id(), page_num);
  return RC::SUCCESS;
}

RC DiskBufferPool::unpin_page(Frame *frame)
{
  const int pin_count = frame->unpin();
  stats_.record_unpin(pin_count == 0);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=UNPIN event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
      frame->frame_id().to_string().c_str(), frame->buffer_pool_id(), frame->page_num(), pin_count);
  return RC::SUCCESS;
}

RC DiskBufferPool::purge_frame(PageNum page_num, Frame *buf, FlushReason reason)
{
  if (buf->pin_count() != 1) {
    LOG_INFO("Begin to free page %d frame_id=%s, but it's pin count > 1:%d.",
        buf->page_num(), buf->frame_id().to_string().c_str(), buf->pin_count());
    return RC::LOCKED_UNLOCK;
  }

  if (buf->dirty()) {
    RC rc = flush_page_internal(*buf, reason);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to flush page %d frame_id=%s during purge page.", buf->page_num(), buf->frame_id().to_string().c_str());
      return rc;
    }
  }

  LOG_DEBUG("Successfully purge frame =%p, page %d frame_id=%s", buf, buf->page_num(), buf->frame_id().to_string().c_str());
  frame_manager_.free(id(), page_num, buf);
  return RC::SUCCESS;
}

RC DiskBufferPool::purge_page(PageNum page_num)
{
  scoped_lock lock_guard(lock_);

  Frame           *used_frame = frame_manager_.get(id(), page_num);
  if (used_frame != nullptr) {
    return purge_frame(page_num, used_frame, FlushReason::EXPLICIT);
  }

  return RC::SUCCESS;
}

RC DiskBufferPool::purge_all_pages(FlushReason reason)
{
  list<Frame *> used = frame_manager_.find_list(id());

  scoped_lock lock_guard(lock_);
  for (list<Frame *>::iterator it = used.begin(); it != used.end(); ++it) {
    Frame *frame = *it;

    purge_frame(frame->page_num(), frame, reason);
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::check_all_pages_unpinned()
{
  list<Frame *> frames = frame_manager_.find_list(id());

  scoped_lock lock_guard(lock_);
  for (Frame *frame : frames) {
    frame->unpin();
    if (frame->page_num() == BP_HEADER_PAGE && frame->pin_count() > 1) {
      LOG_WARN("This page has been pinned. id=%d, pageNum:%d, pin count=%d",
          id(), frame->page_num(), frame->pin_count());
    } else if (frame->page_num() != BP_HEADER_PAGE && frame->pin_count() > 0) {
      LOG_WARN("This page has been pinned. id=%d, pageNum:%d, pin count=%d",
          id(), frame->page_num(), frame->pin_count());
    }
  }
  LOG_INFO("all pages have been checked of id %d", id());
  return RC::SUCCESS;
}

RC DiskBufferPool::flush_page(Frame &frame, FlushReason reason)
{
  scoped_lock lock_guard(lock_);
  return flush_page_internal(frame, reason);
}

RC DiskBufferPool::flush_page_internal(Frame &frame, FlushReason reason)
{
  // The better way is use mmap the block into memory,
  // so it is easier to flush data to file.

  const auto flush_start = std::chrono::steady_clock::now();
  RC rc = log_handler_.flush_page(frame.page());
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to log flush frame= %s, rc=%s", frame.to_string().c_str(), strrc(rc));
    // ignore error handle
  }

  frame.set_check_sum(crc32(frame.page().data, BP_PAGE_DATA_SIZE));

  rc = dblwr_manager_.add_page(this, frame.page_num(), frame.page());
  if (OB_FAIL(rc)) {
    return rc;
  }

  frame.clear_dirty();
  const uint64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - flush_start).count();
  frame_manager_.stats().record_flush(reason, duration_ns);
  stats_.record_flush(reason, duration_ns);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=FLUSH event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d flush_reason=%s duration_ns=%llu",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()), frame.frame_id().to_string().c_str(),
      frame.buffer_pool_id(), frame.page_num(), flush_reason_name(reason),
      static_cast<unsigned long long>(duration_ns));
  LOG_DEBUG("Flush block. file desc=%d, frame=%s", file_desc_, frame.to_string().c_str());

  return RC::SUCCESS;
}

RC DiskBufferPool::flush_all_pages()
{
  list<Frame *> used = frame_manager_.find_list(id());
  for (Frame *frame : used) {
    RC rc = flush_page(*frame, FlushReason::EXPLICIT);
    frame->unpin();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to flush all pages");
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::flush_dirty_pages(size_t max_pages, DirtyPageFlushResult &result, FlushReason reason)
{
  result = {};
  RC first_error = RC::SUCCESS;
  list<Frame *> frames = frame_manager_.find_list(id());

  for (Frame *frame : frames) {
    const bool limit_reached = max_pages > 0 && result.flushed_count >= max_pages;
    if (!frame->dirty() || limit_reached) {
      frame->unpin();
      continue;
    }

    // find_list 为快照临时增加了一个 pin，因此 1 表示此前没有业务持有者。
    if (frame->pin_count() != 1) {
      ++result.skipped_pinned_count;
      frame->unpin();
      continue;
    }

    RC rc = flush_page(*frame, reason);
    frame->unpin();
    if (rc == RC::SUCCESS) {
      ++result.flushed_count;
    } else {
      ++result.failed_count;
      if (first_error == RC::SUCCESS) {
        first_error = rc;
      }
    }
  }
  return first_error;
}

RC DiskBufferPool::recover_page(PageNum page_num)
{
  int byte = 0, bit = 0;
  byte = page_num / 8;
  bit  = page_num % 8;

  scoped_lock lock_guard(lock_);
  if (!(file_header_->bitmap[byte] & (1 << bit))) {
    file_header_->bitmap[byte] |= (1 << bit);
    file_header_->allocated_pages++;
    file_header_->page_count++;
    hdr_frame_->mark_dirty();
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::write_page(PageNum page_num, Page &page)
{
  scoped_lock lock_guard(wr_lock_);
  const auto write_start = std::chrono::steady_clock::now();
  RC rc = io_backend_->write_page(file_desc_, file_name_.c_str(), page_num, page);
  const uint64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - write_start).count();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  frame_manager_.stats().record_disk_write(BP_PAGE_SIZE, duration_ns);
  stats_.record_disk_write(BP_PAGE_SIZE, duration_ns);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=DISK_WRITE event_seq=%llu io_backend=%s buffer_pool_id=%d page_num=%d bytes=%d duration_ns=%llu",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()), page_io_backend_name(io_backend_type()),
      id(), page_num, BP_PAGE_SIZE, static_cast<unsigned long long>(duration_ns));
  LOG_TRACE("write_page: buffer_pool_id:%d, page_num:%d, lsn=%d, check_sum=%d", id(), page_num, page.lsn, page.check_sum);
  return RC::SUCCESS;
}

RC DiskBufferPool::redo_allocate_page(LSN lsn, PageNum page_num)
{
  if (hdr_frame_->lsn() >= lsn) {
    return RC::SUCCESS;
  }

  // scoped_lock lock_guard(lock_); // redo 过程中可以不加锁
  if (page_num < file_header_->page_count) {
    Bitmap bitmap(file_header_->bitmap, file_header_->page_count);
    if (bitmap.get_bit(page_num)) {
      LOG_WARN("page %d has been allocated. file=%s", page_num, file_name_.c_str());
      return RC::SUCCESS;
    }

    bitmap.set_bit(page_num);
    file_header_->allocated_pages++;
    hdr_frame_->mark_dirty();
    return RC::SUCCESS;
  }

  if (page_num > file_header_->page_count) {
    LOG_WARN("page %d is not continuous. file=%s, page_count=%d",
             page_num, file_name_.c_str(), file_header_->page_count);
    return RC::INTERNAL;
  }

  // page_num == file_header_->page_count
  if (file_header_->page_count >= BPFileHeader::MAX_PAGE_NUM) {
    LOG_WARN("file buffer pool is full. page count %d, max page count %d",
        file_header_->page_count, BPFileHeader::MAX_PAGE_NUM);
    return RC::INTERNAL;
  }

  file_header_->allocated_pages++;
  file_header_->page_count++;
  hdr_frame_->set_lsn(lsn);
  hdr_frame_->mark_dirty();
  
  // TODO 应该检查文件是否足够大，包含了当前新分配的页面

  Bitmap bitmap(file_header_->bitmap, file_header_->page_count);
  bitmap.set_bit(page_num);
  LOG_TRACE("[redo] allocate new page. file=%s, pageNum=%d", file_name_.c_str(), page_num);
  return RC::SUCCESS;
}

RC DiskBufferPool::redo_deallocate_page(LSN lsn, PageNum page_num)
{
  if (hdr_frame_->lsn() >= lsn) {
    return RC::SUCCESS;
  }

  if (page_num >= file_header_->page_count) {
    LOG_WARN("page %d is not exist. file=%s", page_num, file_name_.c_str());
    return RC::INTERNAL;
  }

  Bitmap bitmap(file_header_->bitmap, file_header_->page_count);
  if (!bitmap.get_bit(page_num)) {
    LOG_WARN("page %d has been deallocated. file=%s", page_num, file_name_.c_str());
    return RC::INTERNAL;
  }

  bitmap.clear_bit(page_num);
  file_header_->allocated_pages--;
  hdr_frame_->set_lsn(lsn);
  hdr_frame_->mark_dirty();
  LOG_TRACE("[redo] deallocate page. file=%s, pageNum=%d", file_name_.c_str(), page_num);
  return RC::SUCCESS;
}

RC DiskBufferPool::allocate_frame(PageNum page_num, Frame **buffer)
{
  auto purger = [this](Frame *frame) {
    const bool dirty = frame->dirty();
    if (!frame->dirty()) {
      DiskBufferPool *victim_pool = nullptr;
      if (frame->buffer_pool_id() == id()) {
        victim_pool = this;
      } else {
        bp_manager_.get_buffer_pool(frame->buffer_pool_id(), victim_pool);
      }
      if (victim_pool != nullptr) {
        victim_pool->stats_.record_eviction(false);
      }
      return RC::SUCCESS;
    }

    RC rc = RC::SUCCESS;
    if (frame->buffer_pool_id() == id()) {
      rc = this->flush_page_internal(*frame, FlushReason::EVICTION);
    } else {
      rc = bp_manager_.flush_page(*frame, FlushReason::EVICTION);
    }

    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to aclloc block due to failed to flush old block. rc=%s", strrc(rc));
    } else {
      DiskBufferPool *victim_pool = nullptr;
      if (frame->buffer_pool_id() == id()) {
        victim_pool = this;
      } else {
        bp_manager_.get_buffer_pool(frame->buffer_pool_id(), victim_pool);
      }
      if (victim_pool != nullptr) {
        victim_pool->stats_.record_eviction(dirty);
      }
    }
    return rc;
  };

  while (true) {
    Frame *frame = frame_manager_.alloc(id(), page_num);
    if (frame != nullptr) {
      *buffer = frame;
      LOG_DEBUG("allocate frame %p, page num %d, frame=%s", frame, page_num, frame->to_string().c_str());
      return RC::SUCCESS;
    }

    LOG_TRACE("frames are all allocated, so we should purge some frames to get one free frame");
    const int purged = frame_manager_.purge_frames(1 /*count*/, purger);
    if (purged == 0) {
      frame_manager_.stats().record_no_buffer_failure();
      stats_.record_no_buffer_failure();
      const BufferPoolSnapshot state = frame_manager_.snapshot();
      LOG_TRACE("[BUFFER_POOL_TRACE] event=NO_BUFFER event_seq=%llu policy=%s io_backend=%s capacity=%zu used=%zu pinned_frames=%zu dirty_frames=%zu",
          static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
          state.replacement_policy.c_str(), page_io_backend_name(io_backend_type()), state.capacity,
          state.used_frames, state.pinned_frames, state.dirty_frames);
      LOG_WARN("failed to allocate frame: all frames are pinned or cannot be flushed. capacity=%zu used=%zu pinned=%zu dirty=%zu",
          state.capacity, state.used_frames, state.pinned_frames, state.dirty_frames);
      log_pinned_frames("no-buffer", false);
      return RC::BUFFERPOOL_NOBUF;
    }
  }
  return RC::BUFFERPOOL_NOBUF;
}

RC DiskBufferPool::check_page_num(PageNum page_num)
{
  if (page_num >= file_header_->page_count) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_name_.c_str());
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if ((file_header_->bitmap[page_num / 8] & (1 << (page_num % 8))) == 0) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_name_.c_str());
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::load_page(PageNum page_num, Frame *frame)
{
  Page &page = frame->page();
  RC rc = dblwr_manager_.read_page(this, page_num, page);
  if (OB_SUCC(rc)) {
    return rc;
  }

  scoped_lock lock_guard(wr_lock_);
  const auto read_start = std::chrono::steady_clock::now();
  rc = io_backend_->read_page(file_desc_, file_name_.c_str(), page_num, page);
  const uint64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - read_start).count();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  frame_manager_.stats().record_disk_read(BP_PAGE_SIZE, duration_ns);
  stats_.record_disk_read(BP_PAGE_SIZE, duration_ns);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=DISK_READ event_seq=%llu io_backend=%s buffer_pool_id=%d page_num=%d bytes=%d duration_ns=%llu",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()), page_io_backend_name(io_backend_type()),
      id(), page_num, BP_PAGE_SIZE, static_cast<unsigned long long>(duration_ns));
  frame->set_page_num(page_num);

  LOG_DEBUG("Load page %s:%d, file_desc:%d, frame=%s",
            file_name_.c_str(), page_num, file_desc_, frame->to_string().c_str());
  return RC::SUCCESS;
}

int DiskBufferPool::file_desc() const { return file_desc_; }

BufferPoolSnapshot DiskBufferPool::snapshot() const
{
  BufferPoolSnapshot result = frame_manager_.snapshot(id());
  result.buffer_pool_id = id();
  result.file_name = file_name_;
  result.io_backend = page_io_backend_name(io_backend_type());
  result.stats = stats_.snapshot();
  // 当前值由 Frame 快照计算，比仅在 DiskBufferPool 公共入口统计更准确。
  result.stats.current_pinned_frames = result.pinned_frames;
  result.stats.dirty_pages_current = result.dirty_frames;
  return result;
}

void DiskBufferPool::log_pinned_frames(const char *context, bool warning) const
{
  const BufferPoolSnapshot state = snapshot();
  if (state.pinned_frames == 0) {
    return;
  }

  constexpr size_t MAX_DIAGNOSTIC_FRAMES = 8;
  size_t emitted = 0;
  for (const FrameSnapshot &frame : state.frames) {
    if (frame.pin_count <= 0 || emitted >= MAX_DIAGNOSTIC_FRAMES) {
      continue;
    }
    if (warning) {
      LOG_WARN("buffer pool pinned frame diagnostic. context=%s frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d dirty=%d",
          context, frame.frame_id.c_str(), frame.buffer_pool_id, frame.page_num, frame.pin_count, frame.dirty);
    } else {
      LOG_TRACE("[BUFFER_POOL_TRACE] event=PINNED_DIAGNOSTIC event_seq=%llu context=%s frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d dirty=%d",
          static_cast<unsigned long long>(next_buffer_pool_event_sequence()), context, frame.frame_id.c_str(),
          frame.buffer_pool_id, frame.page_num, frame.pin_count, frame.dirty);
    }
    ++emitted;
  }
}

////////////////////////////////////////////////////////////////////////////////
BufferPoolManager::BufferPoolManager(
    int memory_size /* = 0 */, BufferPoolReplacementPolicy replacement_policy, PageIOBackendType io_backend_type)
    : frame_manager_("BufPool", replacement_policy), io_backend_type_(io_backend_type)
{
  if (memory_size <= 0) {
    memory_size = MEM_POOL_ITEM_NUM * DEFAULT_ITEM_NUM_PER_POOL * BP_PAGE_SIZE;
  }
  const int page_num = max(memory_size / BP_PAGE_SIZE, 1);
  frame_manager_.init(1, page_num);
  LOG_INFO("buffer pool manager init with memory size %d, page num: %d, policy: %s, io_backend: %s",
      memory_size, page_num, buffer_pool_replacement_policy_name(replacement_policy),
      page_io_backend_name(io_backend_type));
}

BufferPoolManager::~BufferPoolManager()
{
  unordered_map<string, DiskBufferPool *> tmp_bps;
  tmp_bps.swap(buffer_pools_);

  for (auto &iter : tmp_bps) {
    delete iter.second;
  }

  LOG_INFO("[BUFFER_POOL_STATS] policy=%s,io_backend=%s,%s",
      buffer_pool_replacement_policy_name(replacement_policy()), page_io_backend_name(io_backend_type_),
      stats().to_string().c_str());
}

RC BufferPoolManager::init(unique_ptr<DoubleWriteBuffer> dblwr_buffer)
{
  dblwr_buffer_ = std::move(dblwr_buffer);
  return RC::SUCCESS;
}

RC BufferPoolManager::create_file(const char *file_name)
{
  int fd = open(file_name, O_RDWR | O_CREAT | O_EXCL, S_IREAD | S_IWRITE);
  if (fd < 0) {
    LOG_ERROR("Failed to create %s, due to %s.", file_name, strerror(errno));
    return RC::SCHEMA_DB_EXIST;
  }

  close(fd);

  /**
   * Here don't care about the failure
   */
  fd = open(file_name, O_RDWR);
  if (fd < 0) {
    LOG_ERROR("Failed to open for readwrite %s, due to %s.", file_name, strerror(errno));
    return RC::IOERR_ACCESS;
  }

  Page page;
  memset(&page, 0, BP_PAGE_SIZE);

  BPFileHeader *file_header    = (BPFileHeader *)page.data;
  file_header->allocated_pages = 1;
  file_header->page_count      = 1;
  file_header->buffer_pool_id  = next_buffer_pool_id_.fetch_add(1);

  char *bitmap = file_header->bitmap;
  bitmap[0] |= 0x01;
  unique_ptr<PageIOBackend> io_backend = create_page_io_backend(io_backend_type_);
  const auto write_start = std::chrono::steady_clock::now();
  RC rc = io_backend->write_page(fd, file_name, BP_HEADER_PAGE, page);
  const uint64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - write_start).count();
  if (rc != RC::SUCCESS) {
    close(fd);
    return rc;
  }
  frame_manager_.stats().record_disk_write(BP_PAGE_SIZE, duration_ns);

  close(fd);
  LOG_INFO("Successfully create %s.", file_name);
  return RC::SUCCESS;
}

RC BufferPoolManager::open_file(LogHandler &log_handler, const char *_file_name, DiskBufferPool *&_bp)
{
  string file_name(_file_name);

  scoped_lock lock_guard(lock_);
  if (buffer_pools_.find(file_name) != buffer_pools_.end()) {
    LOG_WARN("file already opened. file name=%s", _file_name);
    return RC::BUFFERPOOL_OPEN;
  }

  DiskBufferPool *bp = new DiskBufferPool(*this, frame_manager_, *dblwr_buffer_, log_handler, io_backend_type_);
  RC              rc = bp->open_file(_file_name);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open file name");
    delete bp;
    return rc;
  }

  if (bp->id() >= next_buffer_pool_id_.load()) {
    next_buffer_pool_id_.store(bp->id() + 1);
  }

  buffer_pools_.insert(pair<string, DiskBufferPool *>(file_name, bp));
  id_to_buffer_pools_.insert(pair<int32_t, DiskBufferPool *>(bp->id(), bp));
  LOG_DEBUG("insert buffer pool into fd buffer pools. fd=%d, bp=%p, lbt=%s", bp->file_desc(), bp, lbt());
  _bp = bp;
  return RC::SUCCESS;
}

RC BufferPoolManager::close_file(const char *_file_name)
{
  string file_name(_file_name);

  lock_.lock();

  auto iter = buffer_pools_.find(file_name);
  if (iter == buffer_pools_.end()) {
    LOG_TRACE("file has not opened: %s", _file_name);
    lock_.unlock();
    return RC::INTERNAL;
  }

  id_to_buffer_pools_.erase(iter->second->id());

  DiskBufferPool *bp = iter->second;
  buffer_pools_.erase(iter);
  lock_.unlock();

  delete bp;
  return RC::SUCCESS;
}

RC BufferPoolManager::flush_page(Frame &frame, FlushReason reason)
{
  int buffer_pool_id = frame.buffer_pool_id();

  scoped_lock lock_guard(lock_);
  auto             iter = id_to_buffer_pools_.find(buffer_pool_id);
  if (iter == id_to_buffer_pools_.end()) {
    LOG_WARN("unknown buffer pool of id %d", buffer_pool_id);
    return RC::INTERNAL;
  }

  DiskBufferPool *bp = iter->second;
  return bp->flush_page(frame, reason);
}

BufferPoolSnapshot BufferPoolManager::snapshot() const
{
  BufferPoolSnapshot result = frame_manager_.snapshot();
  result.io_backend = page_io_backend_name(io_backend_type_);
  return result;
}

RC BufferPoolManager::get_buffer_pool(int32_t id, DiskBufferPool *&bp)
{
  bp = nullptr;

  scoped_lock lock_guard(lock_);

  auto iter = id_to_buffer_pools_.find(id);
  if (iter == id_to_buffer_pools_.end()) {
    LOG_WARN("unknown buffer pool of id %d", id);
    return RC::INTERNAL;
  }
  
  bp = iter->second;
  return RC::SUCCESS;
}
