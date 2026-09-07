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
#include <dev/serial_drv.h>
#include <dev/tty.h>
#include <void/boot.h>
#include <void/voidfs.h>
#include <ipc/ipc.h>
#include <svc/svc.h>
#include <mm/user_mem.h>
#include <mm/pmm.h>          /* PAGE_SIZE */
#include <elf/elf.h>         /* elf_nx_enabled() */

#define PAGE_MASK  (~(PAGE_SIZE - 1))

/* ── sys_brk ─────────────────────────────────────────────────────────────
 * brk(2)/sbrk(2) merged into one syscall, Linux brk semantics: the kernel
 * tracks brk_current and moves it on demand; sbrk(n) is a userspace wrapper
 * that queries then moves.
 *
 *   addr == 0                     → return brk_current (sbrk(0))
 *   addr < heap_start             → return brk_current unchanged
 *   addr in [brk_current, heap]   → grow/shrink, return the NEW break
 *   grow runs out of frames       → return -ENOMEM, break unchanged
 *
 * Shrink is best-effort (release pages where safe); Linux makes brk shrink
 * infallible too (ENOMEM is only possible on grow).  Old break is never
 * returned by the kernel — the sbrk(±n) arithmetic lives in libvoid where
 * the userspace break cache (a process global) can hold it.
 */
int64_t sys_brk(process_t *p, uint64_t addr) {
    if (!p || !p->heap_start) return -VE_INVAL;
    if (addr == 0) return (int64_t)p->brk_current;        /* sbrk(0) */
    if (addr < p->heap_start)  return (int64_t)p->brk_current; /* no-op */

    int r = um_brk_set(p, addr);
    if (r) return (int64_t)r;
    return (int64_t)p->brk_current;
}

/* Flag / prot constants (Linux x86_64 values, mirrored in userland/void.h).
 * The kernel accepts only the anonymous/private subset and rejects anything
 * else loudly rather than silently mis-map. */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* ── mmap prot → VMM flags ──────────────────────────────────────────────
 * PROT_READ alone maps a readable (no-write) page.  x86 paging cannot
 * express read-disabled pages, so PROT_NONE still maps the page readable;
 * mmap refuses PROT_NONE rather than pretend.  PROT_EXEC adds nothing to
 * the PTE (the loader only ever clears NX; x86 has no exec-disable missing
 * here), and when the CPU has no NX, PROT_NONE<->EXEC distinctions collapse.
 */
static uint64_t mmap_prot_flags(int prot) {
    uint64_t f = 0;
    if (prot & PROT_WRITE)  f |= VMM_WRITE;
    if (!(prot & PROT_EXEC) && elf_nx_enabled()) f |= VMM_NX;
    return f;
}

/* ── sys_mmap ───────────────────────────────────────────────────────────
 * Anonymous MAP_PRIVATE mapping of `length` bytes.
 *
 *   addr == 0    → reserve top-down below the stack (um_mmap_reserve)
 *   addr != 0    → fixed map; must be page-aligned and land on free pages
 *
 * Pages are allocated eagerly: Void has no on-demand user page-fault
 * handler (a user #PF kills the process), so every mapped page must be
 * present from the start.  PROT_NONE is rejected (x86 can't express it),
 * as are file/offset forms and any flag outside MAP_PRIVATE|ANONYMOUS.
 */
long sys_mmap(process_t *p, uint64_t addr, uint64_t length, int prot,
              int flags, int fd, uint64_t offset) {
    if (!p || !p->cr3) return -VE_PERM;

    /* ── argument checks ───────────────────────────────────────────── */
    if (length == 0) return -VE_INVAL;
    if (length >= USER_MMAP_TOP) return -VE_NOMEM;     /* absurd size   */
    if (fd != -1 || offset != 0) return -VE_INVAL;     /* anon only      */
    if (flags != (MAP_PRIVATE | MAP_ANONYMOUS))
        return -VE_INVAL;                              /* exactly anon+private */
    if (prot == PROT_NONE) return -VE_INVAL;           /* no EXEC nuance */

    uint64_t n = (length + PAGE_SIZE - 1) & PAGE_MASK; /* page align    */

    /* ── choose base ──────────────────────────────────────────────── */
    uint64_t base;
    if (addr == 0) {
        int r = um_mmap_reserve(p, n, &base);
        if (r) return r;
    } else {
        if ((addr & (PAGE_SIZE - 1)) != 0) return -VE_INVAL;  /* aligned */
        if (addr >= USER_MMAP_TOP)         return -VE_INVAL;  /* stack   */
        if (addr + n < addr)               return -VE_NOMEM;  /* wrap    */
        if (!um_range_clear(p, addr, addr + n)) return -VE_INVAL; /* overlap */
        base = addr;
    }

    /* ── eager page allocation ─────────────────────────────────────── */
    uint64_t f = mmap_prot_flags(prot);
    for (uint64_t va = base; va < base + n; va += PAGE_SIZE)
        if (process_alloc_user_page(p, va, f) != VOID_OK) {
            /* Partial mapping: release what we mapped and fail. */
            for (uint64_t v = base; v < va; v += PAGE_SIZE)
                vmm_unmap_page((uint64_t *)p->cr3, v);
            return -VE_NOMEM;
        }
    return (long)base;
}

/* ── sys_munmap ─────────────────────────────────────────────────────────
 * Release a page-aligned [addr, addr+length) range.  Every page inside the
 * caller-owned user half at/above US_CODE_BASE is unmapped and its frame
 * freed; the range may span partly-mapped spans (page walk finds them).  A
 * range is refused only when it would touch the user stack — which no
 * process may unmap.  Linux keeps the code/data image mapped; an exec'd
 * new image re-maps it rather than munmap'ing.
 */
long sys_munmap(process_t *p, uint64_t addr, uint64_t length) {
    if (!p || !p->cr3) return -VE_PERM;
    if (length == 0) return -VE_INVAL;
    if ((addr & (PAGE_SIZE - 1)) != 0) return -VE_INVAL;

    uint64_t n = (length + PAGE_SIZE - 1) & PAGE_MASK;
    if (addr + n < addr || addr + n > USER_MMAP_TOP) return -VE_INVAL; /* stack */

    um_release_from(p, addr);
    return 0;
}

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

bool user_range_ok(process_t *p, uint64_t base, uint64_t len, bool need_write) {
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
bool copy_from_user(process_t *p, void *dst, uint64_t usrc, uint64_t len) {
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
bool copy_to_user(process_t *p, uint64_t udst, const void *src, uint64_t len) {
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
 * Dispatch on the fd type. FD_TTY (the console) reads staged input from the
 * line discipline — echo/canonical/backspace/EOF handled in tty.c — which
 * splices on top of the serial device. Non-blocking: 0 means nothing ready,
 * exactly as the old FD_SERIAL read behaved. FD_VFS serves from the embedded
 * tree via the VFS layer. Returns byte count, 0 (no data / EOF), or -error. */
static int64_t sys_read(process_t *p, int32_t fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS)        return -VE_BADF;
    if (count == 0)                     return 0;

    if (p->fds[fd].type == FD_VFS)
        return sys_vfs_read(p, fd, ubuf, count);

    if (p->fds[fd].type != FD_TTY && p->fds[fd].type != FD_SERIAL)
        return -VE_BADF;

    char buf[256];
    uint64_t to_read = count > sizeof(buf) ? sizeof(buf) : count;
    int nread;
    if (p->fds[fd].type == FD_TTY)
        nread = tty_read((uint64_t)(uintptr_t)buf, to_read);
    else
        nread = dev_read(serial_drv_instance(), (uint64_t)(uintptr_t)buf, to_read);
    if (nread < 0) return (int64_t)nread;

    if (nread > 0) {
        if (!copy_to_user(p, ubuf, buf, (uint64_t)nread))
            return -VE_FAULT;
    }
    return (int64_t)nread;
}

/* ── sys_write ───────────────────────────────────────────────────────────
 * Dispatch on the fd type. FD_TTY/FD_SERIAL write to the console; FD_VFS
 * goes to the VFS layer (which currently rejects writes — the tree is
 * readonly). Returns byte count, or -error. */
static int64_t sys_write(process_t *p, int32_t fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS)        return -VE_BADF;
    if (count == 0)                     return 0;

    if (p->fds[fd].type == FD_VFS)
        return sys_vfs_write(p, fd, ubuf, count);

    if (p->fds[fd].type != FD_TTY && p->fds[fd].type != FD_SERIAL)
        return -VE_BADF;

    void *inst = serial_drv_instance();
    if (!inst) return -VE_IO;            /* console not up (boot-order bug) */

    /* Bounded staging buffer: a huge count becomes several iterations
     * rather than a huge kernel stack frame. */
    char buf[256];
    uint64_t written = 0;
    while (written < count) {
        uint64_t chunk = count - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        if (!copy_from_user(p, buf, ubuf + written, chunk))
            return written ? (int64_t)written : -VE_FAULT;

        int w;
        if (p->fds[fd].type == FD_TTY)
            w = tty_write(buf, chunk);
        else
            w = dev_write(inst, buf, chunk);
        if (w < 0) return written ? (int64_t)written : (int64_t)w;
        written += (uint64_t)w;
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

    case SYS_brk:
        frame->rax = (uint64_t)sys_brk(p, frame->rdi);
        return frame;

    case SYS_mmap:
        /* args: addr(RDI) length(RSI) prot(RDX) flags(R10) fd(R8) offset(R9).
         * SYSCALL clobbers RCX, so arg3 rides in R10 exactly like IPC. */
        frame->rax = (uint64_t)sys_mmap(p, frame->rdi, frame->rsi,
                                        (int)frame->rdx, (int)frame->r10,
                                        (int)frame->r8, frame->r9);
        return frame;

    case SYS_munmap:
        frame->rax = (uint64_t)sys_munmap(p, frame->rdi, frame->rsi);
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

    case SYS_fork:
        return process_fork_current(frame);

    case SYS_execve:
        return process_execve_current(frame, frame->rdi);

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

    case SYS_ipc_connect:
        frame->rax = (uint64_t)(int64_t)sys_ipc_connect(
            (pid_t_v)(int32_t)frame->rdi, (uint32_t)frame->rsi);
        return frame;

    case SYS_svc_name_register:
        frame->rax = (uint64_t)(int64_t)sys_sr_register(
            (const char *)frame->rdi, (int32_t)frame->rsi);
        return frame;

    case SYS_svc_name_unregister:
        frame->rax = (uint64_t)(int64_t)sys_sr_unregister(
            (const char *)frame->rdi, (int32_t)frame->rsi);
        return frame;

    case SYS_svc_lookup:
        frame->rax = (uint64_t)(int64_t)sys_sr_lookup((const char *)frame->rdi);
        return frame;

    case SYS_ioctl: {
        int32_t f = (int32_t)frame->rdi;
        /* Only the console fds route to tty_ioctl; a non-TTY fd is -VE_BADF. */
        if (f < 0 || f >= MAX_FDS) { frame->rax = (uint64_t)(-VE_BADF); }
        else if (p && p->fds[f].type == FD_TTY)
            frame->rax = (uint64_t)(int64_t)tty_ioctl(frame->rsi, (void *)frame->rdx);
        else
            frame->rax = (uint64_t)(int64_t)-VE_BADF;
        return frame;
    }

    case SYS_open:
        frame->rax = (uint64_t)sys_vfs_open(p, frame->rdi, (int)frame->rsi);
        return frame;

    case SYS_close:
        frame->rax = (uint64_t)sys_vfs_close(p, (int32_t)frame->rdi);
        return frame;

    case SYS_lseek:
        frame->rax = (uint64_t)sys_vfs_lseek(p, (int32_t)frame->rdi,
                                             (int64_t)frame->rsi,
                                             (int)frame->rdx);
        return frame;

    case SYS_getcwd:
        frame->rax = (uint64_t)sys_vfs_getcwd(p, frame->rdi, frame->rsi);
        return frame;

    case SYS_chdir:
        frame->rax = (uint64_t)sys_vfs_chdir(p, frame->rdi);
        return frame;

    case SYS_readdir:
        frame->rax = (uint64_t)sys_vfs_readdir(p, (int32_t)frame->rdi,
                                               frame->rsi);
        return frame;

    default:
        kprintf("[SYSCALL] pid %u: unknown syscall %u\n\r",
                p ? (uint64_t)p->pid : (uint64_t)-1, nr);
        frame->rax = (uint64_t)(int64_t)(-VE_NOSYS);
        return frame;
    }
}
