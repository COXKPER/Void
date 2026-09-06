/* VoidOS — kernel C entry point
 * Called from entry.asm after BSS zero and stack setup.
 * Orchestrates early init in strict dependency order.
 */
#include <void/types.h>
#include <void/boot.h>
#include <dev/console.h>
#include <dev/serial.h>
#include <arch/x86_64/idt.h>
#include <dev/lapic.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <proc/process.h>
#include <syscall/syscall.h>

/* Forward declarations for subsystems */
extern void gdt_init(void);
extern void idt_init(void);
extern void pmm_init(void);
extern void vmm_init(void);
extern void kheap_init(void);

/* Ring-3 test blobs (kernel/proc/user_test.asm) */
extern const uint8_t user_prog_start[], user_prog_end[];
extern const uint8_t user_bad_start[],  user_bad_end[];

/* ELF test images (kernel/elf/elf_blobs.asm) */
extern const uint8_t elf_test_start[], elf_test_end[];
extern const uint8_t elf_packed_start[], elf_packed_end[];
extern const uint8_t elf_init_start[], elf_init_end[];

/* ELF self-check (kernel/elf/elf_selftest.c) */
uint32_t elf_selftest(const void *image, uint64_t size);

/* ── timer IRQ → scheduler ───────────────────────────────────────────── */
static isr_frame_t *timer_handler(isr_frame_t *frame) {
    return sched_tick(frame);
}

/* ── address-space isolation self-test (kernel side) ─────────────────────
 * Maps the same user VA in two processes, writes a distinct value through
 * each, then re-reads under each CR3.  Shared page tables would make both
 * reads return the same value. */
static void test_address_space_isolation(void) {
    const uint64_t probe = 0x0000000000401000ULL;
    uint64_t saved_cr3 = vmm_get_cr3();

    process_t *a = process_alloc(0);
    process_t *b = process_alloc(0);
    if (!a || !b) {
        kprintf("[TEST] FAIL: could not allocate two processes\n\r");
        return;
    }

    if (process_alloc_user_page(a, probe, VMM_WRITE) != VOID_OK ||
        process_alloc_user_page(b, probe, VMM_WRITE) != VOID_OK) {
        kprintf("[TEST] FAIL: could not map user probe page\n\r");
        return;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(a->cr3) : "memory");
    *(volatile uint64_t *)probe = 0xAAAAAAAAAAAAAAAAULL;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(b->cr3) : "memory");
    *(volatile uint64_t *)probe = 0xBBBBBBBBBBBBBBBBULL;

    __asm__ volatile ("mov %0, %%cr3" : : "r"(a->cr3) : "memory");
    uint64_t va = *(volatile uint64_t *)probe;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(b->cr3) : "memory");
    uint64_t vb = *(volatile uint64_t *)probe;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");

    kprintf("[TEST] %s: same VA holds 0x%x in pid %u, 0x%x in pid %u\n\r",
            (va == 0xAAAAAAAAAAAAAAAAULL && vb == 0xBBBBBBBBBBBBBBBBULL)
                ? "PASS" : "FAIL",
            va, (uint64_t)a->pid, vb, (uint64_t)b->pid);

    uint64_t kprobe = vmm_virt_to_phys((uint64_t *)a->cr3, (uint64_t)&saved_cr3);
    kprintf("[TEST] %s: kernel mappings reachable from pid %u\n\r",
            kprobe ? "PASS" : "FAIL", (uint64_t)a->pid);

    process_destroy(a);
    process_destroy(b);
}

/* ── init thread: spawns the init process and reaps it ──────────────────
 * Runs as a kernel thread owned by pid 0, spawning the first real userland
 * ELF process and waiting for it to exit. */
static void init_thread(void *arg) {
    (void)arg;

    kprintf("\n\r[init] spawning ELF init process...\n\r");

    pid_t_v init_pid = process_spawn_elf(elf_init_start,
                                        (uint64_t)(elf_init_end - elf_init_start),
                                        0);
    if (init_pid < 0) {
        kprintf("[init] FAIL: process_spawn_elf returned %d\n\r", (int)init_pid);
        return;
    }

    kprintf("[init] spawned init pid %u\n\r", (uint64_t)init_pid);

    /* Reap init. process_try_reap returns 0 while the child is still
     * running, so yield and retry rather than spinning on the CPU. */
    while (1) {
        int32_t status = 0;
        pid_t_v r = process_try_reap(0, init_pid, &status);
        if (r > 0) {
            kprintf("[init] reaped pid %u, status %u\n\r",
                    (uint64_t)r, (uint64_t)(uint32_t)status);
            break;
        } else if (r < 0) {
            kprintf("[init] reap error: %d\n\r", (int)r);
            break;
        } else {
            sched_yield();
        }
    }

    kprintf("[init] Phase 5B init process exited. Kernel stable.\n\r");
}

/* ════════════════════════════════════════════════════════════════════════
 *  kernel_main — called once from entry.asm, never returns
 * ════════════════════════════════════════════════════════════════════════ */
void NO_RETURN kernel_main(void) {
    serial_init();
    console_init();
    kprintf("\n\r[VoidOS] Bootstrapping kernel...\n\r");

    boot_init();

    gdt_init();
    kprintf("[VoidOS] GDT loaded.\n\r");

    idt_init();
    kprintf("[VoidOS] IDT loaded.\n\r");

    kprintf("[VoidOS] HHDM offset:    0x%016x\n\r", g_boot.hhdm_offset);
    if (g_boot.fb)
        kprintf("[VoidOS] Framebuffer:    %ux%u @ %ubpp\n\r",
                g_boot.fb->width, g_boot.fb->height, (uint64_t)g_boot.fb->bpp);
    kprintf("[VoidOS] Usable memory:  %u KiB\n\r", g_boot.total_usable_memory / 1024);

    pmm_init();
    vmm_init();
    kheap_init();

    idt_register_irq(0, timer_handler);
    lapic_init(100);

    sched_init();
    process_init();
    syscall_init();

    test_address_space_isolation();

    sched_create_kthread(init_thread, NULL);

    sched_start();
    interrupts_enable();
    kprintf("[VoidOS] Kernel ready. Scheduler active.\n\r");

    for (;;)
        __asm__ volatile ("hlt");

    __builtin_unreachable();
}
