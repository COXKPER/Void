/* VoidOS — Kernel Panic implementation
 * See <void/kpanic.h> for contract.
 *
 * Framebuffer output: we draw directly into the GOP framebuffer returned
 * by Limine.  Text is rendered with a minimal 8×8 bitmap font baked in
 * (PC BIOS font pattern, code page 437 subset) — just enough for a
 * readable panic message.  Background is set to red (0xFF0000) for
 * immediate visual distinction.
 */
#include <void/kpanic.h>
#include <void/types.h>
#include <void/boot.h>
#include <dev/serial.h>

/* ── 8×8 bitmap font (first 128 ASCII chars, PC BIOS font subset) ───────
 * Each char is 8 bytes; bit N of byte M = pixel (N, M).
 * We embed only the printable ASCII range 0x20–0x7E plus a few
 * control replacements.  Full 128-char table would be 1024 bytes;
 * we use a compact version with just what a panic message needs.   */
#include "font8x8.inc"

/* ── framebuffer text state ───────────────────────────────────────────── */
static uint32_t fb_fg_color = 0xFFFFFF;   /* white text  */
static uint32_t fb_bg_color = 0xFF0000;   /* red background */
static uint64_t fb_cursor_x = 0;
static uint64_t fb_cursor_y = 0;

/* ── fb_putchar — draw one character to the GOP framebuffer ───────────── */
static void fb_putchar(char c) {
    boot_fb_t *fb = g_boot.fb;
    if (!fb) return;

    /* newline */
    if (c == '\n') {
        fb_cursor_x = 0;
        fb_cursor_y += 8;
        if (fb_cursor_y + 8 > fb->height) {
            /* simple scroll: move everything up 8 lines */
            uint8_t *pixels = (uint8_t *)fb->base;
            uint64_t row_bytes = fb->pitch;
            uint64_t scroll = 8 * row_bytes;
            for (uint64_t y = 0; y < fb->height - 8; y++) {
                for (uint64_t x = 0; x < row_bytes; x++) {
                    pixels[y * row_bytes + x] = pixels[(y * row_bytes) + x + scroll];
                }
            }
            /* clear bottom 8 lines */
            for (uint64_t y = fb->height - 8; y < fb->height; y++) {
                for (uint64_t x = 0; x < fb->width; x++) {
                    uint32_t *px = (uint32_t *)((uint8_t *)fb->base + y * fb->pitch + x * 4);
                    *px = fb_bg_color;
                }
            }
            fb_cursor_y = fb->height - 8;
        }
        return;
    }
    if (c == '\r') { fb_cursor_x = 0; return; }

    /* only render printable ASCII */
    uint8_t uc = (uint8_t)c;
    if (uc < 0x20 || uc > 0x7E) uc = '?';

    const uint8_t *glyph = &font8x8[(uc - 0x20) * 8];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint64_t px = fb_cursor_x + col;
            uint64_t py = fb_cursor_y + row;
            if (px >= fb->width || py >= fb->height) continue;
            uint32_t *pixel = (uint32_t *)((uint8_t *)fb->base + py * fb->pitch + px * (fb->bpp / 8));
            *pixel = (bits & (0x80 >> col)) ? fb_fg_color : fb_bg_color;
        }
    }
    fb_cursor_x += 8;
    if (fb_cursor_x + 8 > fb->width) {
        fb_cursor_x = 0;
        fb_cursor_y += 8;
    }
}

/* ── fb_puts ──────────────────────────────────────────────────────────── */
static void fb_puts(const char *s) {
    if (!s) return;
    while (*s) fb_putchar(*s++);
}

/* ── fb_clear — fill entire framebuffer with background color ─────────── */
static void fb_clear(void) {
    boot_fb_t *fb = g_boot.fb;
    if (!fb) return;
    for (uint64_t y = 0; y < fb->height; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            uint32_t *pixel = (uint32_t *)((uint8_t *)fb->base + y * fb->pitch + x * (fb->bpp / 8));
            *pixel = fb_bg_color;
        }
    }
    fb_cursor_x = 0;
    fb_cursor_y = 0;
}

/* ── capture current CPU state ────────────────────────────────────────── */
static panic_regs_t capture_regs(void) {
    panic_regs_t r = {0};
    __asm__ volatile (
        "mov %%rax, %0\n\t"
        "mov %%rbx, %1\n\t"
        "mov %%rcx, %2\n\t"
        "mov %%rdx, %3\n\t"
        "mov %%rsi, %4\n\t"
        "mov %%rdi, %5\n\t"
        "mov %%rbp, %6\n\t"
        "mov %%r8,  %7\n\t"
        "mov %%r9,  %8\n\t"
        "mov %%r10, %9\n\t"
        "mov %%r11, %10\n\t"
        "mov %%r12, %11\n\t"
        "mov %%r13, %12\n\t"
        "mov %%r14, %13\n\t"
        "mov %%r15, %14\n\t"
        : "=m"(r.rax), "=m"(r.rbx), "=m"(r.rcx), "=m"(r.rdx),
          "=m"(r.rsi), "=m"(r.rdi), "=m"(r.rbp),
          "=m"(r.r8),  "=m"(r.r9),  "=m"(r.r10), "=m"(r.r11),
          "=m"(r.r12), "=m"(r.r13), "=m"(r.r14), "=m"(r.r15)
    );
    __asm__ volatile ("mov %%cr2, %0" : "=r"(r.cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(r.cr3));
    __asm__ volatile ("pushfq; pop %0" : "=r"(r.rflags));
    /* RIP and RSP are approximated — we're inside this function */
    __asm__ volatile ("mov %%rsp, %0" : "=r"(r.rsp));
    return r;
}

/* ── internal: print register dump to both serial and framebuffer ─────── */
static void dump_regs(const panic_regs_t *r) {
    const char *names[] = {
        "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
        "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15",
        "RIP", "RFLAGS", "CR2", "CR3", NULL
    };
    const uint64_t *vals[] = {
        &r->rax, &r->rbx, &r->rcx, &r->rdx, &r->rsi, &r->rdi, &r->rbp, &r->rsp,
        &r->r8,  &r->r9,  &r->r10, &r->r11, &r->r12, &r->r13, &r->r14, &r->r15,
        &r->rip, &r->rflags, &r->cr2, &r->cr3, NULL
    };

    for (int i = 0; names[i]; i++) {
        kprintf("  %s=0x%016x  ", names[i], (uint32_t)*vals[i]);
        /* also to framebuffer */
        fb_putchar(' '); fb_putchar(' ');
        fb_puts(names[i]);
        fb_puts("=0x");
        /* hex for fb — reuse serial_put_ulong is not available here,
         * so we do a simple inline hex printer */
        {
            char hex[17];
            uint64_t v = *vals[i];
            hex[16] = '\0';
            for (int j = 15; j >= 0; j--) {
                uint8_t nib = v & 0xF;
                hex[j] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
                v >>= 4;
            }
            fb_puts(hex);
        }
        fb_putchar('\n');
        if ((i + 1) % 2 == 0) kprintf("\n\r");
    }
}

/* ── internal: crude stack trace (walk RBP chain, max 16 frames) ─────── */
static void dump_stack_trace(uint64_t rbp) {
    kprintf("\n\rStack trace:\n\r");
    fb_puts("\nStack trace:\n");

    for (int i = 0; i < 16; i++) {
        if (rbp == 0) break;
        uint64_t rip = *(uint64_t *)(rbp + 8);
        kprintf("  [%d] 0x%016x\n\r", i, (uint32_t)rip);
        fb_puts("  [");
        fb_putchar('0' + i);
        fb_puts("] 0x");
        {
            char hex[17];
            uint64_t v = rip;
            hex[16] = '\0';
            for (int j = 15; j >= 0; j--) {
                uint8_t nib = v & 0xF;
                hex[j] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
                v >>= 4;
            }
            fb_puts(hex);
        }
        fb_putchar('\n');
        rbp = *(uint64_t *)rbp;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  kpanic — fatal kernel error, never returns
 * ════════════════════════════════════════════════════════════════════════ */
NO_RETURN void kpanic(const char *fmt, ...) {
    interrupts_disable();

    /* TODO: SMP — broadcast IPI HALT to all other cores */

    /* Capture register state */
    panic_regs_t regs = capture_regs();

    /* ── Output to serial (COM1) ───────────────────────────────────── */
    kprintf("\n\r!!! KERNEL PANIC !!!\n\r");

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    /* We can't reuse kprintf's va_list easily, so we do a simple
     * string-only pass for the panic message.  For a full-featured
     * version we'd add a kvprintf helper. */
    kprintf("%s", fmt);  /* best-effort; complex formats may not expand */
    __builtin_va_end(ap);

    kprintf("\n\r");

    /* ── Output to framebuffer ─────────────────────────────────────── */
    fb_clear();
    fb_puts("!!! KERNEL PANIC !!!\n\n");
    fb_puts(fmt);
    fb_puts("\n\n");

    /* ── Register dump ─────────────────────────────────────────────── */
    kprintf("Register dump:\n\r");
    fb_puts("Register dump:\n");
    dump_regs(&regs);

    /* ── Stack trace ───────────────────────────────────────────────── */
    dump_stack_trace(regs.rbp);

    /* ── Halt this core forever ────────────────────────────────────── */
    cpu_halt();
    __builtin_unreachable();
}

/* ════════════════════════════════════════════════════════════════════════
 *  kpanic_regs — panic with pre-captured register snapshot (from IDT)
 * ════════════════════════════════════════════════════════════════════════ */
NO_RETURN void kpanic_regs(const panic_regs_t *regs, const char *fmt, ...) {
    interrupts_disable();

    /* TODO: SMP — broadcast IPI HALT */

    kprintf("\n\r!!! KERNEL PANIC !!!\n\r");
    kprintf("%s", fmt);
    kprintf("\n\r");

    fb_clear();
    fb_puts("!!! KERNEL PANIC !!!\n\n");
    fb_puts(fmt);
    fb_puts("\n\n");

    dump_regs(regs);
    dump_stack_trace(regs->rbp);

    cpu_halt();
    __builtin_unreachable();
}
