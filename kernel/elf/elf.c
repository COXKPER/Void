/* VoidOS — ELF64 executable loader
 *
 * Two phases, deliberately separate:
 *
 *   elf_validate()          pure inspection of a kernel-resident buffer.
 *                           Touches no process state, so a malformed image
 *                           is rejected before anything has been mapped.
 *   elf_load_into_process()  maps and populates an address space.
 *
 * The image always lives in kernel memory (embedded blob today, a VFS read
 * later), never in user memory — so the loader never dereferences a user
 * pointer, and every access is bounded by `image_size`.
 *
 * Segment contents are written through the HHDM alias of the freshly
 * allocated frame, not through the user virtual address.  That is what lets
 * a read-only segment be populated without ever mapping it writable: the
 * kernel writes via a different mapping of the same physical page, so there
 * is no "map RW then downgrade" window.
 *
 * Written against the System V AMD64 ABI and the ELF64 gABI.  Keira
 * (github.com/mrvlous/keira, GPL-2.0) was read as one of several design
 * references; no code was taken from it — see the Phase 4 provenance note
 * in CLAUDE.md.
 */
#include <elf/elf.h>
#include <proc/process.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <void/boot.h>
#include <dev/serial.h>

/* Lower-half canonical user range.  The bottom 64 KiB is left unmapped so a
 * NULL dereference faults instead of hitting a real page. */
#define USER_MIN_VADDR  0x0000000000010000ULL
#define USER_MAX_VADDR  0x0000800000000000ULL   /* exclusive */

/* Cap concurrently loadable segments: bounds the O(n²) overlap check and the
 * on-stack range table.  Real static executables use 2–4. */
#define ELF_MAX_LOADS   16

#define IA32_EFER       0xC0000080
#define EFER_NXE        (1ULL << 11)

#define PAGE_MASK       (~(PAGE_SIZE - 1))

static bool nx_ok;

/* ── NX enable ───────────────────────────────────────────────────────────
 * PTE bit 63 is *reserved* unless EFER.NXE is set — setting it without NXE
 * turns every mapping into a reserved-bit page fault.  So NX support is
 * probed once and VMM_NX is only ever emitted when this succeeded. */
void elf_init(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(0x80000000U));
    if (eax >= 0x80000001U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                  : "a"(0x80000001U));
        if (edx & (1U << 20)) {                 /* NX/XD supported */
            uint32_t lo, hi;
            __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_EFER));
            uint64_t efer = ((uint64_t)hi << 32) | lo;
            efer |= EFER_NXE;
            __asm__ volatile ("wrmsr" : : "c"(IA32_EFER),
                              "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)));
            nx_ok = true;
        }
    }

    kprintf("[ELF] Loader ready. NX/XD %s.\n\r",
            nx_ok ? "enabled" : "unavailable (segments stay executable)");
}

bool elf_nx_enabled(void) { return nx_ok; }

/* ── embedded image table ────────────────────────────────────────────────
 * There is no VFS yet: every executable the kernel can run is a fixed name
 * built into .rodata by elf_blobs.asm.  execve() resolves the user's path
 * against this table; a real filesystem replaces it wholesale later. */
elf_status_t elf_find_embedded(const char *name, const elf_blob_t *out) {
    static const struct { const char *name;
                          const uint8_t *start, *end; } blobs[] = {
        { "init",       elf_init_start,          elf_init_end          },
        { "elf_test",   elf_test_start,          elf_test_end          },
        { "ipc_test",   elf_ipc_test_start,      elf_ipc_test_end      },
        { "fork_test",  elf_forkexec_test_start, elf_forkexec_test_end },
        { "calc",       elf_calc_start,          elf_calc_end          },
        { "srv_test",   elf_srv_test_start,      elf_srv_test_end      },
        { "lifecycle",  elf_lifecycle_test_start, elf_lifecycle_test_end },
    };

    if (!name || !out) return ELF_ERR_INVAL;

    for (uint64_t i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
        const char *a = name, *b = blobs[i].name;
        int diff = 0;
        do { diff = *a - *b; if (*a) a++; if (*b) b++; } while (diff == 0 && *a && *b);
        if (diff == 0) {
            ((elf_blob_t *)out)->base = blobs[i].start;
            ((elf_blob_t *)out)->size = (uint64_t)(blobs[i].end - blobs[i].start);
            return ELF_OK;
        }
    }
    return ELF_ERR_NOEXEC;
}

/* ── program header accessor ─────────────────────────────────────────────
 * Only ever called after elf_validate() has confirmed the whole table lies
 * inside the image, so the arithmetic here cannot leave the buffer. */
static const Elf64_Phdr *phdr_at(const Elf64_Ehdr *eh, uint16_t i) {
    return (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff
                                + (uint64_t)i * sizeof(Elf64_Phdr));
}

/* ── elf_validate ────────────────────────────────────────────────────── */
elf_status_t elf_validate(const void *image, uint64_t size, elf_loader_t *ctx) {
    if (!image || !ctx) return ELF_ERR_INVAL;

    /* Every field read below lives inside the fixed-size header, so this one
     * check licenses all of them. */
    if (size < sizeof(Elf64_Ehdr)) return ELF_ERR_NOEXEC;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;

    /* ── identity ───────────────────────────────────────────────────── */
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        return ELF_ERR_NOEXEC;
    if (eh->e_ident[EI_CLASS]   != ELFCLASS64)  return ELF_ERR_NOEXEC;
    if (eh->e_ident[EI_DATA]    != ELFDATA2LSB) return ELF_ERR_NOEXEC;
    if (eh->e_ident[EI_VERSION] != EV_CURRENT)  return ELF_ERR_NOEXEC;

    /* ── header self-consistency ────────────────────────────────────── */
    if (eh->e_type    != ET_EXEC)             return ELF_ERR_NOEXEC;
    if (eh->e_machine != EM_X86_64)           return ELF_ERR_NOEXEC;
    if (eh->e_version != EV_CURRENT)          return ELF_ERR_NOEXEC;
    if (eh->e_ehsize  != sizeof(Elf64_Ehdr))  return ELF_ERR_NOEXEC;

    /* Exact size, not a minimum: an oversized e_phentsize would make the
     * stride skip past real header bytes into arbitrary file data. */
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return ELF_ERR_NOEXEC;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM) return ELF_ERR_NOEXEC;

    /* Table must lie wholly inside the image.  Subtracting only after the
     * bound check keeps this overflow-free; the product is bounded by
     * ELF_MAX_PHNUM * 56 and cannot wrap. */
    uint64_t tbl_bytes = (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr);
    if (eh->e_phoff > size)               return ELF_ERR_NOEXEC;
    if (tbl_bytes > size - eh->e_phoff)   return ELF_ERR_NOEXEC;

    /* ── program headers ────────────────────────────────────────────── */
    struct { uint64_t start, end; } seen[ELF_MAX_LOADS];
    uint32_t nloads = 0;
    bool entry_ok = false;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD) continue;   /* PT_NOTE/PT_GNU_* etc: ignored */
        if (ph->p_memsz == 0)      continue;   /* nothing to map */

        if (nloads >= ELF_MAX_LOADS) return ELF_ERR_NOEXEC;

        /* file range */
        if (ph->p_filesz > ph->p_memsz)          return ELF_ERR_NOEXEC;
        if (ph->p_offset > size)                 return ELF_ERR_NOEXEC;
        if (ph->p_filesz > size - ph->p_offset)  return ELF_ERR_NOEXEC;

        /* virtual range: wrap first, then bounds.  Checking vend against
         * USER_MAX_VADDR rejects kernel-half and non-canonical targets in
         * one comparison, since everything at or above that limit is one or
         * the other. */
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return ELF_ERR_NOEXEC;
        uint64_t vend = ph->p_vaddr + ph->p_memsz;
        if (ph->p_vaddr < USER_MIN_VADDR) return ELF_ERR_NOEXEC;
        if (vend > USER_MAX_VADDR)        return ELF_ERR_NOEXEC;

        /* permissions: refuse write+execute outright rather than mapping a
         * page Ring 3 could rewrite and then run. */
        if (!(ph->p_flags & PF_R))                          return ELF_ERR_NOEXEC;
        if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X))    return ELF_ERR_NOEXEC;

        /* p_align: must be a power of two, and the gABI congruence
         * p_offset ≡ p_vaddr (mod p_align) must hold.  That congruence is
         * exactly what makes an intra-page copy offset well defined. */
        if (ph->p_align > 1) {
            if (ph->p_align & (ph->p_align - 1)) return ELF_ERR_NOEXEC;
            if ((ph->p_vaddr  & (ph->p_align - 1)) !=
                (ph->p_offset & (ph->p_align - 1)))
                return ELF_ERR_NOEXEC;
        }

        /* must not collide with the stack this loader is about to map */
        if (ph->p_vaddr < USER_STACK_TOP &&
            vend > USER_STACK_TOP - USER_STACK_SIZE)
            return ELF_ERR_NOEXEC;

        /* Byte ranges must not overlap each other.  Sharing a *page* at a
         * boundary is fine and handled at load time; overlapping bytes would
         * make one segment's BSS clobber another's contents. */
        for (uint32_t j = 0; j < nloads; j++)
            if (ph->p_vaddr < seen[j].end && vend > seen[j].start)
                return ELF_ERR_NOEXEC;

        seen[nloads].start = ph->p_vaddr;
        seen[nloads].end   = vend;
        nloads++;

        if ((ph->p_flags & PF_X) &&
            eh->e_entry >= ph->p_vaddr && eh->e_entry < vend)
            entry_ok = true;
    }

    if (nloads == 0) return ELF_ERR_NOEXEC;
    if (!entry_ok)   return ELF_ERR_NOEXEC;   /* never jump outside code */

    ctx->image      = (const uint8_t *)image;
    ctx->image_size = size;
    ctx->ehdr       = eh;
    ctx->entry      = eh->e_entry;
    return ELF_OK;
}

/* ── page flag derivation ────────────────────────────────────────────────
 * PRESENT and USER are added by process_map_user(); this contributes only
 * intent.  x86 paging has no read-disable, so PF_R needs no bit. */
static uint64_t seg_page_flags(uint32_t p_flags) {
    uint64_t f = 0;
    if (p_flags & PF_W)  f |= VMM_WRITE;
    if (!(p_flags & PF_X) && nx_ok) f |= VMM_NX;
    return f;
}

/* ── map one segment's pages ─────────────────────────────────────────────
 * A page may already be present because an adjacent segment started inside
 * it.  In that case the frame is reused and permissions are unioned — and if
 * the union would be simultaneously writable and executable, the binary is
 * refused instead of silently producing a W+X page. */
static elf_status_t map_segment_pages(process_t *p, const Elf64_Phdr *ph) {
    uint64_t flags = seg_page_flags(ph->p_flags);
    uint64_t first = ph->p_vaddr & PAGE_MASK;
    uint64_t last  = (ph->p_vaddr + ph->p_memsz - 1) & PAGE_MASK;

    for (uint64_t va = first; va <= last; va += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte((uint64_t *)p->cr3, va);

        if (!(pte & VMM_PRESENT)) {
            /* Fresh frame, zeroed by the allocator — which is what makes the
             * BSS tail of the last file page correct with no extra work. */
            if (process_alloc_user_page(p, va, flags) != VOID_OK)
                return ELF_ERR_NOMEM;
            continue;
        }

        uint64_t merged = pte & ~(VMM_WRITE | VMM_NX);
        if ((pte & VMM_WRITE) || (flags & VMM_WRITE)) merged |= VMM_WRITE;
        if ((pte & VMM_NX)    && (flags & VMM_NX))    merged |= VMM_NX;

        if (nx_ok && (merged & VMM_WRITE) && !(merged & VMM_NX))
            return ELF_ERR_NOEXEC;      /* shared page would be W+X */

        if (vmm_map_page((uint64_t *)p->cr3, va, pte & 0x000FFFFFFFFFF000ULL,
                         merged | VMM_PRESENT | VMM_USER) != VOID_OK)
            return ELF_ERR_NOMEM;
    }
    return ELF_OK;
}

/* ── copy one segment's file bytes ───────────────────────────────────────
 * Writes through the HHDM alias, page by page.  vmm_virt_to_phys() already
 * folds the intra-page offset into its result, so an unaligned p_vaddr needs
 * no special case here — only the chunk size does. */
static elf_status_t copy_segment_data(process_t *p, const elf_loader_t *ctx,
                                      const Elf64_Phdr *ph) {
    uint64_t done = 0;
    while (done < ph->p_filesz) {
        uint64_t va    = ph->p_vaddr + done;
        uint64_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > ph->p_filesz - done) chunk = ph->p_filesz - done;

        uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, va);
        if (!phys) return ELF_ERR_NOMEM;

        uint8_t       *dst = (uint8_t *)(phys + g_boot.hhdm_offset);
        const uint8_t *src = ctx->image + ph->p_offset + done;
        for (uint64_t i = 0; i < chunk; i++) dst[i] = src[i];
        done += chunk;
    }
    return ELF_OK;
}

/* ── initial user stack ──────────────────────────────────────────────────
 * Minimal but already System V shaped: RSP is 16-byte aligned and points at
 * argc, with null argv/envp terminators and an AT_NULL auxv entry above it.
 * A future execve() fills these in place rather than reshaping the frame. */
static elf_status_t setup_user_stack(process_t *p, uint64_t *out_rsp) {
    uint64_t flags = VMM_WRITE | (nx_ok ? VMM_NX : 0);

    for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE)
        if (process_alloc_user_page(p, USER_STACK_TOP - USER_STACK_SIZE + off,
                                    flags) != VOID_OK)
            return ELF_ERR_NOMEM;

    uint64_t rsp = (USER_STACK_TOP - 32) & ~0xFULL;
    uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, rsp);
    if (!phys) return ELF_ERR_NOMEM;

    /* argc=0, argv[0]=NULL, envp[0]=NULL, auxv AT_NULL — 32 bytes, and the
     * stack pages are already zeroed, so this is belt-and-braces. */
    uint64_t *slot = (uint64_t *)(phys + g_boot.hhdm_offset);
    slot[0] = 0; slot[1] = 0; slot[2] = 0; slot[3] = 0;

    *out_rsp = rsp;
    return ELF_OK;
}

/* ── elf_load_into_process ───────────────────────────────────────────── */
elf_status_t elf_load_into_process(const elf_loader_t *ctx, process_t *p,
                                   uint64_t *out_entry, uint64_t *out_rsp) {
    if (!ctx || !ctx->ehdr || !p || !out_entry || !out_rsp) return ELF_ERR_INVAL;

    const Elf64_Ehdr *eh = ctx->ehdr;

    /* Map every segment before copying anything: a NOMEM halfway through
     * then leaves a uniformly empty-but-mapped space for the caller to tear
     * down, rather than a half-written one. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        elf_status_t s = map_segment_pages(p, ph);
        if (s != ELF_OK) return s;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        elf_status_t s = copy_segment_data(p, ctx, ph);
        if (s != ELF_OK) return s;
    }

    elf_status_t s = setup_user_stack(p, out_rsp);
    if (s != ELF_OK) return s;

    *out_entry = ctx->entry;
    return ELF_OK;
}
