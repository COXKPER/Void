/* VoidOS — Physical Memory Manager (bitmap-based)
 * Manages 4 KiB physical frames using a bitmap allocator.
 * Initialised from g_boot.mmap; only BOOT_MEM_USABLE regions are free.
 */
#ifndef VOID_PMM_H
#define VOID_PMM_H 1

#include <void/types.h>

#define PAGE_SIZE  4096ULL
#define PAGE_SHIFT 12

/* ── public API ───────────────────────────────────────────────────────── */

/* Initialise PMM from boot memory map. Must be called after boot_init(). */
void pmm_init(void);

/* Allocate one physical 4 KiB frame. Returns physical address, or 0 on OOM. */
uint64_t pmm_alloc_frame(void);

/* Free a previously allocated physical frame. */
void pmm_free_frame(uint64_t phys_addr);

/* Return count of free frames. */
uint64_t pmm_free_count(void);

/* Return total managed frames. */
uint64_t pmm_total_count(void);

#endif /* VOID_PMM_H */
