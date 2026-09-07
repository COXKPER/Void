/* VoidOS — user address-space layout: heap (brk) and anonymous mmap
 *
 * Region policy — see include/mm/user_mem.h for the diagram.  This module
 * owns the two dynamically-shaped user regions.  Page ownership stays in
 * process_t.umap[]; the ELF loader's PRESENT-guard dedup for shared
 * boundary pages depends on that flat record, so heap pages are only ever
 * released through umap-driven helpers that keep the record consistent.
 */
#include <mm/user_mem.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <void/boot.h>
#include <syscall/syscall.h>   /* VE_* errno values */

#define USER_MMAP_CAP  0x0000800000000000ULL   /* lower-half limit       */
#define PAGE_MASK      (~(PAGE_SIZE - 1))

/* ── umap[] helpers ──────────────────────────────────────────────────── */
static int umap_index(process_t *p, uint64_t va) {
    for (uint32_t i = 0; i < p->umap_count; i++)
        if (p->umap[i].virt == va) return (int)i;
    return -1;
}

/* Remove umap index `i` by swapping in the tail.  O(1), order never
 * matters — nobody walks the record by address. */
static void umap_remove(process_t *p, uint32_t i) {
    p->umap[i] = p->umap[p->umap_count - 1];
    p->umap_count--;
}

/* ── user heap (brk) ─────────────────────────────────────────────────── */

bool um_brk_contains(process_t *p, uint64_t addr) {
    return p && p->heap_start && addr >= p->heap_start && addr < p->brk_current;
}

/* Grow the heap from brk_current up to new_brk (page-aligned), mapping a
 * zeroed user page at each 4 KiB step.  The loop bound new_brk/PAGE_SIZE is
 * capped at USER_MMAP_TOP / PAGE_SIZE, itself far below the 64-bit space, so
 * the counter cannot overflow. */
static int um_brk_grow(process_t *p, uint64_t new_brk) {
    for (uint64_t va = p->brk_current; va < new_brk; va += PAGE_SIZE) {
        if (process_alloc_user_page(p, va, p->brk_perm) != VOID_OK)
            return -VE_NOMEM;
    }
    return 0;
}

/* Set the break.  Growing maps zeroed pages; shrinking un-maps and frees
 * only whole pages wholly at/above the new break, keeping the page the
 * break lands on mapped.  Errors: ENOMEM (no frame), EINVAL (below BSS). */
int um_brk_set(process_t *p, uint64_t new_brk) {
    if (!p || !p->heap_start) return -VE_INVAL;
    if (new_brk < p->heap_start) return -VE_INVAL;

    if (new_brk > p->brk_current) {
        int r = um_brk_grow(p, new_brk);
        if (r) return r;
        p->brk_current = new_brk;
    } else if (new_brk < p->brk_current) {
        /* Shrink.  Release whole pages at/above the new break.  The break
         * itself can land mid-page; that partial page stays mapped. */
        uint64_t start = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
        if (umap_index(p, start) >= 0) {
            vmm_unmap_page((uint64_t *)p->cr3, start);
            pmm_free_frame(p->umap[umap_index(p, start)].phys);
            umap_remove(p, (uint32_t)umap_index(p, start));
        }
        for (uint32_t i = 0; i < p->umap_count; ) {
            if (p->umap[i].virt >= start && p->umap[i].virt < USER_MMAP_TOP) {
                vmm_unmap_page((uint64_t *)p->cr3, p->umap[i].virt);
                pmm_free_frame(p->umap[i].phys);
                umap_remove(p, i);
            } else {
                i++;
            }
        }
        p->brk_current = new_brk;
    }
    return 0;
}

/* ── anonymous mmap region ────────────────────────────────────────────── */
uint64_t um_mmap_floor(void) {
    return (USER_STACK_TOP - USER_STACK_SIZE);
}