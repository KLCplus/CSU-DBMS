/**
 * @file disk_buffer_pool.h
 * @brief 分页文件缓存的主体：文件头、Frame 管理器、分页文件、BufferPool 管理器
 * @ingroup BufferPool
 * @details 本文件是整个 OS 存储层的目录。BPFrameManager 提供所有分页文件共享的内
 * 存 Frame 池，DiskBufferPool 管理其中一个分页文件，BufferPoolManager 管理多个
 * DiskBufferPool 并持有唯一的 FrameManager。
 */
#pragma once

#include <fcntl.h>
#include <functional>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <optional>

#include "common/lang/bitmap.h"
#include "common/lang/lru_cache.h"
#include "common/lang/mutex.h"
#include "common/lang/memory.h"
#include "common/lang/unordered_map.h"
#include "common/mm/mem_pool.h"
#include "common/sys/rc.h"
#include "common/types.h"
#include "storage/os/page/frame.h"
#include "storage/os/page/page.h"
#include "storage/os/diagnostics/buffer_pool_log.h"
#include "storage/os/diagnostics/buffer_pool_stats.h"
#include "storage/os/diagnostics/buffer_pool_diagnostics.h"
#include "storage/os/io/page_io_backend.h"
#include "storage/os/replacement/replacement_policy.h"

class BufferPoolManager;
class DiskBufferPool;
class DoubleWriteBuffer;
class LogHandler;
class BufferPoolLogHandler;

/**
 * @brief BufferPool 的实现
 * @defgroup BufferPool
 */

/// 文件头中位于 BPFileHeader 之后的子头长度，历史遗留宏
#define BP_FILE_SUB_HDR_SIZE (sizeof(BPFileSubHeader))

/**
 * @brief BufferPool的文件第一个页面，存放一些元数据信息，包括了后面每页的分配信息。
 * @ingroup BufferPool
 * @details
 * @code
 * TODO 1. 当前的做法，只能分配比较少的页面，你可以扩展一下，支持更多的页面或无限多的页面吗？
 *         可以参考Linux ext(n)和Windows NTFS等文件系统
 *      2. 当前使用bitmap存放页面分配情况，但是这种方法在页面非常多的时候，查找空闲页面的
 *         效率非常低，你有办法优化吗？
 * @endcode
 */
struct BPFileHeader
{
  int32_t buffer_pool_id;   //! buffer pool id
  int32_t page_count;       //! 当前文件一共有多少个页面
  int32_t allocated_pages;  //! 已经分配了多少个页面
  char    bitmap[0];        //! 页面分配位图, 第0个页面(就是当前页面)，总是1

  /**
   * 能够分配的最大的页面个数，即bitmap的字节数 乘以8
   */
  static const int MAX_PAGE_NUM =
      (BP_PAGE_DATA_SIZE - sizeof(buffer_pool_id) - sizeof(page_count) - sizeof(allocated_pages)) * 8;

  /** @brief 转为包含 id、页数与已分配页数的调试字符串 */
  string to_string() const;
};

/**
 * @brief 管理页面Frame
 * @ingroup BufferPool
 * @details 管理内存中的页帧。内存是有限的，内存中能够存放的页帧个数也是有限的。
 * 当内存中的页帧不够用时，需要从内存中淘汰一些页帧，以便为新的页帧腾出空间。
 * 这个管理器负责为所有的BufferPool提供页帧管理服务，也就是所有的BufferPool磁盘文件
 * 在访问时都使用这个管理器映射到内存。
 */
class BPFrameManager
{
public:
  /**
   * @brief 构造 Frame 管理器
   * @param tag 用于日志与内存池的标识
   * @param replacement_policy 淘汰策略类型，默认 LRU
   */
  BPFrameManager(const char *tag, BufferPoolReplacementPolicy replacement_policy = BufferPoolReplacementPolicy::LRU);

  /**
   * @brief 初始化内存池与 Frame 缓存
   * @param pool_num 分页文件个数上限
   * @param item_num_per_pool 每个分页文件预留的 Frame 数
   */
  RC init(int pool_num, int item_num_per_pool = DEFAULT_ITEM_NUM_PER_POOL);

  /** @brief 释放缓存中全部 Frame 并回收内存池 */
  RC cleanup();

  /**
   * @brief 获取指定的页面
   *
   * @param buffer_pool_id buffer Pool标识
   * @param page_num  页面号
   * @return Frame* 页帧指针
   */
  Frame *get(int buffer_pool_id, PageNum page_num);

  /**
   * @brief 列出所有指定文件的页面
   *
   * @param buffer_pool_id buffer Pool标识
   * @return list<Frame *> 页帧列表
   */
  list<Frame *> find_list(int buffer_pool_id);

  /**
   * @brief 分配一个新的页面
   *
   * @param buffer_pool_id buffer Pool标识
   * @param page_num 页面编号
   * @return Frame* 页帧指针
   */
  Frame *alloc(int buffer_pool_id, PageNum page_num);

  /**
   * 尽管frame中已经包含了buffer_pool_id和page_num，但是依然要求
   * 传入，因为frame可能忘记初始化或者没有初始化
   */
  RC free(int buffer_pool_id, PageNum page_num, Frame *frame);

  /**
   * 如果不能从空闲链表中分配新的页面，就使用这个接口，
   * 尝试从pin count=0的页面中淘汰一些
   * @param count 想要purge多少个页面
   * @param purger 需要在释放frame之前，对页面做些什么操作。当前是刷新脏数据到磁盘
   * @return 返回本次清理了多少个页面
   */
  int purge_frames(int count, function<RC(Frame *frame)> purger);

  /** @brief 返回当前缓存中实际驻留的 Frame 个数 */
  size_t frame_num() const { return frames_.count(); }

  /**
   * 测试使用。返回已经从内存申请的个数
   */
  size_t total_frame_num() const { return allocator_.get_size(); }

  /**
   * @brief 导出只读诊断快照
   * @param buffer_pool_id 指定分页文件 id；传 -1 表示汇总全部文件
   * @return 不暴露任何内部指针的 BufferPoolSnapshot
   */
  BufferPoolSnapshot snapshot(int buffer_pool_id = -1) const;

  /** @brief 返回当前使用的淘汰策略类型 */
  BufferPoolReplacementPolicy replacement_policy_type() const { return replacement_policy_type_; }

  /** @brief 返回淘汰策略对象，供诊断读取其元数据 */
  const ReplacementPolicy &replacement_policy() const { return *replacement_policy_; }

  /** @brief 返回全局统计对象 */
  BufferPoolStats            &stats() { return stats_; }

  /** @brief 返回只读的全局统计对象 */
  const BufferPoolStats      &stats() const { return stats_; }

private:
  /** @brief 在锁内按 FrameId 查找 Frame，get 的公共实现 */
  Frame *get_internal(const FrameId &frame_id);

  /** @brief 在锁内回收 Frame，free 的公共实现 */
  RC     free_internal(const FrameId &frame_id, Frame *frame);

private:
  /** @brief FrameId 的哈希函数适配器，供 LruCache 使用 */
  class BPFrameIdHasher
  {
  public:
    /** @brief 转发给 FrameId::hash */
    size_t operator()(const FrameId &frame_id) const { return frame_id.hash(); }
  };

  using FrameLruCache  = common::LruCache<FrameId, Frame *, BPFrameIdHasher>;
  using FrameAllocator = common::MemPoolSimple<Frame>;

  mutex          lock_;                     ///< 保护 frames_ 与 allocator_ 的管理锁
  FrameLruCache  frames_;                   ///< FrameId 到 Frame 的映射，附带 LRU 能力
  FrameAllocator allocator_;                ///< Frame 对象的内存池
  BufferPoolReplacementPolicy replacement_policy_type_;      ///< 当前策略类型
  unique_ptr<ReplacementPolicy> replacement_policy_;         ///< 策略实例
  BufferPoolStats              stats_;                       ///< 全局统计
  size_t                       capacity_ = 0;                ///< 可驻留的 Frame 数上限
};

/**
 * @brief 用于遍历BufferPool中的所有页面
 * @ingroup BufferPool
 */
class BufferPoolIterator
{
public:
  BufferPoolIterator();
  ~BufferPoolIterator();

  /**
   * @brief 用分页文件的分配 bitmap 初始化迭代器
   * @param bp 目标分页文件
   * @param start_page 起始页号
   */
  RC      init(DiskBufferPool &bp, PageNum start_page = 0);

  /** @brief 判断是否还有下一个已分配的页 */
  bool    has_next();

  /** @brief 返回下一个已分配的页号 */
  PageNum next();

  /** @brief 重置到起始位置 */
  RC      reset();

private:
  common::Bitmap bitmap_;                 ///< 分页文件分配位图的副本
  PageNum        current_page_num_ = -1;  ///< 当前迭代到的页号
};

/**
 * @brief BufferPool的实现
 * @ingroup BufferPool
 * @details 一个文件被划分成多个相同大小的页面，并在需要访问的时候，会从文件读取到内存中。
 * DiskBufferPool 就负责管理磁盘文件，以及负责管理页面在文件与内存中的交互，比如读取、写回。
 */
class DiskBufferPool final
{
public:
  /**
   * @brief 构造一个分页文件对象
   * @param bp_manager 所属的 BufferPoolManager
   * @param frame_manager 共享的 Frame 管理器
   * @param dblwr_manager 双写缓冲管理器
   * @param log_handler 日志处理器，用于页分配与释放的日志
   * @param io_backend_type I/O 后端类型，legacy 或 positional
   */
  DiskBufferPool(BufferPoolManager &bp_manager, BPFrameManager &frame_manager, DoubleWriteBuffer &dblwr_manager,
      LogHandler &log_handler, PageIOBackendType io_backend_type);
  ~DiskBufferPool();

  /**
   * 根据文件名打开一个分页文件
   */
  RC open_file(const char *file_name);

  /**
   * 关闭分页文件
   */
  RC close_file();

  /**
   * 根据文件ID和页号获取指定页面到缓冲区，返回页面句柄指针。
   */
  RC get_this_page(PageNum page_num, Frame **frame);

  /**
   * @brief 在指定文件中分配一个新的页面，并将其放入缓冲区，返回页面句柄指针。
   * @details 分配页面时，如果文件中有空闲页，就直接分配一个空闲页；
   * 如果文件中没有空闲页，则扩展文件规模来增加新的空闲页。
   */
  RC allocate_page(Frame **frame);

  /**
   * @brief 释放某个页面，将此页面设置为未分配状态
   *
   * @param page_num 待释放的页面
   */
  RC dispose_page(PageNum page_num);

  /**
   * @brief 释放指定文件关联的页的内存
   * 如果已经脏， 则刷到磁盘，除了pinned page
   */
  RC purge_page(PageNum page_num);

  /**
   * @brief 关闭文件时刷回并释放全部页
   * @param reason 刷盘原因，默认 SHUTDOWN
   */
  RC purge_all_pages(FlushReason reason = FlushReason::SHUTDOWN);

  /**
   * @brief 用于解除pageHandle对应页面的驻留缓冲区限制
   *
   * 在调用GetThisPage或AllocatePage函数将一个页面读入缓冲区后，
   * 该页面被设置为驻留缓冲区状态，以防止其在处理过程中被置换出去，
   * 因此在该页面使用完之后应调用此函数解除该限制，使得该页面此后可以正常地被淘汰出缓冲区
   */
  RC unpin_page(Frame *frame);

  /**
   * 检查是否所有页面都是pin count == 0状态(除了第1个页面)
   * 调试使用
   */
  RC check_all_pages_unpinned();

  /** @brief 返回底层文件的文件描述符 */
  int file_desc() const;

  /**
   * 如果页面是脏的，就将数据刷新到double write buffer
   */
  RC flush_page(Frame &frame, FlushReason reason = FlushReason::EXPLICIT);

  /**
   * 刷新所有页面到double write buffer，即使pin count不是0
   */
  RC flush_all_pages();

  /** 只刷新当前未被业务 pin 的脏页。max_pages=0 表示不限制数量。 */
  RC flush_dirty_pages(size_t max_pages, DirtyPageFlushResult &result,
      FlushReason reason = FlushReason::EXPLICIT);

  /** @brief 导出本分页文件的只读诊断快照 */
  BufferPoolSnapshot snapshot() const;

  /** @brief 导出本分页文件的统计快照 */
  BufferPoolStatsSnapshot stats() const { return stats_.snapshot(); }

  /** @brief 返回本分页文件使用的 I/O 后端类型 */
  PageIOBackendType io_backend_type() const { return io_backend_->type(); }

  /**
   * 回放日志时处理page0中已被认定为不存在的page
   */
  RC recover_page(PageNum page_num);

  /**
   * 刷新页面到磁盘
   */
  RC write_page(PageNum page_num, Page &page);

  /** @brief 恢复阶段重做一次页分配，按 LSN 更新文件头 bitmap */
  RC redo_allocate_page(LSN lsn, PageNum page_num);

  /** @brief 恢复阶段重做一次页释放，按 LSN 更新文件头 bitmap */
  RC redo_deallocate_page(LSN lsn, PageNum page_num);

public:
  /** @brief 返回分页文件 id */
  int32_t id() const { return buffer_pool_id_; }

  /** @brief 返回分页文件名 */
  const char *filename() const { return file_name_.c_str(); }

protected:
  /**
   * @brief 为指定页号申请一个已 pin 的 Frame
   * @details 先找空闲 Frame，不够时调用替换策略选 victim；victim 为脏则先刷盘再回收。
   * 全部 Frame 都被 pin 时返回 BUFFERPOOL_NOBUF 并记录 NO_BUFFER 诊断。
   * @param page_num 页号
   * @param buf 输出参数，返回申请到的 Frame
   */
  RC allocate_frame(PageNum page_num, Frame **buf);

  /**
   * 刷新指定页面到磁盘(flush)，并且释放关联的Frame
   */
  RC purge_frame(PageNum page_num, Frame *used_frame, FlushReason reason = FlushReason::EXPLICIT);

  /** @brief 校验页号是否落在本文件的有效范围内 */
  RC check_page_num(PageNum page_num);

  /**
   * 加载指定页面的数据到内存中
   */
  RC load_page(PageNum page_num, Frame *frame);

  /**
   * 如果页面是脏的，就将数据刷新到磁盘
   */
  RC flush_page_internal(Frame &frame, FlushReason reason);

  /**
   * @brief 打印当前仍被 pin 的 Frame 清单，用于 pin starvation 诊断
   * @param context 日志前缀，说明触发场景
   * @param warning 为 true 时以 WARN 级别输出
   */
  void log_pinned_frames(const char *context, bool warning) const;

private:
  BufferPoolManager   &bp_manager_;     /// BufferPool 管理器
  BPFrameManager      &frame_manager_;  /// Frame 管理器
  DoubleWriteBuffer   &dblwr_manager_;  /// Double Write Buffer 管理器
  BufferPoolLogHandler log_handler_;    /// BufferPool 日志处理器
  unique_ptr<PageIOBackend> io_backend_;  /// 目标分页文件的 I/O 后端
  BufferPoolStats stats_;                /// 当前分页文件的统计；全局统计仍由 FrameManager 持有

  int file_desc_ = -1;  /// 文件描述符
  /// 由于在最开始打开文件时，没有正确的buffer pool id不能加载header frame，所以单独从文件中读取此标识
  int32_t       buffer_pool_id_ = -1;
  Frame        *hdr_frame_      = nullptr;  /// 文件头页面
  BPFileHeader *file_header_    = nullptr;  /// 文件头
  set<PageNum>  disposed_pages_;            /// 已经释放的页面

  string file_name_;  /// 文件名

  common::Mutex lock_;     /// 保护文件头与分配状态的锁
  common::Mutex wr_lock_;  /// 串行化目标分页文件的读写，保证 I/O 顺序

private:
  friend class BufferPoolIterator;
};

/**
 * @brief BufferPool的管理类
 * @ingroup BufferPool
 */
class BufferPoolManager final
{
public:
  /**
   * @brief 构造 BufferPool 管理器
   * @param memory_size 缓存可用内存字节数，0 表示使用默认容量
   * @param replacement_policy 淘汰策略类型
   * @param io_backend_type I/O 后端类型
   */
  BufferPoolManager(int memory_size = 0,
      BufferPoolReplacementPolicy replacement_policy = BufferPoolReplacementPolicy::LRU,
      PageIOBackendType io_backend_type = PageIOBackendType::LEGACY);
  ~BufferPoolManager();

  /**
   * @brief 注入双写缓冲实现并初始化 Frame 管理器
   * @param dblwr_buffer 双写缓冲对象
   */
  RC init(unique_ptr<DoubleWriteBuffer> dblwr_buffer);

  /** @brief 新建一个分页文件并写入初始文件头 */
  RC create_file(const char *file_name);

  /** @brief 打开一个已存在的分页文件 */
  RC open_file(LogHandler &log_handler, const char *file_name, DiskBufferPool *&bp);

  /** @brief 关闭一个分页文件并释放其对象 */
  RC close_file(const char *file_name);

  /** @brief 刷新单个 Frame 对应的页 */
  RC flush_page(Frame &frame, FlushReason reason = FlushReason::EXPLICIT);

  /** @brief 返回全局唯一的 Frame 管理器 */
  BPFrameManager    &get_frame_manager() { return frame_manager_; }

  /** @brief 返回双写缓冲实现 */
  DoubleWriteBuffer *get_dblwr_buffer() { return dblwr_buffer_.get(); }

  /** @brief 返回全局统计快照 */
  BufferPoolStatsSnapshot stats() const { return frame_manager_.stats().snapshot(); }

  /** @brief 清零全局统计 */
  void reset_stats() { frame_manager_.stats().reset(); }

  /** @brief 返回当前淘汰策略类型 */
  BufferPoolReplacementPolicy replacement_policy() const { return frame_manager_.replacement_policy_type(); }

  /** @brief 返回 I/O 后端类型 */
  PageIOBackendType io_backend_type() const { return io_backend_type_; }

  /** @brief 导出全局只读诊断快照 */
  BufferPoolSnapshot snapshot() const;

  /**
   * @brief 根据ID获取对应的BufferPool对象
   * @details 在做redo时，需要根据ID获取对应的BufferPool对象，然后让bufferPool对象自己做redo
   * @param id buffer pool id
   * @param bp buffer pool 对象
   */
  RC get_buffer_pool(int32_t id, DiskBufferPool *&bp);

private:
  BPFrameManager frame_manager_;  ///< 所有分页文件共享的 Frame 管理器

  unique_ptr<DoubleWriteBuffer> dblwr_buffer_;  ///< 双写缓冲实现

  common::Mutex                            lock_;                 ///< 保护下面两张表
  unordered_map<string, DiskBufferPool *>  buffer_pools_;         ///< 文件名到分页文件对象
  unordered_map<int32_t, DiskBufferPool *> id_to_buffer_pools_;   ///< id 到分页文件对象
  atomic<int32_t>                          next_buffer_pool_id_{1};  // 系统启动时，会打开所有的表，这样就可以知道当前系统最大的ID是多少了
  PageIOBackendType                        io_backend_type_ = PageIOBackendType::LEGACY;  ///< I/O 后端类型
};
