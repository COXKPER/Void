/* libvoid — service registry wrappers (userland)
 *
 * Thin wrappers around the Void service-registry syscalls, matching the
 * generic 4-arg x86_64 ABI.  The data plane (sending requests, receiving
 * responses) uses the plain IPC wrappers in <ipc.h>.
 */
#ifndef LIBVOID_SVC_H
#define LIBVOID_SVC_H 1

#include <stdint.h>
#include <stddef.h>

/* ── service registry syscall numbers (Void slots 68-70) ─────────────── */
#define SVC_SYS_NAME_REGISTER   68
#define SVC_SYS_NAME_UNREGISTER 69
#define SVC_SYS_LOOKUP          70

/* ── error codes (already negative; returned verbatim, negated errno) ── */
#define SVC_EOK       0
#define SVC_EFAULT   -14     /* EFAULT: bad user pointer / name/strlen   */
#define SVC_ENAMELEN -22     /* EINVAL: name empty / too long            */
#define SVC_ENAME    -2      /* ENOENT: no such service                  */
#define SVC_ECONFLICT -1     /* EPERM:  name taken / wrong owner on delete*/
#define SVC_EFULL    -12     /* ENOMEM: registry at capacity             */

/* register a service name bound to a local endpoint handle. */
static inline long sr_register(const char *name, int handle) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(SVC_SYS_NAME_REGISTER), "D"((long)name),
                       "S"((long)handle)
                     : "rcx", "r11", "memory");
    return ret;
}

/* unregister a service name.  handle must be the one used to register. */
static inline long sr_unregister(const char *name, int handle) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(SVC_SYS_NAME_UNREGISTER), "D"((long)name),
                       "S"((long)handle)
                     : "rcx", "r11", "memory");
    return ret;
}

/* look up a service by name; returns a NEW IPC handle to its endpoint, or
 * a negative error.  The handle is used with the plain IPC data plane. */
static inline long sr_lookup(const char *name) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(SVC_SYS_LOOKUP), "D"((long)name)
                     : "rcx", "r11", "memory");
    return ret;
}

#endif /* LIBVOID_SVC_H */