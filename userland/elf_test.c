/* VoidOS — Phase 5A ELF user test program
 *
 * A genuine freestanding ELF64 executable: no libc, no crt0, no relocations.
 * Linked by userland/user.ld at 0x400000, entered at _start with the RSP the
 * kernel's loader set up.
 *
 * It exists to prove the loader actually works, so it deliberately touches
 * one thing per segment kind:
 *
 *   .rodata  read a string constant and write() it
 *   .data    read an initialised value, then modify it (proves R+W)
 *   .bss     read an uninitialised value and confirm it is zero
 *   .text    executing at all proves the R+X mapping
 *
 * Exit status encodes which checks passed, so the kernel can assert on it
 * rather than on parsing console text.
 */

/* ── syscall numbers (must match kernel/include/syscall/syscall.h) ────── */
#define SYS_write       1
#define SYS_sched_yield 24
#define SYS_getpid      39
#define SYS_exit        60

/* Inline syscall wrappers.  R10 replaces RCX for arg3 because the SYSCALL
 * instruction clobbers RCX with the return RIP; R11 is clobbered likewise. */
static long sys1(long nr, long a) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(nr), "D"(a)
                                : "rcx", "r11", "memory");
    return r;
}
static long sys3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)
                                : "rcx", "r11", "memory");
    return r;
}
static long sys0(long nr) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(nr) : "rcx", "r11", "memory");
    return r;
}

/* ── segment-resident test data ──────────────────────────────────────────
 * volatile keeps the compiler from folding these into immediates, which
 * would defeat the whole point of testing where they live. */
static const char msg_hello[] = "[elf] hello from a real ELF64 binary\n";
static const char msg_ok[]    = "[elf] rodata+data+bss checks passed\n";

static volatile unsigned long data_val = 0xD0D0CAFEUL;   /* .data */
static volatile unsigned long bss_val;                   /* .bss  */
static volatile unsigned long bss_arr[512];              /* .bss, > 1 page */

static unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void) {
    long pid = sys0(SYS_getpid);

    sys3(SYS_write, 1, (long)msg_hello, (long)ustrlen(msg_hello));

    /* Bit flags accumulate into the exit status: 1=data, 2=bss, 4=bss array,
     * 8=data writable.  A missing bit names the exact failed check. */
    long checks = 0;

    if (data_val == 0xD0D0CAFEUL) checks |= 1;

    if (bss_val == 0)             checks |= 2;

    /* Spot-check across more than one page so a loader that only zeroes the
     * first BSS page is caught. */
    int zeroed = 1;
    for (int i = 0; i < 512; i++)
        if (bss_arr[i] != 0) { zeroed = 0; break; }
    if (zeroed) checks |= 4;

    data_val = 0x1234;
    if (data_val == 0x1234)       checks |= 8;

    sys0(SYS_sched_yield);

    /* Values must survive a context switch — a CR3 or mapping bug shows up
     * here rather than silently passing. */
    if (data_val != 0x1234) checks &= ~8;

    if (checks == 15)
        sys3(SYS_write, 1, (long)msg_ok, (long)ustrlen(msg_ok));

    /* Exit 0 on full success; otherwise 0x40 | checks, which is non-zero
     * even when every individual check failed. */
    sys1(SYS_exit, checks == 15 ? 0 : (0x40 | checks));

    for (;;) { }   /* exit never returns; keeps the compiler quiet */
    (void)pid;
}
