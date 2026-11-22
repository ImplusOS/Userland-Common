/*
 * The AArch64 guest's interface to the rest of the translator: its
 * descriptor, the system call front end and signal frames.
 *
 * System calls arrive in the asm-generic numbering with AArch64 structure
 * layouts and are handed to the canonical (x86_64) layer -- the same
 * translation Kernel/Source/Compat/Linux/arm64/Syscall_LinuxArm64.c does
 * in the kernel: renumbering, struct stat, struct epoll_event (packed on
 * x86_64), four O_* flag values, and clone's argument order.
 */

#include "a64.h"
#include "host.h"

#define HWCAP_FP    (1ULL << 0)
#define HWCAP_ASIMD (1ULL << 1)

const xl_guest_desc_t g_xl_guest = {
    .name = "aarch64",
    .elf_machine = 183,         /* EM_AARCH64 */
    .platform = "aarch64",
    .hwcap = HWCAP_FP | HWCAP_ASIMD,
    .hwcap2 = 0,
    .minsigstksz = 5120,
    .cpu_size = sizeof(a64cpu_t),
    .lib_path = "/lib64:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib",
};

/* ---- CPU state ------------------------------------------------------------------- */

void guest_cpu_init(gthread_t *t, uint64_t pc, uint64_t sp)
{
    a64cpu_t *c = A64(t);
    memset(c, 0, sizeof(*c));
    c->pc = pc;
    c->x[A64_SP] = sp;
}

void guest_thread_enter(gthread_t *t)
{
    a64_fpcr_apply(A64(t));
}

uint64_t guest_pc(gthread_t *t) { return A64(t)->pc; }
uint64_t guest_sp(gthread_t *t) { return A64(t)->x[A64_SP]; }

void guest_dump(gthread_t *t)
{
    a64cpu_t *c = A64(t);
    for (int k = 0; k < 32; k += 4) {
        if (k == 28) {
            xl_log("  x28 %016lx  x29 %016lx  x30 %016lx  sp  %016lx",
                   c->x[28], c->x[29], c->x[30], c->x[A64_SP]);
            break;
        }
        xl_log("  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx",
               k, c->x[k], k + 1, c->x[k + 1], k + 2, c->x[k + 2], k + 3, c->x[k + 3]);
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->pc;
    xl_log("  pc  %016lx  nzcv %x  tpidr %lx  insn %02x%02x%02x%02x",
           c->pc, c->nzcv >> 28, c->tpidr, p[3], p[2], p[1], p[0]);
}

void guest_clone_child(gthread_t *child, uint64_t sp, bool set_tls, uint64_t tls)
{
    a64cpu_t *c = A64(child);
    c->x[0] = 0;
    if (sp != 0u) {
        c->x[A64_SP] = sp;
    }
    if (set_tls) {
        c->tpidr = tls;
    }
    c->excl_size = 0;
}

void guest_fix_uname(void *utsname)
{
    char *machine = (char *)utsname + 4 * 65;
    memset(machine, 0, 65);
    memcpy(machine, "aarch64", 8);
}

long guest_arch_syscall(gthread_t *t, long nr, uint64_t a[6])
{
    (void)t; (void)nr; (void)a;
    return -XL_ENOSYS;
}

/* ---- system calls ------------------------------------------------------------------- */

/* asm-generic number -> x86_64 number, calls with identical arguments */
static const uint16_t k_direct[][2] = {
    {0, 206}, {1, 207}, {2, 209}, {3, 210}, {4, 208}, {5, 188}, {6, 189}, {7, 190},
    {8, 191}, {9, 192}, {10, 193}, {11, 194}, {12, 195}, {13, 196}, {14, 197},
    {15, 198}, {16, 199}, {17, 79}, {18, 212}, {19, 290}, {20, 291}, {23, 32},
    {24, 292}, {26, 294}, {27, 254}, {28, 255}, {29, 16}, {30, 251}, {31, 252},
    {32, 73}, {33, 259}, {34, 258}, {35, 263}, {36, 266}, {37, 265}, {38, 264},
    {39, 166}, {40, 165}, {41, 155}, {43, 137}, {44, 138}, {45, 76}, {46, 77},
    {47, 285}, {48, 269}, {49, 80}, {50, 81}, {51, 161}, {52, 91}, {53, 268},
    {54, 260}, {55, 93}, {57, 3}, {58, 153}, {59, 293}, {60, 179}, {61, 217},
    {62, 8}, {63, 0}, {64, 1}, {65, 19}, {66, 20}, {67, 17}, {68, 18}, {69, 295},
    {70, 296}, {71, 40}, {72, 270}, {73, 271}, {74, 289}, {75, 278}, {76, 275},
    {77, 276}, {78, 267}, {81, 162}, {82, 74}, {83, 75}, {84, 277}, {85, 283},
    {86, 286}, {87, 287}, {88, 280}, {89, 163}, {90, 125}, {91, 126}, {92, 135},
    {93, 60}, {94, 231}, {95, 247}, {96, 218}, {97, 272}, {98, 202}, {99, 273},
    {100, 274}, {101, 35}, {102, 36}, {103, 38}, {104, 246}, {105, 175}, {106, 176},
    {107, 222}, {108, 224}, {109, 225}, {110, 223}, {111, 226}, {112, 227},
    {113, 228}, {114, 229}, {115, 230}, {116, 103}, {117, 101}, {118, 142},
    {119, 144}, {120, 145}, {121, 143}, {122, 203}, {123, 204}, {124, 24},
    {125, 146}, {126, 147}, {127, 148}, {128, 219}, {129, 62}, {130, 200},
    {131, 234}, {132, 131}, {133, 130}, {134, 13}, {135, 14}, {136, 127},
    {137, 128}, {138, 129}, {139, 15}, {140, 141}, {141, 140}, {142, 169},
    {143, 114}, {144, 106}, {145, 113}, {146, 105}, {147, 117}, {148, 118},
    {149, 119}, {150, 120}, {151, 122}, {152, 123}, {153, 100}, {154, 109},
    {155, 121}, {156, 124}, {157, 112}, {158, 115}, {159, 116}, {160, 63},
    {161, 170}, {162, 171}, {163, 97}, {164, 160}, {165, 98}, {166, 95},
    {167, 157}, {168, 309}, {169, 96}, {170, 164}, {171, 159}, {172, 39},
    {173, 110}, {174, 102}, {175, 107}, {176, 104}, {177, 108}, {178, 186},
    {179, 99}, {180, 240}, {181, 241}, {182, 242}, {183, 243}, {184, 244},
    {185, 245}, {186, 68}, {187, 71}, {188, 70}, {189, 69}, {190, 64}, {191, 66},
    {192, 220}, {193, 65}, {194, 29}, {195, 31}, {196, 30}, {197, 67}, {198, 41},
    {199, 53}, {200, 49}, {201, 50}, {202, 43}, {203, 42}, {204, 51}, {205, 52},
    {206, 44}, {207, 45}, {208, 54}, {209, 55}, {210, 48}, {211, 46}, {212, 47},
    {213, 187}, {214, 12}, {215, 11}, {216, 25}, {217, 248}, {218, 249},
    {219, 250}, {221, 59}, {223, 221}, {224, 167}, {225, 168}, {227, 26},
    {228, 149}, {229, 150}, {230, 151}, {231, 152}, {232, 27}, {233, 28},
    {234, 216}, {235, 237}, {236, 239}, {237, 238}, {238, 256}, {239, 279},
    {240, 297}, {241, 298}, {242, 288}, {243, 299}, {260, 61}, {261, 302},
    {262, 300}, {263, 301}, {264, 303}, {265, 304}, {266, 305}, {267, 306},
    {268, 308}, {269, 307}, {270, 310}, {271, 311}, {272, 312}, {273, 313},
    {274, 314}, {275, 315}, {276, 316}, {277, 317}, {278, 318}, {279, 319},
    {280, 321}, {281, 322}, {282, 323}, {283, 324}, {284, 325}, {285, 326},
    {286, 327}, {287, 328}, {288, 329}, {289, 330}, {290, 331}, {291, 332},
    {292, 333}, {293, 334}, {294, 320},
};

#define NR_MAX 512
static int16_t g_map[NR_MAX];
static volatile int g_map_ready;

static void map_init(void)
{
    for (int i = 0; i < NR_MAX; ++i) g_map[i] = -1;
    for (size_t i = 0; i < XL_ARRAY_LEN(k_direct); ++i) g_map[k_direct[i][0]] = (int16_t)k_direct[i][1];
    for (int i = 424; i <= 462; ++i) g_map[i] = (int16_t)i;
    g_map_ready = 1;
}

/* O_* values that differ (AArch64 / x86_64) */
#define A_O_DIRECTORY 0040000u
#define A_O_NOFOLLOW  0100000u
#define A_O_DIRECT    0200000u
#define A_O_LARGEFILE 0400000u
#define X_O_DIRECT    0040000u
#define X_O_LARGEFILE 0100000u
#define X_O_DIRECTORY 0200000u
#define X_O_NOFOLLOW  0400000u
#define O_SWAPPED (0040000u | 0100000u | 0200000u | 0400000u)

static uint64_t oflags_in(uint64_t f)
{
    uint64_t o = f & ~(uint64_t)O_SWAPPED;
    if (f & A_O_DIRECTORY) o |= X_O_DIRECTORY;
    if (f & A_O_NOFOLLOW)  o |= X_O_NOFOLLOW;
    if (f & A_O_DIRECT)    o |= X_O_DIRECT;
    if (f & A_O_LARGEFILE) o |= X_O_LARGEFILE;
    return o;
}

static uint64_t oflags_out(uint64_t f)
{
    uint64_t o = f & ~(uint64_t)O_SWAPPED;
    if (f & X_O_DIRECTORY) o |= A_O_DIRECTORY;
    if (f & X_O_NOFOLLOW)  o |= A_O_NOFOLLOW;
    if (f & X_O_DIRECT)    o |= A_O_DIRECT;
    if (f & X_O_LARGEFILE) o |= A_O_LARGEFILE;
    return o;
}

typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
} x_stat_t;

typedef struct {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint32_t unused4, unused5;
} g_stat_t;

_Static_assert(sizeof(x_stat_t) == 144, "x86_64 stat");
_Static_assert(sizeof(g_stat_t) == 128, "generic stat");

static long stat_out(long r, const x_stat_t *x, uint64_t user)
{
    if (r < 0) return r;
    g_stat_t g;
    memset(&g, 0, sizeof(g));
    g.dev = x->dev; g.ino = x->ino; g.mode = x->mode; g.nlink = (uint32_t)x->nlink;
    g.uid = x->uid; g.gid = x->gid; g.rdev = x->rdev; g.size = x->size;
    g.blksize = (int32_t)x->blksize; g.blocks = x->blocks;
    g.atime = x->atime; g.atime_ns = x->atime_ns;
    g.mtime = x->mtime; g.mtime_ns = x->mtime_ns;
    g.ctime = x->ctime; g.ctime_ns = x->ctime_ns;
    memcpy((void *)(uintptr_t)user, &g, sizeof(g));
    return r;
}

typedef struct __attribute__((packed)) { uint32_t events; uint64_t data; } x_epoll_event_t;
typedef struct { uint32_t events; uint32_t pad; uint64_t data; } g_epoll_event_t;
#define EPOLL_BATCH 256

static long a64_sys(gthread_t *t, long nr, uint64_t a[6])
{
    if (XL_UNLIKELY(!g_map_ready)) map_init();
    uint64_t b[6];
    memcpy(b, a, sizeof(b));
    switch (nr) {
    case 56:    /* openat */
        b[2] = oflags_in(a[2]);
        return xl_syscall(t, 257, b);
    case 437: { /* openat2 */
        uint64_t how[3] = { 0, 0, 0 };
        size_t n = a[3] < sizeof(how) ? (size_t)a[3] : sizeof(how);
        memcpy(how, (const void *)(uintptr_t)a[2], n);
        how[0] = oflags_in(how[0]);
        b[2] = (uint64_t)(uintptr_t)how;
        b[3] = sizeof(how);
        return xl_syscall(t, 437, b);
    }
    case 25: {  /* fcntl */
        if ((int)a[1] == 4) b[2] = oflags_in(a[2]);
        long r = xl_syscall(t, 72, b);
        if ((int)a[1] == 3 && r >= 0) r = (long)oflags_out((uint64_t)r);
        return r;
    }
    case 79: {  /* newfstatat(dfd, path, buf, flags) */
        x_stat_t x;
        b[2] = (uint64_t)(uintptr_t)&x;
        return stat_out(xl_syscall(t, 262, b), &x, a[2]);
    }
    case 80: {  /* fstat(fd, buf) */
        x_stat_t x;
        b[1] = (uint64_t)(uintptr_t)&x;
        return stat_out(xl_syscall(t, 5, b), &x, a[1]);
    }
    case 220:   /* clone(flags, stack, ptid, tls, ctid) */
        b[3] = a[4];
        b[4] = a[3];
        return xl_syscall(t, 56, b);
    case 222:   /* mmap: no BTI/MTE pages here */
    case 226:   /* mprotect */
        b[2] &= 7u;
        return xl_syscall(t, nr == 222 ? 9 : 10, b);
    case 21: {  /* epoll_ctl(epfd, op, fd, event) */
        x_epoll_event_t x;
        if (a[3] != 0u) {
            g_epoll_event_t g;
            memcpy(&g, (const void *)(uintptr_t)a[3], sizeof(g));
            x.events = g.events;
            x.data = g.data;
            b[3] = (uint64_t)(uintptr_t)&x;
        }
        return xl_syscall(t, 233, b);
    }
    case 22: case 441: { /* epoll_pwait, epoll_pwait2 */
        x_epoll_event_t ev[EPOLL_BATCH];
        int64_t max = (int32_t)a[2];
        if (max <= 0) return -XL_EINVAL;
        if (max > EPOLL_BATCH) max = EPOLL_BATCH;
        b[1] = (uint64_t)(uintptr_t)ev;
        b[2] = (uint64_t)max;
        long r = xl_syscall(t, nr == 22 ? 281 : 441, b);
        for (long i = 0; i < r; ++i) {
            x_epoll_event_t x;
            memcpy(&x, &ev[i], sizeof(x));
            g_epoll_event_t g = { x.events, 0, x.data };
            memcpy((void *)((uintptr_t)a[1] + (uintptr_t)i * sizeof(g)), &g, sizeof(g));
        }
        return r;
    }
    default:
        break;
    }
    if (nr >= 0 && nr < NR_MAX && g_map[nr] >= 0) {
        return xl_syscall(t, g_map[nr], b);
    }
    XL_DEBUG("aarch64 system call %ld is not supported", nr);
    return -XL_ENOSYS;
}

void guest_syscall(gthread_t *t)
{
    a64cpu_t *c = A64(t);
    uint64_t a[6] = { c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5] };
    long r = a64_sys(t, (long)c->x[8], a);
    if (r == XL_ERESTART) {
        c->pc -= 4u;            /* back onto the SVC; x0 still holds the argument */
        return;
    }
    if (r == XL_EJUSTRETURN) {
        return;
    }
    c->x[0] = (uint64_t)r;
}

/* ---- signal frames (arch/arm64/kernel/signal.c) ----------------------------------------- */

#define SF_SIGINFO   0u
#define SF_UC        128u
#define UC_STACK     16u
#define UC_SIGMASK   40u
#define UC_MCONTEXT  176u
#define MC_FAULT     0u
#define MC_REGS      8u
#define MC_SP        256u
#define MC_PC        264u
#define MC_PSTATE    272u
#define MC_RESERVED  288u
#define SF_SIZE      (SF_UC + UC_MCONTEXT + MC_RESERVED + 4096u)
#define FPSIMD_MAGIC 0x46508001u
#define FPSIMD_SIZE  528u

static uint64_t g_trampoline;

static uint64_t default_restorer(void)
{
    if (g_trampoline == 0u) {
        static const uint32_t code[] = { 0xD2801168u /* mov x8, #139 */, 0xD4000001u /* svc #0 */ };
        uint8_t *p = xl_map(XL_PAGE_SIZE);
        if (p != NULL) {
            memcpy(p, code, sizeof(code));
            g_trampoline = (uint64_t)(uintptr_t)p;
        }
    }
    return g_trampoline;
}

bool guest_signal_setup(gthread_t *t, int sig, const xl_siginfo_t *info,
                        const xl_ksigaction_t *act, uint64_t saved_mask)
{
    a64cpu_t *c = A64(t);
    const uint64_t old_sp = c->x[A64_SP];
    uint64_t top = xl_signal_frame_top(t, sig, act->flags, old_sp);
    const bool on_alt = top != old_sp || xl_signal_on_altstack(t, old_sp);
    const uint64_t record = (top - 16u) & ~15ULL;
    const uint64_t sp = record - SF_SIZE;

    uint8_t *base = (uint8_t *)(uintptr_t)sp;
    memset(base, 0, SF_SIZE);
    memcpy(base + SF_SIGINFO, info, 128);
    uint8_t *uc = base + SF_UC;
    struct { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; } st = {
        t->ss_sp,
        (t->ss_flags & XL_SS_DISABLE) != 0 ? XL_SS_DISABLE : (on_alt ? XL_SS_ONSTACK : 0),
        0, t->ss_size,
    };
    memcpy(uc + UC_STACK, &st, sizeof(st));
    memcpy(uc + UC_SIGMASK, &saved_mask, 8);
    uint8_t *mc = uc + UC_MCONTEXT;
    uint64_t fault = info->u.addr;
    uint64_t pstate = c->nzcv;
    memcpy(mc + MC_FAULT, &fault, 8);
    memcpy(mc + MC_REGS, c->x, 31u * 8u);
    memcpy(mc + MC_SP, &old_sp, 8);
    memcpy(mc + MC_PC, &c->pc, 8);
    memcpy(mc + MC_PSTATE, &pstate, 8);
    a64_fpsr_collect(c);
    uint32_t head[4] = { FPSIMD_MAGIC, FPSIMD_SIZE, c->fpsr, c->fpcr };
    memcpy(mc + MC_RESERVED, head, sizeof(head));
    memcpy(mc + MC_RESERVED + 16u, c->v, 512);
    uint64_t rec[2] = { c->x[29], c->x[30] };
    memcpy((void *)(uintptr_t)record, rec, sizeof(rec));

    c->x[0] = (uint64_t)sig;
    c->x[1] = sp + SF_SIGINFO;
    c->x[2] = sp + SF_UC;
    c->x[29] = record;
    c->x[30] = (act->flags & XL_SA_RESTORER) != 0u && act->restorer != 0u ?
               act->restorer : default_restorer();
    c->x[A64_SP] = sp;
    c->pc = act->handler;
    c->excl_size = 0;
    return true;
}

long guest_rt_sigreturn(gthread_t *t)
{
    a64cpu_t *c = A64(t);
    const uint64_t sp = c->x[A64_SP];
    const uint8_t *uc = (const uint8_t *)(uintptr_t)(sp + SF_UC);
    const uint8_t *mc = uc + UC_MCONTEXT;
    uint64_t mask, pstate;
    memcpy(&mask, uc + UC_SIGMASK, 8);
    memcpy(c->x, mc + MC_REGS, 31u * 8u);
    memcpy(&c->x[A64_SP], mc + MC_SP, 8);
    memcpy(&c->pc, mc + MC_PC, 8);
    memcpy(&pstate, mc + MC_PSTATE, 8);
    c->nzcv = (uint32_t)pstate & 0xF0000000u;
    uint32_t head[4];
    memcpy(head, mc + MC_RESERVED, sizeof(head));
    if (head[0] == FPSIMD_MAGIC && head[1] >= FPSIMD_SIZE) {
        c->fpsr = head[2];
        c->fpcr = head[3];
        memcpy(c->v, mc + MC_RESERVED + 16u, 512);
        a64_fpcr_apply(c);
    }
    c->excl_size = 0;
    xl_signal_set_guest_mask(t, mask);
    return XL_EJUSTRETURN;
}
