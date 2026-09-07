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
        { "vfs_test",   elf_vfs_test_start,      elf_vfs_test_end      },
        { "mm_test",    elf_mm_test_start,       elf_mm_test_end       },
        { "malloc_test", elf_malloc_test_start,   elf_malloc_test_end   },
        { "ld-void.so", elf_ld_void_so_start,     elf_ld_void_so_end    },
        { "dynamic_test", elf_dynamic_test_start, elf_dynamic_test_end },
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

/* ── image top (for the initial heap break) ──────────────────────────────
 * Highest byte offset (vaddr + memsz, biased by the DYN base) over every
 * PT_LOAD segment.  Callers page-align this to place the initial brk just
 * past the image.  Runs after elf_validate(), so every field access is
 * already bounds-checked. */
uint64_t elf_load_end(const elf_loader_t *ctx) {
    if (!ctx || !ctx->ehdr) return 0;
    const Elf64_Ehdr *eh = ctx->ehdr;
    uint64_t end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint64_t e = ph->p_vaddr + ctx->base + ph->p_memsz;
        if (e > end) end = e;
    }
    return end;
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
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return ELF_ERR_NOEXEC;
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

    /* ET_DYN: fixed deterministic base (no ASLR), above existing tests,
     * below the stack.  Pre-bias addressing is relative to this. */
    uint64_t base = (eh->e_type == ET_DYN) ? ELF_DYN_BASE : 0;

    /* PT_INTERP: the bytes are the interpreter path (ld.so).  Track the
     * valid bounds so the loader can run the interp instead of the binary. */
    char     interp_path[64];
    uint64_t interp_off = 0, interp_len = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);

        if (ph->p_type == PT_INTERP) {
            /* Interpreter path: a NUL-terminated string inside the file. */
            if (ph->p_filesz == 0 || ph->p_offset >= size) return ELF_ERR_NOEXEC;
            if (ph->p_filesz >= sizeof(interp_path))       return ELF_ERR_NOEXEC;
            if (ph->p_offset + ph->p_filesz > size)        return ELF_ERR_NOEXEC;
            interp_off = ph->p_offset;
            interp_len = ph->p_filesz;
            continue;
        }

        if (ph->p_type == PT_DYNAMIC) continue;   /* consumed by rtld */

        if (ph->p_type != PT_LOAD) continue;   /* PT_NOTE/PT_GNU_* etc: ignored */
        if (ph->p_memsz == 0)      continue;   /* nothing to map */

        if (nloads >= ELF_MAX_LOADS) return ELF_ERR_NOEXEC;

        /* file range */
        if (ph->p_filesz > ph->p_memsz)          return ELF_ERR_NOEXEC;
        if (ph->p_offset > size)                 return ELF_ERR_NOEXEC;
        if (ph->p_filesz > size - ph->p_offset)  return ELF_ERR_NOEXEC;

        /* virtual range: wrap first, then bounds.  For ET_DYN the p_vaddr is
         * relative — the load base makes it the real user address.  Checking
         * the biased vend against USER_MAX_VADDR rejects kernel-half and
         * non-canonical targets in one comparison. */
        uint64_t vaddr = ph->p_vaddr + base;
        if (vaddr < ph->p_vaddr) return ELF_ERR_NOEXEC;             /* wrapped */
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return ELF_ERR_NOEXEC;
        uint64_t vend = vaddr + ph->p_memsz;
        if (vaddr < USER_MIN_VADDR) return ELF_ERR_NOEXEC;
        if (vend > USER_MAX_VADDR)  return ELF_ERR_NOEXEC;

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
        if (vaddr < USER_STACK_TOP &&
            vend > USER_STACK_TOP - USER_STACK_SIZE)
            return ELF_ERR_NOEXEC;

        /* Byte ranges must not overlap each other.  Sharing a *page* at a
         * boundary is fine and handled at load time; overlapping bytes would
         * make one segment's BSS clobber another's contents. */
        for (uint32_t j = 0; j < nloads; j++)
            if (vaddr < seen[j].end && vend > seen[j].start)
                return ELF_ERR_NOEXEC;

        seen[nloads].start = vaddr;
        seen[nloads].end   = vend;
        nloads++;

        if ((ph->p_flags & PF_X) &&
            eh->e_entry >= ph->p_vaddr && eh->e_entry < ph->p_vaddr + ph->p_memsz)
            entry_ok = true;
    }

    if (nloads == 0) return ELF_ERR_NOEXEC;
    if (!entry_ok)   return ELF_ERR_NOEXEC;   /* never jump outside code */

    /* If the binary asked for an interpreter, copy its path (already range
     * checked above) so the loader can hand control to ld.so. */
    if (interp_len) {
        if (interp_len >= sizeof(ctx->interp)) return ELF_ERR_NOEXEC;
        for (uint64_t i = 0; i < interp_len; i++)
            ctx->interp[i] = (char)((const uint8_t *)image)[interp_off + i];
        ctx->interp[interp_len] = '\0';
        ctx->has_interp = true;
    } else {
        ctx->has_interp = false;
    }

    ctx->image      = (const uint8_t *)image;
    ctx->image_size = size;
    ctx->ehdr       = eh;
    ctx->base       = base;
    ctx->entry      = eh->e_entry;   /* pre-bias for ET_DYN; loader biases */
    /* PT_PHDR address for the auxv: the program-header table's runtime VA.
     * e_phoff is a file offset; for an ELF loaded at `base` with a 4 KiB-
     * aligned p_align it equals base + e_phoff.  For ET_EXEC there is no
     * base, so this is just e_phoff (a lower-half VA from the link). */
    ctx->phdr       = eh->e_phoff + base;
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
 * refused instead of silently producing a W+X page. `base` biases the
 * (relative) p_vaddr into the real user address. */
static elf_status_t map_segment_pages(process_t *p, const Elf64_Phdr *ph,
                                      uint64_t base) {
    uint64_t flags = seg_page_flags(ph->p_flags);
    uint64_t first = (ph->p_vaddr + base) & PAGE_MASK;
    uint64_t last  = (ph->p_vaddr + base + ph->p_memsz - 1) & PAGE_MASK;

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
 * no special case here — only the chunk size does.  `base` biases the
 * (relative) p_vaddr. */
static elf_status_t copy_segment_data(process_t *p, const elf_loader_t *ctx,
                                      const Elf64_Phdr *ph, uint64_t base) {
    uint64_t done = 0;
    while (done < ph->p_filesz) {
        uint64_t va    = ph->p_vaddr + base + done;
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
 * Full System V AMD64 initial frame, seeded word-by-word through the HHDM:
 *
 *   [ argc ] [ argv[0] ] [ NULL ] [ envp=NULL ] [ auxv AT_* pairs ] [ AT_NULL ]
 *
 * argv[0] is a NUL-terminated string written just below the word frame (the
 * classic "strings live at the bottom of the initial stack" layout).  A static
 * crt0 ignores the frame entirely, so every ET_EXEC test stays compatible;
 * the rtld reads AT_PHDR/AT_PHNUM/AT_BASE from here to relocate the main
 * binary.  AT_BASE is ELF_DYN_BASE for ET_DYN and 0 for ET_EXEC.
 *
 * ponytail: argv/envp are a fixed one-element vector — real argv arrives when
 * execve() grows a user-supplied vector; nothing here needs reshaping then. */
static elf_status_t setup_user_stack(process_t *p, const elf_loader_t *ctx,
                                     uint64_t *out_rsp) {
    uint64_t flags = VMM_WRITE | (nx_ok ? VMM_NX : 0);

    for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE)
        if (process_alloc_user_page(p, USER_STACK_TOP - USER_STACK_SIZE + off,
                                    flags) != VOID_OK)
            return ELF_ERR_NOMEM;

    /* System V auxv a_type values.  AT_NULL terminates the pair list. */
    enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5,
           AT_PAGESZ = 6, AT_BASE = 7, AT_ENTRY = 9 };

    static const char argv0[] = "program";
    const uint64_t argv0_len = sizeof(argv0);          /* incl. NUL */

    /* Words: argc, argv[0], argv NUL, envp NUL, then 7 auxv pairs. */
    const uint64_t n_words = 4 + 2 * 7;
    uint64_t rsp = (USER_STACK_TOP - n_words * 8) & ~0xFULL;

    /* argv0 string sits just below the word frame, 16-byte aligned.  Both it
     * and the frame are inside the top stack page, which is mapped above. */
    uint64_t str_va = (rsp - argv0_len) & ~0xFULL;
    uint64_t str_phys = vmm_virt_to_phys((uint64_t *)p->cr3, str_va);
    if (!str_phys) return ELF_ERR_NOMEM;
    uint8_t *sdst = (uint8_t *)(str_phys + g_boot.hhdm_offset);
    for (uint64_t i = 0; i < argv0_len; i++) sdst[i] = (uint8_t)argv0[i];

    uint64_t phys = vmm_virt_to_phys((uint64_t *)p->cr3, rsp);
    if (!phys) return ELF_ERR_NOMEM;
    uint64_t *slot = (uint64_t *)(phys + g_boot.hhdm_offset);

    uint64_t w = 0;
    slot[w++] = 1;                              /* argc                     */
    slot[w++] = str_va;                         /* argv[0]                  */
    slot[w++] = 0;                              /* argv NULL terminator     */
    slot[w++] = 0;                              /* envp[0] = NULL           */
    slot[w++] = AT_PHDR;   slot[w++] = ctx->phdr;          /* biased phdr VA */
    slot[w++] = AT_PHENT;  slot[w++] = sizeof(Elf64_Phdr);
    slot[w++] = AT_PHNUM;  slot[w++] = ctx->ehdr->e_phnum;
    slot[w++] = AT_ENTRY;  slot[w++] = ctx->entry + ctx->base;
    slot[w++] = AT_BASE;   slot[w++] = ctx->base;   /* ELF_DYN_BASE, or 0   */
    slot[w++] = AT_PAGESZ; slot[w++] = PAGE_SIZE;
    slot[w++] = AT_NULL;   slot[w++] = 0;

    *out_rsp = rsp;
    return ELF_OK;
}

/* ── elf_load_segments ──────────────────────────────────────────────────
 * Map and populate every PT_LOAD of `ctx` into `p`'s address space, but do
 * NOT map a stack or touch the initial frame.  Used for the dynamic linker
 * image, which must share the process the kernel already built for the PIE:
 * el_load_into_process() already seeded the auxv describing the *main*
 * binary, and the rtld must not overwrite it with one describing itself. */
static elf_status_t elf_load_segments_only(const elf_loader_t *ctx, process_t *p) {
    const Elf64_Ehdr *eh = ctx->ehdr;

    /* Map every segment before copying anything: a NOMEM halfway through
     * then leaves a uniformly empty-but-mapped space for the caller to tear
     * down, rather than a half-written one. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        elf_status_t s = map_segment_pages(p, ph, ctx->base);
        if (s != ELF_OK) return s;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = phdr_at(eh, i);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        elf_status_t s = copy_segment_data(p, ctx, ph, ctx->base);
        if (s != ELF_OK) return s;
    }
    return ELF_OK;
}

/* ── elf_load_interp ────────────────────────────────────────────────────
 * When the main image carried a PT_INTERP, hoist the named dynamic linker
 * into the same address space and run it first.  The interpreter is itself a
 * statically linked ET_EXEC loaded at its own base; it relocates the main
 * binary by reading the auxv the kernel seeded for it (AT_PHDR/AT_ENTRY/
 * AT_BASE all describe the *main* image), then jumps to AT_ENTRY.
 *
 * The interp path is a filesystem path with no FS to resolve against, so the
 * basename is matched to an embedded blob (the same lookup execve() uses).
 * On success `*out_interp_entry` is the interpreter's entry point; the caller
 * jumps there instead of the main image's entry.
 *
 * ponytail: exactly one interpreter is loaded, in-place, and the main image
 * must have no DT_NEEDED a real loader would satisfy — Void has no writable
 * FS or file-backed mmap yet, so a DSO is linked into the same image.  Real
 * DSO load-on-demand is the upgrade when file-backed mapping lands. */
elf_status_t elf_load_interp(const elf_loader_t *ctx, process_t *p,
                             uint64_t *out_interp_entry) {
    if (!ctx || !ctx->has_interp) return ELF_ERR_INVAL;

    /* Basename of the interpreter path: everything up to the last '/' is a
     * directory that doesn't exist to resolve against. */
    const char *base = ctx->interp;
    for (const char *c = ctx->interp; *c; c++)
        if (*c == '/') base = c + 1;

    elf_blob_t blob = { 0, 0 };
    elf_status_t es = elf_find_embedded(base, &blob);
    if (es != ELF_OK) return es;

    elf_loader_t ic;
    es = elf_validate(blob.base, blob.size, &ic);
    if (es != ELF_OK) return es;
    if (ic.has_interp) return ELF_ERR_NOEXEC;   /* an interpreter must not recurse */

    es = elf_load_segments_only(&ic, p);
    if (es != ELF_OK) return es;

    *out_interp_entry = ic.entry + ic.base;     /* ET_EXEC: pre-bias is final */
    return ELF_OK;
}

/* ── elf_load_into_process ───────────────────────────────────────────── */
elf_status_t elf_load_into_process(const elf_loader_t *ctx, process_t *p,
                                   uint64_t *out_entry, uint64_t *out_rsp) {
    if (!ctx || !ctx->ehdr || !p || !out_entry || !out_rsp) return ELF_ERR_INVAL;

    elf_status_t s = elf_load_segments_only(ctx, p);
    if (s != ELF_OK) return s;

    s = setup_user_stack(p, ctx, out_rsp);
    if (s != ELF_OK) return s;

    /* Entry: ET_DYN is relative, biased by the load base. */
    *out_entry = ctx->entry + ctx->base;
    return ELF_OK;
}
