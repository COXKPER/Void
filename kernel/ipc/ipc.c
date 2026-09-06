/* VoidOS — IPC core implementation
 *
 * Message-passing foundation. Handles endpoint lifecycle, message queue,
 * and cross-process communication. All user pointers are validated via
 * the same HHDM + page-table mechanism used by sys_read/sys_write.
 */
#include <ipc/ipc.h>
#include <ipc/ipc_internal.h>
#include <proc/process.h>
#include <syscall/syscall.h>
#include <void/boot.h>
#include <dev/serial.h>
#include <mm/kheap.h>

/* ── Global Endpoint Registry ────────────────────────────────────── */
ipc_endpoint_registry_entry_t ipc_endpoint_registry[IPC_MAX_TOTAL_ENDPOINTS];
uint32_t ipc_endpoint_registry_count = 0;

/* ── Registry Operations ─────────────────────────────────────────── */

ipc_endpoint_t *ipc_endpoint_lookup(pid_t_v owner_pid, uint32_t endpoint_id) {
    for (uint32_t i = 0; i < ipc_endpoint_registry_count; i++) {
        if (ipc_endpoint_registry[i].endpoint != NULL &&
            ipc_endpoint_registry[i].owner_pid == owner_pid &&
            ipc_endpoint_registry[i].endpoint_id == endpoint_id) {
            return ipc_endpoint_registry[i].endpoint;
        }
    }
    return NULL;
}

int32_t ipc_endpoint_register(ipc_endpoint_t *ep, pid_t_v owner_pid, uint32_t endpoint_id) {
    /* Reuse a released slot first, then grow the high-water mark. */
    uint32_t i;
    for (i = 0; i < ipc_endpoint_registry_count; i++)
        if (ipc_endpoint_registry[i].endpoint == NULL) break;
    if (i == ipc_endpoint_registry_count) {
        if (i >= IPC_MAX_TOTAL_ENDPOINTS) return -VE_NOMEM;
        ipc_endpoint_registry_count++;
    }

    ipc_endpoint_registry[i].endpoint    = ep;
    ipc_endpoint_registry[i].owner_pid   = owner_pid;
    ipc_endpoint_registry[i].endpoint_id = endpoint_id;
    return 0;
}

/* Free one endpoint and release its registry slot to the free pool.
 * ponytail: registry_count is a high-water mark; slots are reused via the
 * NULL scan in ipc_endpoint_register. */
static void endpoint_release(ipc_endpoint_registry_entry_t *e) {
    kfree(e->endpoint);
    e->endpoint = NULL;
}

void ipc_endpoint_unregister_by_owner(pid_t_v owner_pid) {
    for (uint32_t i = 0; i < ipc_endpoint_registry_count; i++) {
        if (ipc_endpoint_registry[i].endpoint != NULL &&
            ipc_endpoint_registry[i].owner_pid == owner_pid)
            endpoint_release(&ipc_endpoint_registry[i]);
    }
}

/* ── Message Queue Operations ────────────────────────────────────── */

int32_t ipc_queue_enqueue(ipc_endpoint_t *ep, const ipc_message_t *msg) {
    if (!ep || !msg) return -VE_INVAL;

    if (ep->queue_count >= IPC_MAX_QUEUE_DEPTH) {
        return -VE_NOMEM;  /* ENOSPC: queue full */
    }

    if (ep->flags & IPC_ENDPOINT_CLOSED) {
        return -VE_BADF;
    }

    ep->queue[ep->queue_tail] = *msg;
    ep->queue_tail = (ep->queue_tail + 1) % IPC_MAX_QUEUE_DEPTH;
    ep->queue_count++;

    return 0;
}

int32_t ipc_queue_dequeue(ipc_endpoint_t *ep, ipc_message_t *msg_out) {
    if (!ep || !msg_out) return -1;

    if (ep->queue_count == 0) {
        return -1;  /* empty queue */
    }

    *msg_out = ep->queue[ep->queue_head];
    ep->queue_head = (ep->queue_head + 1) % IPC_MAX_QUEUE_DEPTH;
    ep->queue_count--;

    return 0;
}

/* ── Handle Table Operations ─────────────────────────────────────── */

static ipc_handle_table_t *handle_table_create(void) {
    ipc_handle_table_t *table = kmalloc(sizeof(ipc_handle_table_t));
    if (!table) return NULL;

    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        table->handles[i].flags = 0;
        table->handles[i].target_pid = 0;
        table->handles[i].target_endpoint_id = 0;
    }
    table->next_local_endpoint_id = 0;

    return table;
}

static void handle_table_destroy(ipc_handle_table_t *table) {
    if (!table) return;
    kfree(table);
}

ipc_handle_table_t *ipc_handle_table_create(void) {
    return handle_table_create();
}

void ipc_handle_table_destroy(ipc_handle_table_t *table) {
    handle_table_destroy(table);
}

/* ── Syscall Handlers ────────────────────────────────────────────── */

int32_t sys_ipc_endpoint_create(void) {
    process_t *p = process_current();
    if (!p || !p->ipc_handles) return -VE_PERM;

    /* Find a free handle slot */
    int32_t handle = -1;
    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        if (!(p->ipc_handles->handles[i].flags & IPC_HANDLE_ALLOCATED)) {
            handle = i;
            break;
        }
    }

    if (handle < 0) return -VE_NOMEM;  /* no free handles */

    /* Allocate an endpoint structure */
    ipc_endpoint_t *ep = kmalloc(sizeof(ipc_endpoint_t));
    if (!ep) return -VE_NOMEM;

    /* Initialize endpoint */
    ep->owner_pid = p->pid;
    ep->endpoint_id = p->ipc_handles->next_local_endpoint_id++;
    ep->flags = 0;
    ep->queue_head = 0;
    ep->queue_tail = 0;
    ep->queue_count = 0;

    /* Register in global registry */
    if (ipc_endpoint_register(ep, ep->owner_pid, ep->endpoint_id) < 0) {
        kfree(ep);
        return -VE_NOMEM;
    }

    /* Link handle to endpoint */
    p->ipc_handles->handles[handle].flags = IPC_HANDLE_ALLOCATED;
    p->ipc_handles->handles[handle].target_pid = ep->owner_pid;
    p->ipc_handles->handles[handle].target_endpoint_id = ep->endpoint_id;

    return (int32_t)handle;
}

int32_t sys_ipc_send(int32_t dest_handle, uint32_t tag,
                     const void *data, uint32_t data_len) {
    process_t *p = process_current();
    if (!p || !p->ipc_handles) return -VE_PERM;

    /* Validate handle range */
    if (dest_handle < 0 || dest_handle >= IPC_MAX_HANDLES) {
        return -VE_BADF;
    }

    /* Validate data length */
    if (data_len > (IPC_MAX_MESSAGE_SIZE - sizeof(uint32_t) * 4)) {
        return -VE_INVAL;
    }

    ipc_handle_entry_t *handle_entry = &p->ipc_handles->handles[dest_handle];

    if (!(handle_entry->flags & IPC_HANDLE_ALLOCATED)) {
        return -VE_BADF;
    }

    /* Look up destination endpoint */
    ipc_endpoint_t *dest_ep = ipc_endpoint_lookup(
        handle_entry->target_pid,
        handle_entry->target_endpoint_id
    );

    if (!dest_ep) {
        return -VE_BADF;  /* endpoint not found or closed */
    }

    /* Build message */
    ipc_message_t msg;
    msg.sender_pid = p->pid;
    msg.tag = tag;
    msg.data_len = data_len;

    /* Validate and copy user data via HHDM */
    if (data_len > 0) {
        if (!user_range_ok(p, (uint64_t)data, data_len, false)) {
            return -VE_FAULT;
        }

        if (!copy_from_user(p, msg.data, (uint64_t)data, data_len)) {
            return -VE_FAULT;
        }
    }

    /* Enqueue message */
    return ipc_queue_enqueue(dest_ep, &msg);
}

int32_t sys_ipc_recv(int32_t handle, uint32_t *tag_out,
                     void *data_out, uint32_t max_len) {
    process_t *p = process_current();
    if (!p || !p->ipc_handles) return -VE_PERM;

    /* Validate handle range */
    if (handle < 0 || handle >= IPC_MAX_HANDLES) {
        return -VE_BADF;
    }

    ipc_handle_entry_t *handle_entry = &p->ipc_handles->handles[handle];

    if (!(handle_entry->flags & IPC_HANDLE_ALLOCATED)) {
        return -VE_BADF;
    }

    /* Look up endpoint */
    ipc_endpoint_t *ep = ipc_endpoint_lookup(
        handle_entry->target_pid,
        handle_entry->target_endpoint_id
    );

    if (!ep) {
        return -VE_BADF;
    }

    /* Try to dequeue a message */
    ipc_message_t msg;
    if (ipc_queue_dequeue(ep, &msg) < 0) {
        return 0;  /* no message available (poll semantics) */
    }

    /* Copy tag if provided */
    if (tag_out) {
        if (!user_range_ok(p, (uint64_t)tag_out, sizeof(uint32_t), true)) {
            return -VE_FAULT;
        }
        if (!copy_to_user(p, (uint64_t)tag_out, &msg.tag, sizeof(uint32_t))) {
            return -VE_FAULT;
        }
    }

    /* Copy data if provided and available */
    uint32_t copy_len = msg.data_len;
    if (copy_len > max_len) copy_len = max_len;

    if (copy_len > 0 && data_out) {
        if (!user_range_ok(p, (uint64_t)data_out, copy_len, true)) {
            return -VE_FAULT;
        }
        if (!copy_to_user(p, (uint64_t)data_out, msg.data, copy_len)) {
            return -VE_FAULT;
        }
    }

    return (int32_t)copy_len;
}

int32_t sys_ipc_close(int32_t handle) {
    process_t *p = process_current();
    if (!p || !p->ipc_handles) return -VE_PERM;

    /* Validate handle range */
    if (handle < 0 || handle >= IPC_MAX_HANDLES) {
        return -VE_BADF;
    }

    ipc_handle_entry_t *handle_entry = &p->ipc_handles->handles[handle];

    if (!(handle_entry->flags & IPC_HANDLE_ALLOCATED)) {
        return -VE_BADF;
    }

    /* Look up and close endpoint */
    ipc_endpoint_t *ep = ipc_endpoint_lookup(
        handle_entry->target_pid,
        handle_entry->target_endpoint_id
    );

    if (ep && ep->owner_pid == p->pid) {
        /* We own this endpoint, mark it closed */
        ep->flags |= IPC_ENDPOINT_CLOSED;
    }

    /* Deallocate handle */
    handle_entry->flags = 0;

    return 0;
}
