/* VoidOS — syscall MSR setup, user-pointer validation, dispatcher
 *
 * Every pointer arriving from Ring 3 is untrusted.  copy_from_user()
 * walks the caller's own page tables to confirm each page is present and
 * USER-accessible before the kernel dereferences anything, so a bad
 * pointer becomes -EFAULT instead of a kernel page fault or a read of
 * kernel memory through a user-supplied address.
 */
#include <syscall/syscall.h>
#include <arch/x86_64/gdt.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <dev/serial.h>
#include <void/boot.h>
#include <ipc/ipc.h>

/* ── MSRs ────────────────────────────────────────────────────────────── */
#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

#define EFER_SCE    (1ULL << 0)     /* System Call Extensions enable */

extern void syscall_entry(void);
extern uint64_t syscall_kernel_rsp;   /* in syscall_entry.asm */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* ── syscall_init ────────────────────────────────────────────────────── */
void syscall_init(void) {
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_SCE);

    /* STAR[47:32] = kernel CS for SYSCALL (SS = that + 8).
     * STAR[63:48] = base for SYSRET: CS = base + 16, SS = base + 8.
     * With base 0x13: SYSRET CS = 0x23 (UCODE|3), SS = 0x1B (UDATA|3). */
    wrmsr(IA32_STAR, ((uint64_t)0x13 << 48) | ((uint64_t)GDT_SEL_KCODE << 32));

    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

    /* Clear IF on entry so a syscall cannot be interrupted before it has
     * finished switching to the kernel stack.  Also clear TF and DF. */
    wrmsr(IA32_FMASK, 0x700);

    kprintf("[SYSCALL] SYSCALL/SYSRET armed, entry @ %p\n\r", (void *)syscall_entry);
}

void syscall_set_kernel_stack(uint64_t rsp) {
    syscall_kernel_rsp = rsp;
}

/* ── user pointer validation ─────────────────────────────────────────────
 * A user buffer is acceptable only if every byte lies in the lower half
 * AND every page backing it is present with the USER bit set in the
 * caller's address space.  Checking USER (not just present) is what stops
 * a process from naming a kernel page it happens to know about. */
#define USER_LIMIT 0x0000800000000000ULL

static bool user_range_ok(process_t *p, uint64_t base, uint64_t len, bool need_write) {
    if (!p || len == 0) return false;
    if (base >= USER_LIMIT) return false;
    if (len > USER_LIMIT) return false;
    if (base + len < base) return false;          /* wrap */
    if (base + len > USER_LIMIT) return false;

    uint64_t first = base & ~(PAGE_SIZE - 1);
    uint64_t last  = (base + len - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t v = first; v <= last; v += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte((uint64_t *)p->cr3, v);
        if (!(pte & VMM_PRESENT)) return false;
        if (!(pte & VMM_USER))    return false;
        if (need_write && !(pte & VMM_WRITE)) return false;
    }
    return true;
}

/* Copy from user space into a kernel buffer through the HHDM, one page at
 * a time.  Reading through the HHDM rather than the user virtual address
 * means the copy cannot be redirected by a concurrent remap. */
static bool copy_from_user(process_t *p, void *dst, uint64_t usrc, uint64_t len) {
    if (!user_range_ok(p, usrc, len, false)) return false;

    uint8_t *out = (uint8_t *)dst;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va     = usrc + done;
        uint64_t page   = va & ~(PAGE_SIZE - 1);
        uint64_t offset = va - page;
        uint64_t chunk  = PAGE_SIZE - offset;
        if (chunk > len - done) chunk = len - done;

        uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, va);
        if (!phys) return false;

        const uint8_t *src = (const uint8_t *)(phys + g_boot.hhdm_offset);
        for (uint64_t i = 0; i < chunk; i++) out[done + i] = src[i];
        done += chunk;
    }
    return true;
}

/* ── copy_to_user ────────────────────────────────────────────────────────
 * Write to user space via HHDM, one page at a time. Validates every page
 * in the range before writing. */
static bool copy_to_user(process_t *p, uint64_t udst, const void *src, uint64_t len) {
    if (!user_range_ok(p, udst, len, true)) return false;

    const uint8_t *in = (const uint8_t *)src;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va     = udst + done;
        uint64_t page   = va & ~(PAGE_SIZE - 1);
        uint64_t offset = va - page;
        uint64_t chunk  = PAGE_SIZE - offset;
        if (chunk > len - done) chunk = len - done;

        uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, va);
        if (!phys) return false;

        uint8_t *dst = (uint8_t *)(phys + g_boot.hhdm_offset);
        for (uint64_t i = 0; i < chunk; i++) dst[i] = in[done + i];
        done += chunk;
    }
    return true;
}

/* ── sys_read ────────────────────────────────────────────────────────────
 * Currently only FD_SERIAL (console) is supported. Reads from serial input
 * (RBR) on port COM1. Non-blocking: returns available chars or 0 if none ready.
 * Returns byte count, 0 (no data ready), or -error. */
static int64_t sys_read(process_t *p, int32_t fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS)        return -VE_BADF;
    if (p->fds[fd].type != FD_SERIAL)   return -VE_BADF;
    if (count == 0)                     return 0;

    /* Bounded staging buffer for reads */
    char buf[256];
    uint64_t nread = 0;
    uint64_t to_read = count > sizeof(buf) ? sizeof(buf) : count;

    /* Non-blocking read: gather available chars up to limit or newline */
    while (nread < to_read) {
        int ch = serial_getchar();
        if (ch == -1) {
            /* No more data available. Return what we have. */
            break;
        }

        buf[nread++] = (char)ch;

        /* Stop on newline (shell convention: read until Enter) */
        if (ch == '\n') {
            break;
        }
    }

    /* Copy staged buffer to user space */
    if (nread > 0) {
        if (!copy_to_user(p, ubuf, buf, nread))
            return nread ? (int64_t)nread : -VE_FAULT;
    }

    return (int64_t)nread;
}

/* ── sys_write ───────────────────────────────────────────────────────────
 * Only console-backed descriptors exist right now; a real VFS replaces the
 * FD_SERIAL branch later without changing the ABI. */
static int64_t sys_write(process_t *p, int32_t fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS)        return -VE_BADF;
    if (p->fds[fd].type != FD_SERIAL)   return -VE_BADF;
    if (count == 0)                     return 0;

    /* Bounded staging buffer: a huge count becomes several iterations
     * rather than a huge kernel stack frame. */
    char buf[256];
    uint64_t written = 0;
    while (written < count) {
        uint64_t chunk = count - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        if (!copy_from_user(p, buf, ubuf + written, chunk))
            return written ? (int64_t)written : -VE_FAULT;

        for (uint64_t i = 0; i < chunk; i++) serial_putchar(buf[i]);
        written += chunk;
    }
    return (int64_t)written;
}

/* ── dispatcher ──────────────────────────────────────────────────────── */
isr_frame_t *syscall_dispatch(isr_frame_t *frame) {
    process_t *p = process_current();
    uint64_t nr  = frame->rax;

    switch (nr) {
    case SYS_read:
        frame->rax = (uint64_t)sys_read(p, (int32_t)frame->rdi,
                                       frame->rsi, frame->rdx);
        return frame;

    case SYS_write:
        frame->rax = (uint64_t)sys_write(p, (int32_t)frame->rdi,
                                        frame->rsi, frame->rdx);
        return frame;

    case SYS_getpid:
        frame->rax = p ? (uint64_t)(int64_t)p->pid : (uint64_t)(int64_t)-VE_PERM;
        return frame;

    case SYS_sched_yield:
        frame->rax = 0;
        /* Reuse the scheduler's switch path: it saves this frame and hands
         * back the next thread's, which syscall_entry then restores. */
        return sched_tick(frame);

    case SYS_exit:
        return process_exit_current(frame, (int32_t)frame->rdi);

    case SYS_wait4:
        return process_wait_current(frame, (pid_t_v)(int32_t)frame->rdi, frame->rsi);

    case SYS_ipc_endpoint_create:
        frame->rax = (uint64_t)(int64_t)sys_ipc_endpoint_create();
        return frame;

    case SYS_ipc_send:
        frame->rax = (uint64_t)(int64_t)sys_ipc_send(
            (int32_t)frame->rdi, (uint32_t)frame->rsi,
            (const void *)frame->rdx, (uint32_t)frame->r10
        );
        return frame;

    case SYS_ipc_recv:
        frame->rax = (uint64_t)(int64_t)sys_ipc_recv(
            (int32_t)frame->rdi, (uint32_t *)frame->rsi,
            (void *)frame->rdx, (uint32_t)frame->r10
        );
        return frame;

    case SYS_ipc_close:
        frame->rax = (uint64_t)(int64_t)sys_ipc_close((int32_t)frame->rdi);
        return frame;

    default:
        kprintf("[SYSCALL] pid %u: unknown syscall %u\n\r",
                p ? (uint64_t)p->pid : (uint64_t)-1, nr);
        frame->rax = (uint64_t)(int64_t)(-VE_NOSYS);
        return frame;
    }
}
