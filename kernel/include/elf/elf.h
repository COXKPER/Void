/* VoidOS — ELF64 user executable loader
 *
 * Loads ELF64 executables into a process address space.
 * Validates strictly; rejects malformed binaries cleanly.
 * Designed for integration with process/scheduler/syscall infrastructure.
 * Future execve() will reuse this layer without redesign.
 */
#ifndef VOID_ELF_H
#define VOID_ELF_H 1

#include <void/types.h>
#include <stdint.h>

/* ── ELF64 type definitions ──────────────────────────────────────────────
 * From System V AMD64 ABI / ELF64 specification. */

typedef struct {
    uint8_t  e_ident[16];      /* EI_MAG0..3: 0x7F,'E','L','F'; then class/endian/version/osabi/... */
    uint16_t e_type;           /* ET_EXEC (2) for executable */
    uint16_t e_machine;        /* EM_X86_64 (62) */
    uint32_t e_version;        /* should be 1 */
    uint64_t e_entry;          /* entry point address */
    uint64_t e_phoff;          /* program header table offset in file */
    uint64_t e_shoff;          /* section header table offset (ignored by loader) */
    uint32_t e_flags;          /* flags (x86_64 defines none) */
    uint16_t e_ehsize;         /* size of ELF header */
    uint16_t e_phentsize;      /* size of program header entry */
    uint16_t e_phnum;          /* number of program header entries */
    uint16_t e_shentsize;      /* size of section header entry (ignored) */
    uint16_t e_shnum;          /* number of section header entries (ignored) */
    uint16_t e_shstrndx;       /* section header string table index (ignored) */
} PACKED Elf64_Ehdr;

typedef struct {
    uint32_t p_type;           /* PT_LOAD (1), PT_DYNAMIC (2), PT_NOTE (4), etc. */
    uint32_t p_flags;          /* PF_X (1), PF_W (2), PF_R (4) */
    uint64_t p_offset;         /* offset in file */
    uint64_t p_vaddr;          /* virtual address */
    uint64_t p_paddr;          /* physical address (ignored in user ELF) */
    uint64_t p_filesz;         /* size in file (may be < p_memsz for BSS) */
    uint64_t p_memsz;          /* size in memory (includes BSS) */
    uint64_t p_align;          /* alignment (2^n) */
} PACKED Elf64_Phdr;

/* ── ELF magic / constants ────────────────────────────────────────────── */
#define EI_MAG0         0       /* e_ident[0] */
#define EI_MAG1         1       /* e_ident[1] */
#define EI_MAG2         2       /* e_ident[2] */
#define EI_MAG3         3       /* e_ident[3] */
#define EI_CLASS        4       /* e_ident[4]: ELFCLASS64 = 2 */
#define EI_DATA         5       /* e_ident[5]: ELFDATA2LSB = 1 (little-endian) */
#define EI_VERSION      6       /* e_ident[6]: EV_CURRENT = 1 */
#define EI_OSABI        7       /* e_ident[7]: ELFOSABI_SYSV = 0 */

#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASS64      2
#define ELFDATA2LSB     1       /* little-endian */
#define EV_CURRENT      1

#define ET_EXEC         2       /* executable */
#define EM_X86_64       62      /* x86-64 */

#define PT_LOAD         1       /* loadable segment */
#define PT_DYNAMIC      2       /* dynamic linking info (skip for now) */

#define PF_X            (1U << 0)       /* executable */
#define PF_W            (1U << 1)       /* writable */
#define PF_R            (1U << 2)       /* readable */

/* ── loader return codes ─────────────────────────────────────────────────
 * Mapped to -errno for syscalls, or standalone for kernel use. */
typedef enum {
    ELF_OK           =  0,
    ELF_ERR_NOMEM    = -12,    /* ENOMEM */
    ELF_ERR_FAULT    = -14,    /* EFAULT: malformed/bad pointer */
    ELF_ERR_NOEXEC   = -8,     /* ENOEXEC: not a valid ELF executable */
    ELF_ERR_INVAL    = -22,    /* EINVAL: invalid argument */
} elf_status_t;

/* Refuse absurd program-header counts before iterating.  A real executable
 * has a handful; a large count is either corruption or an attempt to make
 * the kernel spin. */
#define ELF_MAX_PHNUM   64

/* ── loader context ──────────────────────────────────────────────────────
 * Filled in by elf_validate() and consumed by elf_load_into_process().
 * `image` points at a kernel-owned buffer: the loader never dereferences
 * user memory, so a malformed header can only ever walk off inside this
 * bounded range, which every access is checked against. */
typedef struct {
    const uint8_t    *image;        /* kernel-resident ELF image           */
    uint64_t          image_size;   /* its length in bytes                 */
    const Elf64_Ehdr *ehdr;         /* == image, once validated            */
    uint64_t          entry;        /* validated e_entry                   */
} elf_loader_t;

/* ── public API ──────────────────────────────────────────────────────── */

/* Enable EFER.NXE if the CPU supports it, so PTE bit 63 means
 * "no-execute" rather than "reserved bit set" (which would fault).
 * Call once during boot, before any ELF is loaded. */
void elf_init(void);

/* True when elf_init() found working NX support. */
bool elf_nx_enabled(void);

/* Validate an ELF64 executable held in a kernel buffer.  Checks identity,
 * header self-consistency, program-header table bounds, and every PT_LOAD
 * segment's file range, virtual range, alignment and target half.  Does not
 * touch the process.  On ELF_OK, `ctx` is ready for elf_load_into_process().
 * Any malformed input returns an error; none can fault the kernel. */
elf_status_t elf_validate(const void *image, uint64_t size, elf_loader_t *ctx);

/* Load a validated image into `p`'s address space: maps and populates every
 * PT_LOAD segment, zeroes BSS, applies per-page permissions, and maps an
 * initial user stack.  Writes the entry RIP and initial RSP through the out
 * params.  On failure the address space may be partly populated — the
 * caller owns cleanup (process_destroy tears down everything recorded).
 *
 * Shaped so a future execve() can call it against a fresh address space
 * without changing this interface. */
struct process;
elf_status_t elf_load_into_process(const elf_loader_t *ctx, struct process *p,
                                   uint64_t *out_entry, uint64_t *out_rsp);

/* Resolve a user pathname to a kernel-resident executable image.
 *
 * There is no VFS yet, so every executable is a fixed name built into the
 * kernel (kernel/elf/elf_blobs.asm): "init", "elf_test", "ipc_test",
 * "fork_test".  This maps a short ASCII path to its (base,size) pair.
 * Returns ELF_OK, or ELF_ERR_NOEXEC if the name is unknown.
 *
 * ponytail: when the VFS lands, execve() walks the filesystem here instead
 * and this lookup disappears. */
typedef struct {
    const uint8_t *base;
    uint64_t       size;
} elf_blob_t;
elf_status_t elf_find_embedded(const char *name, const elf_blob_t *out);

/* The blobs are declared in kernel/elf/elf_blobs.asm and consumed only by
 * elf_find_embedded(); execve() never touches the symbols directly. */
extern const uint8_t elf_test_start[], elf_test_end[];
extern const uint8_t elf_packed_start[], elf_packed_end[];
extern const uint8_t elf_init_start[], elf_init_end[];
extern const uint8_t elf_ipc_test_start[], elf_ipc_test_end[];
extern const uint8_t elf_forkexec_test_start[], elf_forkexec_test_end[];
extern const uint8_t elf_calc_start[], elf_calc_end[];
extern const uint8_t elf_srv_test_start[], elf_srv_test_end[];
extern const uint8_t elf_lifecycle_test_start[], elf_lifecycle_test_end[];

#endif /* VOID_ELF_H */
