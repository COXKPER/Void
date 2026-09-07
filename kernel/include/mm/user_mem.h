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

/* True when `addr` lies inside [heap_start, brk_current), i.e. the kernel
 * may dereference it without a user-pointer walk. */
bool um_brk_contains(process_t *p, uint64_t addr);

/* Extend the heap so it spans [heap_start, new_brk), allocating zeroed user
 * pages for the delta.  Shrinking (new_brk below brk_current) only unmaps
 * and frees pages wholly at/above the new break; the partial page the break
 * lands on stays mapped. */
int um_brk_set(process_t *p, uint64_t new_brk);

/* ── anonymous mmap region ────────────────────────────────────────────── */

/* END (exclusive) of the growing-down anonymous region: the stack's bottom
 * edge.  mmap maps with base+len <= USER_MMAP_TOP (the region grows down
 * toward the heap) and refuses anything that would cross it — that would
 * overlap the stack. */

/* Top-down anonymous region metadata: one entry per mapped range, singly
 * linked, addresses excluded (both ends exclusive).  A tiny sorted list is
 * per the Phase 12 spec; it stays small because maps are few. */
typedef struct vm_area {
    uint64_t      start;          /* first byte (page-aligned)        */
    uint64_t      end;            /* one past last byte (page-aligned)*/
    uint64_t      prot;           /* VMM_* intent (WRITE/NX)          */
    uint64_t      flags;          /* VMM_* (PRESENT|USER implied)     */
    uint32_t      kind;           /* VM_AREA_STACK / _HEAP / _ANON    */
    struct vm_area *next;
} vm_area_t;

enum { VM_AREA_STACK = 1, VM_AREA_HEAP, VM_AREA_ANON };

/* Validate that [start,end) overlaps no *allocated* user page — the stack
 * footprint, the heap span [heap_start,brk_current), any PT_LOAD pages.  No
 * vm_area list is consulted because the ELF loader records pages rather
 * than regions; the page tables are the source of truth. */
bool um_range_clear(process_t *p, uint64_t start, uint64_t end);

/* Reserve a page-aligned anonymous mapping of `length` bytes starting at the
 * first unmapped run of that size just below the stack (growing down),
 * returning the reserved base via *out.  Returns 0 on success, -VE_NOMEM
 * when no contiguous clear range exists (including colliding with the stack
 * or the heap). */
int um_mmap_reserve(process_t *p, uint64_t length, uint64_t *out);

/* Unmap and free every allocated user page at/above `start` (used by
 * munmap and brk shrink).  Only pages wholly within the caller-owned
 * lower half are released; the user stack is never touched. */
void um_release_from(process_t *p, uint64_t start);

#endif /* VOID_USER_MEM_H */