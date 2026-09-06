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
#define UART_LSR_RX_READY  0x01   /* bit 0: data ready in RBR */

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

/* ── serial_getchar — non-blocking read of a single byte ────────────────
 * Returns the character if available, or -1 if no data ready. */
int serial_getchar(void) {
    uint8_t lsr = inb(COM1 + UART_LSR);
    if (!(lsr & UART_LSR_RX_READY)) {
        return -1;  /* no data available */
    }
    return (int)(uint8_t)inb(COM1 + UART_RBR);
}

