/* VoidOS — Kernel Panic
 * Unrecoverable Ring-0 error handler.  Only fires on fatal kernel-space
 * conditions (unhandled exception, double fault, heap/TCB corruption,
 * userland.img load failure).  Ring-3 errors must kill the process
 * instead — NEVER panic from userland.
 *
 * When invoked:
 *   1. CLI — disable interrupts on this core
 *   2. (SMP) broadcast IPI HALT to all other cores   [TODO: SMP]
 *   3. Snapshot CPU registers (RIP, RSP, RFLAGS, CR2, CR3, RAX-R15)
 *   4. Print detailed message + register dump + stack trace
 *      to both serial (COM1) and VBE/GOP framebuffer
 *   5. Enter low-power infinite HLT loop
 */
#ifndef VOID_KPANIC_H
#define VOID_KPANIC_H 1

#include <void/types.h>

/* ── CPU snapshot captured at panic time ───────────────────────────────── */
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr2, cr3;
} panic_regs_t;

/* ── kpanic — never returns ───────────────────────────────────────────── */
NO_RETURN void kpanic(const char *fmt, ...);

/* ── kpanic_regs — panic with explicit register snapshot (from IDT) ──── */
NO_RETURN void kpanic_regs(const panic_regs_t *regs, const char *fmt, ...);

#endif /* VOID_KPANIC_H */
