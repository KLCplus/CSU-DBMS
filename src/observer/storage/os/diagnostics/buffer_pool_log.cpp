

#include "storage/os/diagnostics/buffer_pool_log.h"
#include "storage/os/buffer/disk_buffer_pool.h"
#include "storage/clog/log_handler.h"
#include "storage/clog/log_entry.h"

/** @brief 转成包含分页文件 id、页号与操作类型的调试字符串 */
string BufferPoolLogEntry::to_string() const
{
  return string("buffer_pool_id=") + std::to_string(buffer_pool_id) +
         ", page_num=" + std::to_string(page_num) +
         ", operation_type=" + BufferPoolOperation(operation_type).to_string();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/** @brief 绑定所属的分页文件与底层日志处理器 */
BufferPoolLogHandler::BufferPoolLogHandler(DiskBufferPool &buffer_pool, LogHandler &log_handler)
    : buffer_pool_(buffer_pool), log_handler_(log_handler)
{}

/**
 * @brief 为一次页分配写日志
 * @param page_num 分配的页号
 * @param lsn 输出参数，写入该条日志的序列号，随后会被记到文件头页上
 */
RC BufferPoolLogHandler::allocate_page(PageNum page_num, LSN &lsn)
{
  return append_log(BufferPoolOperation::Type::ALLOCATE, page_num, lsn);
}

/** @brief 为一次页释放写日志，lsn 同样写回文件头页 */
RC BufferPoolLogHandler::deallocate_page(PageNum page_num, LSN &lsn)
{
  return append_log(BufferPoolOperation::Type::DEALLOCATE, page_num, lsn);
}

/**
 * @brief 页落盘之前，先等它对应的日志落盘
 * @details 只做一件事：等日志写到不小于该页的 LSN。这就是 WAL 先行：
 * 日志落后于数据页时若发生崩溃，已落盘的数据页将无法被正确恢复。
 * @param page 即将落盘的数据页
 */
RC BufferPoolLogHandler::flush_page(Page &page)
{
  return log_handler_.wait_lsn(page.lsn);
}

/** @brief 组装一条日志记录并追加到日志模块，页号与操作类型是全部内容 */
RC BufferPoolLogHandler::append_log(BufferPoolOperation::Type type, PageNum page_num, LSN &lsn)
{
  BufferPoolLogEntry log;
  log.buffer_pool_id = buffer_pool_.id();
  log.page_num = page_num;
  log.operation_type = BufferPoolOperation(type).type_id();

  return log_handler_.append(lsn, LogModule::Id::BUFFER_POOL, span<const char>(reinterpret_cast<const char *>(&log), sizeof(log)));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BufferPoolLogReplayer
/** @brief 保存 BufferPoolManager 引用，重放时按 id 找回分页文件 */
BufferPoolLogReplayer::BufferPoolLogReplayer(BufferPoolManager &bp_manager) : bp_manager_(bp_manager)
{}

/**
 * @brief 重放一条页分配或释放日志
 * @details 先校验日志体长度，再从记录中取出分页文件 id 与页号，
 * 找到对应的 DiskBufferPool 后调用它自己的 redo 函数。
 * 真正改文件头位图的逻辑在 redo_allocate_page 与 redo_deallocate_page 里，
 * 它们各自用 LSN 做幂等判断，因此重复重放同一条日志不会出错。
 * @param entry 待重放的日志
 */
RC BufferPoolLogReplayer::replay(const LogEntry &entry)
{
  if (entry.payload_size() != sizeof(BufferPoolLogEntry)) {
    LOG_ERROR("invalid buffer pool log entry. payload size=%d, expected=%d, entry=%s",
              entry.payload_size(), sizeof(BufferPoolLogEntry), entry.to_string().c_str());
    return RC::INVALID_ARGUMENT;
  }

  auto log = reinterpret_cast<const BufferPoolLogEntry *>(entry.data());

  LOG_TRACE("replay buffer pool log. entry=%s, log=%s", entry.to_string().c_str(), log->to_string().c_str());
  
  int32_t buffer_pool_id = log->buffer_pool_id;
  DiskBufferPool *buffer_pool = nullptr;
  RC rc = bp_manager_.get_buffer_pool(buffer_pool_id, buffer_pool);
  if (OB_FAIL(rc) || buffer_pool == nullptr) {
    LOG_ERROR("failed to get buffer pool. rc=%s, buffer pool=%p, log=%s, %s", 
              strrc(rc), buffer_pool, entry.to_string().c_str(), log->to_string().c_str());
    return rc;
  }

  BufferPoolOperation operation(log->operation_type);
  switch (operation.type())
  {
    case BufferPoolOperation::Type::ALLOCATE:
      return buffer_pool->redo_allocate_page(entry.lsn(), log->page_num);
    case BufferPoolOperation::Type::DEALLOCATE:
      return buffer_pool->redo_deallocate_page(entry.lsn(), log->page_num);
    default:
      LOG_ERROR("unknown buffer pool operation. operation=%s", operation.to_string().c_str());
      return RC::INTERNAL;
  }
  return RC::SUCCESS;
}
