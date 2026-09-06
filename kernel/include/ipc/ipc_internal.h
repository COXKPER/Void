/* VoidOS — IPC internal structures (kernel-only)
 *
 * Internal implementation details. Not exposed to userland.
 */
#ifndef VOID_IPC_INTERNAL_H
#define VOID_IPC_INTERNAL_H 1

#include <void/types.h>
#include <ipc/ipc.h>
#include <proc/process.h>

/* ── IPC Endpoint (kernel-side message queue) ────────────────────── */
typedef struct ipc_endpoint {
    pid_t_v owner_pid;                          /* Process that created this */
    uint32_t endpoint_id;                       /* Local endpoint ID within owner */
    uint8_t flags;                              /* CLOSED, etc. */

    /* Circular message queue */
    ipc_message_t queue[IPC_MAX_QUEUE_DEPTH];
    uint32_t queue_head;                        /* Next read position */
    uint32_t queue_tail;                        /* Next write position */
    uint32_t queue_count;                       /* Number of messages in queue */
} ipc_endpoint_t;

#define IPC_ENDPOINT_CLOSED 0x01

/* ── Handle Table Entry ──────────────────────────────────────────── */
typedef struct {
    pid_t_v target_pid;                         /* PID that owns the endpoint */
    uint32_t target_endpoint_id;                /* Endpoint ID within target process */
    uint8_t flags;                              /* 0 if unused, 1 if allocated */
} ipc_handle_entry_t;

#define IPC_HANDLE_ALLOCATED 0x01

/* ── Per-Process Handle Table ────────────────────────────────────── */
struct ipc_handle_table {
    ipc_handle_entry_t handles[IPC_MAX_HANDLES];
    uint32_t next_local_endpoint_id;            /* Counter for creating new endpoints */
};

/* ── Global Endpoint Registry ────────────────────────────────────── */
/* Maps (owner_pid, endpoint_id) → ipc_endpoint_t
 * Implemented as a simple linear table for MVP; can upgrade to hash later. */

#define IPC_MAX_TOTAL_ENDPOINTS 1024

typedef struct {
    ipc_endpoint_t *endpoint;                   /* Allocated endpoint, or NULL */
    pid_t_v owner_pid;
    uint32_t endpoint_id;
} ipc_endpoint_registry_entry_t;

extern ipc_endpoint_registry_entry_t ipc_endpoint_registry[IPC_MAX_TOTAL_ENDPOINTS];
extern uint32_t ipc_endpoint_registry_count;

/* ── Registry Operations (kernel-only) ────────────────────────────── */

/* Find an endpoint by (owner_pid, endpoint_id). Returns pointer or NULL. */
ipc_endpoint_t *ipc_endpoint_lookup(pid_t_v owner_pid, uint32_t endpoint_id);

/* Register a new endpoint in the global registry. */
int32_t ipc_endpoint_register(ipc_endpoint_t *ep, pid_t_v owner_pid, uint32_t endpoint_id);

/* Unregister and free all endpoints owned by a process. */
void ipc_endpoint_unregister_by_owner(pid_t_v owner_pid);

/* ── Message Queue Operations ──────────────────────────────────────── */

/* Enqueue a message into an endpoint. Returns 0 on success, -EQUEUE if full. */
int32_t ipc_queue_enqueue(ipc_endpoint_t *ep, const ipc_message_t *msg);

/* Dequeue the first message from an endpoint. Returns 0 on success, -1 if empty. */
int32_t ipc_queue_dequeue(ipc_endpoint_t *ep, ipc_message_t *msg_out);

#endif /* VOID_IPC_INTERNAL_H */
