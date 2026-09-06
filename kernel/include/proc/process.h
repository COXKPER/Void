/* VoidOS — Process abstraction
 *
 * A process owns an address space (PML4) and one or more threads.
 * Kernel mappings (higher-half, PML4 entries 256–511) are shared into
 * every process page table, so a syscall/interrupt can run without
 * switching CR3.  User mappings live in the lower half and are private.
 *
 * Interfaces are shaped for later POSIX semantics (pid_t, exit status,
 * parent/child, fd table) but no POSIX guarantees are made yet.
 */
#ifndef VOID_PROCESS_H
#define VOID_PROCESS_H 1

#include <void/types.h>
#include <arch/x86_64/idt.h>

#define MAX_PROCESSES   64
#define MAX_FDS         16        /* per-process fd table size (grow later) */

/* POSIX-shaped id type; signed so -1 can mean "no process". */
typedef int32_t pid_t_v;

/* ── process states ──────────────────────────────────────────────────── */
typedef enum {
    PROC_UNUSED = 0,   /* slot free                                     */
    PROC_EMBRYO,       /* being constructed, not yet runnable           */
    PROC_READY,        /* runnable, waiting for CPU                     */
    PROC_RUNNING,      /* currently on a CPU                            */
    PROC_BLOCKED,      /* waiting on something (wait(), IPC, I/O)       */
    PROC_ZOMBIE,       /* exited, status not yet reaped by parent       */
} proc_state_t;

/* ── file descriptor abstraction (backing objects come later) ─────────
 * Kept deliberately thin: a type tag plus an opaque handle.  The VFS
 * and pipe/IPC layers will fill `object` in later phases. */
typedef enum {
    FD_NONE = 0,
    FD_SERIAL,        /* console on COM1 — stdin/stdout/stderr for now  */
} fd_type_t;

typedef struct {
    fd_type_t type;
    void     *object;  /* NULL for FD_SERIAL */
    uint64_t  offset;
} fd_entry_t;

/* ── user address space layout ───────────────────────────────────────── */
#define USER_STACK_TOP   0x0000700000000000ULL   /* grows down          */
#define USER_STACK_SIZE  (16 * 4096ULL)          /* 64 KiB              */
#define USER_CODE_BASE   0x0000000000400000ULL   /* 4 MiB, ELF-friendly */

/* ── process control block ───────────────────────────────────────────── */
typedef struct process {
    pid_t_v       pid;
    pid_t_v       ppid;             /* parent pid, -1 for none          */
    proc_state_t  state;
    uint64_t      cr3;              /* physical address of this PML4     */

    /* exit bookkeeping (POSIX-shaped: raw status, decoded by wait later) */
    int32_t       exit_status;
    bool          exited;

    /* the single thread of this process (one thread per process for now) */
    struct thread *thread;

    /* per-process descriptor table */
    fd_entry_t    fds[MAX_FDS];

    /* user memory bookkeeping: pages we allocated, so exit can free them.
     * ponytail: flat list of (virt,phys) pairs, 64 max; replace with a
     * VMA/region list when mmap and demand paging land. */
    struct { uint64_t virt, phys; } umap[64];
    uint32_t      umap_count;

    /* IPC handle table (Phase 7 merger) */
    struct ipc_handle_table *ipc_handles;
} process_t;

/* ── API ──────────────────────────────────────────────────────────────── */

/* Initialise the process table and adopt the running kernel context
 * as pid 0 (the "kernel process"). */
void process_init(void);

/* Allocate a process with a fresh address space cloning kernel mappings.
 * Returns NULL if the table is full or memory is exhausted. */
process_t *process_alloc(pid_t_v parent);

/* Map one page into a process's user address space (VMM_USER enforced)
 * and record it for teardown. */
void_status_t process_map_user(process_t *p, uint64_t virt, uint64_t phys, uint64_t flags);

/* Allocate a zeroed user page at `virt` (frame from PMM). */
void_status_t process_alloc_user_page(process_t *p, uint64_t virt, uint64_t flags);

/* Free every user page and the page tables of a process. */
void process_destroy(process_t *p);

/* Lookup by pid; NULL if absent. */
process_t *process_get(pid_t_v pid);

/* The process owning the currently running thread. */
process_t *process_current(void);

/* Kernel process (pid 0). */
process_t *process_kernel(void);

/* ── lifecycle ────────────────────────────────────────────────────────
 * These take and return an isr_frame_t because exit and a blocking wait
 * both end in a context switch: the frame handed back is the next
 * thread's, which the syscall/interrupt trailer then restores. */

/* Build a Ring-3 process from a blob of position-dependent machine code.
 * `code` is copied into fresh user pages at USER_CODE_BASE and a user
 * stack is mapped below USER_STACK_TOP.  Returns the pid, or -1.
 * ponytail: raw blob loader; kept for the Phase 4 tests, which exercise
 * paths (deliberate faults) that a well-formed ELF cannot express. */
pid_t_v process_spawn_user(const void *code, uint64_t code_len, pid_t_v parent);

/* Build a Ring-3 process from an ELF64 image held in kernel memory.
 * Validates the image, populates a fresh address space from its PT_LOAD
 * segments, and starts a thread at the ELF entry point.  Returns the pid,
 * or a negative elf_status_t on failure — the caller can distinguish
 * "malformed binary" from "out of memory".  Nothing is left running on
 * failure; the partially built process is destroyed.
 *
 * The future execve() path differs only in reusing the *calling* process
 * rather than allocating a new one. */
pid_t_v process_spawn_elf(const void *image, uint64_t size, pid_t_v parent);

/* Terminate the calling process with `status`; never returns to it. */
isr_frame_t *process_exit_current(isr_frame_t *frame, int32_t status);

/* Reap a child.  `pid` of -1 means "any child".  `ustatus` is a user
 * pointer that receives the raw status, or 0 to discard it.  Blocks the
 * caller when a child exists but none has exited yet. */
isr_frame_t *process_wait_current(isr_frame_t *frame, pid_t_v pid, uint64_t ustatus);

/* Non-blocking reap, callable from kernel context.  `pid` of -1 means any
 * child of `parent`.  On success writes the raw status through `status`
 * (when non-NULL), destroys the zombie and returns its pid.
 * Returns 0 if a matching child exists but has not exited, or
 * -VE_CHILD if there is no matching child at all. */
pid_t_v process_try_reap(pid_t_v parent, pid_t_v pid, int32_t *status);

/* Called by the scheduler when a thread is about to run, so faults and
 * syscalls from Ring 3 land on that thread's own kernel stack. */
struct thread;
void process_on_switch(struct thread *t);

#endif /* VOID_PROCESS_H */
