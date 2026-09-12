// Minimal declarations for the mspace subset of dlmalloc.c we use.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *mspace;
mspace create_mspace_with_base(void *base, size_t capacity, int locked);
size_t destroy_mspace(mspace msp);
void *mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void *mem);
void *mspace_realloc(mspace msp, void *mem, size_t newsize);
void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void *mem);
size_t mspace_footprint(mspace msp);
#ifdef __cplusplus
}
#endif
