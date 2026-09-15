/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wenbin1002 on 2024/04/16
//
/**
 * @file double_write_buffer.cpp
 * @brief 双写缓冲的实现
 * @ingroup BufferPool
 * @details 共享表空间文件的布局为：文件头 + 定长的 DoubleWritePage 数组。
 * 第 i 个缓冲页的偏移为 i * DoubleWritePage::SIZE + DoubleWriteBufferHeader::SIZE。
 */
#include <fcntl.h>

#include "storage/os/buffer/double_write_buffer.h"
#include "storage/os/buffer/disk_buffer_pool.h"
#include "common/io/io.h"
#include "common/log/log.h"
#include "common/math/crc.h"
#include <algorithm>

using namespace common;

/**
 * @brief 双写缓冲中一个页面的内存表示
 * @ingroup BufferPool
 * @details 整体会按定长写入共享表空间文件，因此它的字节数决定了文件中的页间距。
 */
struct DoubleWritePage
{
public:
  DoubleWritePage() = default;

  /** @brief 用页键、文件内页索引与页内容构造一个缓冲页 */
  DoubleWritePage(int32_t buffer_pool_id, PageNum page_num, int32_t page_index, Page &page);

public:
  DoubleWritePageKey key;               ///< 页键，(分页文件 id, 页号)
  int32_t            page_index = -1; /// 页面在double write buffer文件中的页索引
  bool               valid = true; /// 表示页面是否有效，在页面被删除时，需要同时标记磁盘上的值。
  Page               page;              ///< 页内容

  static const int32_t SIZE;            ///< 缓冲页字节数，即 sizeof(DoubleWritePage)
};

/**
 * @brief 构造一个缓冲页，把页内容整体拷贝进来
 */
DoubleWritePage::DoubleWritePage(int32_t buffer_pool_id, PageNum page_num, int32_t page_index, Page &_page)
  : key{buffer_pool_id, page_num}, page_index(page_index), page(_page)
{}

const int32_t DoubleWritePage::SIZE = sizeof(DoubleWritePage);

const int32_t DoubleWriteBufferHeader::SIZE = sizeof(DoubleWriteBufferHeader);

/**
 * @brief 构造双写缓冲
 * @param bp_manager 关联的 BufferPoolManager，用于按 id 找回分页文件
 * @param max_pages 内存中最多缓存多少个页
 */
DiskDoubleWriteBuffer::DiskDoubleWriteBuffer(BufferPoolManager &bp_manager, int max_pages /*=16*/) 
  : max_pages_(max_pages), bp_manager_(bp_manager)
{
}

/**
 * @brief 析构：把尚未写回的页刷到真实文件，然后关闭共享表空间文件
 */
DiskDoubleWriteBuffer::~DiskDoubleWriteBuffer()
{
  flush_page();
  close(file_desc_);
}

/**
 * @brief 打开共享表空间文件
 * @details 已经打开过则报错返回；打开成功后调用 load_pages 把上次残留的页读进内存。
 * @param filename 共享表空间文件路径
 */
RC DiskDoubleWriteBuffer::open_file(const char *filename)
{
  if (file_desc_ >= 0) {
    LOG_ERROR("Double write buffer has already opened. file desc=%d", file_desc_);
    return RC::BUFFERPOOL_OPEN;
  }
  
  int fd = open(filename, O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    LOG_ERROR("Failed to open or creat %s, due to %s.", filename, strerror(errno));
    return RC::SCHEMA_DB_EXIST;
  }

  file_desc_ = fd;
  return load_pages();
}

/**
 * @brief 把缓冲区中的页全部写回各自的分页文件，并清空缓冲区
 * @details 顺序很关键：先把共享表空间刷到磁盘，再逐页写真实文件；
 * 每写成功一页就把它标为 invalid 并把这个标记持久化，最后删除内存对象。
 * 这样即使在两步之间掉电，重放时该页仍被视为有效，重复写入同样的内容不会造成错误。
 * 缓冲区装满或关闭数据库时走这条路径。
 */
RC DiskDoubleWriteBuffer::flush_page()
{
  sync();

  for (const auto &pair : dblwr_pages_) {
    RC rc = write_page(pair.second);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    pair.second->valid = false;
    write_page_internal(pair.second);
    delete pair.second;
  }

  dblwr_pages_.clear();
  header_.page_cnt = 0;

  return RC::SUCCESS;
}

/**
 * @brief 把一个页加入缓冲区，并立即写入共享表空间文件
 * @details 命中已有缓冲页时只更新内容和文件，不改变页数统计；新增缓冲页时把页数写回文件头，
 * 并在缓冲页数达到 max_pages_ 时触发一次整体刷盘。
 * @param bp 页所属的分页文件
 * @param page_num 页号
 * @param page 待保护的页内容
 */
RC DiskDoubleWriteBuffer::add_page(DiskBufferPool *bp, PageNum page_num, Page &page)
{
  scoped_lock lock_guard(lock_);
  DoubleWritePageKey key{bp->id(), page_num};
  auto iter = dblwr_pages_.find(key);
  if (iter != dblwr_pages_.end()) {
    iter->second->page = page;
    LOG_TRACE("[cache hit]add page into double write buffer. buffer_pool_id:%d,page_num:%d,lsn=%d, dwb size=%d",
              bp->id(), page_num, page.lsn, static_cast<int>(dblwr_pages_.size()));
    return write_page_internal(iter->second);
  }

  int64_t          page_cnt   = dblwr_pages_.size();
  DoubleWritePage *dblwr_page = new DoubleWritePage(bp->id(), page_num, page_cnt, page);
  dblwr_pages_.insert(pair<DoubleWritePageKey, DoubleWritePage *>(key, dblwr_page));
  LOG_TRACE("insert page into double write buffer. buffer_pool_id:%d,page_num:%d,lsn=%d, dwb size:%d",
            bp->id(), page_num, page.lsn, static_cast<int>(dblwr_pages_.size()));

  RC rc = write_page_internal(dblwr_page);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to write page into double write buffer. rc=%s buffer_pool_id:%d,page_num:%d,lsn=%d.",
        strrc(rc), bp->id(), page_num, page.lsn);
    return rc;
  }

  if (page_cnt + 1 > header_.page_cnt) {
    header_.page_cnt = page_cnt + 1;
    if (lseek(file_desc_, 0, SEEK_SET) == -1) {
      LOG_ERROR("Failed to add page header due to failed to seek %s.", strerror(errno));
      return RC::IOERR_SEEK;
    }

    if (writen(file_desc_, &header_, sizeof(header_)) != 0) {
      LOG_ERROR("Failed to add page header due to %s.", strerror(errno));
      return RC::IOERR_WRITE;
    }
  }

  if (static_cast<int>(dblwr_pages_.size()) >= max_pages_) {
    RC rc = flush_page();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush pages in double write buffer");
      return rc;
    }
  }

  return RC::SUCCESS;
}

/**
 * @brief 把一个缓冲页写入共享表空间文件
 * @details 位置由 page_index 决定，写入的是整个 DoubleWritePage 定长结构。
 * @param page 待写入的缓冲页
 */
RC DiskDoubleWriteBuffer::write_page_internal(DoubleWritePage *page)
{
  int32_t page_index = page->page_index;
  int64_t offset = page_index * DoubleWritePage::SIZE + DoubleWriteBufferHeader::SIZE;
  if (lseek(file_desc_, offset, SEEK_SET) == -1) {
    LOG_ERROR("Failed to add page %lld of %d due to failed to seek %s.", offset, file_desc_, strerror(errno));
    return RC::IOERR_SEEK;
  }

  if (writen(file_desc_, page, DoubleWritePage::SIZE) != 0) {
    LOG_ERROR("Failed to add page %lld of %d due to %s.", offset, file_desc_, strerror(errno));
    return RC::IOERR_WRITE;
  }

  return RC::SUCCESS;
}

/**
 * @brief 把一个缓冲页写回它所属的真实分页文件
 * @details 标为 invalid 的页直接跳过：说明它已经写回过真实文件，再写一次会覆盖后来的更新。
 * @param dblwr_page 待写回的缓冲页
 */
RC DiskDoubleWriteBuffer::write_page(DoubleWritePage *dblwr_page)
{
  DiskBufferPool *disk_buffer = nullptr;
  // skip invalid page
  if (!dblwr_page->valid) {
    LOG_TRACE("double write buffer write page invalid. buffer_pool_id:%d,page_num:%d,lsn=%d",
              dblwr_page->key.buffer_pool_id, dblwr_page->key.page_num, dblwr_page->page.lsn);
    return RC::SUCCESS;
  }
  RC rc = bp_manager_.get_buffer_pool(dblwr_page->key.buffer_pool_id, disk_buffer);
  ASSERT(OB_SUCC(rc) && disk_buffer != nullptr, "failed to get disk buffer pool of %d", dblwr_page->key.buffer_pool_id);

  LOG_TRACE("double write buffer write page. buffer_pool_id:%d,page_num:%d,lsn=%d",
            dblwr_page->key.buffer_pool_id, dblwr_page->key.page_num, dblwr_page->page.lsn);

  return disk_buffer->write_page(dblwr_page->key.page_num, dblwr_page->page);
}

/**
 * @brief 从内存缓冲区中取回一个页
 * @details 只查内存映射，不读共享表空间文件。取回的内容比真实分页文件更新，
 * 因为页刷盘时是先写这里、之后才成批写回真实文件。
 * @return 缓冲区中没有该页时返回 BUFFERPOOL_INVALID_PAGE_NUM
 */
RC DiskDoubleWriteBuffer::read_page(DiskBufferPool *bp, PageNum page_num, Page &page)
{
  scoped_lock lock_guard(lock_);
  DoubleWritePageKey key{bp->id(), page_num};
  auto iter = dblwr_pages_.find(key);
  if (iter != dblwr_pages_.end()) {
    page = iter->second->page;
    LOG_TRACE("double write buffer read page success. bp id=%d, page_num:%d, lsn:%d", bp->id(), page_num, page.lsn);
    return RC::SUCCESS;
  }

  return RC::BUFFERPOOL_INVALID_PAGE_NUM;
}

/**
 * @brief 移除某个分页文件的全部缓冲页，并把它们写回该文件
 * @details 分页文件关闭或删除时调用。写回前按页号升序排序，避免小页号尚未写入就去 seek 更大的
 * 偏移而失败。
 * @param buffer_pool 目标分页文件
 */
RC DiskDoubleWriteBuffer::clear_pages(DiskBufferPool *buffer_pool)
{
  vector<DoubleWritePage *> spec_pages;
  
  auto remove_pred = [&spec_pages, buffer_pool](const pair<DoubleWritePageKey, DoubleWritePage *> &pair) {
    DoubleWritePage *dbl_page = pair.second;
    if (buffer_pool->id() == dbl_page->key.buffer_pool_id) {
      spec_pages.push_back(dbl_page);
      return true;
    }
    return false;
  };

  lock_.lock();
  erase_if(dblwr_pages_, remove_pred);
  lock_.unlock();

  LOG_INFO("clear pages in double write buffer. file name=%s, page count=%d",
           buffer_pool->filename(), spec_pages.size());

  // 页面从小到大排序，防止出现小页面还没有写入，而页面编号更大的seek失败的情况
  sort(spec_pages.begin(), spec_pages.end(), [](DoubleWritePage *a, DoubleWritePage *b) {
    return a->key.page_num < b->key.page_num;
  });

  RC rc = RC::SUCCESS;
  for (DoubleWritePage *dbl_page : spec_pages) {
    rc = buffer_pool->write_page(dbl_page->key.page_num, dbl_page->page);
    if (OB_FAIL(rc)) {
      LOG_WARN("Failed to write page %s:%d to disk buffer pool. rc=%s",
               buffer_pool->filename(), dbl_page->key.page_num, strrc(rc));
      break;
    }
    dbl_page->valid = false;
    write_page_internal(dbl_page);
  }

  for_each(spec_pages.begin(), spec_pages.end(), [](DoubleWritePage *dbl_page) { delete dbl_page; });

  return RC::SUCCESS;
}

/**
 * @brief 启动时把共享表空间文件中的页读回内存
 * @details 先读文件头拿到页数，再逐页读取并对页数据做 CRC32 校验；只有校验通过的页
 * 才进入内存映射，校验失败说明该页当初只写了一半，直接丢弃。
 */
RC DiskDoubleWriteBuffer::load_pages()
{
  if (file_desc_ < 0) {
    LOG_ERROR("Failed to load pages, due to file desc is invalid.");
    return RC::BUFFERPOOL_OPEN;
  }

  if (!dblwr_pages_.empty()) {
    LOG_ERROR("Failed to load pages, due to double write buffer is not empty. opened?");
    return RC::BUFFERPOOL_OPEN;
  }

  if (lseek(file_desc_, 0, SEEK_SET) == -1) {
    LOG_ERROR("Failed to load page header, due to failed to lseek:%s.", strerror(errno));
    return RC::IOERR_SEEK;
  }

  int ret = readn(file_desc_, &header_, sizeof(header_));
  if (ret != 0 && ret != -1) {
    LOG_ERROR("Failed to load page header, file_desc:%d, due to failed to read data:%s, ret=%d",
                file_desc_, strerror(errno), ret);
    return RC::IOERR_READ;
  }

  for (int page_num = 0; page_num < header_.page_cnt; page_num++) {
    int64_t offset = ((int64_t)page_num) * DoubleWritePage::SIZE + DoubleWriteBufferHeader::SIZE;

    if (lseek(file_desc_, offset, SEEK_SET) == -1) {
      LOG_ERROR("Failed to load page %d, offset=%ld, due to failed to lseek:%s.", page_num, offset, strerror(errno));
      return RC::IOERR_SEEK;
    }

    auto dblwr_page = make_unique<DoubleWritePage>();
    Page &page     = dblwr_page->page;
    page.check_sum = (CheckSum)-1;

    ret = readn(file_desc_, dblwr_page.get(), DoubleWritePage::SIZE);
    if (ret != 0) {
      LOG_ERROR("Failed to load page, file_desc:%d, page num:%d, due to failed to read data:%s, ret=%d, page count=%d",
                file_desc_, page_num, strerror(errno), ret, page_num);
      return RC::IOERR_READ;
    }

    const CheckSum check_sum = crc32(page.data, BP_PAGE_DATA_SIZE);
    if (check_sum == page.check_sum) {
      DoubleWritePageKey key = dblwr_page->key;
      dblwr_pages_.insert(pair<DoubleWritePageKey, DoubleWritePage *>(key, dblwr_page.release()));
    } else {
      LOG_TRACE("got a page with an invalid checksum. on disk:%d, in memory:%d", page.check_sum, check_sum);
    }
  }

  LOG_INFO("double write buffer load pages done. page num=%d", dblwr_pages_.size());
  return RC::SUCCESS;
}

/**
 * @brief 崩溃恢复：把共享表空间里残留的页写回真实分页文件
 * @details 复用 flush_page，因为"把缓冲区里的页写回真实文件"正是恢复要做的事。
 */
RC DiskDoubleWriteBuffer::recover()
{
  return flush_page();
}

////////////////////////////////////////////////////////////////
/**
 * @brief 空实现：不做任何保护，页直接写入目标分页文件
 */
RC VacuousDoubleWriteBuffer::add_page(DiskBufferPool *bp, PageNum page_num, Page &page)
{
  return bp->write_page(page_num, page);
}

