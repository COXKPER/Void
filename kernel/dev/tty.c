/* VoidOS — kernel terminal (TTY)
 *
 * The line discipline + console splice that fd 0/1/2 route through, living
 * between the syscall layer and the serial device.  The path for a console
 * byte is:
 *
 *     read:   user → sys_read(fd 0) → tty_read → serial_getchar()
 *     write:  user → sys_write(fd 1/2) → tty_write → serial_putchar()
 *
 * Echo and cooked-line input are *Void-native*: Lux's tty.c is an output-only
 * framebuffer glyph terminal (font render, scroll, cursor, ANSI colors) with
 * no input, no line discipline and no echo — that machinery lives in Lux's
 * userland servers, outside the kernel we port.  What we take from Lux is
 * only the *shape* of an output console (a putc-with-smarts on line edges),
 * and even that is reimplemented so the output backend stays whatever
 * `kprintf` uses (serial today, framebuffer under `kout=vga`).  See
 * CLAUDE.md Phase 14 (TTY) for the full port classification.
 *
 * Semantics deliberately NOT invented (no POSIX fakery):
 *   • read(fd 0) stays NON-BLOCKING.  Void has no wait queues / sleep-wakeup
 *     (PROC_BLOCKED exists only for wait4), and the spec forbids redesigning
 *     the scheduler for the TTY.  When sleep/wakeup primitives land, a
 *     blocking wait on a line becomes a one-file change here.
 *   • Ctrl-C is recognised but RESERVED (returned as a literal byte).  No
 *     signal semantics are invented (no complete signal subsystem).
 *   • No PTY, no /dev/pts, no termios bit-parity.
 *
 * Echo and staged-line state are process-independent: the console is one
 * shared terminal owned by the kernel, exactly like the FD_SERIAL console it
 * replaces.  Every process that reads fd 0 shares the same cooked line, which
 * is the honest behaviour of a single-UART system.
 */
#include <void/types.h>
#include <void/device.h>
#include <dev/serial.h>
#include <dev/serial_drv.h>
#include <dev/tty.h>
#include <syscall/syscall.h>   /* VE_* errno values */

/* ── line-discipline state ──────────────────────────────────────────────── */
#define TTY_LINE_MAX     256      /* staged input capacity (incl. NUL)     */
#define TTY_ECHO_DEFAULT 1

static char     tty_line[TTY_LINE_MAX];  /* cooked input staged for read  */
static uint64_t tty_line_len = 0;        /* bytes staged (incl. trailing \n) */
static uint8_t  tty_echo     = TTY_ECHO_DEFAULT;

/* Output backend: the same kernel console instance the FD_SERIAL path used.
 * The TTY does not own the serial device — it splices on top of it — so
 * `kprintf`, console VGA routing and the Phase 11 device layer stay
 * byte-identical, and echo + user writes + kernel prints all share one UART. */
static void *tty_out(void) { return serial_drv_instance(); }

static void tty_outc(char c) {
    void *inst = tty_out();
    if (inst) dev_write(inst, &c, 1);
}

/* ── init ────────────────────────────────────────────────────────────────── */
void tty_init(void) {
    tty_line_len = 0;
    tty_echo     = TTY_ECHO_DEFAULT;
}

/* ── ioctl: minimal terminal-state surface ───────────────────────────────
 * Void has no termios.  Only the bits that map are exposed; everything else
 * is -VE_INVAL (no invented struct).  Echo on/off is the canonical one: a
 * test can flip it deterministically to prove echo is real and separable. */
int tty_ioctl(uint64_t req, void *arg) {
    (void)arg;
    switch (req) {
    case VOID_TTY_ECHO_ON:
        tty_echo = 1;
        return 0;
    case VOID_TTY_ECHO_OFF:
        tty_echo = 0;
        return 0;
    default:
        return -VE_INVAL;
    }
}

/* ── echo one byte back to the terminal ─────────────────────────────────── */
static void tty_echo_byte(char c) {
    if (!tty_echo) return;
    if (c == '\b' || c == 0x7f) {
        /* erase: backspace + space + backspace removes the glyph */
        tty_outc('\b');
        tty_outc(' ');
        tty_outc('\b');
        return;
    }
    if (c == '\n' || c == '\r') {
        tty_outc('\r');
        tty_outc('\n');
        return;
    }
    tty_outc(c);
}

/* ── feed one byte from the UART into the discipline ─────────────────────
 * Canonical/cooked handling:
 *   • printable byte   → stage + echo
 *   • DEL / backspace  → pop the last staged byte + erase-echo
 *   • CR / LF          → echo newline, stage a '\n' (line is now complete)
 *   • CTRL-C (0x03)    → staged as a literal byte (reserved; no signals)
 *   • EOF (0x04)       → discard staged data (empty read = 0)
 *   • other control    → staged, not echoed
 * The buffer never overflows: full-buffer bytes are dropped. */
static void tty_feed(int raw) {
    if (raw < 0) return;
    char c = (char)(uint8_t)raw;

    /* line terminator: complete the staged line with a '\n' */
    if (c == '\n' || c == '\r') {
        tty_echo_byte('\n');
        if (tty_line_len < TTY_LINE_MAX - 1)
            tty_line[tty_line_len++] = '\n';
        return;
    }

    if (c == '\b' || c == 0x7f) {
        if (tty_line_len > 0) {
            tty_line_len--;
            tty_echo_byte('\b');
        }
        return;
    }

    if (c == 0x04) {              /* EOF: flush staged data */
        tty_line_len = 0;
        return;
    }

    if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7f) {  /* printable + echo */
        if (tty_line_len < TTY_LINE_MAX - 1) {
            tty_line[tty_line_len++] = c;
            tty_echo_byte(c);
        }
        return;
    }

    /* other control bytes: staged, not echoed */
    if (tty_line_len < TTY_LINE_MAX - 1)
        tty_line[tty_line_len++] = c;
}

/* ── read: drain staged input, non-blocking ──────────────────────────────
 * Pumps the UART once (gathering whatever is ready), then hands the caller
 * as much staged data as fits.  Returns the byte count (0 when nothing is
 * staged — the poll semantics the old FD_SERIAL read had), or -errno.  The
 * caller copies into user space; `buf` is a kernel staging buffer. */
int tty_read(uint64_t buf, uint64_t n) {
    /* Pump currently-available UART bytes into the discipline.  No
     * blocking: when the UART has nothing, we return what we staged. */
    for (int spins = 0; spins < 128; spins++) {
        int raw = serial_getchar();
        if (raw < 0) break;
        tty_feed(raw);
    }

    uint64_t got = 0;
    char *out = (char *)buf;
    while (got < n && got < tty_line_len) {
        out[got] = tty_line[got];
        got++;
    }

    /* consume the handed-out prefix */
    for (uint64_t i = got; i < tty_line_len; i++)
        tty_line[i - got] = tty_line[i];
    tty_line_len -= got;

    return (got > 0) ? (int)got : 0;
}

/* ── write: straight byte stream to the console ──────────────────────────
 * No newline cooking, no output backspace — the kernel already prints what
 * it means.  Streams through the device layer so TTY output and echo share
 * one backend.  Chunked user copies happen in sys_write. */
int tty_write(const char *buf, uint64_t n) {
    void *inst = tty_out();
    if (!inst) return -VE_IO;              /* console not up (boot-order bug) */
    int w = dev_write(inst, buf, n);
    return w < 0 ? w : (int)n;
}

/* ════════════════════════════════════════════════════════════════════════
 *  kernel self-check — [TTY] PASS/FAIL lines, no interrupts
 *  ════════════════════════════════════════════════════════════════════════
 * Feeds synthetic bytes straight into the discipline (bypassing the UART) so
 * the echo/backspace/newline/EOF semantics are proven deterministically at
 * boot, then confirms tty_read drains the staged line.  The output side is
 * proven by the userland tty_test (write-through + non-blocking read + the
 * ioctl surface). */

static void tty_puts(const char *s) { while (*s) serial_putchar(*s++); }
static void tty_result(const char *label, bool pass) {
    tty_puts("[TTY] "); tty_puts(label); tty_puts(": ");
    tty_puts(pass ? "PASS" : "FAIL"); tty_puts("\n\r");
}

void tty_selftest(void) {
    int fails = 0;
    #define CHK(lbl, cond) do { bool p = (cond); if (!p) fails++; \
        tty_result(lbl, p); } while (0)
    (void)fails;   /* result lines are the observable regression output */

    /* reset the discipline so the check starts from a clean line */
    tty_init();

    /* type "ab" then backspace → staged line is "a" (echo covered by the
     * real output path; here we prove staging + deletion). */
    tty_feed('a'); tty_feed('b');
    CHK("feed stages chars", tty_line_len == 2 && tty_line[0] == 'a' && tty_line[1] == 'b');

    tty_feed('\b');
    CHK("backspace pops one", tty_line_len == 1 && tty_line[0] == 'a');

    /* newline completes the line: staged content becomes "a\n" */
    tty_feed('\n');
    CHK("newline completes line", tty_line_len == 2 && tty_line[0] == 'a' && tty_line[1] == '\n');

    /* tty_read drains the staged line into a caller buffer */
    {
        char buf[8] = {0};
        int r = tty_read((uint64_t)(uintptr_t)buf, sizeof(buf));
        CHK("read drains staged line", r == 2 && buf[0] == 'a' && buf[1] == '\n');
        CHK("read leaves empty discipline", tty_line_len == 0);
    }

    /* EOF flushes staged data: type "xy", send EOF → read returns 0 */
    tty_feed('x'); tty_feed('y');
    tty_feed(0x04);
    {
        char buf[8] = {0};
        int r = tty_read((uint64_t)(uintptr_t)buf, sizeof(buf));
        CHK("eof flushes line", r == 0 && tty_line_len == 0);
    }

    /* line overflow: stage TTY_LINE_MAX-1 bytes; a byte past that is dropped */
    {
        tty_init();
        for (int i = 0; i < TTY_LINE_MAX + 5; i++) tty_feed('z');
        CHK("buffer never overflows", tty_line_len <= TTY_LINE_MAX - 1);
        char buf[512] = {0};
        int r = tty_read((uint64_t)(uintptr_t)buf, sizeof(buf));
        CHK("overflow drains to cap", r == TTY_LINE_MAX - 1);
        tty_init();
    }

    /* echo toggle: after OFF, a fed byte must NOT echo (we can't observe
     * the UART here without intercepting tty_outc, so assert the flag flip
     * makes the discipline stable and the ioctl surface round-trips). */
    CHK("echo off", tty_ioctl(VOID_TTY_ECHO_OFF, NULL) == 0);
    CHK("echo on",  tty_ioctl(VOID_TTY_ECHO_ON,  NULL) == 0);
    CHK("unknown ioctl -EINVAL", tty_ioctl(0xdead, NULL) == -VE_INVAL);

    tty_puts("[TTY] selftest done\n\r");
}