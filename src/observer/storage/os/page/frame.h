/**
 * @file frame.h
 * @brief 页帧标识符 FrameId 与内存页帧 Frame
 * @ingroup BufferPool
 * @details Frame 是磁盘页进入内存后的管理对象，内部直接持有一个 Page。
 * pin 与 dirty 是两个正交状态：pin 决定能不能被淘汰，dirty 决定淘汰前要不要写回。
 */
#pragma once

#include <pthread.h>
#include <string.h>

#include "common/lang/mutex.h"
#include "common/lang/string.h"
#include "common/lang/atomic.h"
#include "common/lang/unordered_map.h"
#include "common/log/log.h"
#include "common/types.h"
#include "storage/os/page/page.h"

class BufferPoolStats;

/**
 * @brief 页帧标识符
 * @ingroup BufferPool
 * @details 由 buffer_pool_id 与 page_num 共同构成。因为一个 BPFrameManager 被所有
 * 分页文件共享，只凭 page_num 无法区分不同文件中的同一个页号。
 */
class FrameId
{
public:
  FrameId() = default;

  /** @brief 用所属分页文件 id 与页号构造标识 */
  FrameId(int buffer_pool_id, PageNum page_num);

  /** @brief 逐字段比较两个标识是否相同 */
  bool    equal_to(const FrameId &other) const;

  /** @brief 与 equal_to 等价，供容器与断言使用 */
  bool    operator==(const FrameId &other) const;

  /** @brief 计算哈希值，buffer_pool_id 放高 32 位，page_num 放低 32 位 */
  size_t  hash() const;

  /** @brief 返回所属分页文件 id */
  int     buffer_pool_id() const;

  /** @brief 返回页号 */
  PageNum page_num() const;

  /** @brief 设置所属分页文件 id */
  void set_buffer_pool_id(int buffer_pool_id) { buffer_pool_id_ = buffer_pool_id; }

  /** @brief 设置页号 */
  void set_page_num(PageNum page_num) { page_num_ = page_num; }

  /** @brief 转为 "buffer_pool_id:x,page_num:y" 形式的调试字符串 */
  string to_string() const;

private:
  int     buffer_pool_id_ = -1;  ///< 所属分页文件 id，-1 表示尚未初始化
  PageNum page_num_       = -1;  ///< 页号，-1 表示尚未初始化
};

/**
 * @brief 页帧
 * @ingroup BufferPool
 * @details 页帧是磁盘文件在内存中的表示。磁盘文件按照页面来操作，操作之前先映射到内存中，
 * 将磁盘数据读取到内存中，也就是页帧。
 *
 * 当某个页面被淘汰时，如果有些内容曾经变更过，那么就需要将这些内容刷新到磁盘上。这里有
 * 一个dirty标识，用来标识页面是否被修改过。
 *
 * 为了防止在使用过程中页面被淘汰，这里使用了pin count，当页面被使用时，pin count会增加，
 * 当页面不再使用时，pin count会减少。当pin count为0时，页面可以被淘汰。
 */
class Frame
{
public:
  ~Frame()
  {
    // LOG_DEBUG("deallocate frame. this=%p, lbt=%s", this, common::lbt());
  }

  /**
   * @brief reinit 和 reset 在 MemPoolSimple 中使用
   * @details 在 MemPoolSimple 分配和释放一个Frame对象时，不会调用构造函数和析构函数，
   * 而是调用reinit和reset。
   */
  void reinit() {}
  void reset() {}

  /** @brief 把内部 Page 整页清零，页分配复用时使用 */
  void clear_page() { memset(&page_, 0, sizeof(page_)); }

  /** @brief 返回所属分页文件 id */
  int  buffer_pool_id() const { return frame_id_.buffer_pool_id(); }

  /** @brief 设置所属分页文件 id */
  void set_buffer_pool_id(int id) { frame_id_.set_buffer_pool_id(id); }

  /**
   * @brief 在磁盘和内存中内容完全一致的数据页
   * @details 磁盘文件划分为一个个页面，每次从磁盘加载到内存中，也是一个页面，就是 Page。
   * frame 是为了管理这些页面而维护的一个数据结构。
   */
  Page &page() { return page_; }

  /**
   * @brief 每个页面都有一个编号
   * @details 当前页面编号记录在了页面数据中，其实可以不记录，从磁盘中加载时记录在Frame信息中即可。
   */
  PageNum page_num() const { return frame_id_.page_num(); }

  /** @brief 设置页号 */
  void    set_page_num(PageNum page_num) { frame_id_.set_page_num(page_num); }

  /** @brief 返回 (分页文件 id, 页号) 形式的完整标识 */
  FrameId frame_id() const { return frame_id_; }

  /**
   * @brief 为了实现持久化，需要将页面的修改记录记录到日志中，这里记录了日志序列号
   * @details 如果当前页面从磁盘中加载出来时，它的日志序列号比当前WAL(Write-Ahead-Logging)中的一些
   * 序列号要小，那就可以从日志中读取这些更大序列号的日志，做重做操作，将页面恢复到最新状态，也就是redo。
   */
  LSN  lsn() const { return page_.lsn; }

  /** @brief 设置页的日志序列号 */
  void set_lsn(LSN lsn) { page_.lsn = lsn; }

  /**
   * @brief 页面校验和
   * @details 用于校验页面完整性。如果页面写入一半时出现异常，可以通过校验和检测出来。
   */
  CheckSum check_sum() const { return page_.check_sum; }

  /** @brief 设置页校验和 */
  void     set_check_sum(CheckSum check_sum) { page_.check_sum = check_sum; }

  /**
   * @brief 刷新当前内存页面的访问时间
   * @details 由于内存是有限的，比磁盘要小很多。那当我们访问某些文件页面时，可能由于内存不足
   * 而要淘汰一些页面。我们选择淘汰哪些页面呢？这里使用了LRU算法，即最近最少使用的页面被淘汰。
   * 最近最少使用，采用的依据就是访问时间。所以每次访问某个页面时，我们都要刷新一下访问时间。
   */
  void access();

  /** @brief 返回上次 access 记录的时间戳，单位纳秒，取自单调时钟 */
  uint64_t last_access_ns() const { return acc_time_; }

  /**
   * @brief 标记指定页面为“脏”页。
   * @details 如果修改了页面的内容，则应调用此函数，
   * 以便该页面被淘汰出缓冲区时系统将新的页面数据写入磁盘文件
   */
  void mark_dirty();

  /**
   * @brief 重置“脏”标记
   * @details 如果页面已经被写入磁盘文件，则应调用此函数。
   */
  void clear_dirty();

  /** @brief 返回页是否被修改过且尚未写回 */
  bool dirty() const { return dirty_; }

  /** @brief 返回数据区首地址，供上层解释为 Record 页或 B+Tree 页 */
  char *data() { return page_.data; }

  /** @brief 判断该页能否被淘汰。pin count 为 0 才可淘汰，这是唯一的判据 */
  bool can_purge() { return pin_count_.load() == 0; }

  /**
   * @brief 给当前页帧增加引用计数
   * pin通常都会加着frame manager锁来访问。
   * 当我们访问某个页面时，我们不期望此页面被淘汰，所以我们会增加引用计数。
   */
  void pin();

  /**
   * @brief 释放一个当前页帧的引用计数
   * 与pin对应，但是通常不会加着frame manager的锁来访问
   */
  int unpin();

  /** @brief 返回当前引用计数 */
  int pin_count() const { return pin_count_.load(); }

  /** @brief 关联统计对象，pin、unpin 与 dirty 状态变化会记入该对象 */
  void set_stats(BufferPoolStats *stats) { stats_ = stats; }

  /** @brief 加写锁，等价于 write_latch(get_default_debug_xid()) */
  void write_latch();

  /** @brief 加写锁，xid 用于 DEBUG 下校验持锁者身份 */
  void write_latch(intptr_t xid);

  /** @brief 解写锁，等价于 write_unlatch(get_default_debug_xid()) */
  void write_unlatch();

  /** @brief 解写锁，只有持锁者本人可以解 */
  void write_unlatch(intptr_t xid);

  /** @brief 加读锁，读锁可重入 */
  void read_latch();

  /** @brief 加读锁，xid 用于 DEBUG 下校验持锁者身份 */
  void read_latch(intptr_t xid);

  /** @brief 尝试加读锁，不阻塞。返回是否加锁成功 */
  bool try_read_latch();

  /** @brief 解读锁 */
  void read_unlatch();

  /** @brief 解读锁，xid 用于 DEBUG 下递减重入计数 */
  void read_unlatch(intptr_t xid);

  /** @brief 转为包含 frame id、dirty、pin、lsn 的调试字符串 */
  string to_string() const;

private:
  friend class BufferPool;

  bool          dirty_ = false;          ///< 内存内容是否比磁盘新
  atomic<int>   pin_count_{0};           ///< 引用计数，大于 0 时禁止淘汰
  unsigned long acc_time_ = 0;           ///< 最近一次访问时间，单调时钟纳秒
  FrameId       frame_id_;               ///< (分页文件 id, 页号)
  Page          page_;                   ///< 真正的 8192 字节页数据
  BufferPoolStats *stats_ = nullptr;     ///< 统计对象，可为空

  /// 在非并发编译时，加锁解锁动作将什么都不做
  common::RecursiveSharedMutex lock_;

  /// 使用一些手段来做测试，提前检测出头疼的死锁问题
  /// 如果编译时没有增加调试选项，这些代码什么都不做
  common::DebugMutex           debug_lock_;
  intptr_t                     write_locker_          = 0;  ///< 当前写锁持有者，0 表示无人持有
  int                          write_recursive_count_ = 0;  ///< 写锁重入次数
  unordered_map<intptr_t, int> read_lockers_;                ///< 各读锁持有者及其重入次数
};
