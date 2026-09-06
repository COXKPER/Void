/* libvoid — IPC syscall wrappers (userland)
 *
 * Thin wrappers around Void IPC syscalls matching x86_64 ABI.
 */
#ifndef LIBVOID_IPC_H
#define LIBVOID_IPC_H 1

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers */
#define IPC_SYS_ENDPOINT_CREATE  62
#define IPC_SYS_SEND             63
#define IPC_SYS_RECV             64
#define IPC_SYS_CLOSE            65

/* Syscall wrappers */

static inline long ipc_endpoint_create(void) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(IPC_SYS_ENDPOINT_CREATE)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long ipc_send(int handle, uint32_t tag, const void *data, uint32_t len) {
    long ret;
    register uint64_t r10 __asm__("r10") = (uint64_t)len;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(IPC_SYS_SEND), "D"((long)handle), "S"((long)tag),
                       "d"((long)data), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long ipc_recv(int handle, uint32_t *tag, void *data, uint32_t len) {
    long ret;
    register uint64_t r10 __asm__("r10") = (uint64_t)len;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(IPC_SYS_RECV), "D"((long)handle), "S"((long)tag),
                       "d"((long)data), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long ipc_close(int handle) {
    long ret;
    __asm__ volatile ("syscall"
                     : "=a"(ret)
                     : "a"(IPC_SYS_CLOSE), "D"((long)handle)
                     : "rcx", "r11", "memory");
    return ret;
}

#endif /* LIBVOID_IPC_H */
