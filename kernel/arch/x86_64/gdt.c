/* VoidOS — x86_64 GDT initialisation
 * Builds a minimal GDT, loads it via lgdt, and loads the TSS via ltr.
 */
#include <arch/x86_64/gdt.h>
#include <void/types.h>

/* ── GDT table (7 entries: null + 2 kernel + 2 user + TSS(2 slots)) ────
 * TSS descriptor occupies 16 bytes = 2 GDT slots (indices 5 and 6).    */
#define GDT_ENTRY_COUNT 7

static gdt_entry_t g_gdt[GDT_ENTRY_COUNT] ALIGNED(16) = {0};
tss_t              g_tss ALIGNED(16)         = {0};
static gdtr_t      g_gdtr;

/* ── helper: encode a code/data descriptor ────────────────────────────── */
static void gdt_set_entry(uint8_t idx, uint8_t access, uint8_t flags) {
    g_gdt[idx].base_lo         = 0;
    g_gdt[idx].base_mid        = 0;
    g_gdt[idx].base_hi         = 0;
    g_gdt[idx].limit_lo        = 0xFFFF;   /* full limit (granularity=1) */
    g_gdt[idx].flags_limit_hi  = (flags << 4) | 0x0F;
    g_gdt[idx].access          = access;
}

/* ── helper: encode TSS descriptor (two 8-byte slots) ────────────────── */
static void gdt_set_tss(uint8_t idx, uint64_t base, uint64_t limit) {
    /* Low 8 bytes */
    g_gdt[idx].limit_lo        = (uint16_t)(limit & 0xFFFF);
    g_gdt[idx].base_lo         = (uint16_t)(base  & 0xFFFF);
    g_gdt[idx].base_mid        = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[idx].access          = 0x89;     /* present, TSS 64-bit */
    g_gdt[idx].flags_limit_hi  = (uint8_t)(((limit >> 16) & 0x0F));
    g_gdt[idx].base_hi         = (uint8_t)((base >> 24) & 0xFF);

    /* High 8 bytes (upper 32 bits of base) */
    uint32_t base_hi32 = (uint32_t)(base >> 32);
    /* We store this in the next GDT slot as raw bytes */
    uint64_t *hi_slot = (uint64_t *)&g_gdt[idx + 1];
    *hi_slot = (uint64_t)base_hi32;
}

/* ── gdt_init ─────────────────────────────────────────────────────────── */
void gdt_init(void) {
    /* [0] Null descriptor — left zeroed */

    /* [1] Kernel Code Segment — Ring 0, 64-bit, execute/read
     *     Access: Present=1, DPL=0, Code=1, Conforming=0, Read=1, Accessed=0
     *     = 1001 1010 = 0x9A
     *     Flags:  Long=1, Size=0, Granularity=1
     *     = 1010 = 0xA                                                    */
    gdt_set_entry(1, 0x9A, 0xA);

    /* [2] Kernel Data Segment — Ring 0, 64-bit, read/write
     *     Access: Present=1, DPL=0, Data=0, Expand-up=0, Write=1, Accessed=0
     *     = 1001 0010 = 0x92
     *     Flags: Long=0 (data), Size=0, Granularity=1
     *     = 0010 = 0x2                                                    */
    gdt_set_entry(2, 0x92, 0x2);

    /* [3] User Data Segment — Ring 3, read/write
     *     Access: 1111 0010 = 0xF2
     *     Must come BEFORE user code: SYSRET computes SS from
     *     IA32_STAR[63:48]+8 and CS from +16.                            */
    gdt_set_entry(3, 0xF2, 0x2);

    /* [4] User Code Segment — Ring 3, 64-bit, execute/read
     *     Access: 1111 1010 = 0xFA
     *     Flags:  Long=1, Granularity=1 = 0xA                            */
    gdt_set_entry(4, 0xFA, 0xA);

    /* [5-6] TSS descriptor (16 bytes = 2 GDT slots) */
    g_tss.iopb_offset = 0xFFFF;   /* no IOPB */
    /* Zero IST entries — will be set up when IDT is initialised */
    gdt_set_tss(5, (uint64_t)&g_tss, sizeof(tss_t) - 1);

    /* ── Load GDTR ─────────────────────────────────────────────────── */
    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (uint64_t)&g_gdt;

    uint16_t data_sel = GDT_SEL_KDATA;
    __asm__ volatile (
        "lgdt %[gdtr]\n\t"
        /* Reload data segment registers */
        "mov %[sel], %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        /* Far return to reload CS */
        "pushq %[cs_sel]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : [gdtr]"m"(g_gdtr), [sel]"r"(data_sel), [cs_sel]"i"((uint64_t)GDT_SEL_KCODE)
        : "rax", "memory"
    );

    /* ── Load TR (Task Register) ───────────────────────────────────── */
    uint16_t tss_sel = GDT_SEL_TSS;
    __asm__ volatile (
        "ltr %[sel]"
        : : [sel]"r"(tss_sel)
    );
}

/* ── gdt_set_kernel_stack ─────────────────────────────────────────────
 * RSP0 is the stack the CPU switches to on a Ring 3 → Ring 0 transition
 * (interrupt or exception while in user mode).  The scheduler updates it
 * on every switch to a user thread, so a fault in Ring 3 lands on that
 * thread's own kernel stack rather than whatever was there before. */
void gdt_set_kernel_stack(uint64_t rsp0) {
    g_tss.rsp[0] = rsp0;
}
