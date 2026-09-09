#pragma once

// OS layer: fixed-size database pages and their managed in-memory frames.
// BP_PAGE_SIZE remains 8192 bytes and is part of the existing on-disk format.
#include "storage/buffer/page.h"
#include "storage/buffer/frame.h"

