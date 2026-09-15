/**
 * @file double_write_buffer.h
 * @brief 页写入保护：双写缓冲
 * @ingroup BufferPool
 * @details 目标分页文件的一次写入不保证原子，可能只写了一半就掉电。双写缓冲在写真实页之前，
 * 先把整页写入一个共享表空间文件并落盘；真实页写坏时可以从这里找回完整内容。
 */
#pragma once

#include "common/lang/mutex.h"
#include "common/lang/unordered_map.h"
#include "common/types.h"
#include "common/sys/rc.h"
#include "storage/os/page/page.h"

class DiskBufferPool;
struct DoubleWritePage;
class BufferPoolManager;

/**
 * @brief 双写缓冲的抽象接口
 * @ingroup BufferPool
 * @details 只定义页级的写入保护语义，具体是落盘还是直写由实现决定。
 * 上层 DiskBufferPool 只依赖这个接口，因此可以在不启用双写时替换成空实现。
 */
class DoubleWriteBuffer
{
public:
  DoubleWriteBuffer()          = default;
  virtual ~DoubleWriteBuffer() = default;

  /**
   * 将页面加入buffer，并且写入磁盘中的共享表空间
   */
  virtual RC add_page(DiskBufferPool *bp, PageNum page_num, Page &page) = 0;

  /**
   * @brief 尝试从缓冲区中取回指定页
   * @param bp 页所属的分页文件
   * @param page_num 页号
   * @param page 输出参数，取回时写入页内容
   * @return 缓冲区中没有该页时返回 BUFFERPOOL_INVALID_PAGE_NUM
   */
  virtual RC read_page(DiskBufferPool *bp, PageNum page_num, Page &page) = 0;

  /**
   * @brief 清空所有与指定buffer pool关联的页面
   */
  virtual RC clear_pages(DiskBufferPool *bp) = 0;
};

/**
 * @brief 双写缓冲文件的文件头
 * @ingroup BufferPool
 * @details 位于共享表空间文件的最前面，记录当前文件中有效页面的个数。
 */
struct DoubleWriteBufferHeader
{
  int32_t page_cnt = 0;  ///< 文件中页面的个数

  static const int32_t SIZE;  ///< 文件头字节数，即 sizeof(DoubleWriteBufferHeader)
};

// TODO change to FrameId
/**
 * @brief 双写缓冲中一个页面的键
 * @ingroup BufferPool
 * @details 与 FrameId 含义相同，用分页文件 id 加页号定位一个页。
 */
struct DoubleWritePageKey
{
  int32_t buffer_pool_id;  ///< 页所属的分页文件 id
  PageNum page_num;        ///< 页号

  /** @brief 两个字段都相等才视为同一个键 */
  bool operator==(const DoubleWritePageKey &other) const
  {
    return buffer_pool_id == other.buffer_pool_id && page_num == other.page_num;
  }
};

/**
 * @brief DoubleWritePageKey 的哈希函数
 * @ingroup BufferPool
 */
struct DoubleWritePageKeyHash
{
  /** @brief 把两个字段的哈希值异或起来 */
  size_t operator()(const DoubleWritePageKey &key) const
  {
    return hash<int32_t>()(key.buffer_pool_id) ^ hash<PageNum>()(key.page_num);
  }
};

/**
 * @brief 页面二次缓冲区，为了解决页面原子写入的问题
 * @ingroup BufferPool
 * @details 一个页面通常比较大，不能保证要么都写入磁盘成功，要么不写入磁盘。如果存在写入一部分的情况，
 * 我们应该有手段检测出来，否则会遇到灾难性的数据不一致问题。
 * 这里的解决方案是在我们写入真实页面数据之前，先将数据放入一个公共的缓冲区，也就是DoubleWriteBuffer，
 * DoubleWriteBuffer会先在一个共享磁盘文件中写入页面数据，在确定写入成功后，再写入真实的页面。
 * 当我们从磁盘中读取页面时，会校验页面的checksum，如果校验失败，则说明页面写入不完整，这时候可以从
 * DoubleWriteBuffer中读取数据。
 *
 * @note 每次都要保证，不管在内存中还是在文件中，这里的数据都是最新的，都比Buffer pool中的数据要新
 */
class DiskDoubleWriteBuffer : public DoubleWriteBuffer
{
public:
  /**
   * @brief 构造函数
   *
   * @param bp_manager 关联的buffer pool manager
   * @param max_pages  内存中保存的最大页面数
   */
  DiskDoubleWriteBuffer(BufferPoolManager &bp_manager, int max_pages = 16);
  virtual ~DiskDoubleWriteBuffer();

  /**
   * 打开磁盘中的共享表空间文件
   */
  RC open_file(const char *filename);

  /**
   * 将buffer中的页全部写入磁盘，并且清空buffer
   * TODO 目前的解决方案是等buffer装满后再刷盘，可能会导致程序卡住一段时间
   */
  RC flush_page();

  /**
   * 将页面加入buffer，并且写入磁盘中的共享表空间
   */
  RC add_page(DiskBufferPool *bp, PageNum page_num, Page &page) override;

  /**
   * @brief 从内存缓冲区中取回指定页
   * @details 只查内存中的 dblwr_pages_，不读共享表空间文件，因此仅在页面尚未通过 flush_page
   * 写回真实文件之前有效。
   */
  RC read_page(DiskBufferPool *bp, PageNum page_num, Page &page) override;

  /**
   * @brief 清空所有与指定buffer pool关联的页面
   */
  RC clear_pages(DiskBufferPool *bp) override;

  /**
   * 将共享表空间的页读入buffer
   */
  RC recover();

private:
  /**
   * 将buffer中的页面写入对应的磁盘
   */
  RC write_page(DoubleWritePage *page);

  /**
   * 将页面写到当前double write buffer文件中
   * @details 每次页面更新都应该写入到磁盘中。保证double write buffer
   * 内存和文件中的数据都是最新的。
   */
  RC write_page_internal(DoubleWritePage *page);

  /**
   * @brief 将磁盘文件中的内容加载到内存中。在启动时调用
   */
  RC load_pages();

private:
  int                     file_desc_ = -1;  ///< 共享表空间文件的描述符
  int                     max_pages_ = 0;   ///< 内存中最多缓存多少个页，装满即刷
  common::Mutex           lock_;            ///< 保护 dblwr_pages_
  BufferPoolManager      &bp_manager_;      ///< 用于按 id 找回对应的分页文件
  DoubleWriteBufferHeader header_;          ///< 共享表空间文件头

  unordered_map<DoubleWritePageKey, DoubleWritePage *, DoubleWritePageKeyHash> dblwr_pages_;  ///< 页号到缓冲页
};

/**
 * @brief 不启用实际双写时的兼容实现
 * @ingroup BufferPool
 * @details 保持与真实实现完全相同的接口，写入直接落到目标分页文件，读取一律失败，
 * 这样上层 DiskBufferPool 不需要为是否启用双写写分支。
 */
class VacuousDoubleWriteBuffer : public DoubleWriteBuffer
{
public:
  virtual ~VacuousDoubleWriteBuffer() = default;

  /**
   * 将页面加入buffer，并且写入磁盘中的共享表空间
   */
  RC add_page(DiskBufferPool *bp, PageNum page_num, Page &page) override;

  /** @brief 空实现：没有缓冲区，永远取不回页 */
  RC read_page(DiskBufferPool *bp, PageNum page_num, Page &page) override { return RC::BUFFERPOOL_INVALID_PAGE_NUM; }

  /**
   * @brief 清空所有与指定buffer pool关联的页面
   */
  RC clear_pages(DiskBufferPool *bp) override { return RC::SUCCESS; }
};
