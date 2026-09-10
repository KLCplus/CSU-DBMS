#pragma once

// OS layer: fixed-size database pages and their managed in-memory frames.
// BP_PAGE_SIZE remains 8192 bytes and is part of the existing on-disk format.
#include "storage/os/page/page.h"
#include "storage/os/page/frame.h"

