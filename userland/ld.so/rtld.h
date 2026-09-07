/* VoidOS — userland dynamic linker: ELF64 dynamic-linking structures
 *
 * The kernel loads *this* binary (statically linked, ET_EXEC) whenever a
 * PT_INTERP names it, seeds a full System V stack, and jumps to _start.
 * Everything below is the subset of the ELF64 dynamic-linking ABI the rtld
 * actually walks — there is deliberately no shared header with the kernel's
 * <elf/elf.h>: the kernel validates images, the rtld relocates them, and
 * duplicating four small structs is cheaper than a cross-ring dependency.
 *
 * ponytail: only the four relocation types Void's toolchain emits are
 * modelled (RELATIVE / GLOB_DAT / JUMP_SLOT / 64).  Add TLS/IRELATIVE when
 * a __thread variable or an ifunc first appears.
 */
#ifndef VOID_RTLD_H
#define VOID_RTLD_H 1

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef long               int64_t;

/* ── ELF64 headers (only the fields the rtld reads) ──────────────────── */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* ── dynamic section ─────────────────────────────────────────────────── */
typedef struct {
    int64_t  d_tag;
    uint64_t d_val;          /* d_ptr shares the union; both are 64-bit */
} __attribute__((packed)) Elf64_Dyn;

/* ── symbol table ────────────────────────────────────────────────────── */
typedef struct {
    uint32_t st_name;        /* offset into DT_STRTAB */
    uint8_t  st_info;        /* bind<<4 | type */
    uint8_t  st_other;
    uint16_t st_shndx;       /* SHN_UNDEF (0) means "imported" */
    uint64_t st_value;       /* unbiased address within its object */
    uint64_t st_size;
} __attribute__((packed)) Elf64_Sym;

/* ── relocation with explicit addend ─────────────────────────────────── */
typedef struct {
    uint64_t r_offset;       /* unbiased target address */
    uint64_t r_info;         /* sym<<32 | type */
    int64_t  r_addend;
} __attribute__((packed)) Elf64_Rela;

#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xFFFFFFFFU))

/* ── the four relocation types Void's toolchain emits ────────────────── */
#define R_X86_64_64         1    /* S + A                                  */
#define R_X86_64_GLOB_DAT   6    /* S (GOT slot for a data symbol)         */
#define R_X86_64_JUMP_SLOT  7    /* S (PLT slot)                           */
#define R_X86_64_RELATIVE   8    /* B + A (load base, no symbol)           */

/* ── program header / dynamic tags ───────────────────────────────────── */
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_PLTGOT    3
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_JMPREL    23

#define SHN_UNDEF    0

/* ── auxiliary vector types the kernel seeds (see kernel/elf/elf.c) ──── */
#define AT_NULL      0
#define AT_PHDR      3
#define AT_PHENT     4
#define AT_PHNUM     5
#define AT_PAGESZ    6
#define AT_BASE      7
#define AT_ENTRY     9

#endif /* VOID_RTLD_H */
