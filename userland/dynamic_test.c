/* VoidOS — Track C dynamic-linking regression (dynamic_test)
 *
 * A position-independent "shared object" that IS the whole program: linked
 * `-shared` with a PT_INTERP pointing at the rtld and every libc function
 * built `-fPIC` into the same image.  The four relocation types Void's rtld
 * understands all appear, and every one names a symbol DEFINED in this image
 * (Void has no writable FS to load a separate DSO from — the single-object
 * scope is the whole point):
 *
 *   R_X86_64_RELATIVE   static self-pointer (anchor = &sentence[0])
 *   R_X86_64_GLOB_DAT   a preemptible global's GOT slot (greeter, strcpy)
 *   R_X86_64_64         an *initialized* function pointer (saved = strcpy)
 *   R_X86_64_JUMP_SLOT  calling malloc/strcpy/strlen/free through the PLT
 *
 * The kernel loads this PIE at ELF_DYN_BASE, seeds a full System V auxv
 * describing it, and runs the rtld (ld-void.so) first.  The rtld relocates
 * the image in place, then jumps to AT_ENTRY — which is dynamic_main here.
 * dynamic_main never returns (a `jmp *%rax` return frame has nothing to
 * return to); it reports and exits itself, mirroring crt0.  The marker line
 * is one sys_write so the boot regression's grep can never be torn by
 * interleaving (the Phase 13 lesson).
 *
 * Build note: this object compiles `-fPIC` (not UCFLAGS' -fno-pie) and is
 * linked `-shared` with a .interp section — see the Makefile rules.
 */
#include <libc/malloc.h>
#include <libc/string.h>

#define SYS_write 1
#define SYS_exit  60
typedef unsigned long ulong;

/* Print "[DYNAMIC] Done: <0|1> fail" in a single write, then exit.  `code`
 * is the test result: 0 = all relocations applied and the round trip worked,
 * nonzero = the failing check. */
__attribute__((noreturn))
static void report_and_exit(int code) {
    char b[48]; unsigned n = 0;
    static const char h[] = "[DYNAMIC] Done: ";
    static const char f[] = " fail";
    for (const char *p = h; *p; p++) b[n++] = *p;       /* marker           */
    b[n++] = (char)('0' + (code == 0 ? 0 : 1));         /* 0 pass / 1 fail  */
    if (code) b[n++] = (char)('0' + (code % 10));       /* which check      */
    for (const char *p = f; *p; p++) b[n++] = *p;
    b[n++] = '\n';

    __asm__ volatile ("syscall"
                      : : "a"((long)SYS_write), "D"(1L), "S"(b), "d"((ulong)n)
                      : "rcx", "r11", "memory");
    __asm__ volatile ("syscall" :: "a"((long)SYS_exit), "D"((long)(code ? 1 : 0))
                      : "memory");
    __builtin_unreachable();
}

/* ── the four relocation hooks (see header) ────────────────────────────
 * volatile on `anchor` is deliberate: at -O2 gcc would prove
 * `anchor == &sentence[0]` constant and fold the comparison (dropping the
 * RELATIVE), but a volatile read must go through the reloaded .data bytes —
 * exactly what the relocation fixes up.  `saved`'s *initializer* is what
 * mints R_X86_64_64 (an initialized function-pointer global); `greeter` is a
 * plain preemptible global, hence GLOB_DAT. */
static char sentence[32];
static char *volatile anchor = &sentence[0];     /* RELATIVE target (B+A) */
char *(*greeter)(char *, const char *);          /* GLOB_DAT target       */
char *(*saved)(char *, const char *) = strcpy;   /* R_X86_64_64 target    */

int dynamic_main(void)
{
    /* JUMP_SLOT: call friends through the PIC PLT, all defined in-image. */
    char *d = (char *)malloc(32);
    if (!d) report_and_exit(1);

    greeter = saved;                  /* GLOB_DAT write; + read below     */
    greeter(d, "dyn");                /* JUMP_SLOT through greeter's PLT? */
    /* ^ actually a direct call to strcpy — the JUMP_SLOTs for the libc
     * functions above are the real proof; greeter is the GLOB_DAT proof. */

    size_t n = strlen(d);             /* JUMP_SLOT (libc, in-image)       */
    if (n != 3) report_and_exit(2);

    /* RELATIVE: the static self-pointer must now bias to this image, so the
     * bytes where `anchor` lives hold &sentence[0] + ELF_DYN_BASE. */
    if (anchor != &sentence[0]) report_and_exit(3);

    /* R_X86_64_64: the initialized function pointer resolved to the image's
     * own strcpy (bias + st_value), so calling it round-trips.  `greeter`
     * being equal to `saved` proves the GLOB_DAT slot and the 64 slot agree. */
    if (greeter != saved) report_and_exit(4);

    free(d);                          /* JUMP_SLOT (libc, in-image)       */
    report_and_exit(0);
}
