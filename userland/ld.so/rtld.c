/* VoidOS — userland dynamic linker (ld-void.so)
 *
 * ── how control gets here ───────────────────────────────────────────────
 * The kernel's ELF loader, seeing a PT_INTERP in a PIE, loads *both* images:
 * the PIE at ELF_DYN_BASE and this interpreter above it, then jumps here
 * with the System V initial stack the kernel seeded.  The auxiliary vector
 * is the whole interface:
 *
 *   AT_PHDR   → the main image's program-header table (already biased)
 *   AT_PHNUM  → its entry count
 *   AT_ENTRY  → the main image's real entry point (already biased)
 *   AT_BASE   → the main image's load bias  (ELF_DYN_BASE, or 0 for ET_EXEC)
 *
 * From AT_PHDR the rtld finds PT_DYNAMIC, from PT_DYNAMIC the relocation
 * and symbol tables, and it applies every relocation in place.  It then
 * returns AT_ENTRY to the assembly stub, which jumps there.
 *
 * ── deliberate scope ────────────────────────────────────────────────────
 * This is a *relocating* linker, not a loader: it does not open files, so
 * DT_NEEDED is reported and ignored rather than followed.  Void has no
 * writable filesystem and no file-backed mmap, so there is nothing to open
 * a DSO from — the shared objects a Void program needs are linked into the
 * same image, and every relocation the toolchain emits for that shape
 * (RELATIVE / GLOB_DAT / JUMP_SLOT / 64) is resolved here against the
 * image's own dynsym.
 *
 * ponytail: DT_NEEDED is parsed but not loaded — add real DSO loading when
 * file-backed mmap and a writable FS exist; the reloc loop needs no change,
 * only a symbol-lookup scope that spans more than one object.
 *
 * ponytail: no lazy binding — every JUMP_SLOT is bound eagerly at startup.
 * A _dl_runtime_resolve trampoline is the upgrade, and buys nothing until
 * programs are large enough for startup cost to matter.
 */
#include "rtld.h"

/* ── the two syscalls the rtld needs ─────────────────────────────────────
 * Written inline rather than pulled from <void.h>: the rtld is linked on its
 * own and must not acquire a libc dependency to report an error. */
#define SYS_write 1
#define SYS_exit  60

static long sys_write(int fd, const void *buf, unsigned long n) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"((long)SYS_write), "D"((long)fd), "S"(buf), "d"(n)
                      : "rcx", "r11", "memory");
    return r;
}

__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile ("syscall" :: "a"((long)SYS_exit), "D"((long)code) : "memory");
    __builtin_unreachable();
}

static unsigned long slen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *s) { sys_write(1, s, slen(s)); }

/* A fatal linking error kills this process; it never returns to the program
 * with a half-relocated image.  The kernel cannot help us here — the rtld is
 * ordinary Ring 3 code, so the only honest failure is exit(). */
__attribute__((noreturn))
static void fail(const char *why) {
    say("[rtld] fatal: ");
    say(why);
    say("\n");
    sys_exit(127);
}

/* ── auxv lookup ─────────────────────────────────────────────────────────
 * The initial frame is [argc][argv…][NULL][envp…][NULL][auxv pairs][AT_NULL].
 * Walking it means skipping two NUL-terminated vectors first — the rtld must
 * not assume the kernel's current one-argument shape, because execve() will
 * grow it. */
static const uint64_t *auxv_of(const uint64_t *frame) {
    uint64_t argc = frame[0];
    const uint64_t *p = frame + 1 + argc;   /* now at argv's NULL */
    if (*p != 0) return 0;                  /* malformed frame    */
    p++;
    while (*p) p++;                         /* skip envp          */
    return p + 1;                           /* first auxv pair    */
}

static uint64_t auxv_get(const uint64_t *auxv, uint64_t type, int *found) {
    *found = 0;
    if (!auxv) return 0;
    for (const uint64_t *a = auxv; a[0] != AT_NULL; a += 2)
        if (a[0] == type) { *found = 1; return a[1]; }
    return 0;
}

/* ── symbol resolution ───────────────────────────────────────────────────
 * Single-object scope: a symbol is resolved inside the image being relocated.
 * A defined symbol (st_shndx != SHN_UNDEF) resolves to base + st_value.  An
 * undefined one has no second object to search, so it is a hard error rather
 * than a silent zero — a NULL call would fault in Ring 3 much later, with no
 * indication of what went wrong. */
static uint64_t resolve(const Elf64_Sym *symtab, const char *strtab,
                        uint32_t idx, uint64_t base) {
    const Elf64_Sym *s = &symtab[idx];
    if (s->st_shndx == SHN_UNDEF) {
        say("[rtld] undefined symbol: ");
        say(strtab + s->st_name);
        say("\n");
        fail("unresolved import (no DSO loading yet)");
    }
    return base + s->st_value;
}

/* ── apply one relocation table ──────────────────────────────────────────
 * Both DT_RELA and DT_JMPREL are RELA tables of identical shape, so one loop
 * serves both.  Writes go straight through the target VA: the rtld runs in
 * the same address space as the image, and the kernel already mapped every
 * PT_LOAD page writable-or-not per its flags.  A relocation into a read-only
 * page would fault here — which is the correct outcome, since the toolchain
 * only emits relocations into .data/.got. */
static void apply_rela(const Elf64_Rela *rela, uint64_t bytes, uint64_t ent,
                       const Elf64_Sym *symtab, const char *strtab,
                       uint64_t base, unsigned *counts) {
    if (!rela || !bytes) return;
    if (ent == 0) ent = sizeof(Elf64_Rela);

    for (uint64_t off = 0; off + ent <= bytes; off += ent) {
        const Elf64_Rela *r = (const Elf64_Rela *)((const uint8_t *)rela + off);
        uint64_t *target = (uint64_t *)(base + r->r_offset);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t sym  = ELF64_R_SYM(r->r_info);

        switch (type) {
        case R_X86_64_RELATIVE:      /* B + A — no symbol involved */
            *target = base + (uint64_t)r->r_addend;
            counts[0]++;
            break;

        case R_X86_64_GLOB_DAT:      /* S — a GOT slot for a data symbol */
            if (!symtab || !strtab) fail("GLOB_DAT without a symbol table");
            *target = resolve(symtab, strtab, sym, base);
            counts[1]++;
            break;

        case R_X86_64_JUMP_SLOT:     /* S — a PLT slot, bound eagerly */
            if (!symtab || !strtab) fail("JUMP_SLOT without a symbol table");
            *target = resolve(symtab, strtab, sym, base);
            counts[2]++;
            break;

        case R_X86_64_64:            /* S + A */
            if (!symtab || !strtab) fail("R_X86_64_64 without a symbol table");
            *target = resolve(symtab, strtab, sym, base) + (uint64_t)r->r_addend;
            counts[3]++;
            break;

        default:
            /* Refuse rather than skip: an unapplied relocation leaves a
             * dangling slot the program would call into. */
            fail("unsupported relocation type");
        }
    }
}

/* ── rtld_main ───────────────────────────────────────────────────────────
 * Returns the address the stub should jump to.  `frame` is the untouched
 * initial RSP, so argc lives at frame[0]. */
uint64_t rtld_main(const uint64_t *frame) {
    const uint64_t *auxv = auxv_of(frame);
    if (!auxv) fail("malformed initial stack (no auxv)");

    int have_phdr, have_phnum, have_entry, have_base;
    uint64_t phdr_va = auxv_get(auxv, AT_PHDR,  &have_phdr);
    uint64_t phnum   = auxv_get(auxv, AT_PHNUM, &have_phnum);
    uint64_t entry   = auxv_get(auxv, AT_ENTRY, &have_entry);
    uint64_t base    = auxv_get(auxv, AT_BASE,  &have_base);

    if (!have_phdr || !have_phnum) fail("auxv lacks AT_PHDR/AT_PHNUM");
    if (!have_entry || !entry)     fail("auxv lacks AT_ENTRY");
    if (!have_base)                fail("auxv lacks AT_BASE");

    /* ── find PT_DYNAMIC ─────────────────────────────────────────────
     * The phdr table is already at its runtime address (the kernel biased
     * AT_PHDR), but p_vaddr inside each entry is still unbiased. */
    const Elf64_Phdr *ph = (const Elf64_Phdr *)phdr_va;
    const Elf64_Dyn  *dyn = 0;
    for (uint64_t i = 0; i < phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
            break;
        }

    if (!dyn) {
        /* A static image needs no relocation.  This is not an error: the
         * kernel runs the interp for anything that names one, and a
         * statically-linked program that names an interp is simply already
         * complete. */
        say("[rtld] static image, nothing to relocate\n");
        return entry;
    }

    /* ── walk DT_* ───────────────────────────────────────────────────── */
    const Elf64_Rela *rela = 0, *jmprel = 0;
    uint64_t relasz = 0, relaent = 0, pltrelsz = 0;
    const Elf64_Sym *symtab = 0;
    const char *strtab = 0;
    unsigned needed = 0;

    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        /* DT_* pointers are unbiased link-time addresses in a PIE, so each
         * needs the load base added — the same bias the relocations use. */
        case DT_RELA:     rela   = (const Elf64_Rela *)(base + d->d_val); break;
        case DT_RELASZ:   relasz = d->d_val;                              break;
        case DT_RELAENT:  relaent = d->d_val;                             break;
        case DT_JMPREL:   jmprel = (const Elf64_Rela *)(base + d->d_val); break;
        case DT_PLTRELSZ: pltrelsz = d->d_val;                            break;
        case DT_SYMTAB:   symtab = (const Elf64_Sym *)(base + d->d_val);  break;
        case DT_STRTAB:   strtab = (const char *)(base + d->d_val);       break;
        case DT_NEEDED:   needed++;                                       break;
        default: break;   /* hash tables, PLTGOT, SONAME: not needed here */
        }
    }

    if (needed) {
        /* Honest about the limit rather than failing late inside a call into
         * an unloaded object. */
        say("[rtld] warning: DT_NEEDED present but DSO loading is not "
            "implemented; imports must be resolvable in-image\n");
    }

    /* counts: RELATIVE, GLOB_DAT, JUMP_SLOT, 64 */
    unsigned counts[4] = {0, 0, 0, 0};
    apply_rela(rela,   relasz,   relaent, symtab, strtab, base, counts);
    apply_rela(jmprel, pltrelsz, relaent, symtab, strtab, base, counts);

    /* One line, one write: other processes share COM1, and a torn marker
     * makes the boot regression nondeterministic (the Phase 13 lesson). */
    char buf[96];
    unsigned n = 0;
    const char *lead = "[rtld] relocated: ";
    for (const char *q = lead; *q; q++) buf[n++] = *q;
    for (int k = 0; k < 4; k++) {
        unsigned v = counts[k];
        char tmp[12]; int t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) buf[n++] = tmp[--t];
        buf[n++] = (k == 3) ? '\n' : '/';
    }
    sys_write(1, buf, n);

    return entry;
}
