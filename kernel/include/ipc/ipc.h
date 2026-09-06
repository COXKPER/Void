/* VoidOS — IPC core (syscall API and public types)
 *
 * Minimal message-passing foundation for inter-process communication.
 * Designed for future service isolation without breaking existing Void APIs.
 *
 * IPC model:
 * - Endpoints: per-process message queues
 * - Handles: user-space references to endpoints (similar to FDs)
 * - Messages: fixed-size (512 bytes), inline data, tagged
 * - Send/Recv: async/poll for MVP (no blocking yet)
 * - Safety: all user pointers validated via HHDM + page tables
 */
#ifndef VOID_IPC_H
#define VOID_IPC_H 1

#include <void/types.h>

/* ── Syscall numbers (Void x86_64 compatible) ─────────────────────── */
#define SYS_ipc_endpoint_create  62
#define SYS_ipc_send             63
#define SYS_ipc_recv             64
#define SYS_ipc_close            65

/* ── Constants ────────────────────────────────────────────────────── */
#define IPC_MAX_HANDLES       64
#define IPC_MAX_MESSAGE_SIZE  512
#define IPC_MAX_QUEUE_DEPTH   16
#define IPC_INVALID_HANDLE    -1

/* ── Error codes (negated errno, same as Void VE_* pattern) ────────── */
#define IPC_E_OK       0
#define IPC_E_BADF     -9      /* EBADF: bad handle */
#define IPC_E_INVAL    -22     /* EINVAL: invalid argument */
#define IPC_E_FAULT    -14     /* EFAULT: invalid user pointer */
#define IPC_E_QUEUE    -28     /* ENOSPC: queue full */
#define IPC_E_PERM     -1      /* EPERM: not owner */

/* ── Message structure (fits in fixed 512 bytes) ────────────────── */
typedef struct {
    uint32_t sender_pid;       /* PID of sender */
    uint32_t tag;              /* User-defined tag */
    uint32_t data_len;         /* Actual data length (0 to MAX-header) */
    uint32_t _pad;             /* Alignment padding */
    uint8_t data[496];         /* Inline message data */
} ipc_message_t;

_Static_assert(sizeof(ipc_message_t) == 512, "ipc_message_t must be exactly 512 bytes");

/* ── Public API declarations ──────────────────────────────────────── */

/* Create a new IPC endpoint owned by the calling process.
 * Returns a handle (0-63) on success, or negative error code. */
int32_t sys_ipc_endpoint_create(void);

/* Send a message to an endpoint identified by handle.
 * Returns 0 on success, negative error code on failure.
 * Returns -EQUEUE if destination queue is full (caller can retry).
 * All user pointers are validated before message is queued. */
int32_t sys_ipc_send(int32_t dest_handle, uint32_t tag,
                     const void *data, uint32_t data_len);

/* Receive a message from an endpoint identified by handle.
 * Returns number of bytes copied on success, 0 if no message available
 * (poll semantics), negative error code on failure.
 * User buffer must be provided and validated before copy. */
int32_t sys_ipc_recv(int32_t handle, uint32_t *tag_out,
                     void *data_out, uint32_t max_len);

/* Close an IPC endpoint identified by handle.
 * Subsequent sends to this endpoint will fail with -EBADF.
 * Returns 0 on success, negative error code on failure. */
int32_t sys_ipc_close(int32_t handle);

/* ── Kernel-internal declarations (for process.h integration) ────── */

/* Forward declare for process.h */
typedef struct ipc_handle_table ipc_handle_table_t;

/* Allocate and initialize a new IPC handle table for a process. */
ipc_handle_table_t *ipc_handle_table_create(void);

/* Close all endpoints and free a handle table on process exit. */
void ipc_handle_table_destroy(ipc_handle_table_t *table);

/* Unregister all endpoints owned by a process (cleanup on exit). */
void ipc_endpoint_unregister_by_owner(uint32_t owner_pid);

#endif /* VOID_IPC_H */
