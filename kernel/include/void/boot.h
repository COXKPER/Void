/* VoidOS — global kernel boot context
 * Populated once by boot_init() from Limine responses; read-only thereafter.
 * Every subsystem reads from this instead of querying Limine structs directly,
 * so the Limine header is an implementation detail confined to boot.c.
 */
#ifndef VOID_BOOT_H
#define VOID_BOOT_H 1

#include <void/types.h>

/* ── framebuffer info ─────────────────────────────────────────────────── */
typedef struct {
    void    *base;          /* virtual address of pixel buffer   */
    uint64_t width;
    uint64_t height;
    uint64_t pitch;         /* bytes per scanline                */
    uint16_t bpp;           /* bits per pixel                    */
} boot_fb_t;

/* ── memory map entry (normalised from Limine) ────────────────────────── */
typedef enum {
    BOOT_MEM_USABLE,
    BOOT_MEM_RESERVED,
    BOOT_MEM_ACPI_RECLAIMABLE,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_BOOTLOADER_RECLAIMABLE,
    BOOT_MEM_KERNEL_AND_MODULES,
} boot_mem_type_t;

typedef struct {
    uint64_t        base;
    uint64_t        length;
    boot_mem_type_t type;
} boot_mmap_entry_t;

/* ── module info ──────────────────────────────────────────────────────── */
typedef struct {
    void    *base;          /* virtual address of module data    */
    uint64_t length;
    const char *cmdline;    /* module command line               */
} boot_module_t;

/* ── aggregate boot context ───────────────────────────────────────────── */
typedef struct {
    /* HHDM offset: virt = phys + hhdm_offset */
    uint64_t hhdm_offset;

    /* framebuffer (may be NULL if unavailable) */
    boot_fb_t *fb;

    /* physical memory map */
    boot_mmap_entry_t *mmap;
    uint64_t           mmap_count;

    /* boot modules (kernel is not included; index 0 = userland.img) */
    boot_module_t *modules;
    uint64_t       module_count;

    /* total usable physical memory (bytes, rounded to page boundary) */
    uint64_t total_usable_memory;
} boot_info_t;

/* ── global singleton ─────────────────────────────────────────────────── */
extern boot_info_t g_boot;

/* ── initializer (called once from kernel_main before anything else) ──── */
void boot_init(void);

#endif /* VOID_BOOT_H */
