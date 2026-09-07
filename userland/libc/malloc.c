/* VoidOS — userland heap allocator (Phase 13)
 *
 * 16-byte-aligned boundary-tag allocator over two backing stores:
 *   - the brk heap (this file): arena grown from the kernel break in
 *     page-aligned chunks (arena-via-init-latch); first-fit split/reuse;
 *     neighbor coalescing on free via boundary tags.
 *   - anonymous mmap (added in the large-allocation step): allocations at
 *     or above MALLOC_MMAP_THRESHOLD are backed by a private mapping that
 *     free() releases with munmap.
 *
 * Block layout (16-byte granularity everywhere):
 *
 *   [header btag_t:16]  size=total bytes incl. header+footer; flags
 *   [payload           ] user bytes; first 16 hold free-list links when free
 *   [footer btag_t:16  ] free blocks only (size+flags mirror the header)
 *
 * flags bits: bit0 ALLOC (this block in use), bit1 PREV (preceding block
 * is free), bit2 MMAP (backed by an anonymous mapping, free() munmaps).
 *
 * The PREV bit is what makes left-coalescing safe: a footer only exists
 * before a block when the preceding block is free, so free() only reads
 * the 16 bytes at block-16 when PREV is set.  When a block is split, the
 * remainder keeps the header/footer discipline, so the fence post stays
 * exact with no user-visible extra cost.  malloc(0) returns a real,
 * freeable, unique block (minimal size) — documented, POSIX-shaped.
 *
 * Double-free is a deterministic bug report + exit, not UB.  All state is
 * process-local static data, so fork() (eager page copy) and exec()
 * (fresh .bss) need no special handling here.
 */
#include <libc/malloc.h>
#include <libc/string.h>
#include <void.h>

/* ── structure ─────────────────────────────────────────────────────────── */

#define B_ALLOC  (1ULL << 0)   /* block in use                          */
#define B_PREV   (1ULL << 1)   /* the block immediately before is free  */
#define B_MMAP   (1ULL << 2)   /* backed by an anonymous mmap           */

typedef struct {
    uint64_t size;             /* total bytes incl. header + footer     */
    uint64_t flags;
} btag_t;                      /* 16 bytes                              */

#define HEADER_SZ 16
#define MIN_BLOCK 32           /* header + 16 bytes of free-link payload */

/* free-list node lives in the payload of a free block */
typedef struct fnode { struct fnode *prev, *next; } fnode_t;

#define H(b)   ((btag_t *)(b))            /* header at block start       */
#define FN(b)  ((fnode_t *)((char *)(b) + HEADER_SZ))  /* free links     */
#define FOOT(b) ((btag_t *)((char *)(b) + H(b)->size - HEADER_SZ))

#define PAGE_SZ 4096

/* ── arena state (process-local; exec recreates it as zero .bss) ──────── */

static char  *a_start;         /* arena base = the break at first alloc   */
static char  *a_end;           /* arena end == kernel break, grows only   */
static fnode_t *free_head;     /* explicitly-linked free list             */

/* ── error reporting: deterministic, visible, process-killing ────────── */

static void memerr(const char *m) {
    sys_write(2, m, strlen(m));
    sys_exit(1);               /* the regression run sees the message     */
}

/* ── free-list plumbing ───────────────────────────────────────────────── */

static void fl_unlink(fnode_t *n) {
    if (n->prev) n->prev->next = n->next;
    else         free_head = n->next;
    if (n->next) n->next->prev = n->prev;
    n->prev = n->next = NULL;
}

/* Insert `blk` (a free block) into the list; write its footer and mark
 * the following block's PREV bit.  Caller guarantees blk's header flags
 * are already consistent (ALLOC clear, PREV reflecting the block before). */
static void fl_insert(char *blk) {
    btag_t *h = H(blk);
    fnode_t *n = FN(blk);
    n->prev = NULL;
    n->next = free_head;
    if (free_head) free_head->prev = n;
    free_head = n;

    btag_t *f = FOOT(blk);
    f->size  = h->size;
    f->flags = h->flags;

    char *nx = blk + h->size;
    if (nx < a_end) H(nx)->flags |= B_PREV;
}

/* The free block whose end is exactly the arena end, or NULL.  This is
 * the only free block arena_grow can extend in place. */
static char *top_free_block(void) {
    for (fnode_t *n = free_head; n; n = n->next) {
        char *blk = (char *)n - HEADER_SZ;
        if (blk + H(blk)->size == a_end)
            return blk;
    }
    return NULL;
}

/* ── arena growth ──────────────────────────────────────────────────────── */

/* Initialize the arena from the current kernel break.  Called lazily on
 * the first allocation; the sbrk(0) path reads the break and never moves
 * it (the Phase 12 query fix). */
static int arena_init(void) {
    if (!a_start) {
        void *b = sys_sbrk(0);
        if (b == (void *)-1) return -1;
        a_start = (char *)b;
        a_end   = (char *)b;
    }
    return 0;
}

/* Extend the arena so at least `need` more bytes are joinable free space.
 * Grows in page-aligned chunks; a free top block is extended in place. */
static int arena_grow(size_t need) {
    size_t chunk = (need + PAGE_SZ - 1) / PAGE_SZ * PAGE_SZ;
    if (chunk < PAGE_SZ) chunk = PAGE_SZ;

    void *old = sys_sbrk((long)chunk);
    if (old == (void *)-1) return -1;

    if ((char *)old != a_end) {
        /* kernel break disagrees with the latch — defensive recovery: a
         * fresh free block at `old`, no row with the unknown gap before it. */
        a_end = (char *)old + chunk;
        btag_t *h = H(old);
        h->size  = chunk;
        h->flags = 0;
        btag_t *f = FOOT(h); f->size = chunk; f->flags = 0;
        fl_insert((char *)old);
        return 0;
    }

    char *tb = top_free_block();
    if (tb) {
        /* extend the top free block in place — no new list node */
        btag_t *h = H(tb);
        h->size += chunk;
        btag_t *f = FOOT(tb); f->size = h->size; f->flags = h->flags;
        a_end += chunk;
        return 0;
    }

    /* create a fresh free block above an allocated top */
    btag_t *h = H(a_end);
    h->size  = chunk;
    h->flags = 0;              /* the block before it is allocated        */
    a_end += chunk;
    btag_t *f = FOOT(h); f->size = chunk; f->flags = 0;
    fl_insert((char *)h);
    return 0;
}

/* ── allocation ────────────────────────────────────────────────────────── */

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

/* First-fit cut of a free block covering >= need bytes, returning the
 * 16-aligned payload, or NULL.  Splits the remainder when it is large
 * enough to be its own block. */
static void *cut_first_fit(size_t need) {
    for (fnode_t *n = free_head; n; n = n->next) {
        char *blk = (char *)n - HEADER_SZ;
        btag_t *h = H(blk);
        if (h->size < need) continue;

        if (h->size - need >= MIN_BLOCK) {
            /* split: prefix P becomes allocated, remainder R stays free  */
            btag_t *r = H(blk) + need / 16;   /* r at blk + need           */
            r->size  = h->size - need;
            r->flags = 0;          /* P (allocated) precedes R             */
            btag_t *rf = FOOT((char *)r);
            rf->size  = r->size; rf->flags = r->flags;

            fnode_t *rn = FN((char *)r);
            rn->prev = n->prev; rn->next = n->next;
            if (rn->prev) rn->prev->next = rn; else free_head = rn;
            if (rn->next) rn->next->prev = rn;

            h->size  = need;
            h->flags = (h->flags & (B_PREV | B_MMAP)) | B_ALLOC;
            return (char *)blk + HEADER_SZ;
        }

        /* take the whole block */
        h->flags |= B_ALLOC;
        char *nx = blk + h->size;
        if (nx < a_end) H(nx)->flags &= ~B_PREV;   /* now preceded by alloc */
        fl_unlink(n);
        return (char *)blk + HEADER_SZ;
    }
    return NULL;
}

/* ── the public API  ────────────────────────────────────────────────────── */

/* mmap-backed allocation: a full private anonymous mapping of `need`
 * bytes.  The block begins at the mapping base with a normal header tagged
 * B_MMAP, so free() branches on the flag and munmaps instead of touching
 * the free list.  Page-rounded to the kernel's 4 KiB granularity. */
static void *mmap_alloc(size_t size) {
    size_t need = HEADER_SZ + align16(size);
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    size_t rounded = (need + PAGE_SZ - 1) / PAGE_SZ * PAGE_SZ;

    void *base = sys_mmap((void *)0, rounded, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;

    btag_t *h = H(base);
    h->size  = rounded;
    h->flags = B_ALLOC | B_MMAP;
    return (char *)base + HEADER_SZ;
}

static void *heap_alloc(size_t size) {
    size_t need = HEADER_SZ + align16(size);
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    if (arena_init() != 0) return NULL;

    void *p = cut_first_fit(need);
    if (!p && arena_grow(need) == 0)
        p = cut_first_fit(need);
    return p;
}

static void heap_free(char *v) {
    /* The block header lives 16 bytes before the caller's payload pointer
     * (H(v) would read the payload as a header — a real, found bug). */
    char *blk = v - (ptrdiff_t)HEADER_SZ;
    btag_t *h = H(blk);
    if (!(h->flags & B_ALLOC))
        memerr("malloc: double free\n");   /* deterministic policy        */

    char *nx  = blk + h->size;

    /* right coalesce: absorb a following free block */
    if (nx < a_end && !(H(nx)->flags & B_ALLOC)) {
        h->size += H(nx)->size;
        fl_unlink(FN(nx));
    }

    /* left coalesce: absorb into the preceding free block */
    if (h->flags & B_PREV) {
        uint64_t psz = *(uint64_t *)(blk - (ptrdiff_t)HEADER_SZ); /* prev footer size */
        char *pb = blk - (ptrdiff_t)psz;                       /* prev block        */
        btag_t *ph = H(pb);
        ph->size += h->size;
        blk = pb, h = ph;
        fl_unlink(FN(blk));
    }

    /* mark free: clear ALLOC, keep PREV (whether the block before is free) */
    h->flags &= ~B_ALLOC;
    fl_insert(blk);
}

/* ── public: malloc / free ─────────────────────────────────────────────── */

void *malloc(size_t size) {
    /* large — at, say, 128 KiB and up — go to their own mapping so a single
     * big allocation never fragments the shared brk heap; small ones share
     * the arena.  Callers can't tell the difference from the pointer. */
    if (size >= MALLOC_MMAP_THRESHOLD)
        return mmap_alloc(size);
    return heap_alloc(size);
}

void free(void *ptr) {
    if (!ptr) return;
    char *v = (char *)ptr;
    if (H(v - (ptrdiff_t)HEADER_SZ)->flags & B_MMAP) {
        /* release the whole mapping — the block is exactly one mmap */
        char *blk = v - (ptrdiff_t)HEADER_SZ;
        sys_munmap(blk, H(blk)->size);
        return;
    }
    heap_free(v);
}

/* ── self-check (the regression test calls it between phases) ───────────
 * Walks the free list verifying structural invariants; returns 0 when the
 * allocator is consistent, a negative code otherwise.  Adversarial:
 * checks sizes, blocks-in-arena, footer mirror, no adjacent free blocks. */
int malloc_selfcheck(void) {
    size_t n = 0;
    for (fnode_t *f = free_head; f; f = f->next) {
        if (++n > 1000000) return -1;              /* cycle                 */
        char *blk = (char *)f - HEADER_SZ;
        btag_t *h = H(blk);
        if (h->flags & B_ALLOC)      return -2;     /* free list has a used blk */
        if (h->size < MIN_BLOCK)     return -3;
        if (h->size & 15)            return -4;
        if (blk < a_start || blk + h->size > a_end) return -5;
        btag_t *ft = FOOT(blk);
        if (ft->size != h->size || ft->flags != h->flags) return -6;
        if (h->flags & B_PREV) {
            uint64_t psz = *(uint64_t *)(blk - HEADER_SZ);
            char *pb = blk - (ptrdiff_t)psz;
            if (pb >= a_start && !(H(pb)->flags & B_ALLOC)) return -7; /* adj free */
        }
        char *nx = blk + h->size;
        if (nx < a_end && !(H(nx)->flags & B_ALLOC)) return -8;       /* adj free */
    }
    return 0;
}