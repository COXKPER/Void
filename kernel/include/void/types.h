/* VoidOS — base kernel types
 * No <stdlib.h> / <stdio.h> — bare-metal only.
 * All kernel code includes this instead of host libc headers.
 */
#ifndef VOID_TYPES_H
#define VOID_TYPES_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── status / error codes ─────────────────────────────────────────────── */
typedef enum {
    VOID_OK         =  0,
    VOID_ERR        = -1,
    VOID_ERR_NOMEM  = -2,
    VOID_ERR_INVAL  = -3,
    VOID_ERR_BUSY   = -4,
    VOID_ERR_NOENT  = -5,
} void_status_t;

/* ── compiler hints ───────────────────────────────────────────────────── */
#define ALWAYS_INLINE   __attribute__((always_inline)) inline
#define NO_RETURN       __attribute__((noreturn))
#define PACKED          __attribute__((packed))
#define ALIGNED(n)      __attribute__((aligned(n)))
#define UNUSED          __attribute__((unused))
#define SECTION(s)      __attribute__((section(s)))

/* ── port I/O stubs (full implementation in arch/x86_64/io.h later) ──── */
static ALWAYS_INLINE void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static ALWAYS_INLINE void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static ALWAYS_INLINE void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ── I/O delay (post-outb wait on slow legacy devices) ────────────────── */
static ALWAYS_INLINE void io_delay(void) {
    outb(0x80, 0);
}

/* ── interrupt flag helpers ───────────────────────────────────────────── */
static ALWAYS_INLINE void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static ALWAYS_INLINE void interrupts_disable(void) { __asm__ volatile ("cli"); }

static ALWAYS_INLINE bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

/* ── halt ─────────────────────────────────────────────────────────────── */
static ALWAYS_INLINE void cpu_halt(void) {
    __asm__ volatile ("cli; hlt");
    __builtin_unreachable();
}

#endif /* VOID_TYPES_H */
