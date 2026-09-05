/* VoidOS — Kernel thread scheduler
 * Preemptive round-robin scheduler driven by LAPIC timer.
 * Kernel threads only (Ring 0). Process/userspace comes later.
 */
#ifndef VOID_SCHED_H
#define VOID_SCHED_H 1

#include <void/types.h>
#include <arch/x86_64/idt.h>

/* Kernel stack size per thread (also the size process_spawn_user records
 * in the TCB, so it lives here rather than inside sched.c). */
#define KTHREAD_STACK_SIZE  (16 * 4096ULL)   /* 64 KiB */

/* ── thread states ───────────────────────────────────────────────────── */
typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_EXITED,
} thread_state_t;

/* ── Thread Control Block ────────────────────────────────────────────── */
struct process;   /* forward: proc/process.h */

typedef struct thread {
    uint64_t        rsp;            /* saved kernel RSP (top of isr_frame_t) */
    uint64_t        cr3;            /* page table (kernel CR3 for kthreads) */
    uint64_t       *stack_base;     /* base of allocated kernel stack */
    uint64_t        stack_size;     /* size in bytes */
    uint32_t        tid;            /* thread ID */
    thread_state_t  state;
    struct process *proc;           /* owning process (NULL = raw kthread) */
    struct thread  *next;           /* circular ready queue link */
} thread_t;

typedef void (*thread_func_t)(void *arg);

/* ── public API ───────────────────────────────────────────────────────── */

/* Initialise scheduler. Converts current execution context into thread 0. */
void sched_init(void);

/* Create a new kernel thread. Returns thread ID, or 0 on failure. */
uint32_t sched_create_kthread(thread_func_t func, void *arg);

/* Register an already-built thread frame as a runnable thread in address
 * space `cr3`, owned by `proc`.  Used by process_spawn_user() to enqueue a
 * Ring-3 thread whose isr_frame_t was constructed by the caller.
 * `rsp` must point at that frame.  Returns the thread, or NULL if full. */
thread_t *sched_adopt_thread(uint64_t rsp, uint64_t cr3, void *proc,
                             uint64_t *kstack_base, uint64_t kstack_size);

/* Allocate a kernel stack for a thread; returns stack top, 0 on failure.
 * Writes the base address to *out_base. */
uint64_t sched_alloc_kstack(uint64_t *out_base);

/* Voluntarily yield the CPU to the next ready thread. */
void sched_yield(void);

/* Exit the current thread. Does not return. */
NO_RETURN void sched_exit(void);

/* Called from timer IRQ handler to potentially switch threads.
 * Returns frame pointer to restore (may differ from input on context switch). */
isr_frame_t *sched_tick(isr_frame_t *frame);

/* Get current thread. */
thread_t *sched_current(void);

/* Activate preemptive scheduling (call after creating initial threads). */
void sched_start(void);

#endif /* VOID_SCHED_H */
