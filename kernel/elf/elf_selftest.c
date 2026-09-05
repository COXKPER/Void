/* VoidOS — ELF loader self-check (Phase 5A)
 *
 * Mutates a copy of the real test executable one field at a time and asserts
 * elf_validate() rejects each result.  Using a genuine binary as the base
 * matters: every case differs from a *loadable* image in exactly one way, so
 * a pass proves that specific check fired rather than some earlier one.
 *
 * Runs at boot before any user process starts.  A failure here means the
 * loader would accept a malformed binary, so it is reported loudly.
 */
#include <elf/elf.h>
#include <proc/process.h>
#include <dev/serial.h>

#define SCRATCH_MAX  (32 * 1024)

/* .bss, not the heap: this runs before the heap has grown and the size is
 * known at compile time. */
static uint8_t  scratch[SCRATCH_MAX];
static uint64_t scratch_len;

static Elf64_Ehdr *sc_ehdr(void) { return (Elf64_Ehdr *)scratch; }

/* First PT_LOAD in the scratch copy.  The test binary's is index 0 (R+X
 * text), but finding it by type keeps the checks working if the link script
 * ever reorders segments. */
static Elf64_Phdr *sc_first_load(void) {
    Elf64_Ehdr *eh = sc_ehdr();
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(scratch + eh->e_phoff
                                        + (uint64_t)i * sizeof(Elf64_Phdr));
        if (ph->p_type == PT_LOAD) return ph;
    }
    return NULL;
}

static void sc_reset(const uint8_t *img, uint64_t len) {
    scratch_len = len;
    for (uint64_t i = 0; i < len; i++) scratch[i] = img[i];
}

/* Run one case: `len` lets a case shrink the apparent image size. */
static bool expect_reject(const char *name, uint64_t len) {
    elf_loader_t ctx;
    elf_status_t s = elf_validate(scratch, len, &ctx);
    if (s == ELF_OK) {
        kprintf("[ELF-TEST] FAIL: accepted malformed image (%s)\n\r", name);
        return false;
    }
    return true;
}

/* ── elf_selftest ────────────────────────────────────────────────────────
 * Returns the number of failures. */
uint32_t elf_selftest(const void *image, uint64_t size) {
    if (size > SCRATCH_MAX) {
        kprintf("[ELF-TEST] SKIP: image %u B exceeds %u B scratch\n\r",
                size, (uint64_t)SCRATCH_MAX);
        return 0;
    }

    uint32_t fails = 0, cases = 0;
    const uint8_t *img = (const uint8_t *)image;
    elf_loader_t ctx;

    /* Baseline: the pristine image must be accepted, or every rejection
     * below proves nothing. */
    if (elf_validate(image, size, &ctx) != ELF_OK) {
        kprintf("[ELF-TEST] FAIL: rejected the valid reference image\n\r");
        return 1;
    }
    cases++;

    /* ── identity ───────────────────────────────────────────────────── */
    sc_reset(img, size); scratch[0] = 0x7E;
    fails += !expect_reject("bad magic", size); cases++;

    sc_reset(img, size); scratch[EI_CLASS] = 1;              /* ELFCLASS32 */
    fails += !expect_reject("wrong class", size); cases++;

    sc_reset(img, size); scratch[EI_DATA] = 2;               /* big-endian */
    fails += !expect_reject("wrong endianness", size); cases++;

    sc_reset(img, size); scratch[EI_VERSION] = 0;
    fails += !expect_reject("bad ident version", size); cases++;

    /* ── header fields ──────────────────────────────────────────────── */
    sc_reset(img, size); sc_ehdr()->e_machine = 3;           /* EM_386 */
    fails += !expect_reject("wrong machine", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_type = 1;              /* ET_REL */
    fails += !expect_reject("not ET_EXEC", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_version = 0;
    fails += !expect_reject("bad e_version", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_ehsize = 52;           /* 32-bit size */
    fails += !expect_reject("bad e_ehsize", size); cases++;

    /* Truncation: too short even for the fixed header. */
    sc_reset(img, size);
    fails += !expect_reject("truncated header", sizeof(Elf64_Ehdr) - 1); cases++;

    /* ── program header table ───────────────────────────────────────── */
    sc_reset(img, size); sc_ehdr()->e_phentsize = 56 + 8;
    fails += !expect_reject("oversized e_phentsize", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_phentsize = 32;
    fails += !expect_reject("undersized e_phentsize", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_phnum = 0;
    fails += !expect_reject("zero e_phnum", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_phnum = 4096;
    fails += !expect_reject("absurd e_phnum", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_phoff = size - 8;
    fails += !expect_reject("phdr table past EOF", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_phoff = 0xFFFFFFFFFFFFFF00ULL;
    fails += !expect_reject("e_phoff overflow", size); cases++;

    /* ── segment file range ─────────────────────────────────────────── */
    sc_reset(img, size); sc_first_load()->p_filesz = sc_first_load()->p_memsz + 1;
    fails += !expect_reject("p_filesz > p_memsz", size); cases++;

    sc_reset(img, size); sc_first_load()->p_offset = size + 0x1000;
    fails += !expect_reject("p_offset past EOF", size); cases++;

    sc_reset(img, size);
    sc_first_load()->p_filesz = 0xFFFFFFFFFFFFFFF0ULL;
    sc_first_load()->p_memsz  = 0xFFFFFFFFFFFFFFF0ULL;
    fails += !expect_reject("file range overflow", size); cases++;

    /* ── segment virtual range ──────────────────────────────────────── */
    sc_reset(img, size);
    sc_first_load()->p_vaddr = 0xFFFFFFFFFFFFF000ULL;
    fails += !expect_reject("vaddr+memsz overflow", size); cases++;

    sc_reset(img, size); sc_first_load()->p_vaddr = 0xFFFFFFFF80000000ULL;
    fails += !expect_reject("kernel-half vaddr", size); cases++;

    sc_reset(img, size); sc_first_load()->p_vaddr = 0x0000800000000000ULL;
    fails += !expect_reject("non-canonical vaddr", size); cases++;

    sc_reset(img, size); sc_first_load()->p_vaddr = 0x1000;
    fails += !expect_reject("vaddr below user minimum", size); cases++;

    /* Overlaps the stack range this loader would map. */
    sc_reset(img, size);
    sc_first_load()->p_vaddr = USER_STACK_TOP - 0x2000;
    fails += !expect_reject("vaddr collides with user stack", size); cases++;

    /* ── permissions ────────────────────────────────────────────────── */
    sc_reset(img, size); sc_first_load()->p_flags = PF_R | PF_W | PF_X;
    fails += !expect_reject("W+X segment", size); cases++;

    sc_reset(img, size); sc_first_load()->p_flags = PF_X;    /* no PF_R */
    fails += !expect_reject("segment without PF_R", size); cases++;

    /* ── alignment ──────────────────────────────────────────────────── */
    sc_reset(img, size); sc_first_load()->p_align = 3;       /* not 2^n */
    fails += !expect_reject("p_align not a power of two", size); cases++;

    sc_reset(img, size); sc_first_load()->p_vaddr += 1;      /* breaks congruence */
    fails += !expect_reject("p_offset/p_vaddr congruence broken", size); cases++;

    /* ── entry point ────────────────────────────────────────────────── */
    sc_reset(img, size); sc_ehdr()->e_entry = 0;
    fails += !expect_reject("null entry point", size); cases++;

    sc_reset(img, size); sc_ehdr()->e_entry = 0xFFFFFFFF80000000ULL;
    fails += !expect_reject("kernel-half entry point", size); cases++;

    /* Inside the image's address range but not inside an executable
     * segment — the check that stops a jump into .rodata or .data. */
    sc_reset(img, size);
    sc_ehdr()->e_entry = sc_first_load()->p_vaddr + sc_first_load()->p_memsz + 0x8;
    fails += !expect_reject("entry outside executable segment", size); cases++;

    /* ── degenerate arguments ───────────────────────────────────────── */
    if (elf_validate(NULL, size, &ctx) == ELF_OK) {
        kprintf("[ELF-TEST] FAIL: accepted NULL image\n\r"); fails++;
    }
    cases++;
    if (elf_validate(image, 0, &ctx) == ELF_OK) {
        kprintf("[ELF-TEST] FAIL: accepted zero-length image\n\r"); fails++;
    }
    cases++;

    kprintf("[ELF-TEST] %s: %u/%u validation cases\n\r",
            fails ? "FAIL" : "PASS", (uint64_t)(cases - fails), (uint64_t)cases);
    return fails;
}
