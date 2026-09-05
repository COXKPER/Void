/* VoidOS — Virtual Memory Manager (4-level paging)
 * Operates on x86_64 PML4 page tables.
 * Uses HHDM to access page table frames without identity mapping.
 */
#ifndef VOID_VMM_H
#define VOID_VMM_H 1

#include <void/types.h>

/* ── page table entry flags ───────────────────────────────────────────── */
#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)
#define VMM_PCD       (1ULL << 4)
#define VMM_ACCESSED  (1ULL << 5)
#define VMM_DIRTY     (1ULL << 6)
#define VMM_HUGE      (1ULL << 7)   /* 2 MiB page (PD level) */
#define VMM_GLOBAL    (1ULL << 8)
#define VMM_NX        (1ULL << 63)

/* Common flag combos */
#define VMM_KERN_RW   (VMM_PRESENT | VMM_WRITE)
#define VMM_KERN_RO   (VMM_PRESENT)
#define VMM_USER_RW   (VMM_PRESENT | VMM_WRITE | VMM_USER)
#define VMM_USER_RO   (VMM_PRESENT | VMM_USER)

/* ── public API ───────────────────────────────────────────────────────── */

/* Initialise VMM — reads current CR3, sets up internal state. */
void vmm_init(void);

/* Map a single 4 KiB page: virt → phys with given flags.
 * Allocates intermediate page table levels via PMM as needed.
 * Returns VOID_OK on success. */
void_status_t vmm_map_page(uint64_t *pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a single 4 KiB page. Invalidates TLB entry.
 * Returns the physical address that was mapped, or 0 if not mapped. */
uint64_t vmm_unmap_page(uint64_t *pml4_phys, uint64_t virt);

/* Translate a virtual address to physical using given PML4.
 * Returns physical address or 0 if not mapped. */
uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virt);

/* Return the raw leaf PTE for `virt` (flags included), or 0 if unmapped.
 * Callers use this to inspect PRESENT/USER/WRITE before trusting an
 * address that came from Ring 3. */
uint64_t vmm_get_pte(uint64_t *pml4_phys, uint64_t virt);

/* Get the current CR3 (physical address of active PML4). */
uint64_t vmm_get_cr3(void);

/* Get the kernel PML4 physical address. */
uint64_t vmm_kernel_pml4(void);

#endif /* VOID_VMM_H */
