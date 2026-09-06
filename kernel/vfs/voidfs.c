/* VoidOS — kernel VFS (embedded in-memory filesystem)
 *
 * Provides a working filesystem abstraction for Ring 3: an initramfs-style
 * readonly tree rooted at "/" with files and subdirectories.  It is the
 * temporary backend the spec asks for, structured so the fd/VFS interface
 * stays clean and the tree can later be swapped for a user-space VFS service
 * behind IPC (the fd table and syscall surface do not change).
 *
 * Port provenance (Lux MIT, adapted to Void):
 *   - per-process IO/fd table + openIO/closeIO slot discipline  ~ io.c
 *   - FileDescriptor{position, refcount, id} file-object shape → the VFS
 *     file object (vfs_file_t) with an owned position + open handle.
 *   - cwd-prefix relative-path recipe and kernel-side cwd field  ~ cwd.c
 *   - per-open-file unique id (device files)                    ~ file.c
 * These are PORT/BRING adaptations; the vnode/tree/path-resolution layer
 * below is Void-native (Lux's kernel forwards every file syscall to a
 * user-space server and carries no tree).  See CLAUDE.md Phase 10.
 *
 * Ownership for brain dead simplicity: a readonly tree.  No mounts, no
 * permissions, no hardlinks — exactly the "simplest practical backend"
 * the milestone allows.  write()/O_WRONLY are REFERENCE only.
 */
#include <void/types.h>
#include <void/voidfs.h>
#include <proc/process.h>
#include <syscall/syscall.h>
#include <mm/kheap.h>
#include <dev/serial.h>

/* ── limits ───────────────────────────────────────────────────────────── */
#define VFS_PATH_MAX   512        /* absolute path incl. NUL              */
#define VFS_NAME_MAX   96         /* single path component                */
#define VFS_ROOT_FDS   3          /* fd 0/1/2 are serial, VFS fds start 3 */
#define VFS_TREE_DEPTH 16         /* bounded component list on chdir      */

/* open() access-mode + flags (Linux fcntl values, mirrored in libvoid) */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACC_MODE  3      /* mask of the access-mode bits */

/* ── vnode (filesystem node) ───────────────────────────────────────────── */
typedef enum {
    VFS_REG = 0,       /* regular file  */
    VFS_DIR = 1,       /* directory    */
} vfs_kind_t;

typedef struct vfs_node {
    vfs_kind_t  kind;
    char        name[VFS_NAME_MAX];  /* basename, no slash               */
    struct vfs_node *parent;         /* NULL for root */
    struct vfs_node *child;          /* first child (dirs) */
    struct vfs_node *sibling;        /* next sibling    */
    const char  *data;               /* file contents (readonly) */
    uint64_t    size;
} vfs_node_t;

/* ── open-file object (the fd layer's `object`) ─────────────────────────
 * One per open fd referencing a vnode.  Holds the seek position and, for
 * regular files, a per-open handle id (Lux's FileDescriptor has the same
 * trio — position/refcount/id — minus the socket/device plumbing we skip). */
typedef struct vfs_file_t {
    vfs_node_t *node;
    uint64_t    pos;
    uint64_t    id;            /* global, per-open unique (for future dev) */
    int         refcount;      /* shared on fork; close drops when 0        */
} vfs_file_t;

/* ── the embedded tree ───────────────────────────────────────────────────
 * Static, built once at boot.  The layout the tests expect:
 *   /
 *   ├── hello.txt      "Hello, VoidOS!\n"
 *   ├── test.txt       "This is a test file.\n"
 *   └── etc/
 *       └── version.txt "Phase 10\n"
 * A future initramfs/tar/blob loader builds the same tree from a buffer;
 * the vnode/vfs interface is identical. */

/* ── minimal string helpers (no libc in the kernel) ───────────────────── */
static uint64_t kstrlen(const char *s) {
    if (!s) return 0;
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool kstreq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

/* ── user-string + cwd helpers ────────────────────────────────────────── */

/* Copy a NUL-terminated user string into `out` (cap bytes incl. NUL), one
 * byte at a time, validating each byte's page as it goes.  Returns 0 for a
 * clean copy, or the errno to propagate. */
static int vfs_copy_user_path(process_t *p, uint64_t upath, char *out,
                              uint64_t cap) {
    uint64_t i = 0;
    while (i < cap - 1) {
        if (!user_range_ok(p, upath + i, 1, false)) return VE_FAULT;
        uint8_t c;
        if (!copy_from_user(p, &c, upath + i, 1)) return VE_FAULT;
        out[i++] = (char)c;
        if (c == '\0') return 0;
    }
    return VE_NAMETOOLONG;   /* ran out of room before the NUL */
}

/* Fold a user path into an absolute one: a leading '/' is kept as-is,
 * otherwise the process cwd is prefixed — the Lux recipe (cwd.c).  Returns
 * false when the result does not fit `cap`. */
static bool vfs_absolutize(process_t *p, const char *path, char *abs,
                           uint64_t cap) {
    uint64_t plen = kstrlen(path);
    uint64_t i = 0;
    if (path[0] == '/') {
        if (plen >= cap) return false;
        for (; i < plen; i++) abs[i] = path[i];
    } else {
        uint64_t cw = kstrlen(p->cwd);
        uint64_t total = cw + (cw > 1 ? 1 : 0) + plen;
        if (total >= cap) return false;
        for (; i < cw; i++) abs[i] = p->cwd[i];
        if (cw > 1) abs[i++] = '/';
        for (uint64_t j = 0; j < plen; j++) abs[i++] = path[j];
    }
    abs[i] = '\0';
    return true;
}

/* ── heap-backed tree builder ─────────────────────────────────────────── */
static vfs_node_t *new_node(vfs_kind_t kind, const char *name,
                            const char *data, uint64_t size) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    n->kind = kind;
    n->child = NULL;
    n->sibling = NULL;
    n->parent = NULL;
    n->data = data;
    n->size = size;
    /* copy the name (bounded) */
    uint64_t i = 0;
    while (name[i] && i < VFS_NAME_MAX - 1) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    return n;
}

static vfs_node_t *tree_add_child(vfs_node_t *dir, vfs_node_t *child) {
    if (!dir || !child) return NULL;
    child->parent = dir;
    if (!dir->child) {
        dir->child = child;
    } else {
        vfs_node_t *s = dir->child;
        while (s->sibling) s = s->sibling;
        s->sibling = child;
    }
    return child;
}

static vfs_node_t *tree_find_child(const vfs_node_t *dir, const char *name) {
    for (const vfs_node_t *c = dir->child; c; c = c->sibling)
        if (kstreq(c->name, name)) return (vfs_node_t *)c;
    return NULL;
}

static vfs_node_t *g_root;

/* ── tree construction ───────────────────────────────────────────────── */
static void vfs_build_tree(void) {
    /* the root */
    vfs_node_t *root = new_node(VFS_DIR, "", NULL, 0);
    if (!root) { kprintf("[VFS] FATAL: root alloc failed\n\r"); return; }
    g_root = root;

    /* hello.txt */
    static const char hello_data[] = "Hello, VoidOS!\n";
    vfs_node_t *hello = new_node(VFS_REG, "hello.txt", hello_data,
                                sizeof(hello_data) - 1);
    if (hello) tree_add_child(root, hello);

    /* test.txt */
    static const char test_data[] = "This is a test file.\n";
    vfs_node_t *test = new_node(VFS_REG, "test.txt", test_data,
                               sizeof(test_data) - 1);
    if (test) tree_add_child(root, test);

    /* etc/ */
    vfs_node_t *etc = new_node(VFS_DIR, "etc", NULL, 0);
    if (etc) tree_add_child(root, etc);

    /* etc/version.txt */
    static const char version_data[] = "Phase 10\n";
    vfs_node_t *version = new_node(VFS_REG, "version.txt", version_data,
                                   sizeof(version_data) - 1);
    if (version && etc) tree_add_child(etc, version);

    kprintf("[VFS] embedded tree ready: hello.txt, test.txt, etc/\n\r");
}

/* ── path resolution ─────────────────────────────────────────────────────
 * Resolve an absolute path against the mounted tree.  `path` must begin
 * with '/'.  Handles '/', '//', '.' and '..' components.  Returns the
 * target vnode, or NULL with *err = -VE_* on the first failure. */
static vfs_node_t *vfs_resolve_abs(const char *path, int *err) {
    if (!path || path[0] != '/') { if (err) *err = -VE_INVAL; return NULL; }

    vfs_node_t *cur = g_root;
    const char *p = path + 1;          /* skip initial '/' */
    *err = -VE_NOENT;                  /* default: missing final comp */

    while (*p) {
        /* collapse interior slash runs; an empty string here means the path
         * ended in '/', which the trailing-slash check below resolves */
        while (*p == '/') p++;
        if (!*p) break;

        /* component */
        const char *comp = p;
        while (*p && *p != '/') p++;
        uint64_t clen = (uint64_t)(p - comp);

        if (clen == 1 && comp[0] == '.') {
            continue;                 /* stay */
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (cur->parent) cur = cur->parent;
            continue;
        }

        if (cur->kind != VFS_DIR) { *err = -VE_NOTDIR; return NULL; }

        char name[VFS_NAME_MAX];
        if (clen >= sizeof(name)) { *err = -VE_NAMETOOLONG; return NULL; }
        uint64_t i = 0;
        for (; i < clen; i++) name[i] = comp[i];
        name[i] = '\0';

        vfs_node_t *n = tree_find_child(cur, name);
        if (!n) { *err = -VE_NOENT; return NULL; }   /* not found */

        cur = n;
    }

    /* A trailing slash asserts the target is a directory (POSIX).  A regular
     * file named with a trailing slash — "/hello.txt/" — must not resolve. */
    const char *q = path;
    while (*q) q++;
    if (q > path && q[-1] == '/' && cur->kind != VFS_DIR) {
        *err = -VE_NOTDIR;
        return NULL;
    }

    return cur;
}

/* ── public syscalls ─────────────────────────────────────────────────────
 * All user pointers are forwarded to the caller (syscall.c), which
 * validates them with user_range_ok()/copy_from_user() before we touch
 * them.  Returns negative -VE_* on error, per Void ABI. */

/* sys_open: flags are O_RDONLY=0 / O_WRONLY=1 / O_RDWR=2 (Lux fcntl O_*
 * bits, mirrored above).  Directories open read-only for readdir().  The
 * tree is readonly, so any write-mode open returns -VE_ACCES. */
int64_t sys_vfs_open(process_t *p, uint64_t upath, int flags) {
    if (!p) return -VE_PERM;

    char path[VFS_PATH_MAX];
    int e = vfs_copy_user_path(p, upath, path, sizeof(path));
    if (e) return -e;

    char abs[VFS_PATH_MAX];
    if (!vfs_absolutize(p, path, abs, sizeof(abs))) return -VE_FAULT;

    int err = 0;
    vfs_node_t *node = vfs_resolve_abs(abs, &err);
    if (!node) return (int64_t)err;

    /* the tree is readonly: a regular file must be opened read-only, a
     * directory must be opened read-only (for readdir) too */
    if ((flags & O_ACC_MODE) != O_RDONLY) return -VE_ACCES;

    vfs_file_t *f = kmalloc(sizeof(vfs_file_t));
    if (!f) return -VE_NOMEM;
    f->node = node;
    f->pos = 0;
    f->id = (uint64_t)(uintptr_t)f;   /* unique enough */
    f->refcount = 1;

    /* find a free fd slot (start at 3: 0/1/2 are the console) */
    for (int fd = VFS_ROOT_FDS; fd < MAX_FDS; fd++) {
        if (p->fds[fd].type == FD_NONE) {
            p->fds[fd].type   = FD_VFS;
            p->fds[fd].object = f;
            p->fds[fd].offset = 0;
            return (int64_t)fd;
        }
    }
    kfree(f);
    return -VE_NOMEM;
}

/* sys_read on a VFS fd (fd, buffer, count).  The buffer is validated by the
 * caller (syscall dispatch); we serve from the node's readonly .data. */
int64_t sys_vfs_read(process_t *p, int fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -VE_BADF;
    if (p->fds[fd].type != FD_VFS) return -VE_BADF;
    vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
    if (!f) return -VE_BADF;
    if (f->node->kind != VFS_REG) return -VE_ISDIR;

    if (f->pos >= f->node->size) return 0;     /* EOF */
    uint64_t avail = f->node->size - f->pos;
    if (count > avail) count = avail;

    const void *src = f->node->data + f->pos;
    if (!copy_to_user(p, ubuf, src, count)) return -VE_FAULT;
    f->pos += count;
    return (int64_t)count;
}

/* sys_write on a VFS fd.  The embedded tree is readonly, so any write to
 * a file returns -VE_ACCES (write support is a REFERENCE for a later
 * backend). */
int64_t sys_vfs_write(process_t *p, int fd, uint64_t ubuf, uint64_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -VE_BADF;
    if (p->fds[fd].type != FD_VFS) return -VE_BADF;
    vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
    if (!f) return -VE_BADF;
    (void)ubuf; (void)count;
    return -VE_ACCES;          /* tree is readonly */
}

/* sys_close on a VFS fd: release the file object, free the slot. */
int64_t sys_vfs_close(process_t *p, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -VE_BADF;
    if (p->fds[fd].type != FD_VFS) return -VE_BADF;
    vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
    if (!f) return -VE_BADF;

    f->refcount--;
    if (f->refcount <= 0) kfree(f);
    p->fds[fd].type = FD_NONE;
    p->fds[fd].object = NULL;
    p->fds[fd].offset = 0;
    return 0;
}

/* sys_lseek: adjust the file position.  SEEK_SET=0, SEEK_CUR=1, SEEK_END=2.
 * The vnode only carries the data length, no explicit end offset. */
int64_t sys_vfs_lseek(process_t *p, int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS) return -VE_BADF;
    if (p->fds[fd].type != FD_VFS) return -VE_BADF;
    vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
    if (!f) return -VE_BADF;
    if (f->node->kind != VFS_REG) return -VE_ISDIR;

    int64_t npos;
    switch (whence) {
    case 0: npos = offset; break;
    case 1: npos = (int64_t)f->pos + offset; break;
    case 2: npos = (int64_t)f->node->size + offset; break;
    default: return -VE_INVAL;
    }
    if (npos < 0) return -VE_INVAL;
    f->pos = (uint64_t)npos;
    return npos;
}

/* sys_chdir: resolve path, must be a directory, store absolute in p->cwd.
 * The stored cwd is normalized (no trailing slash except root "/") by
 * rebuilding it root-down from the resolved node. */
int64_t sys_vfs_chdir(process_t *p, uint64_t upath) {
    if (!p) return -VE_PERM;

    char path[VFS_PATH_MAX];
    int e = vfs_copy_user_path(p, upath, path, sizeof(path));
    if (e) return -e;

    char abs[VFS_PATH_MAX];
    if (!vfs_absolutize(p, path, abs, sizeof(abs))) return -VE_FAULT;

    int err = 0;
    vfs_node_t *node = vfs_resolve_abs(abs, &err);
    if (!node) return (int64_t)err;
    if (node->kind != VFS_DIR) return -VE_NOTDIR;

    /* rebuild a normalized absolute path root-down */
    char tmp[VFS_PATH_MAX];
    uint64_t idx = 1; tmp[0] = '/';
    const char *comps[VFS_TREE_DEPTH];
    uint64_t ncomp = 0;
    for (vfs_node_t *n = node; n && n->parent; n = n->parent) {
        if (ncomp >= VFS_TREE_DEPTH) return -VE_NAMETOOLONG;
        comps[ncomp++] = n->name;
    }
    for (uint64_t k = ncomp; k > 0; k--) {
        const char *nm = comps[k - 1];
        uint64_t nmlen = kstrlen(nm);
        if (idx + nmlen + (k > 1 ? 1 : 0) >= sizeof(tmp))
            return -VE_NAMETOOLONG;
        for (uint64_t m = 0; m < nmlen; m++) tmp[idx + m] = nm[m];
        idx += nmlen;
        if (k > 1) tmp[idx++] = '/';
    }
    tmp[idx] = '\0';
    for (uint64_t m = 0; m <= idx; m++) p->cwd[m] = tmp[m];
    return 0;
}

/* sys_getcwd: write the absolute cwd into the user buffer (validated). */
int64_t sys_vfs_getcwd(process_t *p, uint64_t ubuf, uint64_t size) {
    if (!p) return -VE_PERM;
    uint64_t len = kstrlen(p->cwd);
    if (len + 1 > size) return -VE_NAMETOOLONG;
    if (!user_range_ok(p, ubuf, len + 1, true)) return -VE_FAULT;
    if (!copy_to_user(p, ubuf, p->cwd, len + 1)) return -VE_FAULT;
    return (int64_t)len;
}

/* sys_readdir: copy the directory-cursor entry names into the user buffer.
 * fd must be a directory fd.  Returns the length of the entry name (not
 * including NUL) placed at ubuf, or 0 at end of dir, negative on error.
 * The library advances the fd->offset between calls (telldir/seekdir-style
 * position), exactly as calls.com uses the fd offset. */
int64_t sys_vfs_readdir(process_t *p, int fd, uint64_t ubuf) {
    if (fd < 0 || fd >= MAX_FDS) return -VE_BADF;
    if (p->fds[fd].type != FD_VFS) return -VE_BADF;
    vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
    if (!f) return -VE_BADF;
    if (f->node->kind != VFS_DIR) return -VE_NOTDIR;

    /* iterate children; pos is the child cursor (0-based) */
    vfs_node_t *c = f->node->child;
    uint64_t idx = 0;
    while (c && idx < f->pos) { c = c->sibling; idx++; }
    if (!c) return 0;              /* end of directory */

    char name[VFS_NAME_MAX];
    uint64_t nlen = kstrlen(c->name);
    for (uint64_t i = 0; i < nlen; i++) name[i] = c->name[i];
    name[nlen] = '\0';

    if (!user_range_ok(p, ubuf, nlen + 1, true)) return -VE_FAULT;
    if (!copy_to_user(p, ubuf, name, nlen + 1)) return -VE_FAULT;

    f->pos++;                       /* advance cursor */
    return (int64_t)nlen;           /* length of the name, no NUL */
}

/* Take one extra reference to a shared open file (fork's fd-table dup). */
void vfs_file_ref_inc(void *file) {
    vfs_file_t *f = (vfs_file_t *)file;
    if (f) f->refcount++;
}

/* ── fd-table teardown on process destroy/exit ───────────────────────────
 * Close every VFS descriptor still open so no file object leaks. */
void vfs_close_process_fds(process_t *p) {
    if (!p) return;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (p->fds[fd].type == FD_VFS) {
            vfs_file_t *f = (vfs_file_t *)p->fds[fd].object;
            if (f) { f->refcount--; if (f->refcount <= 0) kfree(f); }
            p->fds[fd].type   = FD_NONE;
            p->fds[fd].object = NULL;
            p->fds[fd].offset = 0;
        }
    }
}

/* ── init ─────────────────────────────────────────────────────────────── */
void voidfs_init(void) {
    g_root = NULL;
    kprintf("[VFS] voidfs init\n\r");
    vfs_build_tree();
}