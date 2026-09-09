#pragma once

// Complete OS/storage facade. Prefer a narrower submodule header when possible.
#include "storage/os/record/record_page.h"
#include "storage/os/page/page_model.h"
#include "storage/os/buffer/buffer_pool.h"
#include "storage/os/replacement/replacement.h"
#include "storage/os/io/page_io.h"
#include "storage/os/diagnostics/diagnostics.h"

