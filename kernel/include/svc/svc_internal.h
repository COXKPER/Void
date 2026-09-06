/* VoidOS — service registry internals (shared by svc.c and ipc.c)
 *
 * A service is a named IPC endpoint.  The registry is the kernel-side
 * directory that maps a service name to the endpoint address of the process
 * that registered it.  It is NOT a second IPC implementation: it only holds
 * addresses; message transport stays on the existing endpoint/handle IPC.
 *
 * Ownership model:
 *   - Each registry entry names (owner_pid, endpoint_id).  The owner is the
 *     process that created the endpoint and owns it in the IPC sense.
 *   - Entries die at either end-point of the service's life:
 *       ipc_close (owner drops its endpoint) → svc_on_endpoint_close()
 *       owner process exit                 → svc_cleanup_owner()
 *     Either way a lookup can never reach a dead endpoint by name.
 *
 * Locking: single-CPU-priority boot; both call sites perform a bounded
 * fixed-cost scan.  Add a spinlock when SMP lands.
 */
#ifndef VOID_SVC_INTERNAL_H
#define VOID_SVC_INTERNAL_H 1

#include <void/types.h>
#include <proc/process.h>

#define SVC_NAME_LEN      32        /* max service name, incl. NUL        */
#define SVC_MAX_SERVICES  64        /* registry capacity (fixed array)    */

/* Owner closed its own endpoint.  Drop the registry entry that names that
 * exact (owner_pid, endpoint_id), if any.  Best-effort: most endpoint
 * closes have no service entry, and the IPC close path stays O(64). */
void svc_on_endpoint_close(pid_t_v owner_pid, uint32_t endpoint_id);

/* Owner process is exiting / being destroyed.  Drop every entry it owned. */
void svc_cleanup_owner(pid_t_v owner_pid);

#endif /* VOID_SVC_INTERNAL_H */