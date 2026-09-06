# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**VoidOS** is a custom 64-bit x86_64 operating system implementing a **Hybrid-Microkernel** architecture. The kernel runs in Ring 0 with minimal responsibilities (PMM, VMM, Scheduler, SysCall, IPC). All drivers, filesystems, and network services run as independent Ring 3 modules.

### Architecture Constraints (MUST follow)

- **Boot protocol**: Limine bootloader v9.x. Kernel receives CPU already in **64-bit Long Mode** with PAE/PML4 paging active. No 16-bit legacy code or manual mode switching.
- **Binary format**: ELF64 for kernel + all userland/modules. Flat binary (.bin) only for stage-1 bootloader if needed.
- **Compiler**: Host `gcc` with `-ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel`. Target is effectively x86_64-unknown-none-elf.
- **ABI**: System V AMD64 — RDI/RSI/RDX/RCX/R8/R9 for parameters, RAX for syscall numbers.
- **Memory layout**: Higher-Half Kernel Mapping. Userland = lower-half (`0x0000_0000_0000_0000` – `0x0000_7FFF_FFFF_FFFF`), Kernel = higher-half (`0xFFFF_8000_0000_0000` – `0xFFFF_FFFF_FFFF_FFFF`). Kernel virtual base in linker script: `0xFFFFFFFF80000000`.
- **Syscalls**: Native `SYSCALL`/`SYSRET` only (no INT 0x80). MSRs: IA32_STAR, IA32_LSTAR→`syscall_entry`, IA32_FMASK.
- **Userland handover**: `IRETQ` to Ring 3 with CS/SS selectors at RPL=3, IF=1.
- **Scheduler**: Preemptive Round-Robin driven by LAPIC Timer interrupt (10–100Hz). TCB stores full register state + CR3.
- **IPC**: VFS-based named nodes under `/Devices/IPC/`. Kernel-managed circular ring buffers. `sys_read()` on empty node → process BLOCKED.
- **Panic**: `kpanic()` only for unrecoverable Ring-0 errors. Ring-3 errors kill the process, never panic. On panic: CLI, (SMP: IPI halt), dump registers+stack trace to serial COM1 + framebuffer, HLT loop.

## Build Commands

```bash
make              # Build ISO (build/voidos.iso)
make run          # Run in QEMU with BIOS boot, serial to stdio
make run-uefi     # Run in QEMU with UEFI (OVMF) boot
make clean        # Remove build/ directory
```

The Makefile compiles `.c` files with gcc (freestanding C23), `.asm` files with nasm (elf64), links with GNU ld using `kernel/linker.ld`, then packages an ISO via xorriso with Limine bootloader files from `./limine/`.

## Directory Structure

```
kernel/
├── arch/x86_64/       # Architecture-specific code
│   ├── entry.asm      # _start, Limine request markers, BSS zero, stack setup
│   ├── isr_stubs.asm  # ISR stub macros + isr_common + stub table (vectors 0–47, 255)
│   ├── syscall_entry.asm # SYSCALL stub: stack switch, isr_frame_t build, IRETQ return
│   ├── gdt.c          # GDT + TSS init, gdt_set_kernel_stack (TSS.RSP0)
│   └── idt.c          # IDT init + IRQ dispatch + exception handler (user faults kill proc)
├── boot/
│   └── boot.c         # Parses Limine responses into global g_boot struct
├── dev/
│   ├── serial.c       # COM1 UART driver + kprintf()
│   └── lapic.c        # LAPIC timer + PIC disable + calibration
├── include/
│   ├── arch/x86_64/   # Arch headers (gdt.h, idt.h)
│   ├── boot/
│   │   └── limine.h   # Vendored Limine v9.6.7 protocol header (DO NOT EDIT)
│   ├── dev/           # Device headers (serial.h, lapic.h)
│   ├── mm/            # Memory management headers (pmm.h, vmm.h, kheap.h)
│   ├── proc/          # Process abstraction (process.h)
│   ├── sched/         # Scheduler headers (sched.h)
│   ├── syscall/       # Syscall ABI + numbers + errno (syscall.h)
│   └── void/          # Core kernel API:
│       ├── types.h    # Base types, port I/O macros, compiler hints
│       ├── boot.h     # Boot context struct (g_boot), module/mmap/fb info
│       └── kpanic.h   # Panic API, panic_regs_t snapshot struct
├── lib/
│   ├── kpanic.c       # kpanic() implementation (serial + fb output, font8x8)
│   └── font8x8.inc    # Embedded 8×8 bitmap font data for panic renderer
├── mm/
│   ├── pmm.c          # Physical Memory Manager (bitmap frame allocator)
│   ├── vmm.c          # Virtual Memory Manager (4-level paging, vmm_get_pte)
│   └── kheap.c        # Kernel heap allocator (first-fit free-list)
├── proc/
│   ├── process.c      # Process/address-space abstraction, lifecycle, user spawn
│   └── user_test.asm  # Ring-3 test blobs (raw PIC machine code, no ELF yet)
├── sched/
│   └── sched.c        # Preemptive round-robin scheduler (kernel + user threads)
├── syscall/
│   └── syscall.c      # MSR setup, user-pointer validation, syscall dispatcher
├── main.c             # kernel_main(): orchestrates init sequence
└── linker.ld          # Higher-half linker script (KERNEL_VBASE=0xFFFFFFFF80000000)

userland/
├── include/
│   └── void.h              # libvoid syscall wrappers (write, getpid, sched_yield, exit)
├── crt0.S                  # Minimal entry point: stack alignment + main() + exit
├── init/
│   ├── main.c              # First init process: calls getpid/write/sched_yield/exit
│   └── (no services yet)
├── init.ld                 # Linker script for init.elf (0x400000 base, page-aligned PT_LOAD)
├── user.ld                 # Linker script for Phase 4 test ELF (elf_test.elf)
├── user_packed.ld          # Linker script for .text+.rodata in same page (tests permission union)
├── elf_test.c              # Phase 5A test executable (Phase 4 compatibility)
└── (no VFS, fork/execve, dynamic linking yet)

scripts/
└── limine.cfg         # Limine boot config (kernel path, module definitions)

limine/                # Vendored Limine v9.6.7 (git submodule, DO NOT MODIFY)
```

## Build Dependencies

- `gcc` (host, Ubuntu 11.4+) — used as freestanding cross-compiler
- `nasm` — for entry.asm assembly
- `ld` (GNU binutils) — linking
- `xorriso` — ISO creation
- `qemu-system-x86_64` — testing
- `limine/` directory must contain Limine v9.x binaries (BOOTX64.EFI, limine-bios-cd.bin, etc.)

## Current Implementation Status

**Phase 1 — Bootstrap (COMPLETE):**
- ✅ Makefile + linker.ld (higher-half)
- ✅ entry.asm (Limine handover, BSS zero, stack)
- ✅ boot.c (Limine response parsing → g_boot)
- ✅ GDT + TSS (Ring 0/3 code/data segments)
- ✅ Serial COM1 driver + kprintf()
- ✅ kpanic() skeleton (serial + framebuffer output)

**Phase 2 — Core MM + IDT (COMPLETE):**
- ✅ IDT + exception handlers (#DE, #UD, #DF, #GP, #PF — vectors 0–31)
- ✅ PMM (Physical Memory Manager) — bitmap frame allocator from mmap
- ✅ VMM (Virtual Memory Manager) — 4-level paging, map_page/unmap_page, virt_to_phys
- ✅ Kernel Heap Allocator (kmalloc/kfree) — first-fit free-list on top of VMM

**Phase 3 — Scheduling + Interrupts (COMPLETE):**
- ✅ LAPIC Timer — PIC disabled, LAPIC calibrated via PIT, 100 Hz periodic
- ✅ IRQ infrastructure — handlers for vectors 32–47, LAPIC EOI, spurious vector 255
- ✅ Scheduler — preemptive round-robin, kernel threads, context switch via isr_frame_t swap

**Phase 4 — Process + User Mode + Syscall (COMPLETE):**
- ✅ Process abstraction — per-process PML4, kernel upper half shared by reference
- ✅ PID allocation + process states (EMBRYO/READY/RUNNING/BLOCKED/ZOMBIE)
- ✅ Ring 3 execution — reordered GDT for SYSRET, TSS.RSP0 per thread, IRETQ entry
- ✅ Syscall ABI — SYSCALL/SYSRET MSRs, Linux-compatible numbers, IRETQ return path
- ✅ Syscalls: write(1), sched_yield(24), getpid(39), exit(60), wait4(61)
- ✅ User pointer validation — PRESENT+USER+WRITE checked per page, EFAULT on failure
- ✅ Process lifecycle — spawn_user, exit, wait/reap, parent-child, zombie reaping
- ✅ Ring 3 faults kill the process (SIGSEGV/SIGBUS status), never panic
- ✅ File descriptor abstraction — per-process fd table, fd 0/1/2 → console

**Phase 5A — ELF64 Loader (COMPLETE):**
- ✅ ELF64 validation (magic, machine, type, phdr bounds, file/virtual ranges, alignment congruence, W^X rejection, entry-point placement)
- ✅ PT_LOAD loading (page-by-page via HHDM, BSS zeroing, permission union on shared pages)
- ✅ Two-phase validation+loading (malformed binaries rejected before any mapping)
- ✅ process_spawn_elf() integration with existing scheduler/process lifecycle
- ✅ elf_selftest: 35+ negative tests (mutation-based validation of every check)

**Phase 5B — First Real Userland (COMPLETE):**
- ✅ libvoid: syscall wrappers (write/getpid/sched_yield/exit) matching Linux x86_64 ABI
- ✅ crt0.S: minimal entry point with stack alignment for main() entry
- ✅ init/main.c: first Ring 3 ELF process (test getpid/write/sched_yield/exit)
- ✅ init.elf embedded in kernel, spawned at boot via process_spawn_elf
- ✅ Verified Ring 3 execution, clean shutdown, parent reap
- ✅ All Phase 4 regression tests still passing

**Phase 6 — Next (NOT STARTED):**
- brk/mmap for user heap
- Dynamic linking, libc services

**Phase 6C-1 — Console Input (COMPLETE):**
- ✅ serial_getchar() non-blocking UART input (COM1 RBR polled)
- ✅ sys_read(0) syscall (SYS_read=0, Linux-compatible number)
- ✅ copy_to_user() helper — HHDM + page-table walk (mirror of copy_from_user)
- ✅ init.elf tests sys_read(0): non-blocking read with no data → returns 0
- ✅ All Phase 4/5 regression tests still passing

**Phase 7 — Void + Lux Merger: IPC Foundation (COMPLETE, first merger milestone):**
- ✅ Void-native IPC: endpoints, per-process handle table, 512-byte tagged messages
- ✅ Circular per-endpoint message queue (16 deep) in a global registry (1024 max)
- ✅ Syscalls 62–65: ipc_endpoint_create, ipc_send, ipc_recv, ipc_close — ASYNC/poll semantics
- ✅ No blocking yet; recv on empty queue returns 0 (poll). No VFS-backed /Devices/IPC nodes.
- ✅ All user pointers (data, tag_out, data_out) validated via user_range_ok + HHDM copy
- ✅ Endpoints die at process exit (not reap) via ipc_endpoint_unregister_by_owner — no dangling refs
- ✅ Handle table lazily allocated on first IPC syscall, freed at process_destroy
- ✅ userland/include/ipc.h wrappers use explicit R10 for arg3 (SYSCALL-clobbered RCX)
- ✅ userland/ipc_test.c: 11/11 tests pass in QEMU (loopback, FIFO, EBADF, EFAULT, poll, close, getpid/write regressions)
- ⚠️ Auth/security model not yet enforced (Lux's per-endpoint uid/gid check deferred — it needs a user model first)

**Phase 8 — fork() + execve() + Process Lifecycle (COMPLETE, second merger milestone):**
- ✅ SYS_fork (67, Void slot): `process_fork_current()` — eager private copy of every user page
- ✅ Eager copy, NOT copy-on-write — Void has no user-PF handler or frame refcounts, so shared pages would double-free on independent exit. Correctness first. (COW when uPF + refcounts land.)
- ✅ SYS_execve (59, Linux number): `process_execve_current()` — atomic scratch-space → live-space swap, PID preserved
- ✅ Exec image source is a kernel-resident embedded blob (no VFS): `elf_find_embedded()` resolves short names (init / elf_test / ipc_test / fork_test)
- ✅ Invalid ELF / unknown name → -ENOEXEC, bad pointer → -EFAULT, old image keeps running (validation+load happen before any live-space change)
- ✅ CR3 reloaded explicitly in exec (thread->cr3 alone leaves the CPU on freed tables when rescheduled alone)
- ✅ Orphans reparented on exit (to the exiting process's parent; chain terminates at immortal pid 0), so no zombie is unreachable
- ✅ pid-0 children reaped by the kernel idle loop — no boot-test PID-slot leak
- ✅ `process_destroy_user_space()` extracted, shared by destroy/exit/exec swap
- ✅ userland/forkexec_test.c: 8-bit-check suite — fork PIDs, address-space privacy, .data inheritance, wait4 reap, exec PID preservation, -ENOEXEC, -EFAULT — all pass (checks=0xFF)
- ✅ Full regression rerun in QEMU (×3): all 11 Phase 7 IPC tests, getpid/write/sys_read, address-space isolation, init.elf (status 42)

**Phase 9 — Next (NOT STARTED):**
- VFS layer + real file descriptors (open/read/close)
- brk/mmap for user heap
- Dynamic linking, libc services
- Signals (deferred per Phase 8 spec)

## Key Design Decisions

1. **Single source of truth for boot info**: Only `boot.c` includes `<boot/limine.h>`. All other kernel code reads from the `g_boot` singleton defined in `<void/boot.h>`. This isolates the Limine protocol as an implementation detail.

2. **Static buffers in boot.c**: Since kmalloc doesn't exist yet during early boot, memory map entries and module info are stored in fixed-size static arrays (`STATIC_MMAP_MAX=256`, `STATIC_MOD_MAX=32`). These will be replaced with dynamic allocations once the heap allocator is online.

3. **No stdlib dependency**: All code uses `<void/types.h>` for base types, inline asm for I/O, and `__builtin_va_*` for variadic functions. No `<stdio.h>`, `<stdlib.h>`, or any host OS headers.

4. **kprintf over serial_puts**: Early debug output goes exclusively to COM1 (QEMU maps `-serial stdio`). Framebuffer text rendering exists only in kpanic() for fatal error display.

5. **GDT segment selector values**: KC=0x08, KD=0x10, **UD=0x18, UC=0x20**, TSS=0x28. User Data deliberately precedes User Code: SYSRET derives CS from `IA32_STAR[63:48]+16` and SS from `+8`, so with `STAR[63:48]=0x13` we get CS=0x23 and SS=0x1B. Swapping the two entries silently breaks SYSRET.

6. **Kernel half shared by reference**: `process_alloc()` copies PML4 entries 256–511 from the kernel PML4 into every new address space. Because the entries are copied (not the tables they point to), later kernel-side mappings appear in every process without a fixup pass. The lower half starts empty, so user memory is private. This is also why a syscall or interrupt taken in Ring 3 can run kernel code without switching CR3.

7. **One return path for interrupts and syscalls**: `syscall_entry` builds an `isr_frame_t` byte-identical to the one `isr_common` builds, and returns via `IRETQ` rather than `SYSRET`. Both paths end in `mov rsp, rax; ...; iretq`, so the scheduler can switch threads from inside a syscall the same way it does from a timer interrupt — no separate "preempted in a syscall" case.

8. **User pointers are validated, not trusted**: `copy_from_user()` walks the *calling process's* page tables via `vmm_get_pte()` and requires PRESENT+USER (plus WRITE when writing) on every page in the range, then copies through the HHDM rather than the user VA. A bad pointer returns `-EFAULT`; it can never fault in Ring 0 or read kernel memory on the caller's behalf.

9. **Ring 3 faults kill the process, Ring 0 faults panic**: `isr_dispatch` branches on `CS & 3`. A user fault prints diagnostics and calls `process_exit_current()` with a POSIX-shaped status (SIGSEGV=11 for #PF, SIGBUS=7 for #GP). Only kernel-mode faults are treated as unrecoverable.

10. **Syscall numbers match Linux x86_64**: `read=0`, `write=1`, `sched_yield=24`, `getpid=39`, `execve=59`, `exit=60`, `wait4=61`, then **62–65 are Void-native IPC** (`ipc_endpoint_create`, `ipc_send`, `ipc_recv`, `ipc_close`), and **67 is Void-native `fork`** (no Linux index). arg3 is passed in R10 (not RCX, which SYSCALL clobbers). This is ABI *number* compatibility to keep a future libc port shim-free — it is not a POSIX compliance claim.

11. **IPC uses endpoints + handles, not VFS nodes**: Endpoints are kernel-side, per-process message queues registered in a global table keyed by `(owner_pid, endpoint_id)`. Processes reference them through a per-process handle table (like FD table, capped at 64 handles). Messages are fixed 512-byte structs with inline data + tag. Async/polling for MVP — a real `sys_recv` block is deferred until there are wait queues. This keeps IPC self-contained and free of the not-yet-existing VFS.

12. **Lux security model deferred, not dropped**: Lux's IPC enforces per-endpoint permissions (read/write UID checks). Void has no user concept yet (single root/ring-3), so the check would always pass — carrying it would be dead code. The endpoint/handle architecture leaves a clean seam to add a `uid` to the endpoint and gate at `ipc_send`/`ipc_recv` when user IDs land.

13. **fork uses eager page copy, not COW**: `process_fork_current()` allocates a fresh frame for every umap[] entry the parent has mapped and copies it — nothing is shared at the page level, and the child honours the same WRITE/NX flags by re-reading the parent's PTE. **Why not COW**: Void has no on-demand user page-fault handler (a Ring-3 #PF kills the process) and no per-frame refcounting, so a COW mapping would be owned by two processes that both free it on exit — a double-free. Correct eager copy first; COW is marked as the upgrade path and is safe once a user-PF path and frame refcounts exist. Cost is up to ~64 pages copied per fork, an acceptable trade for a microkernel with few, short-lived processes.

14. **exec is an atomic swap into a scratch address space**: `process_execve_current()` validates the ELF and loads it into a *fresh* PML4 (sharing the kernel upper half) before touching the live one. Only after the scratch is fully built does it: free the old user pages + lower-half tables, install the new CR3, and **explicitly reload the CPU CR3** — updating `thread->cr3` alone is a bug because the scheduler only reloads CR3 when switching to a *different* address space, so a rescheduled exec'd thread would run on freed tables. Any failure before the swap leaves the old image intact and returns -errno. PID, fd table, and IPC handle table all survive.

15. **exec resolves names to embedded blobs (no VFS yet)**: every runnable executable is a fixed short name (`init`, `elf_test`, `ipc_test`, `fork_test`) held in the kernel's `.rodata` via `elf_blobs.asm`. `elf_find_embedded()` maps a validated user pathname to its (base,size). When the VFS lands, execve walks the filesystem here instead and the lookup disappears.

16. **Fork children inherit a copy of the IPC handle table**: `process_fork_current()` deep-copies the child's handle-table *entries* (they name `(target_pid, endpoint_id)`), so the child references the same endpoints the parent does. Endpoints themselves are never duplicated and stay single-owner by pid — `ipc_endpoint_unregister_by_owner()` frees only the endpoints a given pid owns, so when parent and child exit independently nothing is double-freed. No reference counting is introduced (the spec explicitly forbade "incorrect refcounting").
