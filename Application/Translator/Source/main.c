/*
 * Entry point: command line, loading the guest program and its dynamic
 * linker, and the initial stack -- what the kernel's exec does for a native
 * program.
 *
 *   <translator> [-0 argv0] [-d] [-strace] <program> [args...]
 *
 * The kernel runs foreign programs as
 *   <translator> -0 <argv[0]> <program path> <argv[1..]>
 * (the same command line qemu-user takes).
 *
 * The guest is loaded into one anonymous reservation: the whole pages of
 * each segment are mapped privately from the file over it (demand-paged),
 * the partial pages at the segment ends are copied in with pread, so
 * segments that share a page are still handled in one place.
 */

#include "xl.h"
#include "host.h"

char g_xl_exe_path[512];
uint64_t g_xl_guest_stack_lo, g_xl_guest_stack_hi;
char g_xl_self_path[512];
char **g_xl_envp;
uint64_t g_xl_prog_base;

#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31
#define AT_MINSIGSTKSZ 51

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    uint64_t base;
    uint64_t entry;
    uint64_t phdr;
    uint64_t phnum;
    char interp[256];
} elf_image_t;

#define GUEST_STACK_SIZE (8ULL * 1024ULL * 1024ULL)
#define MAX_PHDRS 128u

static bool read_full(int fd, void *buf, uint64_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len != 0u) {
        long n = host_pread(fd, p, (size_t)len, off);
        if (n <= 0) {
            return false;
        }
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return true;
}

/* Bring a segment's file bytes [off, off + len) to address va, inside the
 * image's anonymous reservation. The whole pages in the middle are mapped
 * from the file, privately: the host kernel pages them in on first touch, so
 * a process pays only for the pages it uses. Copying them instead made every
 * process exec'd from Chromium's binary hold all ~350 MB of its segments --
 * half a dozen of them is 2 GB, and with Xorg, Wine and the browser running
 * an 8 GiB machine ran out of memory. The partial pages at either end, which
 * may share a page with a neighbouring segment or with .bss, are read in. */
static bool load_segment(int fd, uint64_t va, uint64_t len, uint64_t off)
{
    const uint64_t ms = XL_PAGE_UP(va), me = XL_PAGE_DOWN(va + len);
    if (me > ms && ((va - off) & (XL_PAGE_SIZE - 1u)) == 0u) {
        long r = host_mmap(ms, me - ms, HOST_PROT_READ | HOST_PROT_WRITE,
                           HOST_MAP_PRIVATE | HOST_MAP_FIXED, fd, off + (ms - va));
        if (r == (long)ms) {
            return read_full(fd, (void *)(uintptr_t)va, ms - va, off) &&
                   read_full(fd, (void *)(uintptr_t)me, va + len - me,
                             off + (me - va));
        }
        /* A failed MAP_FIXED may have taken the reservation with it: put
         * anonymous memory back, and copy the whole segment as before. */
        r = host_mmap(ms, me - ms, HOST_PROT_READ | HOST_PROT_WRITE,
                      HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS | HOST_MAP_FIXED, -1, 0);
        if (r != (long)ms) {
            return false;
        }
    }
    return read_full(fd, (void *)(uintptr_t)va, len, off);
}

static int host_prot_of(uint32_t pf)
{
    int prot = 0;
    if ((pf & (PF_R | PF_X)) != 0u) prot |= HOST_PROT_READ;
    if ((pf & PF_W) != 0u) prot |= HOST_PROT_WRITE;
    return prot;
}

/* Load one ELF image. Returns 0 or a negative errno; *why gets a message. */
static int load_elf(const char *path, elf_image_t *out, const char **why)
{
    memset(out, 0, sizeof(*out));
    long fd = host_openat(HOST_AT_FDCWD, path, HOST_O_RDONLY | HOST_O_CLOEXEC, 0);
    if (fd < 0) {
        *why = "cannot open";
        return (int)fd;
    }
    int rc = -XL_ENOEXEC;
    elf64_ehdr_t eh;
    elf64_phdr_t ph[MAX_PHDRS];
    if (!read_full((int)fd, &eh, sizeof(eh), 0)) {
        *why = "cannot read the ELF header";
        goto out;
    }
    if (memcmp(eh.e_ident, "\177ELF", 4) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        *why = "not a 64-bit little-endian ELF file";
        goto out;
    }
    if (eh.e_machine != g_xl_guest.elf_machine) {
        *why = "built for another CPU than this translator runs";
        goto out;
    }
    if ((eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_phentsize != sizeof(elf64_phdr_t) || eh.e_phnum == 0u ||
        eh.e_phnum > MAX_PHDRS) {
        *why = "unsupported ELF type or program headers";
        goto out;
    }
    if (!read_full((int)fd, ph, (uint64_t)eh.e_phnum * sizeof(elf64_phdr_t),
                   eh.e_phoff)) {
        *why = "cannot read the program headers";
        goto out;
    }

    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz != 0u) {
            if (ph[i].p_filesz > ph[i].p_memsz) {
                *why = "segment with filesz > memsz";
                goto out;
            }
            if (XL_PAGE_DOWN(ph[i].p_vaddr) < lo) lo = XL_PAGE_DOWN(ph[i].p_vaddr);
            if (XL_PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi)
                hi = XL_PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz);
        } else if (ph[i].p_type == PT_INTERP) {
            uint64_t n = ph[i].p_filesz;
            if (n == 0u || n >= sizeof(out->interp) ||
                !read_full((int)fd, out->interp, n, ph[i].p_offset)) {
                *why = "bad PT_INTERP";
                goto out;
            }
            out->interp[n] = '\0';
        }
    }
    if (lo >= hi) {
        *why = "no loadable segments";
        goto out;
    }

    /* One read-write reservation for the whole image, segments copied in,
     * then each part given its own protection. */
    uint64_t span = hi - lo;
    long m;
    if (eh.e_type == ET_DYN) {
        m = host_mmap(0, span, HOST_PROT_READ | HOST_PROT_WRITE,
                      HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS, -1, 0);
    } else {
        m = host_mmap(lo, span, HOST_PROT_READ | HOST_PROT_WRITE,
                      HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS | 0x100000 /* FIXED_NOREPLACE */,
                      -1, 0);
        if (m >= 0 && (uint64_t)m != lo) {
            (void)host_munmap((uint64_t)m, span);
            m = -XL_ENOMEM;
        }
    }
    if (m < 0 && m > -4096) {
        *why = eh.e_type == ET_EXEC ?
            "cannot map a fixed-address (non-PIE) program at its link address" :
            "out of memory";
        rc = (int)m;
        goto out;
    }
    uint64_t base = eh.e_type == ET_DYN ? (uint64_t)m - lo : 0u;

    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0u) {
            continue;
        }
        if (!load_segment((int)fd, base + ph[i].p_vaddr, ph[i].p_filesz,
                          ph[i].p_offset)) {
            *why = "cannot read a segment";
            rc = -XL_EIO;
            goto out;
        }
    }
    (void)host_mprotect(base + lo, span, 0);
    uint64_t prev_end = 0;
    int prev_prot = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0u) {
            continue;
        }
        uint64_t s = XL_PAGE_DOWN(base + ph[i].p_vaddr);
        uint64_t e = XL_PAGE_UP(base + ph[i].p_vaddr + ph[i].p_memsz);
        int prot = host_prot_of(ph[i].p_flags);
        (void)host_mprotect(s, e - s, prot);
        xl_code_map_update(s, e - s, ((ph[i].p_flags & PF_X) ? 4 : 0) |
                                     ((ph[i].p_flags & PF_W) ? 2 : 0) | 1);
        if (s < prev_end) {
            (void)host_mprotect(s, prev_end - s, prot | prev_prot);
        }
        prev_end = e;
        prev_prot = prot;
    }

    uint64_t phdr = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) {
            phdr = base + ph[i].p_vaddr;
        }
    }
    for (unsigned i = 0; phdr == 0u && i < eh.e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD && eh.e_phoff >= ph[i].p_offset &&
            eh.e_phoff - ph[i].p_offset < ph[i].p_filesz) {
            phdr = base + ph[i].p_vaddr + (eh.e_phoff - ph[i].p_offset);
        }
    }
    out->base = base;
    out->entry = base + eh.e_entry;
    out->phdr = phdr;
    out->phnum = eh.e_phnum;
    rc = 0;
out:
    (void)host_close((int)fd);
    return rc;
}

/* ---- initial stack ---------------------------------------------------- */

typedef struct {
    uint64_t top;
    uint64_t cur;
} stack_builder_t;

static uint64_t push_bytes(stack_builder_t *b, const void *p, size_t n)
{
    b->cur -= n;
    memcpy((void *)(uintptr_t)b->cur, p, n);
    return b->cur;
}

static uint64_t push_str(stack_builder_t *b, const char *s)
{
    return push_bytes(b, s, strlen(s) + 1u);
}

static uint64_t host_auxv_get(const uint64_t *auxv, uint64_t type, uint64_t dflt)
{
    for (; auxv[0] != AT_NULL; auxv += 2) {
        if (auxv[0] == type) {
            return auxv[1];
        }
    }
    return dflt;
}

static uint64_t build_stack(int gargc, char **gargv, char **envp,
                            const uint64_t *host_auxv, const char *execfn,
                            const elf_image_t *prog, const elf_image_t *interp)
{
    long m = host_mmap(0, GUEST_STACK_SIZE, HOST_PROT_READ | HOST_PROT_WRITE,
                       HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS | HOST_MAP_NORESERVE,
                       -1, 0);
    if (m < 0 && m > -4096) {
        xl_fatal("cannot allocate the guest stack");
    }
    g_xl_guest_stack_lo = (uint64_t)m;
    g_xl_guest_stack_hi = (uint64_t)m + GUEST_STACK_SIZE;
    stack_builder_t b = { (uint64_t)m + GUEST_STACK_SIZE, (uint64_t)m + GUEST_STACK_SIZE };

    uint8_t rnd[16];
    uint64_t host_rnd = host_auxv_get(host_auxv, AT_RANDOM, 0);
    if (host_rnd != 0u) {
        memcpy(rnd, (const void *)(uintptr_t)host_rnd, sizeof(rnd));
    } else {
        struct { int64_t s, ns; } ts = { 0, 0 };
        (void)host_clock_gettime(1, &ts);
        uint64_t x = (uint64_t)ts.ns * 0x9E3779B97F4A7C15ULL ^ (uint64_t)host_getpid();
        memcpy(rnd, &x, 8);
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        memcpy(rnd + 8, &x, 8);
    }
    uint64_t rnd_at = push_bytes(&b, rnd, sizeof(rnd));
    uint64_t platform_at = push_str(&b, g_xl_guest.platform);
    uint64_t execfn_at = push_str(&b, execfn);

    int envc = 0;
    while (envp[envc] != NULL) ++envc;
    uint64_t *env_at = xl_map(((size_t)envc + (size_t)gargc + 2u) * 8u);
    uint64_t *arg_at = env_at + envc + 1;
    for (int i = envc - 1; i >= 0; --i) env_at[i] = push_str(&b, envp[i]);
    for (int i = gargc - 1; i >= 0; --i) arg_at[i] = push_str(&b, gargv[i]);

    uint64_t aux[64];
    unsigned n = 0;
#define AUX(t, v) do { aux[n++] = (t); aux[n++] = (v); } while (0)
    AUX(AT_PHDR, prog->phdr);
    AUX(AT_PHENT, sizeof(elf64_phdr_t));
    AUX(AT_PHNUM, prog->phnum);
    AUX(AT_PAGESZ, XL_PAGE_SIZE);
    AUX(AT_BASE, interp != NULL ? interp->base : 0u);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, prog->entry);
    AUX(AT_UID, host_auxv_get(host_auxv, AT_UID, 0));
    AUX(AT_EUID, host_auxv_get(host_auxv, AT_EUID, 0));
    AUX(AT_GID, host_auxv_get(host_auxv, AT_GID, 0));
    AUX(AT_EGID, host_auxv_get(host_auxv, AT_EGID, 0));
    AUX(AT_PLATFORM, platform_at);
    AUX(AT_HWCAP, g_xl_guest.hwcap);
    AUX(AT_HWCAP2, g_xl_guest.hwcap2);
    AUX(AT_CLKTCK, host_auxv_get(host_auxv, AT_CLKTCK, 100));
    AUX(AT_SECURE, host_auxv_get(host_auxv, AT_SECURE, 0));
    AUX(AT_RANDOM, rnd_at);
    AUX(AT_EXECFN, execfn_at);
    if (g_xl_guest.minsigstksz != 0u) {
        AUX(AT_MINSIGSTKSZ, g_xl_guest.minsigstksz);
    }
    AUX(AT_NULL, 0);
#undef AUX

    uint64_t words = 1u + (uint64_t)gargc + 1u + (uint64_t)envc + 1u + n;
    uint64_t sp = (b.cur - words * 8u) & ~0xFULL;
    uint64_t *w = (uint64_t *)(uintptr_t)sp;
    *w++ = (uint64_t)gargc;
    for (int i = 0; i < gargc; ++i) *w++ = arg_at[i];
    *w++ = 0;
    for (int i = 0; i < envc; ++i) *w++ = env_at[i];
    *w++ = 0;
    for (unsigned i = 0; i < n; ++i) *w++ = aux[i];
    xl_unmap(env_at, ((size_t)envc + (size_t)gargc + 2u) * 8u);
    return sp;
}

static void usage(void)
{
    xl_fatal("usage: <translator> [-0 argv0] [-d] [-strace] <%s program> [args...]",
             g_xl_guest.name);
}

static void set_exe_path(const char *path)
{
    if (path[0] == '/') {
        xl_snprintf(g_xl_exe_path, sizeof(g_xl_exe_path), "%s", path);
        return;
    }
    char cwd[256];
    uint64_t a[6] = { (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0, 0 };
    if (xl_host_syscall_plain(79 /* getcwd */, a) < 0) {
        cwd[0] = '\0';
    }
    xl_snprintf(g_xl_exe_path, sizeof(g_xl_exe_path), "%s/%s", cwd, path);
}

XL_NORETURN void xl_main(uint64_t *sp);

XL_NORETURN void xl_main(uint64_t *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **e = envp;
    while (*e != NULL) ++e;
    const uint64_t *host_auxv = (const uint64_t *)(e + 1);
    g_xl_envp = envp;
    if (argc > 0 && argv[0] != NULL) {
        xl_snprintf(g_xl_self_path, sizeof(g_xl_self_path), "%s", argv[0]);
    }

#ifdef XL_DEFAULT_STRACE
    g_xl_strace = XL_DEFAULT_STRACE;
#endif
    for (char **p = envp; *p != NULL; ++p) {
        if (strcmp(*p, "XL_DEBUG=1") == 0) g_xl_debug = 1;
        if (strncmp(*p, "XL_STRACE=", 10) == 0) g_xl_strace = (*p)[10] - '0';
    }

    /* The main thread's record, first: the signal handlers find it through
     * the thread pointer. */
    gthread_t *t = xl_thread_alloc();
    if (t == NULL) {
        xl_fatal("out of memory");
    }
    xl_host_set_tp(t);
    t->tid = (int32_t)host_gettid();

    const char *argv0 = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-0") == 0 && i + 1 < argc) {
            argv0 = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-d") == 0) {
            g_xl_debug = 1;
            ++i;
        } else if (strcmp(argv[i], "-strace") == 0) {
            g_xl_strace = 1;
            ++i;
        } else if (strcmp(argv[i], "--") == 0) {
            ++i;
            break;
        } else {
            usage();
        }
    }
    if (i >= argc) {
        usage();
    }
    const char *path = argv[i];
    set_exe_path(path);

    elf_image_t prog, interp;
    const char *why = "";
    int rc = load_elf(path, &prog, &why);
    if (rc < 0) {
        xl_fatal("%s: %s (%d)", path, why, rc);
    }
    g_xl_prog_base = prog.base;
    const elf_image_t *ip = NULL;
    if (prog.interp[0] != '\0') {
        rc = load_elf(prog.interp, &interp, &why);
        if (rc < 0) {
            xl_fatal("%s: interpreter %s: %s (%d)", path, prog.interp, why, rc);
        }
        if (interp.interp[0] != '\0') {
            xl_fatal("%s: interpreter %s has an interpreter itself", path, prog.interp);
        }
        ip = &interp;
    }

    /* Guest argv: argv[0] as the caller gave it, then the arguments. */
    int gargc = argc - i;
    char **gargv = &argv[i];
    if (argv0 != NULL) {
        argv[i] = (char *)argv0;
    }
    uint64_t gsp = build_stack(gargc, gargv, envp, host_auxv, path, &prog, ip);

    XL_DEBUG("%s: %s guest, entry %lx%s%s, stack %lx", path, g_xl_guest.name,
             ip != NULL ? ip->entry : prog.entry, ip != NULL ? " via " : "",
             ip != NULL ? prog.interp : "", gsp);

    xl_signal_init();
    guest_cpu_init(t, ip != NULL ? ip->entry : prog.entry, gsp);
    xl_thread_loop(t);
}
