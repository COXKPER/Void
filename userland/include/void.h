/* libvoid — minimal userland ABI for Ring 3
 *
 * Syscall wrappers matching Linux x86_64 ABI:
 * - Args: RDI, RSI, RDX, RCX, R8, R9
 * - Syscall number in RAX
 * - Return: RAX (or RAX:RDX for 128-bit)
 * - SYSCALL clobbers RCX, R11; caller saves all other regs
 */
#ifndef LIBVOID_VOID_H
#define LIBVOID_VOID_H 1

#include <stdint.h>
#include <stddef.h>

/* ── syscall numbers (Linux x86_64 compatible) ─────────────────────── */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_lseek       8
#define SYS_mmap        9
#define SYS_munmap      11
#define SYS_brk         12
#define SYS_sched_yield 24
#define SYS_getpid      39
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_fork        67      /* Void slot (no Linux index)          */
#define SYS_getcwd      79
#define SYS_chdir       80
#define SYS_readdir     89
#define SYS_ioctl       16      /* Linux x86_64 ioctl(2) number          */

/* ── open(2) flags ──────────────────────────────────────────────────── */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

/* ── mmap(2) prot/flags (Linux x86_64 values) ───────────────────────── */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20    /* == MAP_ANON */
#define MAP_FAILED    ((void *)-1)

/* ── syscall wrappers ───────────────────────────────────────────────── */

/* long syscall0(long num) */
static inline long syscall0(long num) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(num)
                     : "rcx", "r11", "memory");
    return ret;
}

/* long syscall1(long num, long arg0) */
static inline long syscall1(long num, long arg0) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(arg0)
                     : "rcx", "r11", "memory");
    return ret;
}

/* long syscall2(long num, long arg0, long arg1) */
static inline long syscall2(long num, long arg0, long arg1) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(arg0), "S"(arg1)
                     : "rcx", "r11", "memory");
    return ret;
}

/* long syscall3(long num, long arg0, long arg1, long arg2) */
static inline long syscall3(long num, long arg0, long arg1, long arg2) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2)
                     : "rcx", "r11", "memory");
    return ret;
}

/* long syscall6(long num, long a0..a5) — R10 carries arg3 (SYSCALL
 * clobbers RCX, exactly as with the IPC wrappers), R8/R9 args 4/5. */
static inline long syscall6(long num, long a0, long a1, long a2,
                            long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ── POSIX-like wrappers ────────────────────────────────────────────── */

static inline long sys_read(int fd, void *buf, size_t count) {
    return syscall3(SYS_read, fd, (long)buf, count);
}

static inline long sys_write(int fd, const void *buf, size_t count) {
    return syscall3(SYS_write, fd, (long)buf, count);
}

static inline long sys_sched_yield(void) {
    return syscall0(SYS_sched_yield);
}

static inline long sys_getpid(void) {
    return syscall0(SYS_getpid);
}

static inline long sys_fork(void) {
    return syscall0(SYS_fork);
}

static inline long sys_wait4(long pid, long *ustatus, long options) {
    return syscall3(SYS_wait4, pid, (long)ustatus, options);
}

static inline long sys_execve(const char *path, long argv, long envp) {
    return syscall3(SYS_execve, (long)path, argv, envp);
}

static inline long sys_open(const char *path, int flags) {
    return syscall2(SYS_open, (long)path, flags);
}

static inline long sys_close(int fd) {
    return syscall1(SYS_close, fd);
}

static inline long sys_lseek(int fd, long offset, int whence) {
    return syscall3(SYS_lseek, fd, offset, whence);
}

static inline long sys_getcwd(char *buf, size_t size) {
    return syscall2(SYS_getcwd, (long)buf, size);
}

static inline long sys_chdir(const char *path) {
    return syscall1(SYS_chdir, (long)path);
}

/* mmap(2): length bytes of anonymous private memory.  addr==0 picks an
 * address (top-down, below the stack); addr!=0 maps fixed.  Returns the
 * mapping or MAP_FAILED (-1).  */
static inline void *sys_mmap(void *addr, size_t length, int prot, int flags,
                             int fd, long offset) {
    long ret = syscall6(SYS_mmap, (long)addr, (long)length, prot, flags,
                        fd, offset);
    return (ret == -1) ? MAP_FAILED : (void *)ret;
}

static inline long sys_munmap(void *addr, size_t length) {
    return syscall2(SYS_munmap, (long)addr, (long)length);
}

/* brk: set the break to `addr` and return the new break (or the current one
 * on query/no-op), positive, or -errno.  sys_brk returns the NEW break, not
 * the old one, so sbrk() keeps its own break cache in userspace. */
static inline long sys_brk(void *addr) {
    return syscall1(SYS_brk, (long)addr);
}

/* sbrk: increment the break by `inc` (negatives allowed) and return the OLD
 * break — the classic sbrk contract.  A pure query (inc == 0) reads the
 * kernel break directly and never moves it: passing a cached value back into
 * sys_brk would shrink the break to the stale cache.  All other calls use a
 * userspace break cache initialised on first move via sbrk(0).
 * Returns (void*)-1 on error.  */
static inline void *sys_sbrk(long inc) {
    if (inc == 0)
        return (void *)sys_brk(0);        /* read-only: never touches the break */

    static long brk_cache;                /* process-global userspace break (BSS) */
    if (!brk_cache) {
        long zero = sys_brk(0);
        if (zero <= 0) return (void *)-1;
        brk_cache = zero;
    }
    long old = brk_cache;
    long nb  = old + inc;                 /* overflow checked by the kernel below */
    if (nb < 0 || sys_brk((void *)nb) != nb) return (void *)-1;
    brk_cache = nb;
    return (void *)old;
}

static inline long sys_readdir(int fd, char *name) {
    return syscall2(SYS_readdir, fd, (long)name);
}

/* TTY control requests (Phase 14).  Mirrors kernel/include/dev/tty.h.
 * Echo on/off are the canonical testable toggles. */
#define VOID_TTY_ECHO_ON  0x545401
#define VOID_TTY_ECHO_OFF 0x545402

/* ioctl(2): Linux number 16; only the TTY requests above exist.  A
 * non-TTY fd (or an unknown request) is -VE_INVAL/-VE_BADF. */
static inline long sys_ioctl(int fd, unsigned long req, void *arg) {
    return syscall3(SYS_ioctl, fd, req, (long)arg);
}

static inline void sys_exit(int status) {
    syscall1(SYS_exit, status);
    __builtin_unreachable();
}

#endif /* LIBVOID_VOID_H */
