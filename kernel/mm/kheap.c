/* VoidOS — Kernel heap allocator (first-fit free-list)
 * Grows upward from KHEAP_START by mapping 4 KiB pages on demand.
 * Each allocation is prefixed by a block_t header.
 * Correctness first — no perf tricks.
 */
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <void/boot.h>
#include <void/kpanic.h>
#include <dev/serial.h>

/* ── heap virtual address range ──────────────────────────────────────── */
/* Place heap in upper kernel space, well above __kernel_end.
 * 0xFFFFFFFF90000000 gives ~256 MiB gap from kernel base at 0xFFFFFFFF80000000. */
#define KHEAP_START 0xFFFFFFFF90000000ULL
#define KHEAP_MAX   0xFFFFFFFFA0000000ULL  /* 256 MiB max heap */

/* ── block header ────────────────────────────────────────────────────── */
typedef struct block {
    uint64_t       size;   /* usable bytes (excludes header) */
    bool           free;
    struct block  *next;
} block_t;

#define HEADER_SIZE  ((sizeof(block_t) + 15) & ~15ULL)  /* 16-byte aligned */
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* ── state ───────────────────────────────────────────────────────────── */
static block_t *head;          /* first block in the free list */
static uint64_t heap_top;      /* next unmapped virtual address */
static uint64_t heap_mapped;   /* bytes mapped so far */
static uint64_t bytes_used;    /* allocated bytes (excl. headers) */

/* ── grow heap by mapping pages ──────────────────────────────────────── */
static bool heap_grow(uint64_t need) {
    uint64_t target = ALIGN_UP(heap_top + need, PAGE_SIZE);
    if (target > KHEAP_MAX) return false;

    uint64_t cr3 = vmm_kernel_pml4();
    while (heap_top < target) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return false;

        void_status_t s = vmm_map_page((uint64_t *)cr3, heap_top, frame, VMM_KERN_RW);
        if (s != VOID_OK) {
            pmm_free_frame(frame);
            return false;
        }
        heap_top += PAGE_SIZE;
        heap_mapped += PAGE_SIZE;
    }
    return true;
}

/* ── kheap_init ──────────────────────────────────────────────────────── */
void kheap_init(void) {
    heap_top    = KHEAP_START;
    heap_mapped = 0;
    bytes_used  = 0;
    head        = NULL;

    /* Map initial page */
    if (!heap_grow(PAGE_SIZE)) {
        kprintf("[HEAP] FATAL: cannot map initial heap page\n\r");
        cpu_halt();
        __builtin_unreachable();
    }

    /* Create one large free block spanning the initial page */
    head       = (block_t *)KHEAP_START;
    head->size = PAGE_SIZE - HEADER_SIZE;
    head->free = true;
    head->next = NULL;

    kprintf("[HEAP] Initialised at 0x%016x, %u bytes available\n\r",
            KHEAP_START, head->size);
}

/* ── kmalloc ─────────────────────────────────────────────────────────── */
void *kmalloc(uint64_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size, 16);  /* 16-byte alignment */

    /* First fit.  The winner is *unlinked* (its predecessor now points at
     * its remainder / successor) and split in place, so the allocated half
     * is never also reachable from the free list.  A split that left the
     * winner linked — then freed and re-split later — made the same block
     * reachable twice and grew the freelist into cycles. */
    block_t **scan = &head;
    while (*scan) {
        block_t *cur = *scan;
        if (cur->free && cur->size >= size) {
            uint64_t remaining = cur->size - size - HEADER_SIZE;
            if (remaining >= 16) {
                block_t *rem = (block_t *)((uint8_t *)cur + HEADER_SIZE + size);
                rem->size  = remaining;
                rem->free  = true;
                rem->next  = cur->next;   /* remainder keeps the successor */
                cur->size  = size;
                cur->next  = NULL;        /* cur leaves the free list */
                *scan      = rem;         /* ... remainder takes its place */
            } else {
                *scan      = cur->next;   /* no room to split: just pop */
                cur->next  = NULL;
            }
            cur->free = false;
            bytes_used += cur->size;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        scan = &cur->next;
    }

    /* No free block — grow the heap */
    uint64_t total_need = HEADER_SIZE + size;
    uint64_t old_top = heap_top;
    if (!heap_grow(total_need))
        return NULL;

    /* Fresh block grows at old_top.  Merge it with the (possibly free)
     * tail first, then split it — keeping the remainder in the list and
     * handing the allocated half out (same discipline as first-fit). */
    block_t *b   = (block_t *)old_top;
    uint64_t bsz  = (heap_top - old_top) - HEADER_SIZE;

    block_t **slot = &head;
    if (head) {
        while ((*slot)->next) slot = &(*slot)->next;   /* walk to tail   */
        block_t *tail = *slot;
        if (tail->free &&
            (uint8_t *)tail + HEADER_SIZE + tail->size == (uint8_t *)b) {
            tail->size += HEADER_SIZE + bsz;           /* coalesce into tail */
            b    = tail;
            bsz  = tail->size;
        } else {
            tail->next = b;                            /* append */
            slot = &tail->next;
        }
    }

    /* b is now the tail node holding bsz bytes; split and allocate. */
    uint64_t remaining = bsz - size - HEADER_SIZE;
    if (remaining >= 16) {
        block_t *rem = (block_t *)((uint8_t *)b + HEADER_SIZE + size);
        rem->size  = remaining;
        rem->free  = true;
        rem->next  = NULL;         /* rem is the new tail */
        b->size    = size;
        b->next    = NULL;
        *slot      = rem;          /* remainder stays on the list  */
    } else {
        *slot = (b->next);         /* no remainder: pop b wholly  */
        b->next = NULL;
    }
    b->free = false;
    bytes_used += b->size;
    return (void *)((uint8_t *)b + HEADER_SIZE);
}

/* ── coalesce adjacent free blocks ───────────────────────────────────── */
static void coalesce(void) {
    block_t *cur = head;
    uint64_t guard = 0;
    while (cur && cur->next) {
        /* A freelist that ever cycles (heap corruption) would spin here
         * forever; treat that as fatal rather than hang. */
        if (++guard > 1000000) kpanic("kheap freelist cycle");
        if (cur->free && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
            /* Don't advance — check again in case of triple merge */
        } else {
            cur = cur->next;
        }
    }
}

/* ── kfree ───────────────────────────────────────────────────────────── */
void kfree(void *ptr) {
    if (!ptr) return;

    block_t *b = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (b->free) return;  /* double-free guard */

    b->free = true;
    bytes_used -= b->size;
    coalesce();
}

/* ── stats ───────────────────────────────────────────────────────────── */
uint64_t kheap_used(void) { return bytes_used; }
