; VoidOS — Ring 3 test programs (raw position-independent blobs)
;
; These are copied byte-for-byte into a fresh user address space at
; USER_CODE_BASE by process_spawn_user(), so they must not rely on load-time
; relocation: only RIP-relative references and immediate constants appear
; below.  They live in .rodata purely so the kernel can read them; they are
; never executed from kernel space.
;
; ponytail: hand-written blobs, no ELF headers. The ELF loader in Phase 5
; replaces process_spawn_user() and these become ordinary programs.

bits 64

%define SYS_write       1
%define SYS_sched_yield 24
%define SYS_getpid      39
%define SYS_exit        60

; A user VA that both test processes touch, to prove the pages are private.
; USER_STACK_TOP (0x0000700000000000) - 0x8000, inside the mapped stack range.
%define PROBE 0x00006FFFFFFF8000

section .rodata

; ══════════════════════════════════════════════════════════════════════
;  user_prog — exercises write/getpid/yield/exit and proves isolation
; ══════════════════════════════════════════════════════════════════════
global user_prog_start
global user_prog_end
user_prog_start:
    mov     rax, SYS_getpid
    syscall
    mov     rbx, rax                ; rbx = our pid, kept across syscalls

    ; ── write "[uN] hi" ───────────────────────────────────────────────
    sub     rsp, 32
    mov     byte [rsp+0], '['
    mov     byte [rsp+1], 'u'
    mov     rax, rbx
    add     al, '0'
    mov     [rsp+2], al
    mov     byte [rsp+3], ']'
    mov     byte [rsp+4], ' '
    mov     byte [rsp+5], 'h'
    mov     byte [rsp+6], 'i'
    mov     byte [rsp+7], 10
    mov     rax, SYS_write
    mov     rdi, 1
    mov     rsi, rsp
    mov     rdx, 8
    syscall
    add     rsp, 32

    ; ── stamp our pid at PROBE, then let the sibling run ──────────────
    mov     rax, PROBE
    mov     [rax], rbx

    mov     r12, 4                  ; r12: callee-saved, survives syscall
.yield_loop:
    mov     rax, SYS_sched_yield
    syscall
    dec     r12
    jnz     .yield_loop

    ; ── PROBE must still hold our own pid ─────────────────────────────
    mov     rax, PROBE
    mov     rdx, [rax]
    cmp     rdx, rbx
    jne     .leaked

    sub     rsp, 32
    mov     byte [rsp+0], '['
    mov     byte [rsp+1], 'u'
    mov     rax, rbx
    add     al, '0'
    mov     [rsp+2], al
    mov     byte [rsp+3], ']'
    mov     byte [rsp+4], ' '
    mov     byte [rsp+5], 'O'
    mov     byte [rsp+6], 'K'
    mov     byte [rsp+7], 10
    mov     rax, SYS_write
    mov     rdi, 1
    mov     rsi, rsp
    mov     rdx, 8
    syscall
    add     rsp, 32

    mov     rax, SYS_exit
    mov     rdi, rbx                ; exit(pid) so the parent can check it
    syscall

.leaked:
    mov     rax, SYS_write
    mov     rdi, 1
    lea     rsi, [rel .leak_msg]
    mov     rdx, .leak_len
    syscall
    mov     rax, SYS_exit
    mov     rdi, 99
    syscall

.leak_msg: db '[u?] LEAK: saw another process memory', 10
.leak_len equ $ - .leak_msg
user_prog_end:

; ══════════════════════════════════════════════════════════════════════
;  user_bad — hands the kernel a kernel pointer, then dereferences one
; ══════════════════════════════════════════════════════════════════════
global user_bad_start
global user_bad_end
user_bad_start:
    ; write() with a kernel-half buffer must be rejected with -EFAULT (-14),
    ; not silently copied out of kernel memory.
    mov     rax, SYS_write
    mov     rdi, 1
    mov     rsi, 0xFFFFFFFF80000000
    mov     rdx, 64
    syscall

    cmp     rax, -14
    jne     .unexpected

    mov     rax, SYS_write
    mov     rdi, 1
    lea     rsi, [rel .ok_msg]
    mov     rdx, .ok_len
    syscall

    ; Now fault for real: read a kernel address from Ring 3.  The USER bit
    ; is clear on kernel pages, so this is a #PF the kernel must convert
    ; into "process killed", not a panic.
    mov     rax, 0xFFFFFFFF80000000
    mov     rdx, [rax]

    mov     rax, SYS_exit           ; never reached
    mov     rdi, 1
    syscall

.unexpected:
    mov     rax, SYS_write
    mov     rdi, 1
    lea     rsi, [rel .bad_msg]
    mov     rdx, .bad_len
    syscall
    mov     rax, SYS_exit
    mov     rdi, 98
    syscall

.ok_msg:  db '[bad] write(kernel ptr) rejected with EFAULT', 10
.ok_len  equ $ - .ok_msg
.bad_msg: db '[bad] FAIL: kernel pointer was not rejected', 10
.bad_len equ $ - .bad_msg
user_bad_end:
