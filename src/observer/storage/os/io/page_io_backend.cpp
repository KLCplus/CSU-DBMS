/**
 * @file page_io_backend.cpp
 * @brief 目标分页文件 I/O 的两种实现
 * @ingroup BufferPool
 * @details legacy 用 lseek 加 read/write，会改变文件描述符的当前偏移；
 * positional 用 pread/pwrite，偏移由参数给出，不动文件描述符状态。
 * 两种后端都只负责目标分页文件，WAL 与双写缓冲的 I/O 不经过这里，
 * 也不计入分页文件的 disk_reads 与 disk_writes。
 */
#include "storage/os/io/page_io_backend.h"

#include <cerrno>
#include <cstring>
#include <strings.h>
#include <unistd.h>

#include "common/io/io.h"
#include "common/log/log.h"

namespace {

/**
 * @brief 用 lseek 加 read/write 的经典后端
 * @details 一次页读写需要两次系统调用，而且会改变文件描述符的共享偏移，
 * 因此同一分页文件的并发访问必须由调用方的 wr_lock_ 串行化。
 */
class LegacySeekPageIOBackend final : public PageIOBackend
{
public:
/**
 * @brief 从页号对应的偏移读一整页
 * @details 偏移等于页号乘以页大小。先 lseek 定位，再用 readn 保证整页读满；
 * 定位失败返回 IOERR_SEEK，读取失败返回 IOERR_READ。
 * @param fd 文件描述符
 * @param file_name 文件名，仅用于日志
 * @param page_num 页号
 * @param page 输出参数，页内容读到这里
 */
  RC read_page(int fd, const char *file_name, PageNum page_num, Page &page) override
  {
    const int64_t offset = static_cast<int64_t>(page_num) * BP_PAGE_SIZE;
    if (lseek(fd, offset, SEEK_SET) == -1) {
      LOG_ERROR("page io failed. backend=legacy operation=seek-read file=%s page=%d offset=%lld errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), errno, strerror(errno));
      return RC::IOERR_SEEK;
    }
    const int ret = common::readn(fd, &page, BP_PAGE_SIZE);
    if (ret != 0) {
      LOG_ERROR("page io failed. backend=legacy operation=read file=%s page=%d offset=%lld ret=%d errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), ret, errno, strerror(errno));
      return RC::IOERR_READ;
    }
    return RC::SUCCESS;
  }

/** @brief 从页号对应的偏移写一整页，先 lseek 再用 writen 保证整页写完 */
  RC write_page(int fd, const char *file_name, PageNum page_num, const Page &page) override
  {
    const int64_t offset = static_cast<int64_t>(page_num) * BP_PAGE_SIZE;
    if (lseek(fd, offset, SEEK_SET) == -1) {
      LOG_ERROR("page io failed. backend=legacy operation=seek-write file=%s page=%d offset=%lld errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), errno, strerror(errno));
      return RC::IOERR_SEEK;
    }
    const int ret = common::writen(fd, &page, BP_PAGE_SIZE);
    if (ret != 0) {
      LOG_ERROR("page io failed. backend=legacy operation=write file=%s page=%d offset=%lld ret=%d errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), ret, errno, strerror(errno));
      return RC::IOERR_WRITE;
    }
    return RC::SUCCESS;
  }

/** @brief 用 fsync 把文件数据真正刷到磁盘 */
  RC sync(int fd, const char *file_name) override
  {
    if (fsync(fd) != 0) {
      LOG_ERROR("page io failed. backend=legacy operation=sync file=%s errno=%d error=%s",
          file_name, errno, strerror(errno));
      return RC::IOERR_SYNC;
    }
    return RC::SUCCESS;
  }

/** @brief 返回后端类型标识 */
  PageIOBackendType type() const override { return PageIOBackendType::LEGACY; }
};

/**
 * @brief 用 pread/pwrite 的位置化后端
 * @details 偏移由参数给出，既不依赖也不改变文件描述符的当前位置，
 * 一次读写只用一个系统调用，比 legacy 更适合后续做并发 I/O 扩展。
 */
class PositionalPageIOBackend final : public PageIOBackend
{
public:
/** @brief 直接调用 preadn 读一整页，短读、EINTR 与提前 EOF 都由它循环处理 */
  RC read_page(int fd, const char *file_name, PageNum page_num, Page &page) override
  {
    const int64_t offset = static_cast<int64_t>(page_num) * BP_PAGE_SIZE;
    const int ret = common::preadn(fd, &page, BP_PAGE_SIZE, offset);
    if (ret != 0) {
      LOG_ERROR("page io failed. backend=positional operation=pread file=%s page=%d offset=%lld ret=%d errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), ret, errno, strerror(errno));
      return RC::IOERR_READ;
    }
    return RC::SUCCESS;
  }

/** @brief 直接调用 pwriten 写一整页，短写由它循环处理，零长度写入按 I/O 错误处理 */
  RC write_page(int fd, const char *file_name, PageNum page_num, const Page &page) override
  {
    const int64_t offset = static_cast<int64_t>(page_num) * BP_PAGE_SIZE;
    const int ret = common::pwriten(fd, &page, BP_PAGE_SIZE, offset);
    if (ret != 0) {
      LOG_ERROR("page io failed. backend=positional operation=pwrite file=%s page=%d offset=%lld ret=%d errno=%d error=%s",
          file_name, page_num, static_cast<long long>(offset), ret, errno, strerror(errno));
      return RC::IOERR_WRITE;
    }
    return RC::SUCCESS;
  }

/** @brief 用 fsync 把文件数据真正刷到磁盘 */
  RC sync(int fd, const char *file_name) override
  {
    if (fsync(fd) != 0) {
      LOG_ERROR("page io failed. backend=positional operation=sync file=%s errno=%d error=%s",
          file_name, errno, strerror(errno));
      return RC::IOERR_SYNC;
    }
    return RC::SUCCESS;
  }

/** @brief 返回后端类型标识 */
  PageIOBackendType type() const override { return PageIOBackendType::POSITIONAL; }
};

}  // namespace

/** @brief 把后端类型转成名字，用于日志、Trace 与 CLI 展示 */
const char *page_io_backend_name(PageIOBackendType type)
{
  switch (type) {
    case PageIOBackendType::LEGACY: return "legacy";
    case PageIOBackendType::POSITIONAL: return "positional";
  }
  return "unknown";
}

/**
 * @brief 解析命令行传入的后端名，大小写不敏感
 * @return 无法识别时返回 false，由上层决定回退到哪个后端
 */
bool parse_page_io_backend(const string &name, PageIOBackendType &type)
{
  if (strcasecmp(name.c_str(), "legacy") == 0) {
    type = PageIOBackendType::LEGACY;
    return true;
  }
  if (strcasecmp(name.c_str(), "positional") == 0) {
    type = PageIOBackendType::POSITIONAL;
    return true;
  }
  return false;
}

/**
 * @brief 后端工厂
 * @details 让 DiskBufferPool 里不出现按后端分支的 if/else。
 * @return 传入无法识别的枚举值时兜底返回 legacy，以保持既有运行语义
 */
unique_ptr<PageIOBackend> create_page_io_backend(PageIOBackendType type)
{
  switch (type) {
    case PageIOBackendType::LEGACY: return make_unique<LegacySeekPageIOBackend>();
    case PageIOBackendType::POSITIONAL: return make_unique<PositionalPageIOBackend>();
  }
  return make_unique<LegacySeekPageIOBackend>();
}
