; VoidOS — ISR stubs for x86_64 IDT
; Vectors 0–31:  CPU exceptions
; Vectors 32–47: Hardware IRQs (LAPIC / remapped PIC)
; Vector 255:    LAPIC spurious
;
; CPU exceptions that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
; All others: we push a dummy 0 so the stack frame is uniform.

bits 64

extern isr_dispatch      ; C handler: void isr_dispatch(isr_frame_t *frame)

; ── macro: stub WITHOUT error code ──────────────────────────────────────
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push 0              ; dummy error code
    push %1             ; vector number
    jmp isr_common
%endmacro

; ── macro: stub WITH error code (already pushed by CPU) ─────────────────
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push %1             ; vector number (error code already on stack)
    jmp isr_common
%endmacro

; ── CPU exceptions (vectors 0–31) ───────────────────────────────────────
ISR_NOERR 0    ; #DE  Divide Error
ISR_NOERR 1    ; #DB  Debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP  Breakpoint
ISR_NOERR 4    ; #OF  Overflow
ISR_NOERR 5    ; #BR  Bound Range Exceeded
ISR_NOERR 6    ; #UD  Invalid Opcode
ISR_NOERR 7    ; #NM  Device Not Available
ISR_ERR   8    ; #DF  Double Fault
ISR_NOERR 9    ; Coprocessor Segment Overrun (legacy)
ISR_ERR   10   ; #TS  Invalid TSS
ISR_ERR   11   ; #NP  Segment Not Present
ISR_ERR   12   ; #SS  Stack-Segment Fault
ISR_ERR   13   ; #GP  General Protection Fault
ISR_ERR   14   ; #PF  Page Fault
ISR_NOERR 15   ; Reserved
ISR_NOERR 16   ; #MF  x87 Floating-Point
ISR_ERR   17   ; #AC  Alignment Check
ISR_NOERR 18   ; #MC  Machine Check
ISR_NOERR 19   ; #XM  SIMD Floating-Point
ISR_NOERR 20   ; #VE  Virtualization
ISR_ERR   21   ; #CP  Control Protection
ISR_NOERR 22   ; Reserved
ISR_NOERR 23   ; Reserved
ISR_NOERR 24   ; Reserved
ISR_NOERR 25   ; Reserved
ISR_NOERR 26   ; Reserved
ISR_NOERR 27   ; Reserved
ISR_NOERR 28   ; Reserved
ISR_ERR   29   ; #HV  Hypervisor Injection
ISR_ERR   30   ; #SX  Security Exception
ISR_NOERR 31   ; Reserved

; ── Hardware IRQs (vectors 32–47) ───────────────────────────────────────
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; ── LAPIC spurious (vector 255) ─────────────────────────────────────────
ISR_NOERR 255

; ── common handler: save GPRs, call C, restore, iretq ───────────────────
isr_common:
    ; At this point stack has: [SS RSP RFLAGS CS RIP error_code vector]
    ; Save all GPRs to form isr_frame_t
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass pointer to isr_frame_t as first arg (System V: RDI)
    mov rdi, rsp
    call isr_dispatch
    ; isr_dispatch returns (possibly new) RSP in RAX — use it
    mov rsp, rax

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop vector and error code
    add rsp, 16

    iretq

; ── stub table (array of 48 function pointers + spurious) ────────────────
section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr_stub_%+i
%assign i i+1
%endrep

global isr_stub_spurious
isr_stub_spurious:
    dq isr_stub_255
