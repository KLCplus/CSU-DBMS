/**
 * @file page_io_backend.h
 * @brief 目标分页文件的 I/O 边界
 * @ingroup BufferPool
 * @details 上层只依赖 PageIOBackend 这个接口，具体用哪种系统调用由实现决定。
 * 实现只负责目标分页文件，WAL 与双写缓冲的 I/O 不经过这里，也不计入分页文件统计。
 */
#pragma once

#include "common/lang/memory.h"
#include "common/lang/string.h"
#include "common/sys/rc.h"
#include "storage/os/page/page.h"

enum class PageIOBackendType
{
  LEGACY,      ///< lseek 加 read/write 的经典后端
  POSITIONAL,  ///< pread/pwrite 的位置化后端
};

/** @brief 把后端类型转成名字，用于日志、Trace 与 CLI 展示 */
const char *page_io_backend_name(PageIOBackendType type);
/**
 * @brief 解析命令行传入的后端名，大小写不敏感
 * @return 无法识别时返回 false，由上层决定回退到哪个后端
 */
bool parse_page_io_backend(const string &name, PageIOBackendType &type);

/**
 * Page 文件 I/O 边界。实现只负责目标分页文件，不统计 WAL 或 double-write I/O。
 */
class PageIOBackend
{
public:
  virtual ~PageIOBackend() = default;

/** @brief 从页号对应的偏移读一整页 */
  virtual RC read_page(int fd, const char *file_name, PageNum page_num, Page &page) = 0;
/** @brief 向页号对应的偏移写一整页 */
  virtual RC write_page(int fd, const char *file_name, PageNum page_num, const Page &page) = 0;
/** @brief 把文件数据真正刷到磁盘 */
  virtual RC sync(int fd, const char *file_name) = 0;
/** @brief 返回实现对应的类型标识 */
  virtual PageIOBackendType type() const = 0;
};

/**
 * @brief 后端工厂
 * @details 让 DiskBufferPool 里不出现按后端分支的 if/else。
 * @return 传入无法识别的枚举值时兜底返回 legacy，以保持既有运行语义
 */
unique_ptr<PageIOBackend> create_page_io_backend(PageIOBackendType type);
