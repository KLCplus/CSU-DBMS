/**
 * @file disk_buffer_pool.cpp
 * @brief 分页文件缓存的实现：Frame 管理、页的读入与刷出、页的分配与释放
 * @ingroup BufferPool
 * @details 前半部分是 BPFrameManager，负责全局唯一的 Frame 缓存与淘汰；
 * 后半部分是 DiskBufferPool 与 BufferPoolManager，负责具体的分页文件。
 */

#include <errno.h>
#include <string.h>
#include <chrono>

#include "common/io/io.h"
#include "common/lang/mutex.h"
#include "common/lang/algorithm.h"
#include "common/log/log.h"
#include "common/math/crc.h"
#include "storage/os/buffer/disk_buffer_pool.h"
#include "storage/os/diagnostics/buffer_pool_log.h"
#include "storage/db/db.h"

using namespace common;

/// 内存池每个分组预留的 Frame 个数，与 --buffer-size 一起决定缓存容量
static const int MEM_POOL_ITEM_NUM = 20;

////////////////////////////////////////////////////////////////////////////////

/**
 * @brief 转为调试字符串
 * @return 包含页总数与已分配页数的字符串
 */
string BPFileHeader::to_string() const
{
  stringstream ss;
  ss << "pageCount:" << page_count << ", allocatedCount:" << allocated_pages;
  return ss.str();
}

////////////////////////////////////////////////////////////////////////////////

/**
 * @brief 构造 Frame 管理器
 * @param name 内存池标识，用于日志
 * @param replacement_policy 淘汰策略类型，这里创建对应的策略对象
 */
BPFrameManager::BPFrameManager(const char *name, BufferPoolReplacementPolicy replacement_policy)
    : allocator_(name),
      replacement_policy_type_(replacement_policy),
      replacement_policy_(create_replacement_policy(replacement_policy))
{}

/**
 * @brief 初始化 Frame 内存池并计算缓存容量
 * @details 缓存容量等于分组个数乘以每组预留个数，同时也是内存池能提供的 Frame 总数。
 * @param pool_num 分组个数
 * @param item_num_per_pool 每组预留的 Frame 个数
 * @return 内存池初始化失败时返回 NOMEM
 */
RC BPFrameManager::init(int pool_num, int item_num_per_pool)
{
  int ret = allocator_.init(false, pool_num, item_num_per_pool);
  if (ret == 0) {
    capacity_ = static_cast<size_t>(pool_num) * static_cast<size_t>(item_num_per_pool);
    return RC::SUCCESS;
  }
  return RC::NOMEM;
}

/**
 * @brief 清空缓存并回收内存池
 * @details 缓存中仍有 Frame 时拒绝清理并返回 INTERNAL，避免丢掉正在使用的页。
 */
RC BPFrameManager::cleanup()
{
  if (frames_.count() > 0) {
    return RC::INTERNAL;
  }

  frames_.destroy();
  return RC::SUCCESS;
}

/**
 * @brief 挑选并回收若干可淘汰的 Frame
 * @details 分两个阶段：
 * 1. 持锁阶段：把 is_replaceable 回调交给替换策略，由策略按自己的规则选 victim；
 *    选出的 victim 立刻 pin 住，防止在随后的刷盘过程中被别人抢走。
 * 2. 回收阶段：对每个 victim 调用调用方传入的 purger 完成刷盘，成功后才从缓存中真正移除。
 * 把刷盘做成回调，是为了让 FrameManager 不依赖磁盘：它只负责挑页与回收，
 * 具体怎么把脏页写下去由 DiskBufferPool 决定。
 * @param count 期望回收的个数，小于等于 0 时按 1 处理
 * @param purger 回收前的处理动作，当前是把脏页刷到磁盘
 * @return 实际回收成功的 Frame 个数
 * @note 已知问题：purger 是耗时的磁盘 I/O，却仍在 lock_ 的保护范围内执行，
 * 会明显降低并发度，属于待优化的点。
 */
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

/**
 * @brief 按分页文件 id 与页号查找 Frame
 * @details 加锁后转发给 get_internal，是外部拿到 Frame 的主要入口。
 * @return 命中返回已 pin 的 Frame，未命中返回 nullptr
 */
Frame *BPFrameManager::get(int buffer_pool_id, PageNum page_num)
{
  FrameId                     frame_id(buffer_pool_id, page_num);

  lock_guard<mutex> lock_guard(lock_);
  return get_internal(frame_id);
}

/**
 * @brief 在已持锁的前提下按 FrameId 查找 Frame
 * @details 命中后的动作顺序有讲究：先 on_access 更新策略中的访问记录，再 pin 住，
 * 最后 on_pin 通知策略，保证策略看到的 pin 状态与实际一致。
 * @param frame_id 页标识
 * @return 命中返回 Frame，未命中返回 nullptr
 */
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

/**
 * @brief 申请一个 Frame，已在缓存中则直接返回
 * @details 未命中时从内存池取一个 Frame，设置 pool id、页号与统计对象，pin 住后放入缓存，
 * 并依次通知策略 on_insert 与 on_pin。
 * 内存池耗尽时返回 nullptr，由调用方决定是否先淘汰一些旧 Frame。
 * @param buffer_pool_id 分页文件 id
 * @param page_num 页号
 * @return 已 pin 的 Frame；内存池无可用对象时返回 nullptr
 */
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

/**
 * @brief 回收一个 Frame
 * @details 加锁后转发给 free_internal。
 * @note Frame 自身已记录 pool id 与页号，这里仍要求调用方显式传入，
 * 因为 Frame 可能尚未初始化或被改写过，参数比对象本身更可信。
 */
RC BPFrameManager::free(int buffer_pool_id, PageNum page_num, Frame *frame)
{
  FrameId frame_id(buffer_pool_id, page_num);

  lock_guard<mutex> lock_guard(lock_);
  return free_internal(frame_id, frame);
}

/**
 * @brief 在已持锁的前提下回收一个 Frame
 * @details 断言保证三件事：该 Frame 仍在缓存中、传进来的是缓存里那一个、
 * 并且引用计数正好为 1。也就是要求调用方持有这个 Frame 的最后一次 pin，
 * 否则说明有人在用它却被回收了。
 * 之后依次清脏标记、清页号、递减计数、通知策略、从缓存与内存池中移除。
 */
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

/**
 * @brief 列出某个分页文件当前驻留在缓存中的全部 Frame
 * @details 返回前逐个 pin 住，调用方用完必须自行 unpin。
 * @param buffer_pool_id 分页文件 id
 * @return 已 pin 的 Frame 列表
 */
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

/**
 * @brief 把当前 Frame 状态复制成只读 DTO
 * @details 只复制值，不暴露任何 Frame 指针、Page 指针或内部容器，因此结果可以安全地
 * 传给 CLI 与 Web。lock_ 在此是只读语义，故用 const_cast 取得可变引用再加锁。
 * @param buffer_pool_id 只统计该分页文件；-1 表示汇总全部文件
 * @return 含容量、使用量、策略、统计与 Frame 列表的快照
 */
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
/**
 * @brief BufferPoolIterator 的构造与析构
 * @details 构造后 bitmap_ 为空，需要先调用 init 才有可遍历的页号。
 */
BufferPoolIterator::BufferPoolIterator() {}
BufferPoolIterator::~BufferPoolIterator() {}
/**
 * @brief 用分页文件的分配位图初始化迭代器
 * @details 位图是指向文件头内部数据的，不是拷贝，因此遍历结果会随文件头的分配状态实时变化。
 * start_page 为 0 时从第 0 页开始，否则从 start_page 的前一页之后开始。
 */
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

/** @brief 判断后面是否还有已分配的页 */
bool BufferPoolIterator::has_next() { return bitmap_.next_setted_bit(current_page_num_ + 1) != -1; }

/**
 * @brief 返回下一个已分配的页号
 * @return 已分配的页号；没有下一页时返回 -1，此时不推进游标
 */
PageNum BufferPoolIterator::next()
{
  PageNum next_page = bitmap_.next_setted_bit(current_page_num_ + 1);
  if (next_page != -1) {
    current_page_num_ = next_page;
  }
  return next_page;
}

/**
 * @brief 重置遍历位置
 * @note 这里把游标置为 0，而 init 传入 0 时置为 -1，两者的起点并不一致。
 * 当前源码中没有调用者，属于遗留接口。
 */
RC BufferPoolIterator::reset()
{
  current_page_num_ = 0;
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
/**
 * @brief 构造一个分页文件对象
 * @details 只保存各个管理器的引用并创建 I/O 后端，真正的文件在 open_file 中打开。
 * @param bp_manager 所属的 BufferPoolManager
 * @param frame_manager 全局共享的 Frame 管理器
 * @param dblwr_manager 双写缓冲
 * @param log_handler 日志处理器，本对象会用它构造自己的 BufferPoolLogHandler
 * @param io_backend_type I/O 后端类型
 */
DiskBufferPool::DiskBufferPool(
    BufferPoolManager &bp_manager, BPFrameManager &frame_manager, DoubleWriteBuffer &dblwr_manager,
    LogHandler &log_handler, PageIOBackendType io_backend_type)
    : bp_manager_(bp_manager),
      frame_manager_(frame_manager),
      dblwr_manager_(dblwr_manager),
      log_handler_(*this, log_handler),
      io_backend_(create_page_io_backend(io_backend_type))
{}

/** @brief 析构：先关闭文件，把页与关联状态都收拾干净 */
DiskBufferPool::~DiskBufferPool()
{
  close_file();
  LOG_INFO("disk buffer pool exit");
}

/**
 * @brief 打开一个分页文件并常驻其文件头页
 * @details 流程是：打开文件描述符，用 I/O 后端读第 0 页，从页中解析出 buffer_pool_id，
 * 为该页申请一个 Frame 并置为常驻，最后把 file_header_ 指向该 Frame 的 data 区。
 * 此后对分配位图的所有访问都通过 file_header_ 直接改内存，靠 mark_dirty 落盘。
 * @param file_name 分页文件路径
 */
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

/**
 * @brief 关闭分页文件
 * @details 依次完成：解掉文件头页的 pin、做一次 pin 泄漏检查、把全部页按 SHUTDOWN 原因刷回、
 * 清掉双写缓冲中与本文档相关的页、清空已释放页集合、关闭文件描述符，
 * 最后通知 BufferPoolManager 从两张表中移除自己。
 */
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

/**
 * @brief 取指定页到缓冲区并 pin 住，是上层读取页面的唯一入口
 * @details 命中与未命中两条路径：
 * 命中时只做统计与 access 更新，直接返回；未命中时先不持文件锁做一次乐观查找，
 * 拿到锁后再复查一次，避免两个线程同时把同一页装载出两个 Frame，然后申请 Frame 并读盘。
 * @param page_num 页号
 * @param frame 输出参数，返回已 pin 的 Frame
 * @note 调用方使用完毕必须 unpin_page。未命中路径目前是整文件一把大锁，
 * 源码注释里也承认可以按页细化以提高并行度。
 */
RC DiskBufferPool::get_this_page(PageNum page_num, Frame **frame)
{
  RC rc  = RC::SUCCESS;
  *frame = nullptr;
  if ((rc = check_page_num(page_num)) != RC::SUCCESS) {
    return rc;
  }

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

  if ((rc = check_page_num(page_num)) != RC::SUCCESS) {
    return rc;
  }

  // Another thread may have loaded the same page between the optimistic lookup
  // and acquiring the file lock. Reuse that frame instead of creating a duplicate.
  used_match_frame = frame_manager_.get(id(), page_num);
  if (used_match_frame != nullptr) {
    used_match_frame->access();
    stats_.record_pin(used_match_frame->pin_count() == 1);
    *frame = used_match_frame;
    return RC::SUCCESS;
  }

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

/**
 * @brief 分配一个新页并 pin 住
 * @details 优先复用文件中的空洞：扫描分配位图找到第一个未分配的页，
 * 先写分配日志并更新位图，再把该页读进内存、整页清零。
 * 没有空洞时扩展文件：页数达到 MAX_PAGE_NUM 则返回 BUFFERPOOL_NOBUF，
 * 否则在文件末尾新增一页并把它写出去以扩展文件长度。
 * @param frame 输出参数，返回已 pin 的新页 Frame
 * @note 新分配的页一定会被整页清零，调用方拿到的绝不是上一任使用者的残留数据。
 */
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
        LSN lsn = 0;
        rc = log_handler_.allocate_page(i, lsn);
        if (OB_FAIL(rc)) {
          LOG_ERROR("Failed to log allocate page %d, rc=%s", i, strrc(rc));
          lock_.unlock();
          return rc;
        }

        (file_header_->allocated_pages)++;
        file_header_->bitmap[byte] |= (1 << bit);
        hdr_frame_->mark_dirty();
        hdr_frame_->set_lsn(lsn);

        frame_manager_.stats().record_page_allocation();
        stats_.record_page_allocation();
        LOG_TRACE("[BUFFER_POOL_TRACE] event=ALLOCATE event_seq=%llu buffer_pool_id=%d page_num=%d reused=1",
            static_cast<unsigned long long>(next_buffer_pool_event_sequence()), id(), i);

        LOG_DEBUG("allocate a new page without extend buffer pool. page num=%d, buffer pool=%d", i, id());

        lock_.unlock();
        rc = get_this_page(i, frame);
        if (OB_FAIL(rc)) {
          return rc;
        }
        (*frame)->clear_page();
        (*frame)->set_lsn(lsn);
        (*frame)->mark_dirty();
        return RC::SUCCESS;
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
    lock_.unlock();
    return rc;
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

/**
 * @brief 释放一个页，把它在文件头标为未分配
 * @details 第 0 页是文件头，永远不允许释放。释放前会检查该页在内存中的引用计数：
 * get 本身会带来一个临时 pin，计数不为 1 说明还有扫描器或其他使用者持有它，
 * 这时直接拒绝释放并返回 LOCKED_UNLOCK，避免留下悬空句柄。
 * 随后写释放日志、把位图对应位清掉、递减 allocated_pages 并把文件头标脏。
 * @param page_num 待释放的页号
 * @note 释放页只改磁盘上的分配状态，与淘汰 Frame 是两件事。
 */
RC DiskBufferPool::dispose_page(PageNum page_num)
{
  if (page_num == 0) {
    LOG_ERROR("Failed to dispose page %d, because it is the first page. filename=%s", page_num, file_name_.c_str());
    return RC::INTERNAL;
  }
  
  scoped_lock lock_guard(lock_);
  RC validation_rc = check_page_num(page_num);
  if (validation_rc != RC::SUCCESS) {
    return validation_rc;
  }
  Frame           *used_frame = frame_manager_.get(id(), page_num);
  if (used_frame != nullptr) {
    // frame_manager_.get adds one temporary pin. More than one means a scanner or
    // another page user still owns the frame, so disposing it would leave a dangling handler.
    if (used_frame->pin_count() != 1) {
      LOG_WARN("cannot dispose pinned page. frame=%s", used_frame->to_string().c_str());
      unpin_page(used_frame);
      return RC::LOCKED_UNLOCK;
    }
  } else {
    LOG_DEBUG("page not found in memory while disposing it. pageNum=%d", page_num);
  }

  LSN lsn = 0;
  RC rc = log_handler_.deallocate_page(page_num, lsn);
  if (OB_FAIL(rc)) {
    LOG_ERROR("Failed to log deallocate page %d, rc=%s", page_num, strrc(rc));
    if (used_frame != nullptr) {
      unpin_page(used_frame);
    }
    return rc;
  }

  if (used_frame != nullptr) {
    frame_manager_.free(id(), page_num, used_frame);
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

/**
 * @brief 结束一次页面使用，递减引用计数
 * @details 计数减到 0 后这个 Frame 才重新成为可淘汰的候选，统计与 Trace 都在这里记录。
 * @param frame 之前由 get_this_page 或 allocate_page 返回的 Frame
 */
RC DiskBufferPool::unpin_page(Frame *frame)
{
  const int pin_count = frame->unpin();
  stats_.record_unpin(pin_count == 0);
  LOG_TRACE("[BUFFER_POOL_TRACE] event=UNPIN event_seq=%llu frame_id=%s buffer_pool_id=%d page_num=%d pin_count=%d",
      static_cast<unsigned long long>(next_buffer_pool_event_sequence()),
      frame->frame_id().to_string().c_str(), frame->buffer_pool_id(), frame->page_num(), pin_count);
  return RC::SUCCESS;
}

/**
 * @brief 回收一个 Frame，脏页先刷盘
 * @details 只处理引用计数正好为 1 的 Frame，也就是除本函数之外没有别的使用者；
 * 否则返回 LOCKED_UNLOCK 不做处理。脏页先走 flush_page_internal，
 * 刷盘失败就不回收，避免丢数据。
 * @param page_num 页号
 * @param buf 待回收的 Frame
 * @param reason 刷盘原因，会记入统计与 Trace
 */
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

/**
 * @brief 回收指定页对应的 Frame
 * @details 该页不在缓存中时视为成功，因为目的就是让它不在缓存里。
 */
RC DiskBufferPool::purge_page(PageNum page_num)
{
  scoped_lock lock_guard(lock_);

  Frame           *used_frame = frame_manager_.get(id(), page_num);
  if (used_frame != nullptr) {
    RC rc = purge_frame(page_num, used_frame, FlushReason::EXPLICIT);
    if (OB_FAIL(rc)) {
      unpin_page(used_frame);
    }
    return rc;
  }

  return RC::SUCCESS;
}

/**
 * @brief 回收本文件的全部 Frame，关闭文件时使用
 * @details 先经 find_list 取得全部 Frame 并逐个 pin，再放进锁内逐个 purge_frame；
 * 失败的那些要 unpin 回去，并记住第一个错误码返回，不让错误被后续成功掩盖。
 */
RC DiskBufferPool::purge_all_pages(FlushReason reason)
{
  list<Frame *> used = frame_manager_.find_list(id());

  RC first_error = RC::SUCCESS;
  scoped_lock lock_guard(lock_);
  for (Frame *frame : used) {
    RC rc = purge_frame(frame->page_num(), frame, reason);
    if (OB_FAIL(rc)) {
      unpin_page(frame);
      if (first_error == RC::SUCCESS) {
        first_error = rc;
      }
    }
  }
  return first_error;
}

/**
 * @brief 检查是否还有页被业务持有，调试与关闭前使用
 * @details 先经 find_list 取得并 pin 住全部 Frame，再逐个 unpin 掉那个临时 pin，
 * 然后看剩余计数：文件头页期望为 1，其他页期望为 0，不符合就告警。
 * 只告警，不修改引用计数。
 */
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

/**
 * @brief 刷新单个页，公开接口
 * @details 加文件锁后转发给 flush_page_internal。
 * @param frame 待刷新的 Frame
 * @param reason 刷盘原因，默认 EXPLICIT
 */
RC DiskBufferPool::flush_page(Frame &frame, FlushReason reason)
{
  scoped_lock lock_guard(lock_);
  return flush_page_internal(frame, reason);
}

/**
 * @brief 刷新单个页的内部实现
 * @details 顺序不能变：
 * 1. 先经 log_handler_ 刷日志，保证 WAL 先于数据页落盘；
 * 2. 重新计算并写入页校验和；
 * 3. 交给双写缓冲 add_page，整页先落到共享表空间，真实分页文件的写入被推迟到缓冲区成批刷出；
 * 4. 到这里才清除脏标记，因为页已经安全存在于 WAL 与双写缓冲中。
 * @param frame 待刷新的 Frame
 * @param reason 刷盘原因，会记入统计与 FLUSH Trace
 * @note 出错时只记日志不中断，但 add_page 失败会直接返回。
 */
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

/**
 * @brief 刷新本文件的所有页，包括仍被业务 pin 的页
 * @details 全部按 EXPLICIT 原因刷新，每刷完一个立刻 unpin 掉 find_list 带来的临时 pin。
 */
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

/**
 * @brief 安全地批量刷新脏页，只处理当前没有业务持有者的页
 * @details find_list 会为快照临时加一个 pin，所以引用计数为 1 才表示此前没有业务持有者；
 * 计数大于 1 的页会被跳过并计入 skipped_pinned_count，不做强制刷新。
 * @param max_pages 本次最多刷几个；为 0 表示不限制
 * @param result 输出刷成功、跳过与失败的个数
 * @param reason 刷盘原因
 * @return 第一个失败的返回码；全部成功返回 SUCCESS
 * @note 这是为将来做 checkpoint 与后台清理线程预留的入口，当前没有后台线程。
 */
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

/**
 * @brief 日志回放时处理页中被认定为已不存在的页
 * @details 如果位图中该页尚未标记为已分配，就补上位并同时增加已分配页数与总页数。
 */
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

/**
 * @brief 把一页写到目标分页文件
 * @details 经 I/O 后端落盘，成功后记录 disk_writes、字节数与写延迟，并输出 DISK_WRITE Trace。
 * 使用 wr_lock_ 而不是 lock_，串行化的是文件 I/O 本身，与文件头状态无关。
 * @param page_num 页号
 * @param page 页内容
 * @note 这是双写缓冲写回真实文件的落点，也是 WAL 与双写自身的 I/O 之外唯一会增加
 * disk_writes 的地方。
 */
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

/**
 * @brief 恢复时重做一次页分配
 * @details 幂等设计：文件头的 LSN 已经不小于本次 lsn 就直接返回，说明这条日志早已生效。
 * 页号小于当前页数说明是复用空洞；等于当前页数说明要扩展文件；
 * 大于当前页数属于日志不连续，返回 INTERNAL。
 * @param lsn 该条日志的序列号
 * @param page_num 待分配的页号
 * @note 回放阶段单线程执行，源码里明确注释可以不加锁。
 */
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

/**
 * @brief 恢复时重做一次页释放
 * @details 同样先做 LSN 幂等判断，再校验页号有效且当前确实处于已分配状态，
 * 然后清位图、递减已分配页数并把文件头标脏。
 * @param lsn 该条日志的序列号
 * @param page_num 待释放的页号
 */
RC DiskBufferPool::redo_deallocate_page(LSN lsn, PageNum page_num)
{
  if (hdr_frame_->lsn() >= lsn) {
    return RC::SUCCESS;
  }

  if (page_num < 0 || page_num >= file_header_->page_count) {
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

/**
 * @brief 申请一个已 pin 的 Frame，取不到就先淘汰再重试
 * @details purger 回调是 FrameManager 与磁盘之间的唯一桥梁：它负责把 victim 刷回磁盘。
 * 因为 Frame 池在多个分页文件间共享，victim 可能属于别的文件，这时要转交 bp_manager_ 处理，
 * 并把淘汰统计记到 victim 所属的分页文件上。
 * 主循环反复「申请 Frame，失败则淘汰一个再试」，直到成功或一次都淘汰不掉。
 * @param page_num 页号
 * @param buffer 输出参数，返回申请到的 Frame
 * @return 成功返回 SUCCESS；全部 Frame 都被 pin 且无法刷出时返回 BUFFERPOOL_NOBUF，
 * 同时记录 no_buffer_failures、输出 NO_BUFFER Trace 并打印仍被 pin 的 Frame 清单
 */
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

/**
 * @brief 校验页号是否落在有效范围内
 * @details 两道检查：页号介于 0 与 page_count 之间，且分配位图中该页已标记为已分配。
 * 任何一道不通过都返回 BUFFERPOOL_INVALID_PAGE_NUM。
 */
RC DiskBufferPool::check_page_num(PageNum page_num)
{
  if (page_num < 0 || page_num >= file_header_->page_count) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_name_.c_str());
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if ((file_header_->bitmap[page_num / 8] & (1 << (page_num % 8))) == 0) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_name_.c_str());
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  return RC::SUCCESS;
}

/**
 * @brief 把指定页的数据读进 Frame
 * @details 先问双写缓冲要，取到就直接用：那里的内容一定比目标分页文件新，
 * 因为写页时是先写双写缓冲、之后才成批写回真实文件。
 * 取不到才经 I/O 后端读目标分页文件，并记录 disk_reads、字节数与读延迟。
 * @param page_num 页号
 * @param frame 目标 Frame
 * @note 命中双写缓冲时不计入 disk_reads，这是统计口径里缓存命中不增加磁盘读的直接体现。
 */
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

/** @brief 返回底层文件描述符 */
int DiskBufferPool::file_desc() const { return file_desc_; }

/**
 * @brief 导出本分页文件的只读快照
 * @details 先取本文件相关的 Frame 快照，再补上文件维度信息；
 * pinned 与 dirty 的当前值直接用 Frame 快照算出的结果覆盖统计里的值，比只在公共入口
 * 计数更准确。
 */
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

/**
 * @brief 打印仍被 pin 的 Frame 清单，用于 pin starvation 与关闭前的泄漏检查
 * @details 没有 pinned Frame 时什么都不输出。最多列出 8 个，避免日志爆量。
 * @param context 日志前缀，说明触发场景
 * @param warning 为 true 用 WARN 级别输出，否则走 PINNED_DIAGNOSTIC Trace
 */
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
/**
 * @brief 构造 BufferPool 管理器
 * @details memory_size 小于等于 0 时取默认容量，然后换算成 Frame 个数，
 * 至少保证 1 个；Frame 管理器只分一组，容量就是全部可用的 Frame 数。
 * @param memory_size 缓存可用字节数，0 表示使用默认容量
 * @param replacement_policy 淘汰策略类型
 * @param io_backend_type I/O 后端类型
 */
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

/**
 * @brief 析构：先摘掉全部表项再逐个删除分页文件对象
 * @details 用 swap 把表挪到局部变量，避免删除过程中回调 close_file 又去改同一张表。
 * 最后打印一次全局统计汇总。
 */
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

/**
 * @brief 注入双写缓冲实现
 * @param dblwr_buffer 双写缓冲对象，真实实现或空实现
 */
RC BufferPoolManager::init(unique_ptr<DoubleWriteBuffer> dblwr_buffer)
{
  dblwr_buffer_ = std::move(dblwr_buffer);
  return RC::SUCCESS;
}

/**
 * @brief 新建一个分页文件并写入初始文件头
 * @details 先独占创建文件再重新打开，构造第 0 页：已分配页数与总页数都置 1，
 * 分配页号取自自增的 next_buffer_pool_id_，位图第 0 位置 1 表示文件头页已占用。
 * 最后经 I/O 后端把这一页写出去。
 * @param file_name 新文件的路径
 */
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

/**
 * @brief 打开一个已存在的分页文件
 * @details 已在表中则报错返回。创建 DiskBufferPool 并让它打开文件，
 * 之后同步自增 id 计数器，再把对象登记进按文件名与按 id 两张表。
 * @param log_handler 日志处理器
 * @param _file_name 文件路径
 * @param _bp 输出参数，返回分页文件对象
 * @note 返回的是裸指针，但对象所有权在 buffer_pools_ 中，调用方只是借用，不要 delete。
 */
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

/**
 * @brief 关闭并销毁一个分页文件对象
 * @details 先从两张表中摘掉自己，再 delete 触发 DiskBufferPool 的析构与 close_file。
 */
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

/**
 * @brief 按 Frame 所属的分页文件 id 转发刷新请求
 * @details 淘汰路径里 victim 可能属于别的文件，需要这个入口做一次转发。
 */
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

/**
 * @brief 导出全局快照
 * @details 汇总所有分页文件的 Frame 信息，并补上 I/O 后端类型。
 */
BufferPoolSnapshot BufferPoolManager::snapshot() const
{
  BufferPoolSnapshot result = frame_manager_.snapshot();
  result.io_backend = page_io_backend_name(io_backend_type_);
  return result;
}

/**
 * @brief 按 id 找到对应的分页文件对象
 * @details 恢复日志时需要按 id 拿到对象，再让它自己做 redo。
 * @param id 分页文件 id
 * @param bp 输出参数，返回分页文件对象
 */
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
