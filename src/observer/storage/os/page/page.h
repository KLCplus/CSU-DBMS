/**
 * @file page.h
 * @brief 固定大小数据库页的定义
 * @ingroup BufferPool
 * @details 页是内存与磁盘之间传输数据的最小单位。BP_PAGE_SIZE 恒为 8192 字节，
 * 属于磁盘格式的一部分，不能修改；页头 12 字节加数据区 8180 字节刚好等于一个页，
 * 因此上下层可以整块读写。
 */
#pragma once

#include "common/types.h"
#include <stdint.h>

/// 事务标识类型，仅在页级调试信息中使用
using TrxID = int32_t;

/// 非法页号哨兵。任何接口返回该值表示页号无效
static constexpr PageNum BP_INVALID_PAGE_NUM = -1;

/// 分页文件的第 0 页固定为文件头，其中存放 BPFileHeader
static constexpr PageNum BP_HEADER_PAGE = 0;

/// 页总大小，固定 8 KiB。该值同时是磁盘格式的一部分，不可修改
static constexpr const int BP_PAGE_SIZE      = (1 << 13);

/// 数据区大小，等于页总大小减去页头的 LSN 与 CheckSum，即 8192 - 8 - 4 = 8180
static constexpr const int BP_PAGE_DATA_SIZE = (BP_PAGE_SIZE - sizeof(LSN) - sizeof(CheckSum));

/**
 * @brief 表示一个页面，可能放在内存或磁盘上
 * @ingroup BufferPool
 * @details 结构体整体大小正好是 8192 字节，所以 load_page 与 write_page 可以直接
 * 整块读写，不需要拆装。data 区由上层解释：Record 页在其中存放页头、slot bitmap
 * 与定长记录，B+Tree 页在其中存放索引节点。
 */
struct Page
{
  LSN      lsn;                      ///< 日志序列号，崩溃恢复时用于判断该页是否需要 redo
  CheckSum check_sum;                ///< 页校验和，用于检测半页写等损坏
  char     data[BP_PAGE_DATA_SIZE];  ///< 数据区，8180 字节
};
