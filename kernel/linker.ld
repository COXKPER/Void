/* VoidOS kernel linker script — x86_64 higher-half, PIE/ET_DYN
 * Kernel virtual base: 0xFFFFFFFF80000000 (fixed -2GiB slot, compatible
 * with Limine's default HHDM and executable relocation above 0xFFFF800000000000)
 * Physical load address is determined by Limine; the kernel is a PIE
 * ELF64 (ET_DYN) and Limine will slide it at boot.
 *
 * IMPORTANT: Limine request marker sections (.limine_requests_start,
 * .limine_requests, .limine_requests_end) MUST remain as separate
 * output sections with their original names — Limine scans the ELF
 * section headers to find them.  Do NOT merge them into .text.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Limine loads the kernel above 0xFFFF800000000000 by default.
 * Using 0xFFFFFFFF80000000 keeps us in the canonical higher-half and
 * leaves the lower 2GiB of kernel virtual space free for a direct-map.
 */
KERNEL_VBASE = 0xFFFFFFFF80000000;

PHDRS {
    requests PT_LOAD FLAGS(6); /* R+W — Limine requests (separate segment) */
    text     PT_LOAD FLAGS(5); /* R+X */
    rodata   PT_LOAD FLAGS(4); /* R   */
    data     PT_LOAD FLAGS(6); /* R+W */
    bss      PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS {
    . = KERNEL_VBASE;

    /* ── Limine request markers (MUST be separate named sections) ─────── */
    /* CRITICAL: These MUST be in a READ-WRITE segment! Limine writes the
     * response pointer (at offset +0x28 in each request struct) in-place.
     * If mapped R/O (e.g., in :text), Limine cannot write and all responses
     * remain NULL.  See: https://github.com/Limine-Bootloader/limine-protocol  */
    .limine_requests_start : {
        KEEP(*(.limine_requests_start))
    } : requests

    .limine_requests : {
        KEEP(*(.limine_requests))
    } : requests

    .limine_requests_end : {
        KEEP(*(.limine_requests_end))
    } : requests

    /* ── text ─────────────────────────────────────────────────────────── */
    .text ALIGN(0x1000) : {
        *(.text .text.*)
    } : text

    /* ── read-only data ───────────────────────────────────────────────── */
    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
    } : rodata

    /* ── initialized data ─────────────────────────────────────────────── */
    .data ALIGN(0x1000) : {
        *(.data .data.*)
    } : data

    /* ── BSS + early boot stack ────────────────────────────────────────── */
    .bss ALIGN(0x1000) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;

        /* 64 KiB early boot stack (embedded in BSS so it's mapped RW).
         * We advance the location counter to actually reserve space
         * in the segment, then mark the top as the stack pointer target.
         * entry.asm uses this area until PMM/VMM allocate a proper
         * per-CPU kernel stack.                               */
        . = ALIGN(16);
        . = . + 0x10000;                /* reserve 64 KiB */
        __boot_stack_top = .;
    } : bss

    /* ── kernel end guard (PMM will carve from here upwards) ─────────── */
    . = ALIGN(0x1000);
    __kernel_end = .;

    /* discard DWARF / note sections that bloat the image */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
