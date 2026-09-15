/**
 * @file frame.cpp
 * @brief FrameId 与 Frame 的实现
 * @ingroup BufferPool
 */
#include "storage/os/page/frame.h"
#include "storage/os/diagnostics/buffer_pool_stats.h"
#include "session/session.h"
#include "session/thread_data.h"

/**
 * @brief 用所属分页文件 id 与页号构造标识
 * @param buffer_pool_id 分页文件 id
 * @param page_num 页号
 */
FrameId::FrameId(int buffer_pool_id, PageNum page_num) : buffer_pool_id_(buffer_pool_id), page_num_(page_num) {}

/**
 * @brief 逐字段比较两个标识是否相同
 * @param other 另一个标识
 * @return 两个字段都相等时返回 true
 */
bool FrameId::equal_to(const FrameId &other) const
{
  return buffer_pool_id_ == other.buffer_pool_id_ && page_num_ == other.page_num_;
}

/**
 * @brief 比较运算符，转发给 equal_to
 * @param other 另一个标识
 * @return 相等返回 true
 */
bool FrameId::operator==(const FrameId &other) const { return this->equal_to(other); }

/**
 * @brief 计算哈希值
 * @details buffer_pool_id 放高 32 位、page_num 放低 32 位拼成一个 64 位数，
 * 供 FrameManager 内部的哈希表使用。
 * @return 64 位哈希值
 */
size_t FrameId::hash() const { return (static_cast<size_t>(buffer_pool_id_) << 32L) | page_num_; }

/** @brief 返回所属分页文件 id */
int     FrameId::buffer_pool_id() const { return buffer_pool_id_; }

/** @brief 返回页号 */
PageNum FrameId::page_num() const { return page_num_; }

/**
 * @brief 转为调试字符串
 * @return "buffer_pool_id:x,page_num:y" 形式的字符串
 */
string FrameId::to_string() const
{
  stringstream ss;
  ss << "buffer_pool_id:" << buffer_pool_id() << ",page_num:" << page_num();
  return ss.str();
}

////////////////////////////////////////////////////////////////////////////////
/**
 * @brief 取当前上下文的调试标识 xid，用于 DEBUG 下校验锁的持有者
 * @details 优先返回当前 Session 指针；没有 Session 时，例如后台线程，退化为线程号。
 * 仅在 ASSERT 与 TRACE 中使用，Release 编译下不参与任何逻辑。
 * @return 代表当前持有者的整型标识
 */
intptr_t get_default_debug_xid()
{
#if 0
  ThreadData *thd = ThreadData::current();
  intptr_t xid = (thd == nullptr) ? 
                 // pthread_self的返回值类型是pthread_t，pthread_t在linux和mac上不同
                 // 在Linux上是一个整数类型，而在mac上是一个指针。为了能在两个平台上都编译通过，
                 // 就将pthread_self返回值转换两次
                 reinterpret_cast<intptr_t>(reinterpret_cast<void*>(pthread_self())) : 
                 reinterpret_cast<intptr_t>(thd);
#endif
  Session *session = Session::current_session();
  if (session == nullptr) {
    return reinterpret_cast<intptr_t>(reinterpret_cast<void *>(pthread_self()));
  } else {
    return reinterpret_cast<intptr_t>(session);
  }
}

/** @brief 加写锁，使用默认的调试标识 */
void Frame::write_latch() { write_latch(get_default_debug_xid()); }

/**
 * @brief 加写锁
 * @details 先在 debug_lock_ 保护的短临界区内做两项校验，再获取真正的写锁：
 * 必须已经 pin，否则该页可能在加锁期间被淘汰；不允许在持有读锁时升级为写锁，否则必然死锁。
 * @param xid 持有者标识，用于 DEBUG 校验
 */
void Frame::write_latch(intptr_t xid)
{
  {
    scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_.load() > 0,
        "frame lock. write lock failed while pin count is invalid. "
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

    ASSERT(read_lockers_.find(xid) == read_lockers_.end(),
        "frame lock write while holding the read lock."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());
  }

  lock_.lock();

#ifdef DEBUG
  write_locker_ = xid;
  ++write_recursive_count_;
  TRACE("frame write lock success."
        "this=%p, pin=%d, frameId=%s, write locker=%lx(recursive=%d), xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), write_locker_, write_recursive_count_, xid, lbt());
#endif
}

/** @brief 解写锁，使用默认的调试标识 */
void Frame::write_unlatch() { write_unlatch(get_default_debug_xid()); }

/**
 * @brief 解写锁
 * @details 校验调用者就是持锁者本人，写锁重入计数减到 0 时清空持有者，最后释放写锁。
 * @param xid 持有者标识，必须与加锁时一致
 */
void Frame::write_unlatch(intptr_t xid)
{
  // 因为当前已经加着写锁，而且写锁只有一个，所以不再加debug_lock来做校验
  debug_lock_.lock();

  ASSERT(pin_count_.load() > 0,
      "frame lock. write unlock failed while pin count is invalid."
      "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
      this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

  ASSERT(write_locker_ == xid,
      "frame unlock write while not the owner."
      "write_locker=%lx, this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
      write_locker_, this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

  TRACE("frame write unlock success. this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

  if (--write_recursive_count_ == 0) {
    write_locker_ = 0;
  }
  debug_lock_.unlock();

  lock_.unlock();
}

/** @brief 加读锁，使用默认的调试标识 */
void Frame::read_latch() { read_latch(get_default_debug_xid()); }

/**
 * @brief 加读锁。读锁可重入，并在 DEBUG 下记录每个持有者的重入次数
 * @details 同样要求已经 pin，且调用者不能是写锁持有者。
 * @param xid 持有者标识，用于 DEBUG 校验
 */
void Frame::read_latch(intptr_t xid)
{
  {
    scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_ > 0,
        "frame lock. read lock failed while pin count is invalid."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

    ASSERT(xid != write_locker_,
        "frame lock read while holding the write lock."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());
  }

  lock_.lock_shared();

  {
#ifdef DEBUG
    scoped_lock debug_lock(debug_lock_);
    ++read_lockers_[xid];
    TRACE("frame read lock success."
          "this=%p, pin=%d, frameId=%s, xid=%lx, recursive=%d, lbt=%s",
          this, pin_count_.load(), frame_id_.to_string().c_str(), xid, read_lockers_[xid], lbt());
#endif
  }
}

/**
 * @brief 尝试加读锁，不阻塞
 * @details 与 read_latch 的区别是使用 try_lock_shared，拿不到锁立即返回，
 * 用于上层不想在页锁上死等的场景。
 * @return 加锁成功返回 true，失败返回 false
 */
bool Frame::try_read_latch()
{
  intptr_t xid = get_default_debug_xid();
  {
    scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_ > 0,
        "frame try lock. read lock failed while pin count is invalid."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

    ASSERT(xid != write_locker_,
        "frame try to lock read while holding the write lock."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());
  }

  bool ret = lock_.try_lock_shared();
  if (ret) {
#ifdef DEBUG
    debug_lock_.lock();
    ++read_lockers_[xid];
    TRACE("frame read lock success."
          "this=%p, pin=%d, frameId=%s, xid=%lx, recursive=%d, lbt=%s",
          this, pin_count_.load(), frame_id_.to_string().c_str(), xid, read_lockers_[xid], lbt());
    debug_lock_.unlock();
#endif
  }

  return ret;
}

/** @brief 解读锁，使用默认的调试标识 */
void Frame::read_unlatch() { read_unlatch(get_default_debug_xid()); }

/**
 * @brief 解读锁
 * @details DEBUG 下递减该持有者的重入计数，减到 0 才移除记录，最后释放共享锁。
 * @param xid 持有者标识
 */
void Frame::read_unlatch(intptr_t xid)
{
  {
    scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_.load() > 0,
        "frame lock. read unlock failed while pin count is invalid."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

#ifdef DEBUG
    auto read_lock_iter  = read_lockers_.find(xid);
    int  recursive_count = read_lock_iter != read_lockers_.end() ? read_lock_iter->second : 0;
    ASSERT(recursive_count > 0,
        "frame unlock while not holding read lock."
        "this=%p, pin=%d, frameId=%s, xid=%lx, recursive=%d, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, recursive_count, lbt());

    if (1 == recursive_count) {
      read_lockers_.erase(xid);
    } else {
      read_lockers_[xid] = recursive_count - 1;
    }
#endif
  }

  TRACE("frame read unlock success."
        "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

  lock_.unlock_shared();
}

/**
 * @brief 增加引用计数，表示该页正在被使用，期间不允许被淘汰
 * @details 计数为原子自增，线程安全不依赖 debug_lock_。只有计数从 0 变成 1 时
 * 才计入 current_pinned_frames，避免同一页反复 pin 时统计重复累加。
 */
void Frame::pin()
{
  scoped_lock debug_lock(debug_lock_);

  [[maybe_unused]] intptr_t xid       = get_default_debug_xid();
  [[maybe_unused]] int      pin_count = ++pin_count_;
  if (stats_ != nullptr) {
    stats_->record_pin(pin_count == 1);
  }

  TRACE("after frame pin. "
        "this=%p, write locker=%lx, read locker has xid %d? pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, write_locker_, read_lockers_.find(xid) != read_lockers_.end(), 
        pin_count, frame_id_.to_string().c_str(), xid, lbt());
}

/**
 * @brief 减小引用计数，与 pin 配对，表示该页的一次使用已结束
 * @details 计数减到 0 说明该页彻底空闲，此时不允许还有任何 latch 被持有，
 * 这两条纪律直接写成断言，违反会立即暴露。
 * @return 递减之后的引用计数
 */
int Frame::unpin()
{
  [[maybe_unused]] intptr_t xid = get_default_debug_xid();

  ASSERT(pin_count_.load() > 0,
      "try to unpin a frame that pin count <= 0."
      "this=%p, pin=%d, frameId=%s, xid=%lx, lbt=%s",
      this, pin_count_.load(), frame_id_.to_string().c_str(), xid, lbt());

  scoped_lock debug_lock(debug_lock_);

  int pin_count = --pin_count_;
  if (stats_ != nullptr) {
    stats_->record_unpin(pin_count == 0);
  }
  TRACE("after frame unpin. "
        "this=%p, write locker=%lx, read locker has xid? %d, pin=%d, frameId=%s, xid=%lx, lbt=%s",
        this, write_locker_, read_lockers_.find(xid) != read_lockers_.end(), 
        pin_count, frame_id_.to_string().c_str(), xid, lbt());

  if (0 == pin_count) {
    ASSERT(write_locker_ == 0,
           "frame unpin to 0 failed while someone hold the write lock. write locker=%lx, frameId=%s, xid=%lx",
           write_locker_, frame_id_.to_string().c_str(), xid);
    ASSERT(read_lockers_.empty(),
           "frame unpin to 0 failed while someone hold the read locks. reader num=%d, frameId=%s, xid=%lx",
           read_lockers_.size(), frame_id_.to_string().c_str(), xid);
  }
  return pin_count;
}

/**
 * @brief 取当前单调时钟时间
 * @details 使用 CLOCK_MONOTONIC 而不是墙上时钟，系统时间被调整时不会跳变，
 * 因此可以安全地用于计算时间差。
 * @return 当前时间，单位纳秒
 */
unsigned long current_time()
{
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec * 1000 * 1000 * 1000UL + tp.tv_nsec;
}

/**
 * @brief 刷新最近访问时间
 * @details 替换策略依据该时间判断页的新旧程度，所以每次命中都要调用。
 */
void Frame::access() { acc_time_ = current_time(); }

/**
 * @brief 标记该页为脏页
 * @details 幂等：只有从未脏变为脏时才计入统计，重复调用不会重复累加 dirty_pages_current。
 */
void Frame::mark_dirty()
{
  if (!dirty_) {
    dirty_ = true;
    if (stats_ != nullptr) {
      stats_->record_dirty();
    }
  }
}

/**
 * @brief 清除脏标记
 * @details 页内容已经写入磁盘后调用。同样幂等，只有从脏变为干净时才计入统计。
 */
void Frame::clear_dirty()
{
  if (dirty_) {
    dirty_ = false;
    if (stats_ != nullptr) {
      stats_->record_clean();
    }
  }
}

/**
 * @brief 转为调试字符串
 * @return 包含 frame id、dirty、pin、lsn 与对象地址的字符串
 */
string Frame::to_string() const
{
  stringstream ss;
  ss << "frame id:" << frame_id().to_string() << ", dirty=" << dirty() << ", pin=" << pin_count()
     << ", lsn=" << lsn() << ", this=" << this;
  return ss.str();
}
