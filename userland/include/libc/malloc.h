/* VoidOS — libc heap allocator (freestanding, no SSE)
 *
 * Phase 13 public surface: the four classic functions plus the policy
 * knob the allocator implementation uses to pick the backing store.
 * prototyped here so callers and the regression test only include one
 * allocator header.
 *
 * Alignment: every malloc/calloc/realloc return value is 16-byte
 * aligned — mandatory under -mno-sse (x87 doubles) and also what the
 * kernel's 16-b-aligned page mapping gives us naturally.
 *
 * Threshold: allocations of (sizeof-altogether) at least
 * MALLOC_MMAP_THRESHOLD bytes are backed by a private anonymous mmap,
 * released on free; smaller ones come from the brk heap given to the
 * allocator in page-aligned chunks (arena-via-init-latch design).
 * Callers never see the difference.
 */
#ifndef LIBC_MALLOC_H
#define LIBC_MALLOC_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backing-store switch between the brk heap and anonymous mmap. */
#define MALLOC_MMAP_THRESHOLD (128u * 1024u)   /* >= this uses mmap */

/* Allocate size bytes, 16-aligned, or NULL on failure.  malloc(0)
 * returns a unique non-NULL block that free() accepts (documented). */
void *malloc(size_t size);

/* Calloc: zeroed array of nmemb*size bytes, or NULL on overflow or
 * allocation failure. */
void *calloc(size_t nmemb, size_t size);

/* Realloc: resize a block from a prior malloc/calloc/realloc, copying
 * the min(old,new) bytes.  NULL behaves as malloc; size==0 behaves as
 * free+NULL.  On failure returns NULL and the original block is left
 * untouched (deterministically). */
void *realloc(void *ptr, size_t size);

/* Free a block from malloc/calloc/realloc.  free(NULL) is a no-op.
 * Double-free is a deterministic abort, not undefined behavior. */
void free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* LIBC_MALLOC_H */