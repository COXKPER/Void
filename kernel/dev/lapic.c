/* VoidOS — Local APIC driver
 * Disables legacy 8259 PIC, enables LAPIC, calibrates timer via PIT,
 * and starts periodic timer interrupts on IRQ_VECTOR_TIMER.
 */
#include <dev/lapic.h>
#include <void/boot.h>
#include <dev/serial.h>
#include <mm/vmm.h>

/* ── MSR addresses ───────────────────────────────────────────────────── */
#define IA32_APIC_BASE_MSR 0x1B

static volatile uint8_t *lapic_base;  /* HHDM-mapped LAPIC MMIO base */

/* ── MSR helpers ─────────────────────────────────────────────────────── */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── LAPIC register access ───────────────────────────────────────────── */
uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_base + reg);
}

void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/* ── disable legacy 8259 PIC ─────────────────────────────────────────── */
static void pic_disable(void) {
    /* Remap PIC to vectors 0x20–0x2F so any stray IRQs don't hit
     * CPU exception vectors, then mask everything. */
    outb(0x20, 0x11); outb(0xA0, 0x11);   /* ICW1: init + ICW4 needed */
    outb(0x21, 0x20); outb(0xA1, 0x28);   /* ICW2: remap to 0x20/0x28 */
    outb(0x21, 0x04); outb(0xA1, 0x02);   /* ICW3: cascade wiring     */
    outb(0x21, 0x01); outb(0xA1, 0x01);   /* ICW4: 8086 mode          */
    outb(0x21, 0xFF); outb(0xA1, 0xFF);   /* Mask all IRQs             */
}

/* ── PIT-based delay for LAPIC timer calibration ─────────────────────── */
/* PIT channel 2 one-shot, ~10 ms delay (11931 ticks at 1.193182 MHz) */
#define PIT_HZ       1193182
#define PIT_10MS     (PIT_HZ / 100)  /* ~11932 ticks ≈ 10 ms */

static void pit_wait_10ms(void) {
    /* Channel 2, mode 0 (one-shot), lo/hi byte */
    outb(0x43, 0xB0);                  /* channel 2, lobyte/hibyte, mode 0 */
    outb(0x42, (uint8_t)(PIT_10MS & 0xFF));
    outb(0x42, (uint8_t)(PIT_10MS >> 8));

    /* Gate channel 2: set bit 0 of port 0x61 */
    uint8_t gate = inb(0x61);
    outb(0x61, (gate & 0xFC) | 0x01);  /* enable gate, clear output */

    /* Wait for output bit (bit 5 of port 0x61) to go high */
    while (!(inb(0x61) & 0x20))
        __asm__ volatile ("pause");
}

/* ── lapic_init ──────────────────────────────────────────────────────── */
void lapic_init(uint32_t hz) {
    pic_disable();

    /* Find LAPIC physical base from MSR */
    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    uint64_t apic_phys = apic_msr & 0xFFFFFFFFF000ULL;

    /* Ensure APIC is globally enabled */
    apic_msr |= (1 << 11);  /* global enable bit */
    wrmsr(IA32_APIC_BASE_MSR, apic_msr);

    /* Map via HHDM — LAPIC MMIO is not RAM, so Limine doesn't map it.
     * We must explicitly map the page as uncacheable (PWT+PCD). */
    uint64_t lapic_virt = apic_phys + g_boot.hhdm_offset;
    vmm_map_page((uint64_t *)vmm_kernel_pml4(), lapic_virt, apic_phys,
                 VMM_PRESENT | VMM_WRITE | VMM_PWT | VMM_PCD);
    lapic_base = (volatile uint8_t *)lapic_virt;

    /* Enable LAPIC: set spurious vector and software-enable bit */
    lapic_write(LAPIC_SPURIOUS, IRQ_VECTOR_SPURIOUS | 0x100);

    /* Set task priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    kprintf("[LAPIC] Base: phys 0x%016x, MMIO @ %p\n\r",
            apic_phys, (void *)lapic_base);
    kprintf("[LAPIC] ID: %u, Version: 0x%x\n\r",
            lapic_read(LAPIC_ID) >> 24,
            lapic_read(LAPIC_VERSION) & 0xFF);

    /* ── Calibrate timer ─────────────────────────────────────────── */
    /* Use PIT to measure how many LAPIC ticks pass in 10 ms. */
    lapic_write(LAPIC_TIMER_DIV, 0x03);    /* divide by 16 */
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED);  /* masked one-shot */
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    pit_wait_10ms();

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED);  /* stop */

    /* ticks per second = elapsed * 100 (since 10 ms = 1/100 s) */
    uint32_t ticks_per_sec = elapsed * 100;
    uint32_t ticks_per_tick = ticks_per_sec / hz;

    kprintf("[LAPIC] Calibration: %u ticks/10ms, %u ticks/s\n\r",
            elapsed, ticks_per_sec);
    kprintf("[LAPIC] Timer: %u Hz, initial count = %u\n\r",
            (uint64_t)hz, (uint64_t)ticks_per_tick);

    /* Start periodic timer on IRQ_VECTOR_TIMER */
    lapic_write(LAPIC_TIMER_DIV, 0x03);    /* divide by 16 */
    lapic_write(LAPIC_TIMER_LVT, IRQ_VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, ticks_per_tick);
}
