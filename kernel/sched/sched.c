/* VoidOS — Preemptive round-robin kernel thread scheduler
 *
 * Design:
 *   - Each kernel thread has a TCB with a saved RSP pointing to an isr_frame_t
 *   - Timer IRQ calls sched_tick() which saves current RSP, picks next thread,
 *     returns next thread's RSP — isr_common restores from the new stack
 *   - New threads get a synthetic isr_frame_t on their stack so they start
 *     via iretq to their entry function
 *   - Thread 0 = the boot thread (converted from current context)
 *   - An idle thread runs HLT in a loop when nothing else is ready
 */
#include <sched/sched.h>
#include <proc/process.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <arch/x86_64/gdt.h>
#include <dev/serial.h>
#include <void/boot.h>

#define MAX_THREADS         64

/* ── thread table ────────────────────────────────────────────────────── */
static thread_t  threads[MAX_THREADS];
static uint32_t  next_tid;
static thread_t *current;
static thread_t *idle_thread;
static bool      scheduler_active;

/* ── thread entry wrapper ────────────────────────────────────────────── */
/* New threads start here. Calls the real function, then exits. */
static void thread_entry_trampoline(void) {
    /* The function pointer and arg are stashed in r12/r13 by sched_create_kthread */
    thread_func_t func;
    void *arg;
    __asm__ volatile ("mov %%r12, %0" : "=r"(func));
    __asm__ volatile ("mov %%r13, %0" : "=r"(arg));

    interrupts_enable();
    func(arg);
    sched_exit();
}

/* ── allocate a kernel stack (mapped pages) ──────────────────────────── */
static uint64_t *alloc_kstack(uint64_t *out_base) {
    /* Allocate virtual range + physical frames */
    uint64_t cr3 = vmm_kernel_pml4();
    /* ponytail: static vaddr bump allocator; upgrade to proper VA allocator */
    static uint64_t next_stack_vaddr = 0xFFFFFFFF91000000ULL;

    uint64_t base = next_stack_vaddr;
    next_stack_vaddr += KTHREAD_STACK_SIZE + PAGE_SIZE; /* +guard page gap */

    for (uint64_t off = 0; off < KTHREAD_STACK_SIZE; off += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return NULL;
        vmm_map_page((uint64_t *)cr3, base + off, frame, VMM_KERN_RW);
    }

    *out_base = base;
    return (uint64_t *)(base + KTHREAD_STACK_SIZE); /* stack top */
}

/* ── idle thread function ────────────────────────────────────────────── */
static void idle_func(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* ── sched_init ──────────────────────────────────────────────────────── */
void sched_init(void) {
    /* Zero all TCBs */
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = THREAD_EXITED;
        threads[i].tid   = 0;
        threads[i].proc  = NULL;
        threads[i].next  = NULL;
    }

    /* Thread 0: the current boot execution context.
     * Its RSP will be saved on the first timer interrupt. */
    thread_t *t0 = &threads[0];
    t0->tid        = 0;
    t0->state      = THREAD_RUNNING;
    t0->cr3        = vmm_kernel_pml4();
    t0->stack_base = NULL;  /* boot stack, not dynamically allocated */
    t0->stack_size = 0;
    t0->rsp        = 0;     /* will be filled on first context switch */
    t0->next       = t0;    /* circular: points to self initially */

    current   = t0;
    next_tid  = 1;
    scheduler_active = false;

    /* Create idle thread */
    uint32_t idle_tid = sched_create_kthread(idle_func, NULL);
    idle_thread = &threads[idle_tid];

    kprintf("[SCHED] Initialised. Boot thread=0, idle thread=%u\n\r",
            (uint64_t)idle_tid);
}

/* ── sched_create_kthread ────────────────────────────────────────────── */
uint32_t sched_create_kthread(thread_func_t func, void *arg) {
    if (next_tid >= MAX_THREADS) return 0;

    thread_t *t = &threads[next_tid];
    t->tid   = next_tid++;
    t->cr3   = vmm_kernel_pml4();
    t->state = THREAD_READY;

    /* Allocate kernel stack */
    uint64_t base;
    uint64_t *stack_top = alloc_kstack(&base);
    if (!stack_top) return 0;
    t->stack_base = (uint64_t *)base;
    t->stack_size = KTHREAD_STACK_SIZE;

    /* Build a synthetic isr_frame_t on the new stack so iretq starts the thread.
     * isr_common will pop: r15..rax, vector, error_code, then iretq pops rip,cs,rflags,rsp,ss */
    isr_frame_t *frame = (isr_frame_t *)((uint8_t *)stack_top - sizeof(isr_frame_t));

    frame->ss     = GDT_SEL_KDATA;
    frame->rsp    = (uint64_t)stack_top;  /* stack pointer after iretq */
    frame->rflags = 0x202;                /* IF=1 (interrupts enabled) */
    frame->cs     = GDT_SEL_KCODE;
    frame->rip    = (uint64_t)thread_entry_trampoline;
    frame->error_code = 0;
    frame->vector     = 0;

    /* Pass func/arg via callee-saved registers r12/r13 */
    frame->r12 = (uint64_t)func;
    frame->r13 = (uint64_t)arg;
    frame->rax = frame->rbx = frame->rcx = frame->rdx = 0;
    frame->rsi = frame->rdi = frame->rbp = 0;
    frame->r8 = frame->r9 = frame->r10 = frame->r11 = 0;
    frame->r14 = frame->r15 = 0;

    t->rsp = (uint64_t)frame;

    /* Insert into circular ready queue after current */
    t->next = current->next;
    current->next = t;

    return t->tid;
}

/* ── pick next runnable thread ───────────────────────────────────────── */
static thread_t *pick_next(void) {
    thread_t *t = current->next;
    thread_t *start = t;
    do {
        if (t->state == THREAD_READY)
            return t;
        t = t->next;
    } while (t != start);

    /* Nothing ready — run idle */
    return idle_thread;
}

/* ── sched_tick — called from timer IRQ, returns new frame ───────────── */
isr_frame_t *sched_tick(isr_frame_t *frame) {
    if (!scheduler_active) return frame;

    /* Save current thread's RSP (points to its isr_frame_t on its stack) */
    current->rsp = (uint64_t)frame;
    if (current->state == THREAD_RUNNING)
        current->state = THREAD_READY;

    /* Pick next */
    thread_t *next = pick_next();
    next->state = THREAD_RUNNING;

    /* Switch address space if the next thread lives in another one.
     * Safe to do here because every process PML4 shares the kernel's
     * upper half, so this code and this stack stay mapped across the load. */
    if (next->cr3 != current->cr3)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");

    /* Retarget TSS.RSP0 and the SYSCALL stack at the incoming thread
     * before it runs, so a Ring 3 fault or syscall lands on its own stack. */
    process_on_switch(next);

    current = next;

    /* Return the new thread's saved frame — isr_common will restore from it */
    return (isr_frame_t *)next->rsp;
}

/* ── sched_yield ─────────────────────────────────────────────────────────
 * Kernel-side yield: enter the same vector the timer uses so the switch
 * goes through one code path.  isr_dispatch will send a LAPIC EOI with
 * nothing in service, which the LAPIC ignores — safe here because callers
 * always have IF=1 and are therefore not inside a handler.  Ring 3 yields
 * arrive via SYS_sched_yield instead and never touch the EOI path. */
void sched_yield(void) {
    __asm__ volatile ("int $32");
}

/* ── sched_exit ──────────────────────────────────────────────────────── */
NO_RETURN void sched_exit(void) {
    interrupts_disable();
    current->state = THREAD_EXITED;
    interrupts_enable();
    sched_yield();
    /* Should never reach here */
    cpu_halt();
    __builtin_unreachable();
}

/* ── accessors ───────────────────────────────────────────────────────── */
thread_t *sched_current(void) { return current; }

/* ── sched_alloc_kstack — expose stack allocation to process layer ───── */
uint64_t sched_alloc_kstack(uint64_t *out_base) {
    uint64_t base;
    uint64_t *top = alloc_kstack(&base);
    if (!top) return 0;
    *out_base = base;
    return (uint64_t)top;
}

/* ── sched_adopt_thread — enqueue a caller-built thread ──────────────── */
thread_t *sched_adopt_thread(uint64_t rsp, uint64_t cr3, void *proc,
                             uint64_t *kstack_base, uint64_t kstack_size) {
    if (next_tid >= MAX_THREADS) return NULL;

    thread_t *t = &threads[next_tid];
    t->tid        = next_tid++;
    t->rsp        = rsp;
    t->cr3        = cr3;
    t->proc       = proc;
    t->stack_base = kstack_base;
    t->stack_size = kstack_size;
    t->state      = THREAD_READY;

    t->next = current->next;
    current->next = t;
    return t;
}

void sched_start(void) {
    scheduler_active = true;
    kprintf("[SCHED] Scheduler active.\n\r");
}
