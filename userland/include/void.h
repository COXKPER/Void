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
#define SYS_sched_yield 24
#define SYS_getpid      39
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_fork        67      /* Void slot (no Linux index)          */
#define SYS_getcwd      79
#define SYS_chdir       80
#define SYS_readdir     89

/* ── open(2) flags ──────────────────────────────────────────────────── */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

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

static inline long sys_readdir(int fd, char *name) {
    return syscall2(SYS_readdir, fd, (long)name);
}

static inline void sys_exit(int status) {
    syscall1(SYS_exit, status);
    __builtin_unreachable();
}

#endif /* LIBVOID_VOID_H */
