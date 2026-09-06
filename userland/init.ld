/* VoidOS — init.elf linker script
 *
 * Minimal ELF64 executable:
 * - Entry point: _start (from crt0.S)
 * - Base address: 0x400000 (USER_CODE_BASE, ELF standard)
 * - Segments: PT_LOAD with R+X, R, R+W, BSS
 * - Page-aligned for clean PT_LOAD boundaries
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    /* ── Read-Execute Segment (.text) ─────────────────────────────── */
    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    /* ── Read-Only Segment (.rodata) ──────────────────────────────── */
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    /* ── Read-Write Segment (.data) ───────────────────────────────── */
    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    /* ── Zero-Initialized Segment (.bss) ─────────────────────────── */
    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }
}
