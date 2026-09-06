/* VoidOS — service registry API (kernel + userland)
 *
 * A service is a named IPC endpoint.  The kernel registry maps a service
 * name to the endpoint a server registered for it.  Transport stays on the
 * existing endpoint/handle IPC — this is only a name directory, not a
 * second IPC mechanism.
 *
 * Contract (see CLAUDE.md design decision for the full model):
 *   Server  : sr_register(name, handle) / sr_unregister(name, handle)
 *   Client  : sr_lookup(name)  → returns an IPC handle to the server's
 *             endpoint, which the client uses with the plain ipc_send/
 *             ipc_recv data plane.
 *
 * The one privilege escalation (a handle to another process's endpoint) is
 * granted only here, and only for the exact endpoint the server registered.
 *
 * Syscall ABI:  generic 4-arg shape − RDI arg0, RSI arg1, RDX arg2,
 * R10 arg3 − return in RAX (negative = -errno).  arg1/data lengths stay in
 * R10 because SYSCALL clobbers RCX.
 */
#ifndef VOID_SVC_H
#define VOID_SVC_H 1

#include <void/types.h>
#include <ipc/ipc.h>

/* ── syscall numbers (Void slots after SYS_ipc_close=65 / SYS_fork=67) ── */
#define SYS_svc_name_register    68
#define SYS_svc_name_unregister  69
#define SYS_svc_lookup           70

/* ── error codes (already negative; returned verbatim, like VE_*) ─────── */
#define SVC_E_OK      0
#define SVC_E_FAULT  -14      /* EFAULT: bad user pointer/name/strlen      */
#define SVC_E_NAMELEN -22     /* EINVAL: name empty / too long              */
#define SVC_E_NAME   -2       /* ENOENT: no such service                    */
#define SVC_E_CONFLICT -1     /* EPERM:  name taken / wrong owner on delete */
#define SVC_E_FULL   -12      /* ENOMEM: registry at capacity               */
#define SVC_E_BADF   -9       /* EBADF:  bad endpoint handle               */
#define SVC_E_NOMEM  -12      /* ENOMEM: handle table / registry full     */

/* ── public API (kernel) ──────────────────────────────────────────────── */

/* Register a service owned by this process.  name is copied (max SVC_NAME_LEN).
 * handle must be a local endpoint handle created with ipc_endpoint_create.
 * Returns 0, -SVC_E_NAMELEN, -SVC_E_FAULT, -SVC_E_BADF, or -SVC_E_CONFLICT. */
int32_t sys_sr_register(const char *name, int32_t handle);

/* Remove a registration.  Only the registering process may unregister.
 * Returns 0 or negative error. */
int32_t sys_sr_unregister(const char *name, int32_t handle);

/* Look up a service and return a *new handle* (≥ 0) to its endpoint, which
 * the caller then uses with the plain IPC data plane.  The name is validated.
 * Returns a handle, -SVC_E_NAMELEN, -SVC_E_FAULT, -SVC_E_NAME, or -VE_NOMEM. */
int32_t sys_sr_lookup(const char *name);

#endif /* VOID_SVC_H */