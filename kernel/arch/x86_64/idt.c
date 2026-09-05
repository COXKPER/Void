/* VoidOS — x86_64 IDT initialisation + interrupt dispatch
 * Vectors 0–31:  CPU exceptions → print diagnostics, halt
 * Vectors 32–47: Hardware IRQs → call registered handler, EOI
 * Vector 255:    LAPIC spurious → ignore
 *
 * isr_dispatch returns isr_frame_t* — normally the same frame it received,
 * but the scheduler can swap it to switch threads via context switch.
 */
#include <arch/x86_64/idt.h>
#include <arch/x86_64/gdt.h>
#include <proc/process.h>
#include <dev/lapic.h>
#include <dev/serial.h>
#include <void/types.h>

/* ── IDT table (256 entries × 16 bytes = 4 KiB) ─────────────────────── */
static idt_gate_t g_idt[256] ALIGNED(16) = {0};
static idtr_t     g_idtr;

/* ── stub tables from isr_stubs.asm ──────────────────────────────────── */
extern uint64_t isr_stub_table[48];      /* vectors 0–47 */
extern uint64_t isr_stub_spurious;       /* vector 255   */

/* ── IRQ handler registry ────────────────────────────────────────────── */
static irq_handler_t irq_handlers[16] = {0};

void idt_register_irq(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) irq_handlers[irq] = handler;
}

/* ── exception names for diagnostics ─────────────────────────────────── */
static const char *exception_names[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI Non-Maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 FP Exception",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD FP Exception",
    "#VE Virtualization Exception",
    "#CP Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved",
    "#HV Hypervisor Injection",
    "#SX Security Exception",
    "Reserved",
};

/* ── set one IDT gate ────────────────────────────────────────────────── */
static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist) {
    g_idt[vector].offset_lo  = (uint16_t)(handler & 0xFFFF);
    g_idt[vector].selector   = GDT_SEL_KCODE;
    g_idt[vector].ist        = ist;
    g_idt[vector].type_attr  = 0x8E;   /* P=1, DPL=0, type=interrupt gate */
    g_idt[vector].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vector].offset_hi  = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved   = 0;
}

/* ── C interrupt dispatcher (called from isr_common in asm) ────────────
 * Returns the frame pointer to restore — normally same as input,
 * but scheduler can return a different thread's saved frame. */
isr_frame_t *isr_dispatch(isr_frame_t *frame) {
    uint64_t vec = frame->vector;

    /* ── Hardware IRQs (vectors 32–47) ───────────────────────────── */
    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        if (irq_handlers[irq])
            frame = irq_handlers[irq](frame);
        lapic_eoi();
        return frame;
    }

    /* ── LAPIC spurious (vector 255) — no EOI needed ─────────────── */
    if (vec == 255) return frame;

    /* ── CPU exceptions (vectors 0–31) ───────────────────────────── */
    const char *name = (vec < 32) ? exception_names[vec] : "Unknown";

    uint64_t cr2 = 0;
    if (vec == 14)
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    /* A fault taken with CS at RPL 3 came from user code.  Per the design
     * rule, that kills the offending process — it must never panic the
     * kernel.  Only Ring 0 faults are unrecoverable. */
    bool from_user = (frame->cs & 3) == 3;

    if (from_user) {
        process_t *p = process_current();
        kprintf("\n\r[FAULT] pid %u: %s at RIP 0x%016x\n\r",
                p ? (uint64_t)p->pid : (uint64_t)-1, name, frame->rip);
        kprintf("        error_code=0x%x  RSP=0x%016x\n\r",
                frame->error_code, frame->rsp);
        if (vec == 14) {
            /* PF error code bits: 0=present 1=write 2=user 3=rsvd 4=fetch */
            kprintf("        CR2=0x%016x  [%s%s%s]\n\r", cr2,
                    (frame->error_code & 1) ? "protection" : "not-present",
                    (frame->error_code & 2) ? " write" : " read",
                    (frame->error_code & 16) ? " exec" : "");
        }
        kprintf("        Terminating process.\n\r");
        /* Encode the killing signal the way POSIX wait() does: low 7 bits
         * hold the signal number.  SIGSEGV=11 for #PF, SIGBUS=7 for #GP. */
        return process_exit_current(frame, (vec == 14) ? 11 : 7);
    }

    kprintf("\n\r!!! EXCEPTION: vector %u — %s\n\r", vec, name);
    kprintf("    Error code: 0x%016x\n\r", frame->error_code);
    kprintf("    RIP:    0x%016x    CS:  0x%x\n\r", frame->rip, frame->cs);
    kprintf("    RSP:    0x%016x    SS:  0x%x\n\r", frame->rsp, frame->ss);
    kprintf("    RFLAGS: 0x%016x\n\r", frame->rflags);

    if (vec == 14)
        kprintf("    CR2 (fault addr): 0x%016x\n\r", cr2);

    kprintf("    RAX: 0x%016x  RBX: 0x%016x\n\r", frame->rax, frame->rbx);
    kprintf("    RCX: 0x%016x  RDX: 0x%016x\n\r", frame->rcx, frame->rdx);
    kprintf("    RSI: 0x%016x  RDI: 0x%016x\n\r", frame->rsi, frame->rdi);
    kprintf("    RBP: 0x%016x  R8:  0x%016x\n\r", frame->rbp, frame->r8);
    kprintf("    R9:  0x%016x  R10: 0x%016x\n\r", frame->r9,  frame->r10);
    kprintf("    R11: 0x%016x  R12: 0x%016x\n\r", frame->r11, frame->r12);
    kprintf("    R13: 0x%016x  R14: 0x%016x\n\r", frame->r13, frame->r14);
    kprintf("    R15: 0x%016x\n\r", frame->r15);

    kprintf("!!! System halted.\n\r");
    cpu_halt();
    __builtin_unreachable();
}

/* ── idt_init ────────────────────────────────────────────────────────── */
void idt_init(void) {
    /* Install exception handlers (vectors 0–31) + IRQ handlers (32–47) */
    for (int i = 0; i < 48; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], 0);
    }

    /* Install LAPIC spurious vector */
    idt_set_gate(255, isr_stub_spurious, 0);

    /* Load IDTR */
    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr) : "memory");
}
