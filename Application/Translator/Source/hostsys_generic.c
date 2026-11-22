/*
 * Canonical (Linux x86_64) system calls on a host with the asm-generic
 * system call table -- the arm64 translator.
 *
 * Most calls are the same call under another number (both ABIs are LP64
 * and share their structure layouts). The rest:
 *   - calls the generic table dropped: open, stat, poll, select, pipe, dup2,
 *     the path calls without *at, fork, alarm, time, epoll_create/_wait, ...
 *     -> their *at / *2 / p* successors;
 *   - struct stat (generic layout) and struct epoll_event (x86_64 packs it
 *     to 12 bytes) are converted;
 *   - four O_* flags have other values on arm64;
 *   - clone takes tls before child_tid;
 *   - getdents (old struct linux_dirent) is built from getdents64.
 * The inverse of Kernel/Source/Compat/Linux/arm64/Syscall_LinuxArm64.c.
 */

#include "xl.h"
#include "host.h"

#if defined(__aarch64__)

#define AT_FDCWD (-100L)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200

/* x86_64 number -> generic number, for calls with identical arguments. */
static const uint16_t k_direct[][2] = {
    {0, 63}, {1, 64}, {3, 57}, {8, 62}, {9, 222}, {10, 226}, {11, 215},
    {12, 214}, {14, 135}, {16, 29}, {17, 67}, {18, 68}, {19, 65}, {20, 66},
    {24, 124}, {25, 216}, {26, 227}, {27, 232}, {28, 233}, {29, 194},
    {30, 196}, {31, 195}, {32, 23}, {35, 101}, {36, 102}, {38, 103},
    {39, 172}, {40, 71}, {41, 198}, {42, 203}, {43, 202}, {44, 206},
    {45, 207}, {46, 211}, {47, 212}, {48, 210}, {49, 200}, {50, 201},
    {51, 204}, {52, 205}, {53, 199}, {54, 208}, {55, 209}, {59, 221},
    {60, 93}, {61, 260}, {62, 129}, {63, 160}, {64, 190}, {65, 193},
    {66, 191}, {67, 197}, {68, 186}, {69, 189}, {70, 188}, {71, 187},
    {73, 32}, {74, 82}, {75, 83}, {76, 45}, {77, 46}, {79, 17}, {80, 49},
    {81, 50}, {91, 52}, {93, 55}, {95, 166}, {96, 169}, {97, 163},
    {98, 165}, {99, 179}, {100, 153}, {101, 117}, {102, 174}, {103, 116},
    {104, 176}, {105, 146}, {106, 144}, {107, 175}, {108, 177}, {109, 154},
    {110, 173}, {112, 157}, {113, 145}, {114, 143}, {115, 158}, {116, 159},
    {117, 147}, {118, 148}, {119, 149}, {120, 150}, {121, 155}, {122, 151},
    {123, 152}, {124, 156}, {125, 90}, {126, 91}, {127, 136}, {128, 137},
    {129, 138}, {130, 133}, {131, 132}, {135, 92}, {137, 43}, {138, 44},
    {140, 141}, {141, 140}, {142, 118}, {143, 121}, {144, 119}, {145, 120},
    {146, 125}, {147, 126}, {148, 127}, {149, 228}, {150, 229}, {151, 230},
    {152, 231}, {153, 58}, {155, 41}, {157, 167}, {159, 171}, {160, 164},
    {161, 51}, {162, 81}, {163, 89}, {164, 170}, {165, 40}, {166, 39},
    {167, 224}, {168, 225}, {169, 142}, {170, 161}, {171, 162}, {175, 105},
    {176, 106}, {179, 60}, {186, 178}, {187, 213}, {188, 5}, {189, 6},
    {190, 7}, {191, 8}, {192, 9}, {193, 10}, {194, 11}, {195, 12},
    {196, 13}, {197, 14}, {198, 15}, {199, 16}, {200, 130}, {202, 98},
    {203, 122}, {204, 123}, {206, 0}, {207, 1}, {208, 4}, {209, 2},
    {210, 3}, {212, 18}, {216, 234}, {217, 61}, {218, 96}, {219, 128},
    {220, 192}, {221, 223}, {222, 107}, {223, 110}, {224, 108}, {225, 109},
    {226, 111}, {227, 112}, {228, 113}, {229, 114}, {230, 115}, {231, 94},
    {234, 131}, {237, 235}, {238, 237}, {239, 236}, {240, 180}, {241, 181},
    {242, 182}, {243, 183}, {244, 184}, {245, 185}, {246, 104}, {247, 95},
    {248, 217}, {249, 218}, {250, 219}, {251, 30}, {252, 31}, {254, 27},
    {255, 28}, {256, 238}, {258, 34}, {259, 33}, {260, 54}, {263, 35},
    {264, 38}, {265, 37}, {266, 36}, {267, 78}, {268, 53}, {269, 48},
    {270, 72}, {271, 73}, {272, 97}, {273, 99}, {274, 100}, {275, 76},
    {276, 77}, {277, 84}, {278, 75}, {279, 239}, {280, 88}, {283, 85},
    {285, 47}, {286, 86}, {287, 87}, {288, 242}, {289, 74}, {290, 19},
    {291, 20}, {292, 24}, {293, 59}, {294, 26}, {295, 69}, {296, 70},
    {297, 240}, {298, 241}, {299, 243}, {300, 262}, {301, 263}, {302, 261},
    {303, 264}, {304, 265}, {305, 266}, {306, 267}, {307, 269}, {308, 268},
    {309, 168}, {310, 270}, {311, 271}, {312, 272}, {313, 273}, {314, 274},
    {315, 275}, {316, 276}, {317, 277}, {318, 278}, {319, 279}, {320, 294},
    {321, 280}, {322, 281}, {323, 282}, {324, 283}, {325, 284}, {326, 285},
    {327, 286}, {328, 287}, {329, 288}, {330, 289}, {331, 290}, {332, 291},
    {333, 292}, {334, 293},
};

#define NR_MAX 512
static int16_t g_map[NR_MAX];
static volatile int g_map_ready;

static void map_init(void)
{
    for (int i = 0; i < NR_MAX; ++i) {
        g_map[i] = -1;
    }
    for (size_t i = 0; i < XL_ARRAY_LEN(k_direct); ++i) {
        g_map[k_direct[i][0]] = (int16_t)k_direct[i][1];
    }
    /* 424 and up are numbered alike everywhere */
    for (int i = 424; i < NR_MAX && i <= 462; ++i) {
        g_map[i] = (int16_t)i;
    }
    g_map[437] = -1;   /* openat2: flags converted below */
    g_map[441] = -1;   /* epoll_pwait2: events converted below */
    g_map_ready = 1;
}

/* generic numbers used below */
enum {
    G_getcwd = 17, G_eventfd2 = 19, G_epoll_create1 = 20, G_epoll_ctl = 21,
    G_epoll_pwait = 22, G_dup3 = 24, G_fcntl = 25, G_inotify_init1 = 26,
    G_mknodat = 33, G_mkdirat = 34, G_unlinkat = 35, G_symlinkat = 36,
    G_linkat = 37, G_renameat = 38, G_faccessat = 48, G_fchmodat = 53,
    G_fchownat = 54, G_openat = 56, G_pipe2 = 59, G_getdents64 = 61,
    G_pselect6 = 72, G_ppoll = 73, G_signalfd4 = 74, G_readlinkat = 78,
    G_newfstatat = 79, G_fstat = 80, G_utimensat = 88, G_setitimer = 103,
    G_clock_gettime = 113, G_getpgid = 155, G_clone = 220,
    G_openat2 = 437, G_epoll_pwait2 = 441,
};

static long hcall(gthread_t *t, long nr, uint64_t a0, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (t != NULL) {
        return host_safe_syscall(t, nr, (long)a0, (long)a1, (long)a2, (long)a3,
                                 (long)a4, (long)a5);
    }
    return host_syscall6(nr, (long)a0, (long)a1, (long)a2, (long)a3, (long)a4,
                         (long)a5);
}

/* O_* flags: x86_64 / generic values that differ */
#define X_O_DIRECT    0040000u
#define X_O_LARGEFILE 0100000u
#define X_O_DIRECTORY 0200000u
#define X_O_NOFOLLOW  0400000u
#define G_O_DIRECTORY 0040000u
#define G_O_NOFOLLOW  0100000u
#define G_O_DIRECT    0200000u
#define G_O_LARGEFILE 0400000u
#define O_SWAPPED (0040000u | 0100000u | 0200000u | 0400000u)

static uint64_t oflags_to_host(uint64_t f)
{
    uint64_t o = f & ~(uint64_t)O_SWAPPED;
    if (f & X_O_DIRECT)    o |= G_O_DIRECT;
    if (f & X_O_LARGEFILE) o |= G_O_LARGEFILE;
    if (f & X_O_DIRECTORY) o |= G_O_DIRECTORY;
    if (f & X_O_NOFOLLOW)  o |= G_O_NOFOLLOW;
    return o;
}

static uint64_t oflags_from_host(uint64_t f)
{
    uint64_t o = f & ~(uint64_t)O_SWAPPED;
    if (f & G_O_DIRECT)    o |= X_O_DIRECT;
    if (f & G_O_LARGEFILE) o |= X_O_LARGEFILE;
    if (f & G_O_DIRECTORY) o |= X_O_DIRECTORY;
    if (f & G_O_NOFOLLOW)  o |= X_O_NOFOLLOW;
    return o;
}

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
_Static_assert(sizeof(g_stat_t) == 128, "generic stat");

typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
} x_stat_t;
_Static_assert(sizeof(x_stat_t) == 144, "x86_64 stat");

static long stat_out(long r, const g_stat_t *g, uint64_t user)
{
    if (r < 0) {
        return r;
    }
    x_stat_t x;
    memset(&x, 0, sizeof(x));
    x.dev = g->dev; x.ino = g->ino; x.nlink = g->nlink; x.mode = g->mode;
    x.uid = g->uid; x.gid = g->gid; x.rdev = g->rdev; x.size = g->size;
    x.blksize = g->blksize; x.blocks = g->blocks;
    x.atime = g->atime; x.atime_ns = g->atime_ns;
    x.mtime = g->mtime; x.mtime_ns = g->mtime_ns;
    x.ctime = g->ctime; x.ctime_ns = g->ctime_ns;
    memcpy((void *)(uintptr_t)user, &x, sizeof(x));
    return r;
}

typedef struct __attribute__((packed)) { uint32_t events; uint64_t data; } x_epoll_event_t;
typedef struct { uint32_t events; uint32_t pad; uint64_t data; } g_epoll_event_t;

#define EPOLL_BATCH 256

static long epoll_wait_conv(gthread_t *t, long nr, uint64_t a[6], long hnr)
{
    g_epoll_event_t ev[EPOLL_BATCH];
    int64_t max = (int32_t)a[2];
    if (max <= 0) {
        return -XL_EINVAL;
    }
    if (max > EPOLL_BATCH) {
        max = EPOLL_BATCH;
    }
    long r;
    if (nr == 232) {   /* epoll_wait */
        r = hcall(t, hnr, a[0], (uint64_t)(uintptr_t)ev, (uint64_t)max, a[3], 0, 8);
    } else {
        r = hcall(t, hnr, a[0], (uint64_t)(uintptr_t)ev, (uint64_t)max, a[3], a[4], a[5]);
    }
    if (r > 0) {
        x_epoll_event_t *out = (x_epoll_event_t *)(uintptr_t)a[1];
        for (long i = 0; i < r; ++i) {
            x_epoll_event_t e = { ev[i].events, ev[i].data };
            memcpy(&out[i], &e, sizeof(e));
        }
    }
    return r;
}

typedef struct { int64_t sec, nsec; } ts_t;
typedef struct { int64_t sec, usec; } tv_t;

static long getdents_conv(gthread_t *t, uint64_t a[6])
{
    uint8_t buf[4096];
    uint64_t want = a[2] < sizeof(buf) ? a[2] : sizeof(buf);
    long r = hcall(t, G_getdents64, a[0], (uint64_t)(uintptr_t)buf, want, 0, 0, 0);
    if (r <= 0) {
        return r;
    }
    /* linux_dirent64 {ino, off, reclen, type, name} -> linux_dirent
     * {ino, off, reclen, name, pad, type}: the record keeps its size. */
    uint8_t *out = (uint8_t *)(uintptr_t)a[1];
    for (long off = 0; off < r;) {
        uint64_t ino, doff;
        uint16_t reclen;
        memcpy(&ino, buf + off, 8);
        memcpy(&doff, buf + off + 8, 8);
        memcpy(&reclen, buf + off + 16, 2);
        uint8_t type = buf[off + 18];
        const char *name = (const char *)buf + off + 19;
        size_t nlen = strlen(name);
        uint8_t *o = out + off;
        memcpy(o, &ino, 8);
        memcpy(o + 8, &doff, 8);
        memcpy(o + 16, &reclen, 2);
        memmove(o + 18, name, nlen + 1u);
        o[reclen - 1u] = type;
        off += reclen;
    }
    return r;
}

static long translate(gthread_t *t, long nr, uint64_t a[6])
{
    if (XL_UNLIKELY(!g_map_ready)) {
        map_init();
    }
    switch (nr) {
    case 2:    /* open */
        return hcall(t, G_openat, (uint64_t)AT_FDCWD, a[0], oflags_to_host(a[1]), a[2], 0, 0);
    case 85:   /* creat */
        return hcall(t, G_openat, (uint64_t)AT_FDCWD, a[0], 01101 /* O_CREAT|O_WRONLY|O_TRUNC */,
                     a[1], 0, 0);
    case 257:  /* openat */
        return hcall(t, G_openat, a[0], a[1], oflags_to_host(a[2]), a[3], 0, 0);
    case 437: { /* openat2 */
        uint64_t how[3] = { 0, 0, 0 };
        size_t n = a[3] < sizeof(how) ? (size_t)a[3] : sizeof(how);
        memcpy(how, (const void *)(uintptr_t)a[2], n);
        how[0] = oflags_to_host(how[0]);
        return hcall(t, G_openat2, a[0], a[1], (uint64_t)(uintptr_t)how, sizeof(how), 0, 0);
    }
    case 72: { /* fcntl */
        uint64_t arg = a[2];
        if ((int)a[1] == 4 /* F_SETFL */) {
            arg = oflags_to_host(arg);
        }
        long r = hcall(t, G_fcntl, a[0], a[1], arg, a[3], 0, 0);
        if ((int)a[1] == 3 /* F_GETFL */ && r >= 0) {
            r = (long)oflags_from_host((uint64_t)r);
        }
        return r;
    }
    case 4: case 6: { /* stat, lstat */
        g_stat_t g;
        long r = hcall(t, G_newfstatat, (uint64_t)AT_FDCWD, a[0], (uint64_t)(uintptr_t)&g,
                       nr == 6 ? AT_SYMLINK_NOFOLLOW : 0, 0, 0);
        return stat_out(r, &g, a[1]);
    }
    case 5: {   /* fstat */
        g_stat_t g;
        long r = hcall(t, G_fstat, a[0], (uint64_t)(uintptr_t)&g, 0, 0, 0, 0);
        return stat_out(r, &g, a[1]);
    }
    case 262: { /* newfstatat */
        g_stat_t g;
        long r = hcall(t, G_newfstatat, a[0], a[1], (uint64_t)(uintptr_t)&g, a[3], 0, 0);
        return stat_out(r, &g, a[2]);
    }
    case 7: {   /* poll */
        int64_t ms = (int32_t)a[2];
        ts_t ts = { ms / 1000, (ms % 1000) * 1000000 };
        return hcall(t, G_ppoll, a[0], a[1], ms < 0 ? 0 : (uint64_t)(uintptr_t)&ts, 0, 8, 0);
    }
    case 23: {  /* select */
        ts_t ts;
        tv_t *tv = (tv_t *)(uintptr_t)a[4];
        if (tv != NULL) {
            ts.sec = tv->sec;
            ts.nsec = tv->usec * 1000;
        }
        long r = hcall(t, G_pselect6, a[0], a[1], a[2], a[3],
                       tv != NULL ? (uint64_t)(uintptr_t)&ts : 0, 0);
        if (tv != NULL && r >= 0) {
            tv->sec = ts.sec;
            tv->usec = ts.nsec / 1000;
        }
        return r;
    }
    case 21:   /* access */
        return hcall(t, G_faccessat, (uint64_t)AT_FDCWD, a[0], a[1], 0, 0, 0);
    case 22:   /* pipe */
        return hcall(t, G_pipe2, a[0], 0, 0, 0, 0, 0);
    case 33:   /* dup2 */
        if (a[0] == a[1]) {
            long r = hcall(t, G_fcntl, a[0], 1 /* F_GETFD */, 0, 0, 0, 0);
            return r < 0 ? r : (long)a[1];
        }
        return hcall(t, G_dup3, a[0], a[1], 0, 0, 0, 0);
    case 37: {  /* alarm */
        struct { tv_t interval, value; } nv = { { 0, 0 }, { (int64_t)(uint32_t)a[0], 0 } }, ov;
        memset(&ov, 0, sizeof(ov));
        long r = hcall(t, G_setitimer, 0, (uint64_t)(uintptr_t)&nv, (uint64_t)(uintptr_t)&ov, 0, 0, 0);
        if (r < 0) {
            return r;
        }
        return (long)(ov.value.sec + (ov.value.usec != 0 ? 1 : 0));
    }
    case 56:   /* clone(flags, stack, ptid, ctid, tls) */
        return hcall(t, G_clone, a[0], a[1], a[2], a[4], a[3], 0);
    case 57: case 58: /* fork, vfork */
        return hcall(t, G_clone, XL_SIGCHLD, 0, 0, 0, 0, 0);
    case 82:   /* rename */
        return hcall(t, G_renameat, (uint64_t)AT_FDCWD, a[0], (uint64_t)AT_FDCWD, a[1], 0, 0);
    case 83:   /* mkdir */
        return hcall(t, G_mkdirat, (uint64_t)AT_FDCWD, a[0], a[1], 0, 0, 0);
    case 84:   /* rmdir */
        return hcall(t, G_unlinkat, (uint64_t)AT_FDCWD, a[0], AT_REMOVEDIR, 0, 0, 0);
    case 86:   /* link */
        return hcall(t, G_linkat, (uint64_t)AT_FDCWD, a[0], (uint64_t)AT_FDCWD, a[1], 0, 0);
    case 87:   /* unlink */
        return hcall(t, G_unlinkat, (uint64_t)AT_FDCWD, a[0], 0, 0, 0, 0);
    case 88:   /* symlink */
        return hcall(t, G_symlinkat, a[0], (uint64_t)AT_FDCWD, a[1], 0, 0, 0);
    case 89:   /* readlink */
        return hcall(t, G_readlinkat, (uint64_t)AT_FDCWD, a[0], a[1], a[2], 0, 0);
    case 90:   /* chmod */
        return hcall(t, G_fchmodat, (uint64_t)AT_FDCWD, a[0], a[1], 0, 0, 0);
    case 92: case 94: /* chown, lchown */
        return hcall(t, G_fchownat, (uint64_t)AT_FDCWD, a[0], a[1], a[2],
                     nr == 94 ? AT_SYMLINK_NOFOLLOW : 0, 0);
    case 133:  /* mknod */
        return hcall(t, G_mknodat, (uint64_t)AT_FDCWD, a[0], a[1], a[2], 0, 0);
    case 132: { /* utime(path, {actime, modtime}) */
        ts_t ts[2];
        const int64_t *ub = (const int64_t *)(uintptr_t)a[1];
        if (ub != NULL) {
            ts[0].sec = ub[0]; ts[0].nsec = 0;
            ts[1].sec = ub[1]; ts[1].nsec = 0;
        }
        return hcall(t, G_utimensat, (uint64_t)AT_FDCWD, a[0],
                     ub != NULL ? (uint64_t)(uintptr_t)ts : 0, 0, 0, 0);
    }
    case 235: case 261: { /* utimes(path, tv[2]), futimesat(dfd, path, tv[2]) */
        uint64_t dfd = nr == 235 ? (uint64_t)AT_FDCWD : a[0];
        uint64_t path = nr == 235 ? a[0] : a[1];
        const tv_t *tv = (const tv_t *)(uintptr_t)(nr == 235 ? a[1] : a[2]);
        ts_t ts[2];
        if (tv != NULL) {
            for (int i = 0; i < 2; ++i) {
                ts[i].sec = tv[i].sec;
                ts[i].nsec = tv[i].usec * 1000;
            }
        }
        return hcall(t, G_utimensat, dfd, path, tv != NULL ? (uint64_t)(uintptr_t)ts : 0, 0, 0, 0);
    }
    case 201: { /* time */
        ts_t ts = { 0, 0 };
        long r = hcall(t, G_clock_gettime, 0, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
        if (r < 0) {
            return r;
        }
        if (a[0] != 0u) {
            memcpy((void *)(uintptr_t)a[0], &ts.sec, 8);
        }
        return (long)ts.sec;
    }
    case 111:  /* getpgrp */
        return hcall(t, G_getpgid, 0, 0, 0, 0, 0, 0);
    case 213:  /* epoll_create */
        if ((int32_t)a[0] <= 0) {
            return -XL_EINVAL;
        }
        return hcall(t, G_epoll_create1, 0, 0, 0, 0, 0, 0);
    case 233: { /* epoll_ctl */
        g_epoll_event_t ev = { 0, 0, 0 };
        if (a[3] != 0u) {
            x_epoll_event_t x;
            memcpy(&x, (const void *)(uintptr_t)a[3], sizeof(x));
            ev.events = x.events;
            ev.data = x.data;
        }
        return hcall(t, G_epoll_ctl, a[0], a[1], a[2],
                     a[3] != 0u ? (uint64_t)(uintptr_t)&ev : 0, 0, 0);
    }
    case 232:  /* epoll_wait */
    case 281:  /* epoll_pwait */
        return epoll_wait_conv(t, nr, a, G_epoll_pwait);
    case 441:  /* epoll_pwait2 */
        return epoll_wait_conv(t, nr, a, G_epoll_pwait2);
    case 253:  /* inotify_init */
        return hcall(t, G_inotify_init1, 0, 0, 0, 0, 0, 0);
    case 284:  /* eventfd */
        return hcall(t, G_eventfd2, a[0], 0, 0, 0, 0, 0);
    case 282:  /* signalfd */
        return hcall(t, G_signalfd4, a[0], a[1], a[2], 0, 0, 0);
    case 78:   /* getdents */
        return getdents_conv(t, a);
    case 9:    /* mmap: MAP_32BIT has no meaning here */
        return hcall(t, 222, a[0], a[1], a[2], a[3] & ~0x40ULL, a[4], a[5]);
    default:
        break;
    }
    if (nr >= 0 && nr < NR_MAX && g_map[nr] >= 0) {
        return hcall(t, g_map[nr], a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    XL_DEBUG("system call %s (%ld) has no host equivalent", xl_syscall_name(nr), nr);
    return -XL_ENOSYS;
}

long xl_host_syscall(gthread_t *t, long nr, uint64_t a[6])
{
    return translate(t, nr, a);
}

long xl_host_syscall_plain(long nr, uint64_t a[6])
{
    return translate(NULL, nr, a);
}

#endif /* __aarch64__ */
