#pragma once

#include "guest.h"

namespace memory {
// Allocate through the memory manager installed by the game
// (s3eMemorySetUserMemMgr), as the loader's s3eRealloc/s3eFree do.
addr_t user_realloc(addr_t ptr, uint32_t size);
void user_free(addr_t ptr);
}  // namespace memory
