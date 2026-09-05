/* VoidOS — Serial COM1 (UART 8250/16550) implementation
 * No stdio, no stdarg — we use __builtin_va_* for variadic support.
 */
#include <dev/serial.h>
#include <void/types.h>

/* ── UART register offsets ────────────────────────────────────────────── */
#define UART_THR   0   /* Transmitter Holding Register (write) */
#define UART_RBR   0   /* Receiver Buffer Register  (read)  */
#define UART_IER   1   /* Interrupt Enable Register         */
#define UART_FCR   2   /* FIFO Control Register (write)     */
#define UART_LCR   3   /* Line Control Register             */
#define UART_MCR   4   /* Modem Control Register            */
#define UART_LSR   5   /* Line Status Register              */
#define UART_DLL   0   /* Divisor Latch Low  (LCR.DLAB=1)  */
#define UART_DLH   1   /* Divisor Latch High (LCR.DLAB=1)  */

#define UART_LSR_TX_EMPTY  0x20   /* bit 5: THR empty */

/* ── serial_init — configure COM1 at 115200 8N1 ──────────────────────── */
void serial_init(void) {
    outb(COM1 + UART_IER, 0x00);    /* disable all interrupts       */
    outb(COM1 + UART_LCR, 0x80);    /* enable DLAB                  */
    outb(COM1 + UART_DLL, 0x01);    /* divisor lo: 115200 baud      */
    outb(COM1 + UART_DLH, 0x00);    /* divisor hi                   */
    outb(COM1 + UART_LCR, 0x03);    /* 8 bits, no parity, 1 stop    */
    outb(COM1 + UART_FCR, 0xC7);    /* enable FIFO, clear, 14-byte  */
    outb(COM1 + UART_MCR, 0x0B);    /* RTS/DSR set                  */
    outb(COM1 + UART_IER, 0x00);    /* keep interrupts off          */
}

/* ── serial_putchar — blocking write of a single byte ─────────────────── */
void serial_putchar(char c) {
    /* Spin until THR is empty */
    while (!(inb(COM1 + UART_LSR) & UART_LSR_TX_EMPTY)) {
        __asm__ volatile ("pause");
    }
    outb(COM1 + UART_THR, (uint8_t)c);
}

/* ── serial_puts — write a null-terminated string ─────────────────────── */
void serial_puts(const char *s) {
    if (!s) return;
    while (*s) {
        serial_putchar(*s++);
    }
}

/* ── internal: print unsigned long in given base ──────────────────────── */
static void serial_put_ulong(uint64_t val, int base, int width, bool zero_pad) {
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

    /* pad to width */
    while (pos < width) {
        buf[pos++] = zero_pad ? '0' : ' ';
    }

    /* reverse print */
    for (int i = pos - 1; i >= 0; i--) {
        serial_putchar(buf[i]);
    }
}

/* ── internal: print signed long ──────────────────────────────────────── */
static void serial_put_long(int64_t val) {
    if (val < 0) {
        serial_putchar('-');
        val = -val;
    }
    serial_put_ulong((uint64_t)val, 10, 0, false);
}

/* ── kprintf — minimal kernel printf ──────────────────────────────────── */
void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            serial_putchar(*fmt++);
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
            serial_put_long(__builtin_va_arg(ap, int64_t));
            break;
        case 'u':
            serial_put_ulong(__builtin_va_arg(ap, uint64_t), 10, width, zero_pad);
            break;
        case 'x':
            serial_put_ulong(__builtin_va_arg(ap, uint64_t), 16, width, zero_pad);
            break;
        case 'p':
            serial_puts("0x");
            serial_put_ulong(__builtin_va_arg(ap, uint64_t), 16, 16, true);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            serial_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            serial_putchar((char)__builtin_va_arg(ap, int));
            break;
        case '%':
            serial_putchar('%');
            break;
        default:
            serial_putchar('%');
            serial_putchar(*fmt);
            break;
        }
        fmt++;
    }

    __builtin_va_end(ap);
}
