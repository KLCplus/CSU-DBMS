#include "storage/os/io/page_io_backend.h"

#include <cerrno>
#include <cstring>
#include <strings.h>
#include <unistd.h>

#include "common/io/io.h"
#include "common/log/log.h"

namespace {

class LegacySeekPageIOBackend final : public PageIOBackend
{
public:
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

  RC sync(int fd, const char *file_name) override
  {
    if (fsync(fd) != 0) {
      LOG_ERROR("page io failed. backend=legacy operation=sync file=%s errno=%d error=%s",
          file_name, errno, strerror(errno));
      return RC::IOERR_SYNC;
    }
    return RC::SUCCESS;
  }

  PageIOBackendType type() const override { return PageIOBackendType::LEGACY; }
};

class PositionalPageIOBackend final : public PageIOBackend
{
public:
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

  RC sync(int fd, const char *file_name) override
  {
    if (fsync(fd) != 0) {
      LOG_ERROR("page io failed. backend=positional operation=sync file=%s errno=%d error=%s",
          file_name, errno, strerror(errno));
      return RC::IOERR_SYNC;
    }
    return RC::SUCCESS;
  }

  PageIOBackendType type() const override { return PageIOBackendType::POSITIONAL; }
};

}  // namespace

const char *page_io_backend_name(PageIOBackendType type)
{
  switch (type) {
    case PageIOBackendType::LEGACY: return "legacy";
    case PageIOBackendType::POSITIONAL: return "positional";
  }
  return "unknown";
}

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

unique_ptr<PageIOBackend> create_page_io_backend(PageIOBackendType type)
{
  switch (type) {
    case PageIOBackendType::LEGACY: return make_unique<LegacySeekPageIOBackend>();
    case PageIOBackendType::POSITIONAL: return make_unique<PositionalPageIOBackend>();
  }
  return make_unique<LegacySeekPageIOBackend>();
}
