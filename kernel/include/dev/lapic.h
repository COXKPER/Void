/* VoidOS — Local APIC (LAPIC) driver
 * Handles LAPIC timer for preemptive scheduling and EOI.
 * Uses HHDM to access MMIO registers.
 */
#ifndef VOID_LAPIC_H
#define VOID_LAPIC_H 1

#include <void/types.h>

/* LAPIC register offsets (from LAPIC base) */
#define LAPIC_ID          0x020
#define LAPIC_VERSION     0x030
#define LAPIC_TPR         0x080  /* Task Priority Register */
#define LAPIC_EOI         0x0B0  /* End of Interrupt */
#define LAPIC_SPURIOUS    0x0F0  /* Spurious Interrupt Vector Register */
#define LAPIC_ICR_LO      0x300  /* Interrupt Command Register (low) */
#define LAPIC_ICR_HI      0x310  /* Interrupt Command Register (high) */
#define LAPIC_TIMER_LVT   0x320  /* Timer Local Vector Table entry */
#define LAPIC_TIMER_INIT  0x380  /* Timer Initial Count */
#define LAPIC_TIMER_CUR   0x390  /* Timer Current Count */
#define LAPIC_TIMER_DIV   0x3E0  /* Timer Divide Configuration */

/* Timer modes (bits 17:18 of TIMER_LVT) */
#define LAPIC_TIMER_PERIODIC  (1 << 17)
#define LAPIC_TIMER_MASKED    (1 << 16)

/* IDT vectors for hardware interrupts */
#define IRQ_VECTOR_TIMER     32
#define IRQ_VECTOR_SPURIOUS  255

/* ── public API ───────────────────────────────────────────────────────── */

/* Initialise LAPIC: disable PIC, enable LAPIC, calibrate and start timer.
 * hz = desired timer interrupt frequency (e.g. 100 for 100 Hz). */
void lapic_init(uint32_t hz);

/* Send End-of-Interrupt to LAPIC. Must be called at end of every IRQ handler. */
void lapic_eoi(void);

/* Read a LAPIC register. */
uint32_t lapic_read(uint32_t reg);

/* Write a LAPIC register. */
void lapic_write(uint32_t reg, uint32_t val);

#endif /* VOID_LAPIC_H */
