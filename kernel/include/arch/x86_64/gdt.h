/* VoidOS — x86_64 GDT + TSS definitions
 * Minimal 64-bit GDT layout:
 *   [0] Null descriptor
 *   [1] Kernel Code  (Ring 0, 64-bit, executable, read)
 *   [2] Kernel Data  (Ring 0, 64-bit, writable)
 *   [3] User Data    (Ring 3, 64-bit, writable)
 *   [4] User Code    (Ring 3, 64-bit, executable, read)
 *   [5] TSS          (16-byte IST-capable Task State Segment)
 *
 * Selector values:
 *   KC = 0x08   KD = 0x10   UD = 0x18   UC = 0x20   TSS = 0x28
 *
 * User Data MUST precede User Code: SYSRET derives CS from
 * IA32_STAR[63:48]+16 and SS from IA32_STAR[63:48]+8, so with
 * STAR[63:48] = 0x13 we get CS = 0x23 (index 4, RPL 3) and
 * SS = 0x1B (index 3, RPL 3).  Swapping them breaks SYSRET.
 */
#ifndef VOID_GDT_H
#define VOID_GDT_H 1

#include <stdint.h>
#include <void/types.h>

/* ── selector constants ───────────────────────────────────────────────── */
#define GDT_SEL_KCODE  0x08
#define GDT_SEL_KDATA  0x10
#define GDT_SEL_UDATA  0x18
#define GDT_SEL_UCODE  0x20
#define GDT_SEL_TSS    0x28

/* Ring-3 selectors carry RPL=3 in the low two bits. */
#define GDT_SEL_UDATA3 (GDT_SEL_UDATA | 3)   /* 0x1B */
#define GDT_SEL_UCODE3 (GDT_SEL_UCODE | 3)   /* 0x23 */

/* ── GDT entry (8 bytes for code/data, 16 for TSS) ───────────────────── */
typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;   /* flags[7:4] | limit_hi[3:0] */
    uint8_t  base_hi;
} PACKED gdt_entry_t;

/* ── 64-bit TSS (IST-capable) ─────────────────────────────────────────── */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];          /* IST not used yet; RSP0..2 for privilege stacks */
    uint64_t ist[7];          /* IST1..7 */
    uint32_t reserved1;
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;     /* IOPB offset (0xFFFF = no IOPB) */
} PACKED tss_t;

/* ── GDTR payload ─────────────────────────────────────────────────────── */
typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED gdtr_t;

/* ── public API ───────────────────────────────────────────────────────── */
void gdt_init(void);

/* Set TSS.RSP0 — the kernel stack used on Ring 3 → Ring 0 transitions.
 * Called by the scheduler when switching to a user thread. */
void gdt_set_kernel_stack(uint64_t rsp0);

/* Externally accessible TSS so IDT/syscall can update rsp0 */
extern tss_t g_tss;

#endif /* VOID_GDT_H */
