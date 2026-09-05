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
#include <arch/x86_64/gdt.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <void/boot.h>
#include <dev/serial.h>

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

/* ── process_destroy ─────────────────────────────────────────────────── */
void process_destroy(process_t *p) {
    if (!p || p->state == PROC_UNUSED) return;

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
    p->cr3   = 0;
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

    /* Release user memory now; the PCB stays until the parent reaps it so
     * the status is still readable.  Page tables go with it, so this
     * thread must never run again — hence THREAD_EXITED below. */
    for (uint32_t i = 0; i < p->umap_count; i++) {
        vmm_unmap_page((uint64_t *)p->cr3, p->umap[i].virt);
        pmm_free_frame(p->umap[i].phys);
    }
    p->umap_count = 0;

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
