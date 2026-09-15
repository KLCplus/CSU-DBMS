/**
 * @file page_model.h
 * @brief Page 模块的聚合入口
 * @ingroup BufferPool
 * @details 只做转发包含，不定义任何类型；实现仍在 page.h 与 frame.h。上层如果只需要
 * 页与页帧，从这个头文件进入即可。BP_PAGE_SIZE 恒为 8192 字节，属于磁盘格式的一部分。
 */
#pragma once

#include "storage/os/page/page.h"
#include "storage/os/page/frame.h"
