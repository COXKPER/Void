; VoidOS — x86_64 Limine entry point
; Limine boots the kernel in 64-bit long mode with a basic PML4 already set up.
; The kernel is a relocatable ELF64; Limine loads it above 0xFFFF800000000000
; and passes control to _start with no arguments on the stack.
;
; This file:
;   1. Places Limine request markers (start / end)
;   2. Declares all Limine feature requests the kernel relies on
;   3. Provides _start: sets up a 64 KiB stack inside BSS, zeroes BSS,
;      then calls the C kernel_main()
;
; CRITICAL: Each Limine request struct has the layout:
;   id[4]     — 4 × uint64_t = 32 bytes (common magic + feature ID)
;   revision  — uint64_t      = 8 bytes  (request revision, 0 for now)
;   response  — uint64_t      = 8 bytes  (filled by Limine, initially 0)
;   [+ extra fields for some requests like module_request]

bits 64
section .limine_requests_start
    dq 0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0x785c6ed015d3e316, 0x181e920a7852b9d9

section .limine_requests

; ── Base revision (special format: 3 qwords only, NOT a regular request) ─
; Revision 3 = current Limine v9.x API revision (used by all modern kernels)
align 8
global limine_base_revision
limine_base_revision:
    dq 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3

; ── Bootloader info ──────────────────────────────────────────────────────
; ID: LIMINE_BOOTLOADER_INFO_REQUEST = { common magic, 0xf550385a59e6a6e1, 0x855c7d924a1dc1b7 }
align 8
global limine_bootloader_info_request
limine_bootloader_info_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b   ; id[0..1] common magic
    dq 0xf550385a59e6a6e1, 0x855c7d924a1dc1b7   ; id[2..3] feature ID
    dq 0                                           ; revision = 0
    dq 0                                           ; response (filled by Limine)

; ── HHDM (Higher-Half Direct Map) offset ─────────────────────────────────
; ID: LIMINE_HHDM_REQUEST = { common magic, 0x48dcf1cb8ad2b852, 0x63984e959a98244b }
align 8
global limine_hhdm_request
limine_hhdm_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b   ; id[0..1] common magic
    dq 0x48dcf1cb8ad2b852, 0x63984e959a98244b   ; id[2..3] feature ID
    dq 0                                           ; revision = 0
    dq 0                                           ; response (filled by Limine)

; ── Framebuffer ──────────────────────────────────────────────────────────
; ID: LIMINE_FRAMEBUFFER_REQUEST = { common magic, 0x9d5827dcd881dd75, 0xa3148604f6fab11b }
align 8
global limine_framebuffer_request
limine_framebuffer_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b
    dq 0x9d5827dcd881dd75, 0xa3148604f6fab11b
    dq 0                                           ; revision = 0
    dq 0                                           ; response

; ── Memory map ───────────────────────────────────────────────────────────
; ID: LIMINE_MEMMAP_REQUEST = { common magic, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }
align 8
global limine_memmap_request
limine_memmap_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b
    dq 0x67cf3d9d378a806f, 0xe304acdfc50c3c62
    dq 0                                           ; revision = 0
    dq 0                                           ; response

; ── Modules (userland.img, drivers) ──────────────────────────────────────
; ID: LIMINE_MODULE_REQUEST = { common magic, 0x3e7e279702be32af, 0xca1c4f3bd1280cee }
; struct: id[4] + revision + response + internal_module_count + internal_modules
align 8
global limine_module_request
limine_module_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b
    dq 0x3e7e279702be32af, 0xca1c4f3bd1280cee
    dq 0                                           ; revision = 0
    dq 0                                           ; response
    dq 0                                           ; internal_module_count
    dq 0                                           ; internal_modules

; ── RSDP (ACPI tables) ──────────────────────────────────────────────────
; ID: LIMINE_RSDP_REQUEST = { common magic, 0xc5e77b6b397e7b43, 0x27637845accdcf3c }
; struct: id[4] + revision + response
align 8
global limine_rsdp_request
limine_rsdp_request:
    dq 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b
    dq 0xc5e77b6b397e7b43, 0x27637845accdcf3c
    dq 0                                           ; revision = 0
    dq 0                                           ; response

section .limine_requests_end
    dq 0xadc0e0531bb10d03, 0x9572709f31764c62

; ════════════════════════════════════════════════════════════════════════
;  BSS symbols from linker script
; ════════════════════════════════════════════════════════════════════════
extern __bss_start
extern __bss_end
extern __boot_stack_top

; C entry point
extern kernel_main

; ════════════════════════════════════════════════════════════════════════
;  Entry point — called by Limine with interrupts disabled, no stack
; ════════════════════════════════════════════════════════════════════════
section .text
global _start
_start:
    ; ── Zero BSS ─────────────────────────────────────────────────────
    ; The linker guarantees __bss_start and __bss_end are page-aligned.
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    ; ── Set up boot stack (64 KiB, embedded in BSS by linker) ────────
    lea rsp, [rel __boot_stack_top]
    and rsp, ~0xF              ; align to 16 bytes (System V AMD64 ABI)

    ; ── Jump to C ────────────────────────────────────────────────────
    call kernel_main

    ; ── kernel_main should never return; halt if it does ─────────────
.halt:
    cli
    hlt
    jmp .halt
