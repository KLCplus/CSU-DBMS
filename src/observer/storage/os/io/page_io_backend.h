#pragma once

#include "common/lang/memory.h"
#include "common/lang/string.h"
#include "common/sys/rc.h"
#include "storage/os/page/page.h"

enum class PageIOBackendType
{
  LEGACY,
  POSITIONAL,
};

const char *page_io_backend_name(PageIOBackendType type);
bool parse_page_io_backend(const string &name, PageIOBackendType &type);

/**
 * Page 文件 I/O 边界。实现只负责目标分页文件，不统计 WAL 或 double-write I/O。
 */
class PageIOBackend
{
public:
  virtual ~PageIOBackend() = default;

  virtual RC read_page(int fd, const char *file_name, PageNum page_num, Page &page) = 0;
  virtual RC write_page(int fd, const char *file_name, PageNum page_num, const Page &page) = 0;
  virtual RC sync(int fd, const char *file_name) = 0;
  virtual PageIOBackendType type() const = 0;
};

unique_ptr<PageIOBackend> create_page_io_backend(PageIOBackendType type);
