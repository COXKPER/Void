/* VoidOS — Physical Memory Manager (bitmap-based)
 * Each bit represents one 4 KiB frame: 0 = free, 1 = used/reserved.
 * The bitmap is placed in the first usable memory region large enough
 * to hold it, accessed via HHDM.
 */
#include <mm/pmm.h>
#include <void/boot.h>
#include <dev/serial.h>

/* ── bitmap state ────────────────────────────────────────────────────── */
static uint8_t *bitmap;         /* virtual address (HHDM-mapped) */
static uint64_t bitmap_size;    /* bytes */
static uint64_t total_frames;   /* total frames the bitmap tracks */
static uint64_t free_frames;    /* currently free */

/* ── helpers ─────────────────────────────────────────────────────────── */
static inline void bit_set(uint64_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1 << (frame % 8));
}
static inline void bit_clear(uint64_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1 << (frame % 8));
}
static inline bool bit_test(uint64_t frame) {
    return (bitmap[frame / 8] >> (frame % 8)) & 1;
}

/* ── phys ↔ HHDM virt ───────────────────────────────────────────────── */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + g_boot.hhdm_offset);
}

/* ── pmm_init ────────────────────────────────────────────────────────── */
void pmm_init(void) {
    /* 1. Find the highest physical address to size the bitmap */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < g_boot.mmap_count; i++) {
        uint64_t top = g_boot.mmap[i].base + g_boot.mmap[i].length;
        if (top > highest_addr)
            highest_addr = top;
    }

    total_frames = highest_addr / PAGE_SIZE;
    bitmap_size  = (total_frames + 7) / 8;

    /* 2. Find a usable region big enough to hold the bitmap */
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < g_boot.mmap_count; i++) {
        if (g_boot.mmap[i].type != BOOT_MEM_USABLE)
            continue;
        if (g_boot.mmap[i].length >= bitmap_size) {
            bitmap_phys = g_boot.mmap[i].base;
            break;
        }
    }

    if (bitmap_phys == 0) {
        kprintf("[PMM] FATAL: no region large enough for bitmap (%u bytes)\n\r",
                bitmap_size);
        cpu_halt();
        __builtin_unreachable();
    }

    bitmap = (uint8_t *)phys_to_virt(bitmap_phys);

    /* 3. Mark ALL frames as used (1), then free only usable ones */
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    free_frames = 0;

    /* 4. Free frames in usable regions */
    for (uint64_t i = 0; i < g_boot.mmap_count; i++) {
        if (g_boot.mmap[i].type != BOOT_MEM_USABLE)
            continue;

        uint64_t base = g_boot.mmap[i].base;
        uint64_t len  = g_boot.mmap[i].length;

        /* Page-align: round base up, length down */
        uint64_t start_frame = (base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end_frame   = (base + len) / PAGE_SIZE;

        for (uint64_t f = start_frame; f < end_frame; f++) {
            bit_clear(f);
            free_frames++;
        }
    }

    /* 5. Reserve the bitmap's own frames */
    uint64_t bm_start = bitmap_phys / PAGE_SIZE;
    uint64_t bm_end   = (bitmap_phys + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = bm_start; f < bm_end; f++) {
        if (!bit_test(f)) {
            bit_set(f);
            free_frames--;
        }
    }

    /* 6. Reserve frame 0 (null-page guard) */
    if (total_frames > 0 && !bit_test(0)) {
        bit_set(0);
        free_frames--;
    }

    kprintf("[PMM] %u frames total, %u free (%u KiB), bitmap at phys 0x%x\n\r",
            total_frames, free_frames,
            free_frames * PAGE_SIZE / 1024, bitmap_phys);
}

/* ── pmm_alloc_frame ─────────────────────────────────────────────────── */
uint64_t pmm_alloc_frame(void) {
    /* ponytail: linear scan; upgrade to next-fit index or buddy when needed */
    for (uint64_t i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFF)
            continue; /* all 8 frames used */
        for (int bit = 0; bit < 8; bit++) {
            uint64_t frame = i * 8 + bit;
            if (frame >= total_frames)
                return 0;
            if (!bit_test(frame)) {
                bit_set(frame);
                free_frames--;
                return frame * PAGE_SIZE;
            }
        }
    }
    return 0; /* OOM */
}

/* ── pmm_free_frame ──────────────────────────────────────────────────── */
void pmm_free_frame(uint64_t phys_addr) {
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= total_frames)
        return;
    if (bit_test(frame)) {
        bit_clear(frame);
        free_frames++;
    }
}

/* ── stats ───────────────────────────────────────────────────────────── */
uint64_t pmm_free_count(void)  { return free_frames; }
uint64_t pmm_total_count(void) { return total_frames; }
