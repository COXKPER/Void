/* VoidOS — x86_64 IDT definitions
 * 256-entry Interrupt Descriptor Table.
 * Vectors 0–31: CPU exceptions (ISR stubs in isr_stubs.asm)
 * Vectors 32–255: hardware IRQs + software interrupts (future)
 */
#ifndef VOID_IDT_H
#define VOID_IDT_H 1

#include <void/types.h>

/* ── IDT gate descriptor (16 bytes) ───────────────────────────────────── */
typedef struct {
    uint16_t offset_lo;     /* target RIP [15:0]         */
    uint16_t selector;      /* code segment selector     */
    uint8_t  ist;           /* IST index (0 = none)      */
    uint8_t  type_attr;     /* P=1, DPL, type (0x8E=int, 0x8F=trap) */
    uint16_t offset_mid;    /* target RIP [31:16]        */
    uint32_t offset_hi;     /* target RIP [63:32]        */
    uint32_t reserved;
} PACKED idt_gate_t;

/* ── IDTR payload ─────────────────────────────────────────────────────── */
typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED idtr_t;

/* ── Interrupt frame pushed by ISR stub + CPU ─────────────────────────── */
typedef struct {
    /* Pushed by ISR stub (isr_stubs.asm) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* Pushed by CPU on interrupt/exception */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} PACKED isr_frame_t;

/* ── public API ───────────────────────────────────────────────────────── */
void idt_init(void);

/* IRQ handler: receives frame, returns frame (may differ for context switch). */
typedef isr_frame_t *(*irq_handler_t)(isr_frame_t *frame);

/* Register a handler for hardware IRQ 0–15 (mapped to vectors 32–47). */
void idt_register_irq(uint8_t irq, irq_handler_t handler);

#endif /* VOID_IDT_H */
