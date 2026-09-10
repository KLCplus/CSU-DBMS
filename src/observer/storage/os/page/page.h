

#pragma once

#include "common/types.h"
#include <stdint.h>

using TrxID = int32_t;

static constexpr PageNum BP_INVALID_PAGE_NUM = -1;

static constexpr PageNum BP_HEADER_PAGE = 0;

static constexpr const int BP_PAGE_SIZE      = (1 << 13);
static constexpr const int BP_PAGE_DATA_SIZE = (BP_PAGE_SIZE - sizeof(LSN) - sizeof(CheckSum));

/**
 * @brief 表示一个页面，可能放在内存或磁盘上
 * @ingroup BufferPool
 */
struct Page
{
  LSN      lsn;
  CheckSum check_sum;
  char     data[BP_PAGE_DATA_SIZE];
};
