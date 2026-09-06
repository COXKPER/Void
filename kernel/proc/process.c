/* VoidOS — Process abstraction
 *
 * Each process gets a fresh PML4 whose upper half (entries 256–511) is
 * copied from the kernel PML4.  Because those entries are shared *by
 * reference*, later kernel-side mappings (heap growth, new kernel stacks)
 * are visible in every address space without a fixup pass — as long as
 * they land under an already-present PML4 entry, which the fixed kernel
 * layout guarantees.
 *
 * Lower-half entries start empty, so user memory is private per process.
 */
#include <proc/process.h>
#include <sched/sched.h>
#include <syscall/syscall.h>
#include <elf/elf.h>
#include <arch/x86_64/gdt.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <void/boot.h>
#include <dev/serial.h>
#include <ipc/ipc.h>
#include <ipc/ipc_internal.h>   /* handle table layout, for fork's deep copy */
#include <svc/svc_internal.h>   /* service registry owner cleanup on exit   */
#include <void/voidfs.h>        /* VFS fd-table teardown on exit/destroy    */

static process_t proc_table[MAX_PROCESSES];
static pid_t_v   next_pid;

/* ── HHDM helper ─────────────────────────────────────────────────────── */
static inline uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(phys + g_boot.hhdm_offset);
}

/* ── build a fresh address space ─────────────────────────────────────── */
/* Returns the physical address of a new PML4, or 0 on failure. */
static uint64_t address_space_create(void) {
    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) return 0;

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t *kpml4 = phys_to_virt(vmm_kernel_pml4());

    /* Lower half (user space, entries 0–255): empty. */
    for (int i = 0; i < 256; i++)
        pml4[i] = 0;

    /* Upper half (kernel space, entries 256–511): share with the kernel. */
    for (int i = 256; i < 512; i++)
        pml4[i] = kpml4[i];

    return pml4_phys;
}

/* ── process_init ────────────────────────────────────────────────────── */
void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_table[i].state = PROC_UNUSED;
        proc_table[i].pid   = -1;
    }

    /* pid 0 — the kernel itself.  It keeps the boot page tables. */
    process_t *k = &proc_table[0];
    k->pid         = 0;
    k->ppid        = -1;
    k->state       = PROC_RUNNING;
    k->cr3         = vmm_kernel_pml4();
    k->exit_status = 0;
    k->exited      = false;
    k->thread      = sched_current();
    k->umap_count  = 0;
    for (int i = 0; i < MAX_FDS; i++)
        k->fds[i].type = FD_NONE;
    k->cwd[0] = '/';
    k->cwd[1] = '\0';

    if (k->thread) k->thread->proc = k;

    next_pid = 1;
    kprintf("[PROC] Process table ready. Kernel process = pid 0.\n\r");
}

/* ── process_alloc ───────────────────────────────────────────────────── */
process_t *process_alloc(pid_t_v parent) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state != PROC_UNUSED) continue;

        uint64_t cr3 = address_space_create();
        if (!cr3) return NULL;

        p->pid         = next_pid++;
        p->ppid        = parent;
        p->state       = PROC_EMBRYO;
        p->cr3         = cr3;
        p->exit_status = 0;
        p->exited      = false;
        p->thread      = NULL;
        p->umap_count  = 0;

        /* fd 0/1/2 → console, mirroring stdin/stdout/stderr. */
        for (int f = 0; f < MAX_FDS; f++) {
            p->fds[f].type   = FD_NONE;
            p->fds[f].object = NULL;
            p->fds[f].offset = 0;
        }
        p->fds[0].type = FD_SERIAL;
        p->fds[1].type = FD_SERIAL;
        p->fds[2].type = FD_SERIAL;

        /* every process starts in the root directory */
        p->cwd[0] = '/';
        p->cwd[1] = '\0';

        /* IPC handle table: allocated lazily on first endpoint creation */
        p->ipc_handles = NULL;

        return p;
    }
    return NULL;   /* table full */
}

/* ── process_map_user ────────────────────────────────────────────────── */
void_status_t process_map_user(process_t *p, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!p) return VOID_ERR_INVAL;
    /* Refuse anything outside the user half — the kernel half is shared
     * and must never be writable from Ring 3. */
    if (virt >= 0x0000800000000000ULL) return VOID_ERR_INVAL;
    if (p->umap_count >= 64) return VOID_ERR_NOMEM;

    /* PRESENT and USER are not the caller's choice: a page in the user half
     * is by definition present and Ring-3 reachable.  Callers pass intent
     * (VMM_WRITE, VMM_NX). */
    void_status_t s = vmm_map_page((uint64_t *)p->cr3, virt, phys,
                                   flags | VMM_PRESENT | VMM_USER);
    if (s != VOID_OK) return s;

    p->umap[p->umap_count].virt = virt;
    p->umap[p->umap_count].phys = phys;
    p->umap_count++;
    return VOID_OK;
}

/* ── process_alloc_user_page ─────────────────────────────────────────── */
void_status_t process_alloc_user_page(process_t *p, uint64_t virt, uint64_t flags) {
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return VOID_ERR_NOMEM;

    /* Zero it through the HHDM before it is ever visible to Ring 3, so a
     * new process can't read another process's freed data. */
    uint8_t *z = (uint8_t *)phys_to_virt(frame);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) z[i] = 0;

    void_status_t s = process_map_user(p, virt, frame, flags);
    if (s != VOID_OK) pmm_free_frame(frame);
    return s;
}

/* ── process_destroy_user_space ─────────────────────────────────────────
 * Free the *user* half of a process's address space: every frame it handed
 * out (via umap[]) plus the lower-half page tables (PDPT/PD/PT).  The upper
 * half is shared with the kernel by reference and is never freed here.
 * Leaves `p` with no cr3 (0) and a cleared umap so teardown is idempotent.
 *
 * Shared by process_destroy / process_exit_current (full teardown) and by
 * execve's swap (which then installs a fresh cr3). */
static void process_destroy_user_space(process_t *p) {
    /* Free user frames we handed out. */
    for (uint32_t i = 0; i < p->umap_count; i++) {
        vmm_unmap_page((uint64_t *)p->cr3, p->umap[i].virt);
        pmm_free_frame(p->umap[i].phys);
    }
    p->umap_count = 0;

    /* Free the lower-half page tables (PDPT/PD/PT) this process owns.
     * Upper-half entries are shared with the kernel — never free those. */
    uint64_t *pml4 = phys_to_virt(p->cr3);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt = phys_to_virt(pdpt_phys);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_PRESENT)) continue;
            uint64_t pd_phys = pdpt[j] & 0x000FFFFFFFFFF000ULL;
            uint64_t *pd = phys_to_virt(pd_phys);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT)) continue;
                pmm_free_frame(pd[k] & 0x000FFFFFFFFFF000ULL);  /* PT */
            }
            pmm_free_frame(pd_phys);
        }
        pmm_free_frame(pdpt_phys);
        pml4[i] = 0;
    }

    pmm_free_frame(p->cr3);
    p->cr3 = 0;
}

/* ── process_destroy ─────────────────────────────────────────────────── */
void process_destroy(process_t *p) {
    if (!p || p->state == PROC_UNUSED) return;

    process_destroy_user_space(p);

    /* Drop VFS descriptors so no file object leaks. */
    vfs_close_process_fds(p);

    /* Drop IPC state so no other process can reach a stale endpoint. */
    ipc_endpoint_unregister_by_owner(p->pid);
    ipc_handle_table_destroy(p->ipc_handles);
    p->ipc_handles = NULL;

    /* Drop any service names this process registered — a dead service must
     * never stay discoverable. */
    svc_cleanup_owner(p->pid);

    p->state = PROC_UNUSED;
    p->pid   = -1;
}

/* ── lookups ─────────────────────────────────────────────────────────── */
process_t *process_get(pid_t_v pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid)
            return &proc_table[i];
    return NULL;
}

process_t *process_current(void) {
    thread_t *t = sched_current();
    return t ? t->proc : NULL;
}

process_t *process_kernel(void) { return &proc_table[0]; }

/* ── process_on_switch ────────────────────────────────────────────────
 * The CPU only consults TSS.RSP0 on a privilege *increase*, and SYSCALL
 * doesn't consult anything — so both have to be pointed at the incoming
 * thread's kernel stack before it runs.  Kernel threads never take a
 * privilege transition, so pointing them at their own stack top is
 * harmless and keeps the logic branch-free. */
void process_on_switch(thread_t *t) {
    if (!t || !t->stack_base) return;
    uint64_t ktop = (uint64_t)t->stack_base + t->stack_size;
    gdt_set_kernel_stack(ktop);
    syscall_set_kernel_stack(ktop);
}

/* ── process_spawn_user ──────────────────────────────────────────────── */
pid_t_v process_spawn_user(const void *code, uint64_t code_len, pid_t_v parent) {
    if (!code || code_len == 0) return -1;

    process_t *p = process_alloc(parent);
    if (!p) return -1;

    /* ── code pages: copy the blob in through the HHDM ────────────── */
    uint64_t pages = (code_len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = USER_CODE_BASE + i * PAGE_SIZE;
        if (process_alloc_user_page(p, va, 0) != VOID_OK) {   /* R+X, no write */
            process_destroy(p);
            return -1;
        }
        uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, va);
        uint8_t *dst  = (uint8_t *)phys_to_virt(phys);
        const uint8_t *src = (const uint8_t *)code + i * PAGE_SIZE;

        uint64_t chunk = code_len - i * PAGE_SIZE;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        for (uint64_t b = 0; b < chunk; b++) dst[b] = src[b];
    }

    /* ── user stack ───────────────────────────────────────────────── */
    uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t va = USER_STACK_TOP - USER_STACK_SIZE + i * PAGE_SIZE;
        if (process_alloc_user_page(p, va, VMM_WRITE) != VOID_OK) {
            process_destroy(p);
            return -1;
        }
    }

    /* ── kernel stack for this thread's syscalls and faults ───────── */
    uint64_t kbase;
    uint64_t ktop = sched_alloc_kstack(&kbase);
    if (!ktop) { process_destroy(p); return -1; }

    /* ── initial Ring-3 frame ─────────────────────────────────────────
     * Built on the kernel stack so the first entry into this thread goes
     * through the ordinary IRETQ trailer — the same path a preemption
     * return takes, so there is no special-case "first run" code. */
    isr_frame_t *f = (isr_frame_t *)(ktop - sizeof(isr_frame_t));
    for (uint64_t i = 0; i < sizeof(isr_frame_t) / 8; i++)
        ((uint64_t *)f)[i] = 0;

    f->ss     = GDT_SEL_UDATA3;
    f->rsp    = USER_STACK_TOP - 16;      /* 16-byte aligned, room to spare */
    f->rflags = 0x202;                    /* IF=1; IOPL=0, so no port I/O   */
    f->cs     = GDT_SEL_UCODE3;
    f->rip    = USER_CODE_BASE;
    f->vector = 0;

    thread_t *t = sched_adopt_thread((uint64_t)f, p->cr3, p,
                                     (uint64_t *)kbase, KTHREAD_STACK_SIZE);
    if (!t) { process_destroy(p); return -1; }

    p->thread = t;
    p->state  = PROC_READY;
    return p->pid;
}

/* ── process_spawn_elf ───────────────────────────────────────────────────
 * Like process_spawn_user but loads an ELF64 image instead of a raw blob.
 * Assumes a kernel-resident buffer; validation happens before any mapping. */
pid_t_v process_spawn_elf(const void *image, uint64_t size, pid_t_v parent) {
    if (!image || size == 0) return -1;

    /* Validate before allocating anything: a malformed image must not cost a
     * PID or a page table. */
    elf_loader_t ctx;
    elf_status_t es = elf_validate(image, size, &ctx);
    if (es != ELF_OK) return (pid_t_v)es;

    process_t *p = process_alloc(parent);
    if (!p) return -1;

    uint64_t entry, rsp;
    es = elf_load_into_process(&ctx, p, &entry, &rsp);
    if (es != ELF_OK) {
        process_destroy(p);
        return (pid_t_v)es;
    }

    uint64_t kbase;
    uint64_t ktop = sched_alloc_kstack(&kbase);
    if (!ktop) { process_destroy(p); return (pid_t_v)ELF_ERR_NOMEM; }

    /* Same initial frame shape as the blob path — only RIP and RSP differ,
     * and both come from the loader rather than being fixed constants. */
    isr_frame_t *f = (isr_frame_t *)(ktop - sizeof(isr_frame_t));
    for (uint64_t i = 0; i < sizeof(isr_frame_t) / 8; i++)
        ((uint64_t *)f)[i] = 0;

    f->ss     = GDT_SEL_UDATA3;
    f->rsp    = rsp;
    f->rflags = 0x202;                    /* IF=1; IOPL=0, so no port I/O   */
    f->cs     = GDT_SEL_UCODE3;
    f->rip    = entry;
    f->vector = 0;

    thread_t *t = sched_adopt_thread((uint64_t)f, p->cr3, p,
                                     (uint64_t *)kbase, KTHREAD_STACK_SIZE);
    if (!t) { process_destroy(p); return (pid_t_v)ELF_ERR_NOMEM; }

    p->thread = t;
    p->state  = PROC_READY;
    return p->pid;
}

/* ── process_fork_current ───────────────────────────────────────────────
 * Eager copy-on-fork: the child gets a private copy of every user page the
 * parent has mapped.  Void has no COW machinery (no user-page-fault handler,
 * no frame refcounts), so a shared mapping would be owned by two processes
 * that both free it on exit — double-free.  Correct eager copy first;
 * ponytail: real COW when uPF + frame refcounts land.
 *
 * The frame is duplicated byte-for-byte (SYSV regs, user RSP, RFLAGS are the
 * continuation point), then the child's RAX is forced to 0 so the child sees
 * fork()==0 while the parent sees the child's PID. */
isr_frame_t *process_fork_current(isr_frame_t *frame) {
    process_t *p = process_current();
    if (!p || p->pid == 0) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_PERM;
        return frame;
    }
    if (p->state == PROC_ZOMBIE || p->state == PROC_BLOCKED) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_PERM;
        return frame;
    }

    /* Fresh PID + address space; parent for the child is *us*. */
    process_t *c = process_alloc(p->pid);
    if (!c) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM;
        return frame;
    }

    /* ── eager page copy ─────────────────────────────────────────────── */
    for (uint32_t i = 0; i < p->umap_count; i++) {
        uint64_t virt = p->umap[i].virt;
        uint64_t phys = p->umap[i].phys;

        /* The flags a fork must replicate are read from the *parent's* PTE,
         * not from any stored intent: PRESENT/USER are implied, but WRITE and
         * NX must survive the copy so the child honours read-only and
         * no-execute the same way. */
        uint64_t pte = vmm_get_pte((uint64_t *)p->cr3, virt);
        uint64_t flags = 0;
        if (pte & VMM_WRITE) flags |= VMM_WRITE;
        if (pte & VMM_NX)    flags |= VMM_NX;

        /* New frame, zeroed by the allocator; copy the data in through the
         * HHDM so we never touch the user VA directly. */
        uint64_t cphys = pmm_alloc_frame();
        if (!cphys) { process_destroy(c); if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM; return frame; }

        uint8_t *src = (uint8_t *)(phys  + g_boot.hhdm_offset);
        uint8_t *dst = (uint8_t *)(cphys + g_boot.hhdm_offset);
        for (uint64_t b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];

        if (process_map_user(c, virt, cphys, flags) != VOID_OK) {
            pmm_free_frame(cphys);
            process_destroy(c);
            if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM;
            return frame;
        }
    }

    /* ── IPC handle table: child inherits a *copy* of the entries.  Each
     * entry points at an endpoint owned by a specific pid (probably the
     * parent), so the child sharing the target is fine; when either process
     * exits, ipc_endpoint_unregister_by_owner only frees that pid's own
     * endpoints — no refcounting, no cross-process ownership ambiguity. */
    if (p->ipc_handles) {
        c->ipc_handles = ipc_handle_table_create();
        if (!c->ipc_handles) { process_destroy(c); if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM; return frame; }
        for (int i = 0; i < IPC_MAX_HANDLES; i++) {
            c->ipc_handles->handles[i] = p->ipc_handles->handles[i];
        }
        /* endpoint ids are handle numbers now, so the child's re-created
         * endpoints (ids) line up with its own handle array.  No counter. */
    } else {
        c->ipc_handles = NULL;
    }

    /* ── dup the fd table ──────────────────────────────────────────────
     * For FD_VFS the `object` is a shared heap vfs_file_t: the child holds
     * a second reference to the *same* open file (POSIX dup semantics), so
     * bump its refcount — close in either process must not free the object
     * while the other still has it open. */
    for (int f = 0; f < MAX_FDS; f++) {
        c->fds[f] = p->fds[f];
        if (c->fds[f].type == FD_VFS && c->fds[f].object)
            vfs_file_ref_inc(c->fds[f].object);
    }

    /* ── thread: fresh kernel stack + copy of the frame ──────────────── */
    uint64_t kbase;
    uint64_t ktop = sched_alloc_kstack(&kbase);
    if (!ktop) { process_destroy(c); if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM; return frame; }

    isr_frame_t *cf = (isr_frame_t *)(ktop - sizeof(isr_frame_t));
    uint64_t     asz = sizeof(isr_frame_t) / 8;
    for (uint64_t i = 0; i < asz; i++) ((uint64_t *)cf)[i] = ((uint64_t *)frame)[i];

    cf->rax = 0;   /* child observes fork() == 0 */

    thread_t *t = sched_adopt_thread((uint64_t)cf, c->cr3, c,
                                     (uint64_t *)kbase, KTHREAD_STACK_SIZE);
    if (!t) { process_destroy(c); if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM; return frame; }

    c->thread = t;
    c->state  = PROC_READY;

    kprintf("[PROC] fork: pid %u -> pid %u\n\r",
            (uint64_t)p->pid, (uint64_t)c->pid);

    if (frame) frame->rax = (uint64_t)(int64_t)c->pid;
    return frame;
}

/* ── process_execve_current ─────────────────────────────────────────────
 * User classifies the path; the kernel sees only a name and replaces the
 * whole user address space with the freshly validated ELF image.
 *
 * PID, the fd table, and the IPC handle table all survive.  The swap is
 * atomic: the new image is validated and loaded into a *scratch* address
 * space first, so a malformed image or an ENOMEM mid-load leaves the old
 * image untouched and running.  Only after the scratch is fully built does
 * the process switch CR3 and release the old lower half.
 *
 * The frame is rewritten in place (RIP/RSP become the new entry/stack) so
 * syscall_exit finds it later; the thread's kernel stack and CR3 (now the
 * new PML4) are updated to match.
 *
 * Returns the frame.  RAX holds 0 on success, or -errno (the loader's
 * elf_status_t already is -errno shaped). */
isr_frame_t *process_execve_current(isr_frame_t *frame, uint64_t upath) {
    process_t *p = process_current();
    if (!p || p->pid == 0) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_PERM;
        return frame;
    }
    if (p->state == PROC_ZOMBIE) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_PERM;
        return frame;
    }

    /* ── fetch the path: it is short, so a bounded kernel buffer is enough.
     * The name is validated character-wise (a user could hand us 256 'A's).
     * We deliberately DON'T trust the user string further than a fixed
     * prefix: the embedded table only imagines short names. */
    char name[32];
    {
        /* copy_from_user validates the whole range against the caller's PTEs
         * and refuses non-present / non-user / non-writable pages, and won't
         * cross the lower-half limit.  NULL or a bad pointer → EFAULT. */
        if (!copy_from_user(p, name, upath, sizeof(name) - 1)) {
            if (frame) frame->rax = (uint64_t)(int64_t)-VE_FAULT;
            return frame;
        }
        name[sizeof(name) - 1] = '\0';
    }

    /* ── resolve the name to an embedded image ───────────────────────── */
    elf_blob_t blob;
    elf_status_t es = elf_find_embedded(name, &blob);
    if (es != ELF_OK) {
        if (frame) frame->rax = (uint64_t)(int64_t)es;
        return frame;
    }

    /* Validate before touching anything: a malformed blob is rejected with
     * the process intact. */
    elf_loader_t ctx;
    es = elf_validate(blob.base, blob.size, &ctx);
    if (es != ELF_OK) {
        if (frame) frame->rax = (uint64_t)(int64_t)es;
        return frame;
    }

    /* ── scratch address space: new PML4 sharing the kernel upper half ──
     * Loading into scratch means a failed load (NOMEM) never corrupts the
     * live image.  We build a small transient `process_t` purely to hold the
     * umap[] bookkeeping the loader writes through process_map_user(). */
    uint64_t new_cr3 = address_space_create();
    if (!new_cr3) {
        if (frame) frame->rax = (uint64_t)(int64_t)-VE_NOMEM;
        return frame;
    }

    process_t scratch = {0};
    scratch.cr3 = new_cr3;
    scratch.umap_count = 0;

    uint64_t entry, rsp;
    es = elf_load_into_process(&ctx, &scratch, &entry, &rsp);
    if (es != ELF_OK) {
        process_destroy_user_space(&scratch);   /* no upper half to preserve */
        if (frame) frame->rax = (uint64_t)(int64_t)es;
        return frame;
    }

    /* ── atomically swap the address space ───────────────────────────── */
    /* Tear down the old user half; the new one slides in underneath the
     * running thread.  The kernel upper half is shared by reference, so only
     * the lower half is touched — exactly what process_destroy does. */
    process_destroy_user_space(p);

    /* Install the new space.  umap[] is copied from scratch so future teardown
     * (exit, a second exec) frees the new pages. */
    p->cr3 = new_cr3;
    p->umap_count = scratch.umap_count;
    for (uint32_t i = 0; i < scratch.umap_count; i++)
        p->umap[i] = scratch.umap[i];
    p->thread->cr3 = new_cr3;   /* scheduler switches to this CR3 next tick */

    /* Load the new page tables NOW.  Just updating thread->cr3 is not enough:
     * the CPU register still points at the old (now freed) PML4, and the
     * scheduler only reloads CR3 when it actually switches to a thread in a
     * *different* address space — if we're rescheduled alone, user code would
     * run on the freed tables and fault.  Safe here: the new PML4 shares the
     * kernel upper half by reference, so this higher-half code stays mapped
     * across the load. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    /* New entry + stack in the *current* frame; nothing else about the
     * hardware context (GPRs, RFLAGS) changes — PID/fds/IPC all survive. */
    frame->rip = entry;
    frame->rsp = rsp;
    frame->rax = 0;             /* sys_execve returns 0 on success */

    kprintf("[PROC] pid %u exec: %s -> entry 0x%x, rsp 0x%x\n\r",
            (uint64_t)p->pid, name, (uint64_t)entry, (uint64_t)rsp);

    return frame;
}

/* ── process_exit_current ────────────────────────────────────────────── */
isr_frame_t *process_exit_current(isr_frame_t *frame, int32_t status) {
    process_t *p = process_current();
    thread_t  *t = sched_current();

    if (!p || p->pid == 0) {
        /* pid 0 exiting would take the kernel with it. */
        kprintf("[PROC] refusing exit from kernel process\n\r");
        frame->rax = (uint64_t)(int64_t)-1;
        return frame;
    }

    p->exit_status = status;
    p->exited      = true;
    p->state       = PROC_ZOMBIE;

    /* Orphaned children must not dangle: reparent every child of ours to our
     * parent.  The chain always terminates at pid 0 (the immortal kernel
     * process), so no zombie ever becomes unreachable by a reaper. */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *c = &proc_table[i];
        if (c->state != PROC_UNUSED && c->ppid == p->pid)
            c->ppid = p->ppid;
    }

    /* Release user memory now; the PCB stays until the parent reaps it so
     * the status is still readable.  Page tables go with it, so this
     * thread must never run again — hence THREAD_EXITED below. */
    for (uint32_t i = 0; i < p->umap_count; i++) {
        vmm_unmap_page((uint64_t *)p->cr3, p->umap[i].virt);
        pmm_free_frame(p->umap[i].phys);
    }
    p->umap_count = 0;

    /* VFS descriptors die with the process so no file object leaks — each
     * open handle is a heap allocation that must be returned. */
    vfs_close_process_fds(p);

    /* Endpoints die with the process, not at reap time: a zombie must not
     * keep accepting messages nobody will ever read.  So do the service
     * names pointing at them. */
    ipc_endpoint_unregister_by_owner(p->pid);
    svc_cleanup_owner(p->pid);

    /* Wake a parent blocked in wait(). */
    process_t *parent = process_get(p->ppid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_READY;
        if (parent->thread) parent->thread->state = THREAD_READY;
    }

    kprintf("[PROC] pid %u exited with status %u\n\r",
            (uint64_t)p->pid, (uint64_t)(uint32_t)status);

    t->state = THREAD_EXITED;
    return sched_tick(frame);   /* switch away for good */
}

/* ── process_try_reap ─────────────────────────────────────────────────
 * The non-blocking core shared by sys_wait4 and kernel-side waiters.
 * Returns the reaped pid, 0 if a child exists but is still running,
 * or -VE_CHILD when there is no matching child. */
pid_t_v process_try_reap(pid_t_v parent, pid_t_v pid, int32_t *status) {
    bool any_child = false;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *c = &proc_table[i];
        if (c->state == PROC_UNUSED)    continue;
        if (c->ppid != parent)          continue;
        if (pid != -1 && c->pid != pid) continue;
        any_child = true;

        if (c->state != PROC_ZOMBIE) continue;

        pid_t_v reaped = c->pid;
        if (status) *status = c->exit_status;
        process_destroy(c);
        return reaped;
    }

    return any_child ? 0 : -VE_CHILD;
}

/* ── process_wait_current ────────────────────────────────────────────── */
isr_frame_t *process_wait_current(isr_frame_t *frame, pid_t_v pid, uint64_t ustatus) {
    process_t *p = process_current();
    if (!p) { frame->rax = (uint64_t)(int64_t)-VE_PERM; return frame; }

    int32_t st = 0;
    pid_t_v r = process_try_reap(p->pid, pid, &st);

    if (r > 0) {
        if (ustatus) {
            /* Best-effort: a bad status pointer must not cost the caller the
             * child it already reaped, so drop the write instead of failing. */
            uint64_t pte = vmm_get_pte((uint64_t *)p->cr3, ustatus);
            if ((pte & (VMM_PRESENT | VMM_USER | VMM_WRITE))
                    == (VMM_PRESENT | VMM_USER | VMM_WRITE)) {
                uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, ustatus);
                if (phys) *(int32_t *)phys_to_virt(phys) = st;
            }
        }
        frame->rax = (uint64_t)(int64_t)r;
        return frame;
    }

    if (r < 0) {                       /* no such child */
        frame->rax = (uint64_t)(int64_t)r;
        return frame;
    }

    /* A child exists but hasn't exited.  Block; the exiting child marks us
     * READY again, and we re-enter here to retry the reap.
     * ponytail: caller must loop on a 0 return (SA_RESTART-style); real
     * blocking-until-reapable arrives with proper wait queues. */
    p->state = PROC_BLOCKED;
    thread_t *t = sched_current();
    t->state = THREAD_BLOCKED;

    isr_frame_t *next = sched_tick(frame);
    if (next != frame) {
        /* We switched away. When rescheduled, retry from the top: leave
         * RAX as 0 so userspace loops on wait4 again. */
        frame->rax = 0;
        return next;
    }

    /* sched_tick declined to switch — undo the block rather than wedge. */
    p->state = PROC_RUNNING;
    t->state = THREAD_RUNNING;
    frame->rax = 0;
    return frame;
}
