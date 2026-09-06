/* VoidOS — syscall ABI
 *
 * ══ Calling convention (x86_64, SYSCALL/SYSRET) ══════════════════════
 *   RAX  syscall number
 *   RDI  arg0     RSI  arg1     RDX  arg2
 *   R10  arg3     R8   arg4     R9   arg5
 *   RAX  return value (negative = -errno, POSIX style)
 *
 * R10 replaces RCX for arg3 because SYSCALL clobbers RCX with the return
 * RIP; R11 is likewise clobbered with RFLAGS.  This matches the Linux
 * x86_64 convention so a future libc port needs no shim.
 *
 * Entry:  SYSCALL   → syscall_entry (IA32_LSTAR)
 * Return: IRETQ     → uniform with the interrupt path, so the scheduler
 *                     may switch threads inside a syscall.
 *
 * Syscall numbers follow Linux x86_64 where an equivalent exists, so
 * userspace built against those numbers keeps working as the surface
 * grows.  This is number compatibility only — not POSIX compliance.
 */
#ifndef VOID_SYSCALL_H
#define VOID_SYSCALL_H 1

#include <void/types.h>
#include <arch/x86_64/idt.h>

/* Forward declarations */
struct process;
typedef struct process process_t;

/* ── syscall numbers (Linux x86_64 compatible) ───────────────────────── */
#define SYS_read                 0
#define SYS_write                1
#define SYS_sched_yield          24
#define SYS_getpid               39
#define SYS_exit                 60
#define SYS_wait4                61
#define SYS_ipc_endpoint_create  62
#define SYS_ipc_send             63
#define SYS_ipc_recv             64
#define SYS_ipc_close            65

/* ── errno values (negated on return, POSIX names) ───────────────────── */
#define VE_PERM     1    /* EPERM  */
#define VE_NOENT    2    /* ENOENT */
#define VE_BADF     9    /* EBADF  */
#define VE_CHILD   10    /* ECHILD */
#define VE_NOMEM   12    /* ENOMEM */
#define VE_FAULT   14    /* EFAULT */
#define VE_INVAL   22    /* EINVAL */
#define VE_NOSYS   38    /* ENOSYS */

/* Marker placed in isr_frame_t.vector for frames built by syscall_entry,
 * so the scheduler and fault handlers can tell a syscall frame from a
 * real interrupt frame. */
#define SYSCALL_FRAME_VECTOR 0x80

/* ── API ──────────────────────────────────────────────────────────────── */

/* Program IA32_EFER.SCE, STAR, LSTAR and FMASK. Call once per CPU,
 * after gdt_init(). */
void syscall_init(void);

/* Point syscall entry at this thread's kernel stack.  Called by the
 * scheduler on every switch, so a syscall from Ring 3 always lands on
 * the kernel stack belonging to the thread that made it. */
void syscall_set_kernel_stack(uint64_t rsp);

/* C dispatcher, called from syscall_entry. Returns the frame to restore
 * (differs from the input frame when the call caused a context switch). */
isr_frame_t *syscall_dispatch(isr_frame_t *frame);

/* ── User pointer validation and copy (shared by syscalls) ────────── */
bool user_range_ok(struct process *p, uint64_t base, uint64_t len, bool need_write);
bool copy_from_user(struct process *p, void *dst, uint64_t usrc, uint64_t len);
bool copy_to_user(struct process *p, uint64_t udst, const void *src, uint64_t len);

/* ── IPC syscall handlers (Phase 7) ───────────────────────────────── */
int32_t sys_ipc_endpoint_create(void);
int32_t sys_ipc_send(int32_t dest_handle, uint32_t tag,
                     const void *data, uint32_t data_len);
int32_t sys_ipc_recv(int32_t handle, uint32_t *tag_out,
                     void *data_out, uint32_t max_len);
int32_t sys_ipc_close(int32_t handle);

#endif /* VOID_SYSCALL_H */
