/* VoidOS — link script for the dynamic linker (ld-void.so)
 *
 * The rtld is a *statically linked ET_EXEC* at a fixed address: it must be
 * loadable with no linker of its own.  0x400000 is the same base every other
 * static test uses, which is safe because the PIE it relocates lives at
 * ELF_DYN_BASE (0x40000000) — 1 GiB above, no overlap. */
ENTRY(_rtld_start)

SECTIONS {
    . = 0x400000;

    .text : ALIGN(4K) { *(.text .text.*) }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) { *(.bss .bss.* COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
