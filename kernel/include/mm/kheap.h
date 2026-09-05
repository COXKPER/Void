/* VoidOS — Kernel heap allocator
 * Simple first-fit free-list allocator.
 * Grows on demand by mapping new pages via VMM + PMM.
 */
#ifndef VOID_KHEAP_H
#define VOID_KHEAP_H 1

#include <void/types.h>

/* Initialise the kernel heap. Must be called after PMM and VMM. */
void kheap_init(void);

/* Allocate `size` bytes (8-byte aligned). Returns NULL on failure. */
void *kmalloc(uint64_t size);

/* Free a previously kmalloc'd pointer. NULL-safe. */
void kfree(void *ptr);

/* Return total bytes currently allocated (excluding headers). */
uint64_t kheap_used(void);

#endif /* VOID_KHEAP_H */
