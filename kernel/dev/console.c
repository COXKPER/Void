/* VoidOS — Console output routing implementation
 *
 * Unified kprintf that routes to VGA framebuffer or COM1 serial based on
 * kernel boot parameters. Reuses existing formatting logic and output backends.
 */
#include <dev/console.h>
#include <dev/serial.h>
#include <void/boot.h>
#include <void/types.h>

/* ── module state ────────────────────────────────────────────────────────── */
static kout_backend_t kout_backend = KOUT_SERIAL;  /* default: serial (diagnostic) */

/* ── VGA framebuffer text output (reused from kpanic) ──────────────────────
 * Minimal implementation mirroring kpanic.c logic. */

static uint32_t console_fb_cursor_x = 0;
static uint32_t console_fb_cursor_y = 0;
static uint32_t console_fb_fg_color = 0xFFFFFF;   /* white */
static uint32_t console_fb_bg_color = 0x000000;   /* black (not red like panic) */

/* Embedded 8x8 bitmap font (same as kpanic) */
#include "../lib/font8x8.inc"

static void console_fb_putchar(char c) {
    boot_fb_t *fb = g_boot.fb;
    if (!fb) return;

    if (c == '\n') {
        console_fb_cursor_x = 0;
        console_fb_cursor_y += 8;
        if (console_fb_cursor_y + 8 > fb->height) {
            /* scroll up */
            uint8_t *pixels = (uint8_t *)fb->base;
            uint64_t row_bytes = fb->pitch;
            uint64_t scroll = 8 * row_bytes;
            for (uint64_t y = 0; y < fb->height - 8; y++) {
                for (uint64_t x = 0; x < row_bytes; x++) {
                    pixels[y * row_bytes + x] = pixels[y * row_bytes + x + scroll];
                }
            }
            for (uint64_t y = fb->height - 8; y < fb->height; y++) {
                for (uint64_t x = 0; x < fb->width; x++) {
                    uint32_t *px = (uint32_t *)((uint8_t *)fb->base + y * fb->pitch + x * 4);
                    *px = console_fb_bg_color;
                }
            }
            console_fb_cursor_y = fb->height - 8;
        }
        return;
    }

    /* carriage return (for \r\n sequences) */
    if (c == '\r') {
        console_fb_cursor_x = 0;
        return;
    }

    if (console_fb_cursor_x + 8 > fb->width) {
        console_fb_putchar('\n');
    }

    /* draw glyph */
    uint8_t ascii = (uint8_t)c;
    if (ascii >= sizeof(font8x8) / 8) ascii = '?';

    const uint8_t *glyph = &font8x8[ascii * 8];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (1 << col)) ? console_fb_fg_color : console_fb_bg_color;
            uint64_t px_x = console_fb_cursor_x + col;
            uint64_t px_y = console_fb_cursor_y + row;
            if (px_x < fb->width && px_y < fb->height) {
                uint32_t *px = (uint32_t *)((uint8_t *)fb->base + px_y * fb->pitch + px_x * 4);
                *px = color;
            }
        }
    }

    console_fb_cursor_x += 8;
}

/* ── shared formatting logic (extracted from serial.c) ─────────────────────
 * These helpers work with either backend. */

typedef void (*putchar_fn)(char);

static void put_ulong(putchar_fn out, uint64_t val, int base, int width, bool zero_pad) {
    char buf[64];
    static const char digits[] = "0123456789abcdef";
    int pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        while (val) {
            buf[pos++] = digits[val % (unsigned)base];
            val /= (unsigned)base;
        }
    }

    while (pos < width) {
        buf[pos++] = zero_pad ? '0' : ' ';
    }

    for (int i = pos - 1; i >= 0; i--) {
        out(buf[i]);
    }
}

static void put_long(putchar_fn out, int64_t val) {
    if (val < 0) {
        out('-');
        val = -val;
    }
    put_ulong(out, (uint64_t)val, 10, 0, false);
}

static void puts_generic(putchar_fn out, const char *s) {
    if (!s) s = "(null)";
    while (*s) {
        out(*s++);
    }
}

/* ── console routing ─────────────────────────────────────────────────────── */

void console_init(void) {
    /* TODO: parse boot parameters from Limine/bootloader.
     * For now, default to KOUT_SERIAL.
     * In future, check for kout=vga in command line. */
    kout_backend = KOUT_SERIAL;
}

kout_backend_t console_get_backend(void) {
    return kout_backend;
}

void console_set_backend(kout_backend_t backend) {
    kout_backend = backend;
}

/* ── kprintf — unified implementation ────────────────────────────────────── */
void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    /* Select the output function based on backend */
    putchar_fn out = (kout_backend == KOUT_VGA) ? console_fb_putchar : serial_putchar;

    while (*fmt) {
        if (*fmt != '%') {
            out(*fmt++);
            continue;
        }
        fmt++;   /* skip '%' */

        /* flags */
        bool zero_pad = false;
        int  width    = 0;
        if (*fmt == '0') { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 'd': case 'i':
            put_long(out, __builtin_va_arg(ap, int64_t));
            break;
        case 'u':
            put_ulong(out, __builtin_va_arg(ap, uint64_t), 10, width, zero_pad);
            break;
        case 'x':
            put_ulong(out, __builtin_va_arg(ap, uint64_t), 16, width, zero_pad);
            break;
        case 'p':
            puts_generic(out, "0x");
            put_ulong(out, __builtin_va_arg(ap, uint64_t), 16, 16, true);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            puts_generic(out, s);
            break;
        }
        case 'c':
            out((char)__builtin_va_arg(ap, int));
            break;
        case '%':
            out('%');
            break;
        default:
            out('%');
            out(*fmt);
            break;
        }
        fmt++;
    }

    __builtin_va_end(ap);
}
