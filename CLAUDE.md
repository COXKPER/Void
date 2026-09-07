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
│   ├── serial_drv.c   # device-layer wrapper: registers "serial" DEV_CHAR
│   ├── device.c       # device registry + instance layer (Phase 11)
│   └── lapic.c        # LAPIC timer + PIC disable + calibration
├── include/
│   ├── arch/x86_64/   # Arch headers (gdt.h, idt.h)
│   ├── boot/
│   │   └── limine.h   # Vendored Limine v9.6.7 protocol header (DO NOT EDIT)
│   ├── dev/           # Device headers (serial.h, serial_drv.h, lapic.h, console.h)
│   ├── mm/            # Memory management headers (pmm.h, vmm.h, kheap.h, user_mem.h)
│   ├── proc/          # Process abstraction (process.h)
│   ├── sched/         # Scheduler headers (sched.h)
│   ├── syscall/       # Syscall ABI + numbers + errno (syscall.h)
│   └── void/          # Core kernel API:
│       ├── types.h    # Base types, port I/O macros, compiler hints
│       ├── boot.h     # Boot context struct (g_boot), module/mmap/fb info
│       ├── device.h   # Device registry + instance API (Phase 11)
│       └── kpanic.h   # Panic API, panic_regs_t snapshot struct
├── lib/
│   ├── kpanic.c       # kpanic() implementation (serial + fb output, font8x8)
│   └── font8x8.inc    # Embedded 8×8 bitmap font data for panic renderer
├── mm/
│   ├── pmm.c          # Physical Memory Manager (bitmap frame allocator)
│   ├── vmm.c          # Virtual Memory Manager (4-level paging, vmm_get_pte)
│   ├── kheap.c        # Kernel heap allocator (first-fit free-list)
│   └── user_mem.c     # User address-space: heap (brk) + anonymous mmap region
├── proc/
│   ├── process.c      # Process/address-space abstraction, lifecycle, user spawn
│   └── user_test.asm  # Ring-3 test blobs (raw PIC machine code, no ELF yet)
├── sched/
│   └── sched.c        # Preemptive round-robin scheduler (kernel + user threads)
├── syscall/
│   └── syscall.c      # MSR setup, user-pointer validation, syscall dispatcher
├── vfs/
│   └── voidfs.c       # Kernel VFS: embedded readonly tree + fd-layer syscalls
├── main.c             # kernel_main(): orchestrates init sequence
└── linker.ld          # Higher-half linker script (KERNEL_VBASE=0xFFFFFFFF80000000)

userland/
├── include/
│   └── void.h              # libvoid syscall wrappers (write/read/open/close/lseek/cwd/fork/…)
├── crt0.S                  # Minimal entry point: stack alignment + main() + exit
├── init/
│   ├── main.c              # First init process: calls getpid/write/sched_yield/exit
│   └── (no services yet)
├── init.ld                 # Linker script for init.elf (0x400000 base, page-aligned PT_LOAD)
├── user.ld                 # Linker script for Phase 4 test ELF (elf_test.elf)
├── user_packed.ld          # Linker script for .text+.rodata in same page (tests permission union)
├── elf_test.c              # Phase 5A test executable (Phase 4 compatibility)
├── ipc_test.c              # Phase 7 IPC test (11 checks)
├── forkexec_test.c         # Phase 8 fork/exec/lifecycle test (checks=0xFF)
├── vfs_test.c              # Phase 10 VFS test (20+ checks, [VFS] Done: 0 fail)
├── mm_test.c               # Phase 12 brk/sbrk/mmap/munmap + fork-heap test ([MM] Done: 0 fail)
└── services/               # Phase 9 user-space services + their clients
    ├── calc.c              # first service (named endpoint, request→reply over IPC)
    ├── srv_test.c          # client with svc_lookup
    └── lifecycle_test.c    # register → discover → exit → registry-clean → lookup-fails
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

**Phase 9 — User-Space Service / IPC Router Architecture (COMPLETE, third merger milestone):**
- ✅ Kernel service registry: name → (owner_pid, endpoint_id), fixed array (SVC_MAX_SERVICES=64), `name[0]=='\0'` free marker; transport stays on the existing endpoint/handle IPC (registry holds addresses only)
- ✅ New syscalls: `ipc_connect` (66, Lux data-plane pattern), `svc_name_register` (68), `svc_name_unregister` (69), `svc_lookup` (70)
- ✅ `sr_register`/`sr_unregister` require an endpoint handle the *caller owns* with a *live* endpoint (`svc_check_endpoint`); a name can only ever name the registering process's endpoint
- ✅ `sr_lookup` mints the fresh handle — the one address-space-creation privilege; `ipc_connect` re-validates endpoint liveness so a minted handle never outlives the endpoint it names (the destroy/revoke race)
- ✅ Lifecycle cleanup on both ends of a service's life: `svc_cleanup_owner()` (process exit/destroy) and `svc_on_endpoint_close()` (owner `ipc_close`) — a dead service is never discoverable by name
- ✅ First user-space service: `calc` (sub/add/mul/div request→reply over IPC) + `srv_test` client + `lifecycle_test` (start→register→discover→request→reply→exit→registry-clean→lookup-fails)
- ✅ Wire protocol is flat byte arrays with explicit offsets (`calc_proto.h`) because `ipc_send` caps `data_len` at (512 − header)
- ✅ Security boundaries: no cross-memory paths, no forged handles (endpoint liveness re-checked at mint time), all user pointers HHDM-validated; kernel unit check → deterministic QEMU tests ×3 + full Phase 4–8 regression, all green
- 🐛 **Found and fixed a pre-existing kernel heap bug** (see design decision #17): `kmalloc`/`split_block` left the allocated block linked into the freelist → freelist cycles under churn → `coalesce()` livelock. Fix: unlink the winner before splitting (both paths); `split_block` deleted. ≥8 consecutive QEMU runs green post-fix.
- ⚠️ Lux classification: BRING (service==named endpoint, registry-as-directory, handle-mint as the single privilege), PORT (message framing), REFERENCE (per-endpoint uid/gid auth — deferred, needs a user model), SKIP (in-Lux "service context"/capability trees — overbuild)

**Phase 10 — Kernel VFS / File Layer (COMPLETE, fourth merger milestone):**
- ✅ Embedded readonly in-memory filesystem (`kernel/vfs/voidfs.c`): a vnode tree rooted at "/" (hello.txt / test.txt / etc/version.txt), heap-built once at boot, with full path resolution
- ✅ Syscalls 2/3/8/79/80/89/318: `open`, `close`, `lseek`, `getcwd`, `chdir`, `readdir`, `stat` (Linux x86_64 numbers). `read(0)`/`write(1)` now dispatch on fd type: FD_SERIAL (console) vs FD_VFS (embedded tree)
- ✅ fd table → open file → filesystem node → backend layering: per-process `fd_entry_t` (FD_VFS) holds a `vfs_file_t` (node + position + per-open id + refcount). VFS fds start at 3, below that the console
- ✅ Path/cwd resolution: absolute + relative (cwd-prefix, the Lux recipe), `.`, `..`, interior/double slashes, trailing-slash-is-ENOTDIR. `getcwd`/`chdir` normalize/rebuild the absolute path root-down
- ✅ Process integration: `cwd` field on `process_t` (inherited by fork, preserved by exec). `vfs_close_process_fds()` on exit/destroy so no file object leaks; fork deep-copies the fd table and bumps the shared `vfs_file_t` refcount
- ✅ User-pointer safety: every path/buffer validated via `user_range_ok`/`copy_{from,to}_user` (the same HHDM walk as IPC), returns -VE_* errno (ENOENT/EACCES/EISDIR/ENOTDIR/ENAMETOOLONG/EBADF…)
- ✅ `vfs_test`: 20+ deterministic checks in QEMU (open/read/EOF/lseek/ENOENT/EACCES/dir readdir/ENOTDIR/close/EBADF/cwd flow/relative+`..`/trailing-slash) — `[VFS test] Done: 0 fail(s)` ×3
- ✅ Full regression (IPC 11/11, fork/exec checks=0xFF, service registry, lifecycle, init status 42) all green ×3, no panics
- ⚠️ Design notes: the tree is readonly (no mounts/permissions/hardlinks by spec). The fd/VFS interface is deliberately shaped so the tree can later become a *user-space filesystem service* behind IPC — swap the backend, not the fd table or syscall surface
- 🐛 **Lux provenance finding**: Lux's kernel has **no** VFS — every file syscall (`file.c`) is a `requestServer()` IPC forward to a user-space server (lumen). There is no in-kernel vnode/backend/path-walker to port. So Void's vnode/tree/path-resolution is Void-native; what was *actually ported* from Lux is the fd-slot discipline (`io.c` openIO/closeIO), the cwd-prefix recipe (`cwd.c`), the per-open `FileDescriptor{position,refcount,id}` object shape (`file.c`), and the kernel-side cwd field. Lux is MIT; attribution is in `kernel/vfs/voidfs.c`'s header

**Phase 11 — Device Abstraction / Driver Boundary (COMPLETE, fifth milestone):**
- ✅ Minimal device layer (`kernel/dev/device.c`, `kernel/include/void/device.h`): a named device (`void_device_t`) with a small ops table `{open, close, read, write, ioctl}` held in a static registry (`DEV_MAX_DEVICES=32`). `dev_register/unregister/lookup` plus an instance layer — `dev_open()` mints a heap wrapper that pins a driver's per-open handle to the registry slot (birth-magic check), so a forged/stale handle is rejected `-VE_BADF`, and `dev_unregister` refuses while refs are open (`-VE_BUSY`) — the spec's "unregistering with active users handled safely."
- ✅ Serial integrated as the first driver (`kernel/dev/serial_drv.c`): wraps the register-level `serial.c` *unchanged* into the registry as the `"serial"` DEV_CHAR device; `sys_read(0)`/`sys_write(1)` now route through `dev_read`/`dev_write` on a single kernel-minted console instance. Console behavior is byte-identical — `kprintf`, serial/VGA output, `kout=serial`/`kout=vga` all unchanged (console.c and serial.c untouched).
- ✅ Ownership: the console is one shared terminal, so its instance is kernel-owned — opened once at `serial_drv_register()`, never closed, held out-of-band from any fd table. No process holds a device reference, so process exit can never leak one and fd 0/1/2 keep working across fork/exec.
- ✅ Device↔VFS boundary prep: `dev_probe()` walks the registry as the enumeration seam (a future `/dev` vnode backend calls this). No `/dev` tree, no VoidFS changes — per spec.
- ✅ IPC preparation: the device seam is shaped so a future driver can sit behind IPC — process→syscall→kernel device layer→driver is the same interface as process→IPC→device service→kernel device layer→driver. No IPC modified (no concrete integration problem surfaced).
- ✅ Regression: `dev_self_test()` runs at boot before the scheduler — the 8 specified `[DEV]` checks (register/lookup/open/close/invalid/duplicate/unregister/stale) plus three lifetime proofs (BUSY-on-active + still-usable, re-register mints a fresh open, the `"serial"` device is findable). 13/13 PASS ×3, full Phase 4–10 regression (IPC/VFS/forkexec/srv/lifecycle/init status 42) all green ×3, 0 panics / 0 page faults / 0 unknown syscalls.
- ⚠️ Lux classification (kernel device story): **no in-kernel driver table exists in Lux to port** — `kernel/irq.c` forwards IRQ notifications to *user-space* drivers over a socket (`serverSocket()` + `IRQCommand`), `platform.h` is a CPU HAL (ioperm/paging/context, not devices), and `modules.h` is userspace module loading over an initramfs (USTAR). So the named-table + open-refcount discipline in `device.c` is **Void-native**, shaped like Void's own IPC handle table (Phase 7). BRING — verification-by-liveness-before-every-access (the "no stale handle" discipline Void already uses); PORT — interplay between IRQ dispatch and device ownership (as the lapic/timer boundary when a timer driver exists); REFERENCE — per-driver I/O-port privilege handoff (`platformIoperm`) once a user-space driver model exists; SKIP — everything else (Lux's whole driver story is "everything is a server", which Void will *become*, not embed).

**Phase 11 — Next:**
- brk/mmap for user heap ✅ (→ Phase 12)
- Writable backend / real FS (or user-space VFS service behind IPC)
- Dynamic linking, libc services
- Signals (deferred per Phase 8 spec)
- Remaining driver candidates behind the seam: framebuffer/console as a DEV_FB device, keyboard as DEV_INPUT, and a user-space driver demo over IPC

**Phase 13 — Userland Memory Allocator & libc Runtime (COMPLETE, seventh milestone):**
- ✅ Minimal freestanding libc runtime (`userland/include/libc/` + `userland/libc/`): the 9 memory/string primitives (`memcpy memmove memset memcmp strlen strcpy strncpy strcmp strncmp`) as plain byte loops (`-mno-sse -mno-sse2`), then `malloc/calloc/realloc/free` on top of Phase 12's `brk(12)/sbrk` + `mmap(9)/munmap(11)` ABI
- ✅ Boundary-tag allocator: 16-byte-aligned `btag_t{size,flags}` header+footer per block; explicit free list (node in payload); first-fit split/reuse; neighbor coalescing on free via the PREV bit + footer mirror. `MIN_BLOCK=48` = header(16)+node(16)+footer(16) (32 was a node/footer-collision bug). Block tags: `B_ALLOC`/`B_PREV`/`B_MMAP`
- ✅ arena-via-init-latch: first allocation takes `sbrk(0)` as `a_start`/`a_end`; growth in page-aligned chunks by extending the top free block in place (or a fresh block above an allocated top). `malloc(0)` returns a real unique freeable block; `free(NULL)` is a no-op; double-free is a deterministic `memerr` + exit, not UB
- ✅ mmap-backed large allocations: `size ≥ MALLOC_MMAP_THRESHOLD (128 KiB)` → a private anonymous mapping (tagged `B_MMAP`), released whole by `free()`. Below-threshold allos share the brk arena, so one big allocation can never fragment it. `mmap_alloc` rounds to 4 KiB page granularity
- ✅ calloc overflow-safe via `__builtin_mul_overflow` (refuses rather than wraps); realloc grows allocate→copy→free (original preserved when allocation fails — the §16 check), shrinks by splitting the tail off the block in place; overflow-safe `size_to_need()` everywhere (`align16()` can wrap near `SIZE_MAX`, which made `realloc(huge)` take the shrink path — found by the suite)
- ✅ allocator state is process-local statics, so eager-copy fork and atomic-scratch exec need no special handling: fork copies the heap pages with the address space; exec's fresh .bss zeroes the allocator. `malloc_test` has an explicit fork-isolation check (child allocates, parent's selfcheck stays consistent) and an exec-clean-state path
- ✅ `malloc_selfcheck()` — walks the free list verifying size/alignment/in-arena/footer-mirror/no-adjacent-free invariants; returns 0 or a negative code; the suite calls it between phases and after the fork test and the 40-round stress
- ✅ `[MALLOC]` suite (`userland/malloc_test.c`, embedded + spawned at boot): 23 checks — basic sizes/alignment/writability, calloc zero+write+overflow, realloc grow/shrink/null + failure-preserves-original, free reuse, coalescing, mmap-threshold isolation + selfcheck, malloc(0)/free(NULL), fork heap isolation, stress → `[MALLOC test] Done: 0 fail(s)` ×3 from clean build
- ✅ Full regression 3× from clean build: DEV 0 FAIL / IPC / VFS / MM / MALLOC / forkexec `checks=0xFF` / srv / lifecycle / init status 42 all green, **0 panics / 0 faults / 0 unknown syscalls**
- ⚠️ **Test-harness determinism fix** (found while wiring the suite): the `Done:` marker in ipc/vfs/lifecycle/malloc tests was emitted as **three** `sys_write` calls, and other processes' bytes interleave into the shared COM1 stream between calls — tearing the exact line the boot regression greps for (observed `Done: 0\x0b@ fail(s)`). Each test now emits its marker as **one** write; lifecycle also replaced a hardcoded `len=16` on every `say()` with the true string length. The regression is now deterministic, not lucky
- ⚠️ Recorded in memory, not CLAUDE.md (Lux audit provenance): `userland/libc/malloc.c`, `mem.c`, `string.h`, `malloc.h` are **Void-native**, no Lux-derived code (see `[[phase-13-userland-allocator]]`)

**Phase 12 — User Memory: brk/sbrk + mmap/munmap (COMPLETE, sixth milestone):**
- ✅ Per-process heap state (`heap_start`/`brk_current`/`brk_perm` on `process_t`, set by `brk_init()` from the loaded image's top). Kernel break tracks the break; libvoid `sys_sbrk`/`sys_brk` implement the userspace cache + query.
- ✅ `brk(12)/sbrk` — grow maps zeroed user pages (NX kept from the image), shrink frees whole pages at/above the new break. `-ENOMEM` on frame exhaustion (break unchanged), `-EINVAL` below BSS, `sbrk(0)` is a pure read (a fixed libvoid bug — a query used to shrink the break to a stale cache).
- ✅ Anonymous `mmap(9)/munmap(11)` — MAP_PRIVATE|MAP_ANONYMOUS only, PROT_WRITE/READ/EXEC (PROT_NONE refused: x86 PTE can't express read-disabled). Top-down reservation below the stack floor via `um_mmap_reserve()` (page tables are the source of truth for free ranges). Eager page allocation — Void has no user-PF handler. Rejects: length 0, absurd/gigantic length, fixed-map at unaligned, overlapping a mapped page, overlapping the stack, or any junk flags/fd/offset.
- ✅ `kernel/mm/user_mem.c` + `kernel/include/mm/user_mem.h` — heap grow/shrink, `um_range_clear()` (overlap probe), `um_mmap_reserve()` (top-down carve), `um_release_from()` (munmap+brk-shrink share one release path, capped below the stack).
- ✅ fork/exec/exit integration — fork copies `heap_start`/`brk_current`/`brk_perm` to the child (and the eager page copy already duplicates every heap+mmap page via `umap[]`); exec rebuilds them from the new image in the scratch space; exit frees heap+mmap pages through the existing destroy path (they're ordinary `umap[]` pages). `process_t.umap[]` widened 64→256 to hold the new pages.
- ✅ `syscall` ABI uses Linux x86_64 numbers (brk=12, mmap=9, munmap=11); argument order matches Linux (mmap: addr/length/prot/flags via R10). All user pointers validated through the standard `copy_*`/user-range machinery.
- ✅ Page-fault interplay is minimal by design: eager allocation means a valid mapping never #PFs. The one coupling to faulting is that a user #PF (e.g. bad pointer) kills the process — unchanged from before.
- ✅ NX fix en route: the ELF loader's `elf_init()` was never called at boot, so pages were always marked executable. Now called (in `kernel_main` after `kheap_init`); user pages (heap/mmap) are NX when EFER.NXE is available.
- ✅ Dynamic-linking prep: `exec_from_user`/spawn preserve a chunk-granular address layout; no barrier to a future ET_DYN loader, but a real linker is not done (out of scope).
- ✅ `[MM]` regression (userland/mm_test.c, embedded + spawned at boot): 20+ deterministic checks — sbrk init/grow/shrink/below, brk set/query/stable, mmap base/zero/write/second/isoleak, munmap ok/again/never, EINVAL rejections, fork-heap inheritance + isolation (eager copy), wait4-poll reap. `[MM test] Done: 0 fail(s)` ×3. Full Phase 4–11 regression (IPC/VFS/forkexec/srv/lifecycle/init 42) green ×3, 0 panics/0 faults/0 unknown syscalls.
- ⚠️ Remaining (NOT started): user-VFS "writable backend"; shared libraries / dynamic linking; a user-PF handler + COW (fork is still eager copy); signals. mmap is anonymous-only (no file-backed mapping yet; the VFS tree is readonly so file-backed would be dead code).

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

17. **Services are named IPC endpoints; the registry is a directory, not a second IPC** (`kernel/svc/svc.c`): A "service" has exactly one meaning in Void — a process that registered a name for one of its own endpoints. The kernel registry maps name → `(owner_pid, endpoint_id)` in a fixed array (`SVC_MAX_SERVICES=64`, free slot marked `name[0]=='\0'`). Transport stays entirely on the Phase 7 endpoint/handle data plane; the registry holds *addresses only*. `sr_register`/`sr_unregister` (68/69) only accept an endpoint handle the caller owns with a live endpoint in the IPC registry (`svc_check_endpoint`), so a name can only ever name the registering process's endpoint. `sr_lookup` (70) returns a fresh handle — it is the **single** privilege that creates a handle to another process's endpoint, granted only for the exact `(owner_pid, endpoint_id)` a server registered. `ipc_connect` (66, the Lux data-plane pattern) is the same mint operation from a raw pid+endpoint-id and re-validates endpoint liveness in the IPC registry at call time, so a minted handle can never outlive the endpoint it names (the destroy/revoke race). Lifecycle cleanup is total on both ends of a service's life: `svc_cleanup_owner()` on process exit/destroy, `svc_on_endpoint_close()` on owner `ipc_close` — a dead service is never discoverable. Errors are `SVC_E_*`, already-negative, returned verbatim. Security boundaries: no cross-memory paths (all user pointers HHDM-validated), no forged handles (liveness re-checked at mint time), and the handle-mint is the only new privilege — there is deliberately no per-service capability system (a server that registered is already serving; another server doing the same is the model). **Why so minimal**: the spec's binding constraint was "do not turn Void into Lux" — Lux ships a full service framework; Void takes only the two architectural insights (service == named endpoint, minted handle carries the authorization) and implements them as ~200 lines against the existing IPC. **Lux classification**: BRING — service==named endpoint, registry-as-directory, handle-mint as the single privilege; PORT — message framing/request-reply shape; REFERENCE — per-endpoint uid/gid auth (deferred: no user model yet, would be dead code); SKIP — in-Lux "capability/service-context" trees (overbuild). **Future migration**: when VFS lands, names become `/Services/<name>` nodes; when a user model lands, permission checks attach to the mint operation, not to transport.

18. **The kernel heap allocator had a freelist-cycle bug (found + fixed in Phase 9)**: `kmalloc`'s first-fit path called `split_block()` while the winning block was still linked into the freelist, leaving the allocated block — and later its remainder chain — reachable twice. Under churn (Phase 9's many short-lived processes allocating/freeing 768-byte handle tables and 8216-byte endpoints) two references to the same block could both be freed and re-split until the list became cyclic and `coalesce()` livelocked in the timer interrupt. Fix: unlink the winner from the freelist **before** splitting, in both the first-fit and grow paths; `split_block()` deleted. `coalesce()` keeps a cycle guard that converts a recurrence from a hang into a `kpanic` (prefer a loud panic over a silent livelock). Not fixed with refcounting or SLABs — a correct first-fit is the boring, sufficient fix. Optional escalation was originally noted for a slab allocator when the workload needs it (it does not today).

19. **The VFS layer ports Lux's *fd discipline*, not a vnode tree — because Lux has no vnode tree to port** (`kernel/vfs/voidfs.c`): The spec asked to "port the useful parts of Lux's VFS." Reading Lux's kernel, every file syscall in `file.c` is a `requestServer()` IPC forward to a user-space server (lumen) over a socket; there is no in-kernel path-walker, superblock, mount table, or filesystem backend. So a literal "port" is impossible — and inventing a full in-kernel VFS on top would be exactly the kind of unnecessary complexity the spec forbade ("do not add a disk driver just because a filesystem exists in Lux"). **What was actually taken from Lux** (MIT, attributed in `voidfs.c`): the per-process IO/fd-slot allocation discipline (`io.c`'s `openIO`/`closeIO` → Void's fd slot scan from 3), the cwd-prefix relative-path recipe (`cwd.c` — `/` prefix or `cwd + '/' + path`), the `FileDescriptor{position, refcount, id}` open-object shape (`file.c` → Void's `vfs_file_t`), and the kernel-side `cwd` field on the process (`sched.c`). **Everything else is Void-native**: the vnode tree, the `/ . ..` path resolver, the `readdir` cursor, the errno surface, and the fd-type dispatch in `sys_read`/`sys_write`. **Why an embedded readonly tree instead of nothing**: Phase 10's deliverable is "a clean filesystem abstraction that can later become a user-space filesystem service behind IPC." The fd table → open file → node → backend layering is that abstraction; the tree is the temporary backend. The spec explicitly encouraged an initramfs/embedded/in-memory simplification — so the tree is heap-static, readonly, with no mounts/permissions/hardlinks, and `write()` is `-EACCES`. **The user-space-VFS seam**: when the VFS service lands, only the backend changes — `vfs_resolve_abs`/the tree builder are replaced by client calls over IPC, while `fd_entry_t`, the `FD_VFS` type, the syscall numbers, and the `sys_vfs_*` dispatch all stay. **Lux classification**: BRING — fd-slot discipline + open-object shape (id + position + refcount); PORT — cwd-prefix recipe, kernel cwd field; SKIP — the entire Lux "VFS" is an IPC server, which Void will *become* rather than embed.

20. **Void introduces a device boundary *before* implementing complex drivers — because the boundary is the architecture, the drivers are content** (`kernel/dev/device.c`, `kernel/include/void/device.h`): The handoff from "everything is directly poked at boot" to "drivers are named, owned, and replaceable" has to happen *before* the hardware stack grows, and the discipline that makes it safe is generic. Phase 11's contract was explicitly *not* to chase driver quantity (no USB/GPU/net/disk, no /dev, no console rewrite) — so Void now has the smallest thing that is still a real device model: a static registry of named devices with a 5-op table and an instance layer that mints a validated heap handle per open. Why this shape: **(a)** it mirrors the two resource tables Void already trusts — the fd table and the IPC handle table — so one ownership story ("close everything at process exit, refuse to reuse a freed id") applies everywhere; **(b)** an open *instance*, not the device, is what an fd slot will eventually hold, which is exactly the FD_VFS already does with a `vfs_file_t`, so the seam to a future `/dev` vnode backend is a one-line swap; **(c)** unregister refusing while refs are open (`-VE_BUSY`) is the "unregistering with active users handled safely" the spec demanded, and it is the kernel-side analog of Phase 10's "open handles are not invalidated under their owner." **Why not a Linux-style model**: Void has no node/bus/class tree, no permissions, no mount points — importing that machinery would be exactly the overbuild the milestone forbade. **How serial fits**: the console is registered as the `"serial"` DEV_CHAR device and the syscall data plane (`sys_read(0)`/`sys_write(1)`) now goes through `dev_read`/`dev_write` on one kernel-minted instance; the register-level driver (`serial.c`) is untouched, so `kprintf`, VGA routing and `kout=serial`/`kout=vga` are byte-for-byte the same. **The userspace-driver future**: the spec's *"do not move code to userspace just because microkernels do it"* means the seam exists so that when a driver genuinely benefits from isolation it can move behind IPC without reshaping the kernel interface — the kernel device layer *is* the interface a user-space driver service will call. **Lux classification**: Lux's kernel has no in-kernel driver table to BRING (its `irq.c` forwards IRQ notifications to user-space drivers over a socket; `platform.h` is a CPU HAL; `modules.h` is userspace module loading), so the registered-device + open-refcount model is Void-native — the only Lux echo is the verification-by-liveness-before-every-access discipline that already governs IPC handles and VFS fds, which the instance layer applies to devices too. BRING — liveness check at every access; REFERENCE — `platformIoperm` (I/O-port privilege) once a user-space driver model lands; SKIP — the entire "everything is a server" driver story, which Void will *become* rather than embed.

21. **User memory is page-granular and *recorded in umap[]*, not region-granular** (`kernel/mm/user_mem.c`, `process_t.umap[]`): The Phase 12 contract was emphatic — "do not rewrite the VMM/PMM/fork/exec". The clean way to grow a heap and an mmap region on top of the existing VMM was to give `process_t` the two dynamic regions the ELF loader already uses (`heap_start`/`brk_current` for the break, and the same lower-half UM being carved top-down for mmap). **Two decisions follow from keeping the page tables as the source of truth**: **(a)** overlap detection (`um_range_clear()`) walks the page tables (PRESENT), not a separate region list — the `vm_area_t` struct exists only as the spec-asked metadata, the actual allocator consults PTEs. **(b)** ownership stays in `umap[]`, so *every* page is released by the same `um_release_from()`/destroy path — a heap page "reached the count of umap" and got reclaimed by a partial `munmap`, then the process exits, must not double-free: unmapping removes the umap entry, so teardown sees no stale reference. **Why eager page allocation, not demand-fault**: Void has no on-demand user-PF handler (a Ring-3 #PF kills the process), so every mapped byte must be present up front — `sys_mmap`/`um_brk_grow` allocate all frames eagerly. This is why mmap is anonymous-only, why `PROT_NONE` is refused, and why fork is eager copy (decision #13) rather than COW.
- ℹ️ Also fixed a dormant boot bug found while wiring NX: `elf_init()` was never called, so NX was never enabled; user pages (heap/mmap) are now NX when the CPU supports it.

22. **The userland allocator is a boundary-tag/arena design, deliberately chose two "boring" structures and one hard correctness bar** (`userland/libc/malloc.c`): the two pre-approved design decisions — **(a)** 16-byte-aligned boundary tags (each block carries a `btag_t{size,flags}` header *and*, when free, a mirrored footer; the PREV bit + footer make left-coalescing safe because a footer only exists before a free block) and **(b)** arena-via-init-latch (the first allocation latches `a_start`/`a_end` from a pure `sbrk(0)` read — never the kernel moving the break — and growth happens in page-aligned chunks by extending the top free block in place). Three follow-on decisions were forced by the surrounding kernel: **(c)** *mmap for large allocations* (`≥128 KiB`, tagged `B_MMAP`, released whole by `free()`) so a single big allocation can't fragment the shared brk arena — this is only sound because Void's `sys_mmap` is private-anonymous-only and `free()` branches on the tag; **(d)** *realloc grows by allocate→copy→free* (original preserved on allocation failure, the §16 hard requirement) with in-place grow deliberately deferred — "boring over clever"; and **(e)** *eager everything* — the allocator is plain process-local statics, so eager-copy fork (decision #13) and atomic-scratch exec (decision #14) need no locking or re-init, and a `malloc_selfcheck()` free-list walk gives the suite a deterministic invariant to assert between phases. **Why this shape**: the boundary-tag/free-list is the smallest structure that gives both first-fit reuse and coalescing with no extra metadata pass; the init-latch avoids a kernel breaking-API change and keeps `malloc` correct across brk-shrink; and, critically, style stayed boring — no size classes, no tcache, no per-thread arenas, no TLS. `malloc(0)`, `free(NULL)` and double-free are all deterministic and documented (§11 contract); the double-free is a loud exit, not UB. **Why not COW/tcache/etc.**: those all need threads, refcounting, or a user-PF handler Void doesn't have; a first-fit boundary-tag allocator is the correct, sufficient implementation for few short-lived processes today. All state is process-local, so fork (eager page copy) and exec (fresh .bss) reset it for free. **Known simplifications, marked `ponytail:` in the source**: no fast path/TLS, no in-place realloc growth — add when profiling calls for it.
