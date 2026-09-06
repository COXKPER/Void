/* VoidOS — VFS internal API
 *
 * The kernel VFS: an embedded in-memory filesystem (initramfs-style tree)
 * served to Ring 3 through the existing fd table.  The data plane is the
 * Process fd table (fd -> vfs_file_t); the VFS core resolves paths against
 * the tree and the fd layer dispatches open/read/write/close/readdir.
 *
 * This file is included only by kernel translation units (syscall dispatch,
 * process teardown) that must call into voidfs.c; userland never sees it.
 */
#ifndef VOID_VOIDFS_H
#define VOID_VOIDFS_H 1

#include <void/types.h>
#include <proc/process.h>

/* Boot the embedded filesystem: build the readonly tree rooted at "/".
 * Call once during kernel init, after the heap is online. */
void voidfs_init(void);

/* Per-process fd table is the only handle the syscalls expose; the VFS
 * keeps zero state of its own beyond the tree.  All user pointers are
 * validated by the caller through user_range_ok()/copy_from_user(). */

/* ── fd-table teardown ─────────────────────────────────────────────────
 * On process destroy/exit every VFS descriptor this process still holds
 * has its file object released (the executable image refcount / read-only
 * open-file handle).  Serial fds need nothing. */
void vfs_close_process_fds(process_t *p);

/* ── VFS syscall handlers ──────────────────────────────────────────────
 * All int64_t-returning, all negative -VE_* on error.  User pointers are
 * validated inside via user_range_ok()/copy_{from,to}_user (the same
 * HHDM mechanism as syscall.c).  Passed the owning process for that. */
int64_t sys_vfs_open(process_t *p, uint64_t upath, int flags);
int64_t sys_vfs_read(process_t *p, int fd, uint64_t ubuf, uint64_t count);
int64_t sys_vfs_write(process_t *p, int fd, uint64_t ubuf, uint64_t count);
int64_t sys_vfs_close(process_t *p, int fd);
int64_t sys_vfs_lseek(process_t *p, int fd, int64_t offset, int whence);
int64_t sys_vfs_chdir(process_t *p, uint64_t upath);
int64_t sys_vfs_getcwd(process_t *p, uint64_t ubuf, uint64_t size);
int64_t sys_vfs_readdir(process_t *p, int fd, uint64_t ubuf);

/* A VFS file object, opaque to everyone but voidfs.c.  Held in the process
 * fd table as `object`; fork shares the same open handle between parent and
 * child (POSIX dup semantics), so the refcount lets the object live until
 * the last close in any process. */
typedef struct vfs_file_t vfs_file_t;

/* Take one extra reference to a shared open file (fork's fd-table dup). */
void vfs_file_ref_inc(void *file);

#endif /* VOID_VOIDFS_H */