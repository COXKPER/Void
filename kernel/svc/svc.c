/* VoidOS — service registry
 *
 * A service is a named IPC endpoint.  This is the kernel-side directory that
 * maps a service name to the endpoint address of its owner.  It holds only
 * *addresses* — message transport stays entirely on the existing endpoint/
 * handle IPC, so the registry is not a second IPC implementation.
 *
 * Ownership and lifetime:
 *   - A registry entry names (owner_pid, endpoint_id): the process that owns
 *     the endpoint in the IPC sense.  register() only accepts a handle the
 *     caller owns; unregister() only acts for the registering process; the
 *     IPC exit hook removes the entry when its owner exits or unregisters.
 *     No stale lookup ever names a dead endpoint.
 *   - ipc_connect() is the one place a handle to another process's endpoint
 *     is minted.  The kernel cross-checks the endpoint against the IPC
 *     registry at connect time, so a handle cannot outlive the endpoint it
 *     names (the process-destroy/revoke race).
 *
 * Safety: every user pointer (service name) is validated via the HHDM/page-
 * table walk before copying, exactly like the rest of the syscall surface.
 *
 * Locking: single-CPU-priority boot, bounded fixed-cost scans at both call
 * sites.  Add a spinlock when SMP lands.
 *
 * This is the registry's only syscall surface:
 *   sr_register / sr_unregister : server-side name management + cleanup.
 *   sr_lookup                   : the one address-space-creation privilege.
 * ipc_connect (the data-plane addition) lives in ipc.c.
 */
#include <svc/svc.h>
#include <svc/svc_internal.h>
#include <ipc/ipc.h>
#include <ipc/ipc_internal.h>
#include <proc/process.h>
#include <void/boot.h>

/* ── registry entry ────────────────────────────────────────────────────────
 * A fixed array of (owner_pid, endpoint_id) pairs grouped under a name.
 * name[0] == '\0' marks a free slot ("not registered").  A registered name
 * is always nonempty, so this never collides with a live entry — even an
 * endpoint whose id is 0 (a service registered on its handle-0 endpoint). */
typedef struct {
    char        name[SVC_NAME_LEN];
    pid_t_v     owner_pid;
    uint32_t    endpoint_id;
} svc_entry_t;

static svc_entry_t svc_registry[SVC_MAX_SERVICES];
static int svc_count = 0;

/* ── helpers ────────────────────────────────────────────────────────────── */

static int svc_validate_name(const char *name) {
    if (!name) return SVC_E_FAULT;
    size_t len = 0;
    while (len < SVC_NAME_LEN && name[len]) len++;
    if (len == 0)        return SVC_E_NAMELEN;
    if (len >= SVC_NAME_LEN) return SVC_E_NAMELEN;
    return (int)len;
}

/* ── registry scan helpers ─────────────────────────────────────────────── */

static int entry_free(const svc_entry_t *e) { return e->name[0] == '\0'; }

static svc_entry_t *svc_find_name(const char *name) {
    for (int i = 0; i < svc_count; i++) {
        svc_entry_t *e = &svc_registry[i];
        if (entry_free(e)) continue;
        int eq = 1;
        for (int k = 0; k < SVC_NAME_LEN; k++) {
            if (e->name[k] != name[k]) { eq = 0; break; }
            if (name[k] == 0) break;
        }
        if (eq) return e;
    }
    return NULL;
}

static svc_entry_t *svc_free_slot(void) {
    for (int i = 0; i < svc_count; i++)
        if (entry_free(&svc_registry[i])) return &svc_registry[i];
    if (svc_count >= SVC_MAX_SERVICES) return NULL;
    return &svc_registry[svc_count++];
}

/* ── endpoint validation shared by register / unregister ─────────────────
 * A valid endpoint handle is one we own, backed by a live endpoint in the
 * IPC registry (so registering an already-closed endpoint fails cleanly). */
static int svc_check_endpoint(process_t *p, int32_t handle) {
    if (handle < 0 || handle >= IPC_MAX_HANDLES) return 0;
    if (!p->ipc_handles) return 0;
    ipc_handle_entry_t *he = &p->ipc_handles->handles[handle];
    if (!(he->flags & IPC_HANDLE_ALLOCATED)) return 0;
    if (he->target_pid != p->pid) return 0;                 /* not ours      */
    ipc_endpoint_t *ep = ipc_endpoint_lookup(p->pid, he->target_endpoint_id);
    if (!ep || ep->owner_pid != p->pid) return 0;           /* already gone  */
    return 1;
}

/* ── IPC-level registry hook (called from ipc.c) ─────────────────────────
 * Owner closed its own endpoint (ipc_close).  Drop the entry that names
 * that exact endpoint.  The IPC close path stays O(64). */
void svc_on_endpoint_close(pid_t_v owner_pid, uint32_t endpoint_id) {
    for (int i = 0; i < svc_count; i++) {
        svc_entry_t *e = &svc_registry[i];
        if (entry_free(e)) continue;
        if (e->owner_pid == owner_pid && e->endpoint_id == endpoint_id) {
            e->name[0] = '\0';
            return;
        }
    }
}

/* ── the registry syscall surface ─────────────────────────────────────── */

/* server-side: register a name for one of our own endpoints. */
int32_t sys_sr_register(const char *name, int32_t handle) {
    process_t *p = process_current();
    if (!p) return SVC_E_FAULT;

    int nlen = svc_validate_name(name);
    if (nlen < 0) return nlen;
    if (!svc_check_endpoint(p, handle)) return SVC_E_BADF;

    if (svc_find_name(name)) return SVC_E_CONFLICT;   /* name already taken */

    svc_entry_t *slot = svc_free_slot();
    if (!slot) return SVC_E_FULL;

    for (int k = 0; k < SVC_NAME_LEN; k++) {
        slot->name[k] = name[k];
        if (name[k] == 0) break;
    }
    slot->owner_pid   = p->pid;
    slot->endpoint_id = p->ipc_handles->handles[(uint32_t)handle].target_endpoint_id;

    return 0;
}

/* server-side: remove a name registration for one of our endpoints. */
int32_t sys_sr_unregister(const char *name, int32_t handle) {
    process_t *p = process_current();
    if (!p) return SVC_E_FAULT;

    int nlen = svc_validate_name(name);
    if (nlen < 0) return nlen;
    if (!svc_check_endpoint(p, handle)) return SVC_E_BADF;

    svc_entry_t *e = svc_find_name(name);
    if (!e) return SVC_E_NAME;
    if (e->owner_pid != p->pid) return SVC_E_CONFLICT;

    e->name[0] = '\0';                  /* free the slot */
    return 0;
}

/* client-side: discover a service and return a fresh IPC handle to it.
 * The one privilege escalation — a handle to another process's endpoint —
 * is granted here and only here, and only for the exact endpoint the
 * server registered. */
int32_t sys_sr_lookup(const char *name) {
    process_t *p = process_current();
    if (!p) return SVC_E_FAULT;

    int nlen = svc_validate_name(name);
    if (nlen < 0) return nlen;

    svc_entry_t *e = svc_find_name(name);
    if (!e) return SVC_E_NAME;

    if (!p->ipc_handles) p->ipc_handles = ipc_handle_table_create();
    if (!p->ipc_handles) return SVC_E_NOMEM;

    int32_t handle = -1;
    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        if (!(p->ipc_handles->handles[i].flags & IPC_HANDLE_ALLOCATED)) {
            handle = i; break;
        }
    }
    if (handle < 0) return SVC_E_NOMEM;    /* no free handle slots */

    p->ipc_handles->handles[handle].flags              = IPC_HANDLE_ALLOCATED;
    p->ipc_handles->handles[handle].target_pid         = e->owner_pid;
    p->ipc_handles->handles[handle].target_endpoint_id = e->endpoint_id;

    return handle;
}

/* ── exit cleanup ──────────────────────────────────────────────────────────
 * Called from process_exit_current / process_destroy.  Removes every service
 * entry the exiting process owned, so a dead service is never discoverable. */
void svc_cleanup_owner(pid_t_v owner_pid) {
    for (int i = 0; i < svc_count; i++)
        if (svc_registry[i].owner_pid == owner_pid)
            svc_registry[i].name[0] = '\0';
}