/* VoidOS — Virtual Memory Manager (4-level paging)
 * Works with Limine's existing page tables via HHDM.
 * All page table frames are accessed as: virt = phys + hhdm_offset.
 */
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <void/boot.h>
#include <dev/serial.h>

/* ── internal state ──────────────────────────────────────────────────── */
static uint64_t kernel_cr3;   /* physical address of kernel PML4 */

/* ── HHDM helpers ────────────────────────────────────────────────────── */
static inline uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(phys + g_boot.hhdm_offset);
}

/* ── page table index extraction ─────────────────────────────────────── */
#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define PHYS_MASK 0x000FFFFFFFFFF000ULL  /* bits [51:12] = physical address */

/* ── allocate a zeroed page table frame ──────────────────────────────── */
static uint64_t alloc_table(void) {
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;
    /* Zero the frame via HHDM */
    uint8_t *p = (uint8_t *)phys_to_virt(frame);
    for (int i = 0; i < 4096; i++) p[i] = 0;
    return frame;
}

/* ── ensure a page table entry exists at a given level ───────────────── */
static uint64_t *walk_or_create(uint64_t *table, uint64_t index, uint64_t flags) {
    if (!(table[index] & VMM_PRESENT)) {
        uint64_t new_frame = alloc_table();
        if (!new_frame) return NULL;
        table[index] = new_frame | flags;
    }
    return phys_to_virt(table[index] & PHYS_MASK);
}

/* ── vmm_map_page ────────────────────────────────────────────────────── */
void_status_t vmm_map_page(uint64_t *pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = phys_to_virt((uint64_t)pml4_phys);

    /* Intermediate levels always need PRESENT|WRITE (and USER if user page) */
    uint64_t tbl_flags = VMM_PRESENT | VMM_WRITE;
    if (flags & VMM_USER) tbl_flags |= VMM_USER;

    uint64_t *pdpt = walk_or_create(pml4, PML4_IDX(virt), tbl_flags);
    if (!pdpt) return VOID_ERR_NOMEM;

    uint64_t *pd = walk_or_create(pdpt, PDPT_IDX(virt), tbl_flags);
    if (!pd) return VOID_ERR_NOMEM;

    uint64_t *pt = walk_or_create(pd, PD_IDX(virt), tbl_flags);
    if (!pt) return VOID_ERR_NOMEM;

    pt[PT_IDX(virt)] = (phys & PHYS_MASK) | flags;
    /* Invalidate TLB for this address */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return VOID_OK;
}

/* ── vmm_unmap_page ──────────────────────────────────────────────────── */
uint64_t vmm_unmap_page(uint64_t *pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt((uint64_t)pml4_phys);

    uint64_t pml4e = pml4[PML4_IDX(virt)];
    if (!(pml4e & VMM_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_virt(pml4e & PHYS_MASK);
    uint64_t pdpte = pdpt[PDPT_IDX(virt)];
    if (!(pdpte & VMM_PRESENT)) return 0;

    uint64_t *pd = phys_to_virt(pdpte & PHYS_MASK);
    uint64_t pde = pd[PD_IDX(virt)];
    if (!(pde & VMM_PRESENT)) return 0;

    uint64_t *pt = phys_to_virt(pde & PHYS_MASK);
    uint64_t pte = pt[PT_IDX(virt)];
    if (!(pte & VMM_PRESENT)) return 0;

    uint64_t phys = pte & PHYS_MASK;
    pt[PT_IDX(virt)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return phys;
}

/* ── vmm_virt_to_phys ────────────────────────────────────────────────── */
uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt((uint64_t)pml4_phys);

    uint64_t pml4e = pml4[PML4_IDX(virt)];
    if (!(pml4e & VMM_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_virt(pml4e & PHYS_MASK);
    uint64_t pdpte = pdpt[PDPT_IDX(virt)];
    if (!(pdpte & VMM_PRESENT)) return 0;
    if (pdpte & VMM_HUGE) return (pdpte & 0xFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);

    uint64_t *pd = phys_to_virt(pdpte & PHYS_MASK);
    uint64_t pde = pd[PD_IDX(virt)];
    if (!(pde & VMM_PRESENT)) return 0;
    if (pde & VMM_HUGE) return (pde & 0xFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);

    uint64_t *pt = phys_to_virt(pde & PHYS_MASK);
    uint64_t pte = pt[PT_IDX(virt)];
    if (!(pte & VMM_PRESENT)) return 0;

    return (pte & PHYS_MASK) | (virt & 0xFFF);
}

/* ── vmm_get_pte ─────────────────────────────────────────────────────────
 * Returns the raw leaf entry (with flags) so callers can check
 * PRESENT/USER/WRITE.  Huge pages return the PDPTE/PDE itself, whose flag
 * bits mean the same thing at those levels. */
uint64_t vmm_get_pte(uint64_t *pml4_phys, uint64_t virt) {
    uint64_t *pml4 = phys_to_virt((uint64_t)pml4_phys);

    uint64_t pml4e = pml4[PML4_IDX(virt)];
    if (!(pml4e & VMM_PRESENT)) return 0;

    uint64_t *pdpt = phys_to_virt(pml4e & PHYS_MASK);
    uint64_t pdpte = pdpt[PDPT_IDX(virt)];
    if (!(pdpte & VMM_PRESENT)) return 0;
    if (pdpte & VMM_HUGE) return pdpte;

    uint64_t *pd = phys_to_virt(pdpte & PHYS_MASK);
    uint64_t pde = pd[PD_IDX(virt)];
    if (!(pde & VMM_PRESENT)) return 0;
    if (pde & VMM_HUGE) return pde;

    uint64_t *pt = phys_to_virt(pde & PHYS_MASK);
    return pt[PT_IDX(virt)];
}

/* ── CR3 helpers ─────────────────────────────────────────────────────── */
uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uint64_t vmm_kernel_pml4(void) { return kernel_cr3; }

/* ── vmm_init ────────────────────────────────────────────────────────── */
void vmm_init(void) {
    kernel_cr3 = vmm_get_cr3();

    /* Verify HHDM mapping: translate a known kernel virtual address
     * through the page tables and confirm it resolves correctly. */
    uint64_t test_virt = (uint64_t)&kernel_cr3;  /* a known kernel variable */
    uint64_t test_phys = vmm_virt_to_phys((uint64_t *)kernel_cr3, test_virt);

    kprintf("[VMM] Kernel CR3: 0x%016x\n\r", kernel_cr3);
    kprintf("[VMM] HHDM verify: virt 0x%016x → phys 0x%016x\n\r",
            test_virt, test_phys);

    if (test_phys == 0) {
        kprintf("[VMM] WARNING: HHDM translation failed for kernel var!\n\r");
    }

    /* Verify HHDM offset mapping works */
    uint64_t hhdm_test = g_boot.hhdm_offset;
    uint64_t hhdm_phys = vmm_virt_to_phys((uint64_t *)kernel_cr3, hhdm_test);
    kprintf("[VMM] HHDM base: virt 0x%016x → phys 0x%016x\n\r",
            hhdm_test, hhdm_phys);
}
