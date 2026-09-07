/* VoidOS — user address-space layout helpers (heap region, mmap region)
 *
 * Region policy for the user lower half, top of memory downward:
 *
 *   [0x0000_0000_0040_0000]  ELF image (text/rodata/data/bss, from PHDRs)
 *   [heap_start .. brk]      heap, grows UP via brk()           (Phase 12)
 *   [mmap_base .. mmap_top]  anonymous mmap region, grows DOWN  (Phase 12)
 *   [0x0000_7FFF_FFE0_0000]  user stack (64 KiB), top at 0x0000_7000_0000_0000
 *
 * The heap and the mmap region are the dynamically-shaped parts; the ELF
 * image and the stack are fixed at load time.  Everything below the ELF is
 * kernel-owned; everything at or above USER_LIMIT is the kernel half.
 *
 * `brk` bounds the low (heap) region.  The mmap region is carved below the
 * stack first, so the two grow toward each other and can never overlap by
 * construction as long as brk_current stays below the mmap floor.
 */
#ifndef VOID_USER_MEM_H
#define VOID_USER_MEM_H 1

#include <void/types.h>
#include <proc/process.h>

/* Highest VA the mmap region may reach.  The stack lives just below
 * USER_STACK_TOP; the mmap region grows down from the first page under the
 * stack's 64 KiB footprint.  A map at or above this floor would collide
 * with the stack, so mmap refuses it. */
#define USER_MMAP_TOP     (USER_STACK_TOP - USER_STACK_SIZE)

/* ── user heap (brk) ──────────────────────────────────────────────────── */

/* True when `addr` lies inside [heap_start, brk_current). */
bool um_brk_contains(process_t *p, uint64_t addr);

/* Extend the heap so it spans [heap_start, new_brk), allocating zeroed user
 * pages for the delta.  Shrinking (new_brk below brk_current) only unmaps
 * and frees pages wholly at/above the new break; the partial page the break
 * lands on stays mapped. */
int um_brk_set(process_t *p, uint64_t new_brk);

/* ── anonymous mmap region ────────────────────────────────────────────── */

/* Page-aligned floor for the growing-down anonymous region: the first page
 * below the user stack's 64 KiB. */
uint64_t um_mmap_floor(void);

#endif /* VOID_USER_MEM_H */