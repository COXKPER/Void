/* VoidOS — boot initialisation
 * Validates Limine responses and populates g_boot for the rest of the kernel.
 * This file is the *only* translation unit that includes <boot/limine.h>;
 * everything else reads from g_boot.
 */
#include <void/types.h>
#include <void/boot.h>
#include <dev/serial.h>
#include <boot/limine.h>

/* ── Limine request declarations (defined in entry.asm) ───────────────── */
extern struct limine_hhdm_request         limine_hhdm_request;
extern struct limine_framebuffer_request  limine_framebuffer_request;
extern struct limine_memmap_request       limine_memmap_request;
extern struct limine_module_request       limine_module_request;
extern struct limine_rsdp_request         limine_rsdp_request;

/* ── global boot context ──────────────────────────────────────────────── */
boot_info_t g_boot = {0};

/* ── Limine memmap type → boot_mem_type_t ─────────────────────────────── */
static boot_mem_type_t convert_memmap_type(uint64_t limine_type) {
    switch (limine_type) {
    case LIMINE_MEMMAP_USABLE:                  return BOOT_MEM_USABLE;
    case LIMINE_MEMMAP_RESERVED:                return BOOT_MEM_RESERVED;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:        return BOOT_MEM_ACPI_RECLAIMABLE;
    case LIMINE_MEMMAP_ACPI_NVS:               return BOOT_MEM_ACPI_NVS;
    case LIMINE_MEMMAP_BAD_MEMORY:             return BOOT_MEM_BAD;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return BOOT_MEM_BOOTLOADER_RECLAIMABLE;
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return BOOT_MEM_KERNEL_AND_MODULES;
#if LIMINE_API_REVISION >= 2
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return BOOT_MEM_KERNEL_AND_MODULES;
#endif
    default:                                    return BOOT_MEM_RESERVED;
    }
}

/* ── boot_init ────────────────────────────────────────────────────────── */
void boot_init(void) {
    /* ── HHDM offset ──────────────────────────────────────────────── */
    struct limine_hhdm_response *hhdm = limine_hhdm_request.response;
    if (!hhdm) {
        kprintf("[boot] FATAL: HHDM response is NULL!\n\r");
        cpu_halt();
        __builtin_unreachable();
    }
    g_boot.hhdm_offset = hhdm->offset;

    /* ── Framebuffer ──────────────────────────────────────────────── */
    struct limine_framebuffer_response *fb = limine_framebuffer_request.response;
    if (fb && fb->framebuffer_count > 0) {
        struct limine_framebuffer *lfb = fb->framebuffers[0];
        /* We store framebuffer info in a static struct so g_boot.fb
         * is valid for the entire kernel lifetime.  We cast away
         * Limine's LIMINE_PTR indirection; in non-LIMINE_NO_POINTERS
         * mode these are real pointers. */
        static boot_fb_t kfb;
        kfb.base   = (void *)lfb->address;
        kfb.width  = lfb->width;
        kfb.height = lfb->height;
        kfb.pitch  = lfb->pitch;
        kfb.bpp    = lfb->bpp;
        g_boot.fb  = &kfb;
    } else {
        g_boot.fb = (void *)0;
    }

    /* ── Memory map ───────────────────────────────────────────────── */
    struct limine_memmap_response *mmap = limine_memmap_request.response;
    if (!mmap || mmap->entry_count == 0) {
        kprintf("[boot] FATAL: Memory map response is NULL/empty!\n\r");
        cpu_halt();
        __builtin_unreachable();
    }

    /* We need a contiguous array of boot_mmap_entry_t.  We can't
     * kmalloc yet (heap doesn't exist), so we use a static buffer
     * large enough for realistic firmware maps (~256 entries max). */
    #define STATIC_MMAP_MAX 256
    static boot_mmap_entry_t mmap_buf[STATIC_MMAP_MAX];

    uint64_t count = mmap->entry_count;
    if (count > STATIC_MMAP_MAX) {
        count = STATIC_MMAP_MAX;   /* truncate — will be fixed when kmalloc exists */
    }

    g_boot.total_usable_memory = 0;
    for (uint64_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        mmap_buf[i].base   = e->base;
        mmap_buf[i].length = e->length;
        mmap_buf[i].type   = convert_memmap_type(e->type);

        if (mmap_buf[i].type == BOOT_MEM_USABLE) {
            g_boot.total_usable_memory += e->length;
        }
    }
    g_boot.mmap      = mmap_buf;
    g_boot.mmap_count = count;

    /* ── ACPI RSDP ────────────────────────────────────────────────────
     * Limine scans the RDSP per the boot protocol; no physical EBDA scan.
     * 0 means the platform is not ACPI-compliant (acpi_init is optional). */
    struct limine_rsdp_response *rsdp = limine_rsdp_request.response;
    if (rsdp && rsdp->address) {
        g_boot.acpi_rsdp = (uint64_t)(uintptr_t)rsdp->address;
        kprintf("[boot] ACPI RSDP @ 0x%016x\n\r", rsdp->address);
    } else {
        g_boot.acpi_rsdp = 0;
        kprintf("[boot] No ACPI RSDP (platform not ACPI-compliant)\n\r");
    }

    /* ── Modules ──────────────────────────────────────────────────── */
    struct limine_module_response *mod = limine_module_request.response;
    if (mod && mod->module_count > 0) {
        #define STATIC_MOD_MAX 32
        static boot_module_t mod_buf[STATIC_MOD_MAX];

        uint64_t mcount = mod->module_count;
        if (mcount > STATIC_MOD_MAX) {
            mcount = STATIC_MOD_MAX;
        }

        for (uint64_t i = 0; i < mcount; i++) {
            struct limine_file *f = mod->modules[i];
            mod_buf[i].base    = (void *)f->address;
            mod_buf[i].length  = f->size;
            mod_buf[i].cmdline = (const char *)f->cmdline;
        }
        g_boot.modules      = mod_buf;
        g_boot.module_count = mcount;
    } else {
        g_boot.modules      = (void *)0;
        g_boot.module_count = 0;
    }
}
