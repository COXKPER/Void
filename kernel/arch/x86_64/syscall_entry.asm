; VoidOS — SYSCALL entry stub
;
; SYSCALL does NOT switch stacks and does NOT push anything.  It only:
;   RCX <- return RIP
;   R11 <- return RFLAGS
;   RIP <- IA32_LSTAR
;   CS  <- STAR[47:32]        SS <- STAR[47:32] + 8
;   RFLAGS &= ~IA32_FMASK     (we mask IF, so we enter with interrupts off)
;
; So this stub must switch to the calling thread's kernel stack itself and
; then build an isr_frame_t byte-identical to the one isr_common builds.
; Returning through IRETQ (not SYSRET) keeps one single return path, which
; is what lets the scheduler preempt a thread while it sits in a syscall.
;
; RCX and R11 are architecturally clobbered by SYSCALL; the ABI documents
; them as caller-saved, so restoring the return RIP/RFLAGS into them on the
; way out is harmless.

bits 64

%define GDT_SEL_UDATA3  0x1B      ; user data, RPL 3
%define GDT_SEL_UCODE3  0x23      ; user code, RPL 3
%define SYSCALL_FRAME_VECTOR 0x80

extern syscall_dispatch

section .data
; Kernel stack top for the thread currently on this CPU.  Single-CPU only;
; SMP will move this into per-CPU storage reached through swapgs/GS.
global syscall_kernel_rsp
syscall_kernel_rsp: dq 0
; Scratch slot for the user RSP during the stack switch.  Safe because IF is
; cleared by FMASK on entry, so nothing can interleave before we push it.
syscall_user_rsp:   dq 0

section .text
global syscall_entry
syscall_entry:
    mov [rel syscall_user_rsp], rsp
    mov rsp, [rel syscall_kernel_rsp]

    ; ── IRETQ trailer (pushed in the order the CPU would) ───────────
    push qword GDT_SEL_UDATA3            ; ss
    push qword [rel syscall_user_rsp]    ; rsp  (user stack)
    push r11                             ; rflags (saved by SYSCALL)
    push qword GDT_SEL_UCODE3            ; cs
    push rcx                             ; rip  (saved by SYSCALL)

    ; ── vector / error_code, matching the ISR stubs ──────────────────
    push qword 0                         ; error_code
    push qword SYSCALL_FRAME_VECTOR      ; vector = 0x80

    ; ── GPRs, same order as isr_common ───────────────────────────────
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

    mov rdi, rsp
    call syscall_dispatch
    mov rsp, rax                 ; may be another thread's frame

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

    add rsp, 16                  ; drop vector + error_code
    iretq
