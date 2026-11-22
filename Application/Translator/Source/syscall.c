/*
 * The guest-independent system call layer.
 *
 * Guest front ends hand calls here in the canonical ABI: Linux x86_64
 * numbers and structure layouts (Kernel/Source/Compat/Linux makes the same
 * choice). What this layer handles itself is what depends on the guest CPU
 * or on the translator: signal dispositions, masks and frames, thread and
 * process creation, thread exit, the thread pointer, /proc/self/exe (which
 * names the translator to the host kernel), and calls that would hand the
 * host kernel guest code or guest system call numbers (seccomp, rseq).
 * Everything else goes to the host through hostsys_<abi>.c.
 */

#include "xl.h"
#include "host.h"

enum {
    C_read = 0, C_write = 1, C_open = 2, C_close = 3, C_stat = 4, C_mmap = 9,
    C_mprotect = 10, C_munmap = 11, C_mremap = 25, C_madvise = 28, C_shmat = 30,
    C_shmdt = 67, C_rt_sigaction = 13, C_rt_sigprocmask = 14,
    C_rt_sigreturn = 15, C_poll = 7, C_select = 23, C_pause = 34,
    C_nanosleep = 35, C_clone = 56, C_fork = 57, C_vfork = 58, C_execve = 59,
    C_exit = 60, C_uname = 63, C_readlink = 89, C_ptrace = 101,
    C_rt_sigtimedwait = 128, C_rt_sigsuspend = 130, C_sigaltstack = 131,
    C_modify_ldt = 154, C_prctl = 157, C_arch_prctl = 158, C_iopl = 172,
    C_ioperm = 173, C_set_thread_area = 205, C_io_getevents = 208,
    C_get_thread_area = 211, C_semtimedop = 220, C_clock_nanosleep = 230,
    C_exit_group = 231, C_epoll_wait = 232, C_readlinkat = 267,
    C_pselect6 = 270, C_ppoll = 271, C_epoll_pwait = 281, C_seccomp = 317,
    C_bpf = 321, C_execveat = 322, C_rseq = 334, C_io_uring_setup = 425,
    C_io_uring_enter = 426, C_io_uring_register = 427, C_clone3 = 435,
    C_epoll_pwait2 = 441,
};

#define CLONE_VM    0x00000100ULL
#define CLONE_VFORK 0x00004000ULL
#define PR_SET_SECCOMP 22
#define PROT_EXEC 4ULL
#define PROT_READ 1ULL

/* Calls Linux never re-runs after a handler, SA_RESTART or not. */
static bool never_restarted(long nr)
{
    switch (nr) {
    case C_pause: case C_rt_sigsuspend: case C_rt_sigtimedwait: case C_poll:
    case C_ppoll: case C_select: case C_pselect6: case C_epoll_wait:
    case C_epoll_pwait: case C_epoll_pwait2: case C_nanosleep:
    case C_clock_nanosleep: case C_io_getevents: case C_semtimedop:
        return true;
    default:
        return false;
    }
}

/* ---- read-only code ----------------------------------------------------------
 *
 * Which address ranges hold code the guest cannot write without a system
 * call first: mapped executable and not writable. A guest CPU's translation
 * cache can trust a block decoded from such a range until the next change
 * to the address space (g_xl_code_epoch), instead of comparing it against
 * guest memory on every entry. Everything else is compared every time.
 */

#define CODE_RANGES_MAX 1024
static struct { uint64_t start, end; } g_ro_code[CODE_RANGES_MAX];
static int g_ro_n;
static xl_lock_t g_ro_lock;
volatile uint64_t g_xl_code_epoch = 1;

/* invalidation log (xl.h) */
#define INVAL_LOG 256u
static struct { uint64_t start, end; } g_inval[INVAL_LOG];
volatile uint64_t g_xl_inval_seq;

bool xl_code_inval_since(uint64_t *seen, void (*fn)(uint64_t, uint64_t, void *), void *arg)
{
    bool ok = true;
    xl_lock(&g_ro_lock);
    const uint64_t now = g_xl_inval_seq;
    if (now - *seen > INVAL_LOG) {
        ok = false;
    } else {
        for (uint64_t k = *seen; k < now; ++k) {
            fn(g_inval[k % INVAL_LOG].start, g_inval[k % INVAL_LOG].end, arg);
        }
    }
    *seen = now;
    xl_unlock(&g_ro_lock);
    return ok;
}

/* Returns whether [s, e) overlapped read-only code. */
static bool ro_remove(uint64_t s, uint64_t e)
{
    bool hit = false;
    for (int i = 0; i < g_ro_n;) {
        uint64_t rs = g_ro_code[i].start, re = g_ro_code[i].end;
        if (re <= s || rs >= e) { ++i; continue; }
        hit = true;
        if (rs < s && re > e) {                 /* split */
            if (g_ro_n < CODE_RANGES_MAX) {
                g_ro_code[g_ro_n].start = e;
                g_ro_code[g_ro_n].end = re;
                ++g_ro_n;
                g_ro_code[i].end = s;
            } else {
                g_ro_code[i] = g_ro_code[--g_ro_n];
                continue;
            }
            ++i;
        } else if (rs < s) {
            g_ro_code[i].end = s;
            ++i;
        } else if (re > e) {
            g_ro_code[i].start = e;
            ++i;
        } else {
            g_ro_code[i] = g_ro_code[--g_ro_n];
        }
    }
    return hit;
}

void xl_code_map_update(uint64_t addr, uint64_t len, int prot)
{
    const uint64_t s = XL_PAGE_DOWN(addr), e = XL_PAGE_UP(addr + len);
    xl_lock(&g_ro_lock);
    if (ro_remove(s, e) || (s == 0 && e == 0)) {
        g_inval[g_xl_inval_seq % INVAL_LOG].start = s == e ? 0 : s;
        g_inval[g_xl_inval_seq % INVAL_LOG].end = s == e ? ~0ULL : e;
        __atomic_add_fetch(&g_xl_inval_seq, 1, __ATOMIC_SEQ_CST);
    }
    if (prot >= 0 && (prot & 4) != 0 && (prot & 2) == 0 && g_ro_n < CODE_RANGES_MAX && e > s) {
        g_ro_code[g_ro_n].start = s;
        g_ro_code[g_ro_n].end = e;
        ++g_ro_n;
    }
    __atomic_add_fetch(&g_xl_code_epoch, 1, __ATOMIC_SEQ_CST);
    xl_unlock(&g_ro_lock);
}

bool xl_code_is_readonly(uint64_t start, uint64_t end)
{
    bool r = false;
    xl_lock(&g_ro_lock);
    for (int i = 0; i < g_ro_n; ++i) {
        if (start >= g_ro_code[i].start && end <= g_ro_code[i].end) {
            r = true;
            break;
        }
    }
    xl_unlock(&g_ro_lock);
    return r;
}

static void track_mapping(long nr, const uint64_t in[6], long r)
{
    if (r < 0 && r > -4096) {
        return;
    }
    switch (nr) {
    case C_mmap: xl_code_map_update((uint64_t)r, in[1], (int)in[2]); break;
    case C_mprotect: xl_code_map_update(in[0], in[1], (int)in[2]); break;
    case C_munmap: xl_code_map_update(in[0], in[1], -1); break;
    case C_mremap:
        xl_code_map_update(in[0], in[1], -1);
        xl_code_map_update((uint64_t)r, in[2], -1);
        break;
    case C_madvise:
        if (in[2] == 4u /* MADV_DONTNEED */ || in[2] == 9u /* MADV_REMOVE */) {
            xl_code_map_update(in[0], in[1], -1);
        }
        break;
    case C_shmat: case C_shmdt:
        xl_code_map_update(0, 0, -1);
        break;
    default:
        break;
    }
}

/* An ELF file for the guest CPU? */
static bool is_guest_elf(const char *path)
{
    if (path == NULL) {
        return false;
    }
    long fd = host_openat(HOST_AT_FDCWD, path, HOST_O_RDONLY | HOST_O_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    uint8_t h[20];
    long n = host_pread((int)fd, h, sizeof(h), 0);
    (void)host_close((int)fd);
    return n == (long)sizeof(h) && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' &&
           (uint16_t)(h[18] | (h[19] << 8)) == g_xl_guest.elf_machine;
}

/* The host kernel cannot run a guest program itself (a plain Linux host,
 * no binfmt_misc entry for it; ImplusOS dispatches such programs to the
 * translator on its own): run it through this translator, with the
 * command line the kernel would have given it. */
static long exec_through_self(gthread_t *t, uint64_t a[6])
{
    char **gargv = (char **)(uintptr_t)a[1];
    static char *argv[1024];
    unsigned n = 0;
    argv[n++] = g_xl_self_path;
    argv[n++] = (char *)"-0";
    argv[n++] = (gargv != NULL && gargv[0] != NULL) ? gargv[0] : (char *)(uintptr_t)a[0];
    argv[n++] = (char *)(uintptr_t)a[0];
    if (gargv != NULL && gargv[0] != NULL) {
        for (unsigned k = 1; gargv[k] != NULL && n + 1u < XL_ARRAY_LEN(argv); ++k) {
            argv[n++] = gargv[k];
        }
    }
    argv[n] = NULL;
    uint64_t b[6] = { (uint64_t)(uintptr_t)g_xl_self_path, (uint64_t)(uintptr_t)argv, a[2], 0, 0, 0 };
    return xl_host_syscall(t, C_execve, b);
}

static bool is_proc_self_exe(uint64_t p)
{
    const char *s = (const char *)(uintptr_t)p;
    if (s == NULL) {
        return false;
    }
    if (strcmp(s, "/proc/self/exe") == 0 || strcmp(s, "/proc/thread-self/exe") == 0) {
        return true;
    }
    if (strncmp(s, "/proc/", 6) != 0) {
        return false;
    }
    char own[32];
    xl_snprintf(own, sizeof(own), "/proc/%d/exe", (int)host_getpid());
    return strcmp(s, own) == 0;
}

static long readlink_exe(uint64_t buf, uint64_t size)
{
    size_t n = strlen(g_xl_exe_path);
    if (n > size) {
        n = (size_t)size;
    }
    memcpy((void *)(uintptr_t)buf, g_xl_exe_path, n);
    return (long)n;
}

static long sys_inner(gthread_t *t, long nr, uint64_t a[6])
{
    switch (nr) {
    case C_rt_sigaction:
        return xl_sys_rt_sigaction(t, (int)a[0], a[1], a[2], a[3]);
    case C_rt_sigprocmask:
        return xl_sys_rt_sigprocmask(t, (int)a[0], a[1], a[2], a[3]);
    case C_rt_sigreturn:
        return guest_rt_sigreturn(t);
    case C_sigaltstack:
        return xl_sys_sigaltstack(t, a[0], a[1]);
    case C_rt_sigsuspend:
        return xl_sys_rt_sigsuspend(t, a[0], a[1]);
    case C_pause: {
        uint64_t m = t->guest_mask;
        return xl_sys_rt_sigsuspend(t, (uint64_t)(uintptr_t)&m, 8);
    }

    case C_clone:
        return xl_sys_clone(t, a[0], a[1], a[2], a[3], a[4]);
    case C_fork:
        return xl_sys_clone(t, XL_SIGCHLD, 0, 0, 0, 0);
    case C_vfork:
        return xl_sys_clone(t, CLONE_VM | CLONE_VFORK | XL_SIGCHLD, 0, 0, 0, 0);
    case C_clone3:
        /* glibc falls back to clone() */
        return -XL_ENOSYS;
    case C_exit:
        xl_thread_exit(t, (int)a[0]);
    case C_exit_group:
#ifdef XL_PROFILE
        { extern void guest_prof_dump(void); guest_prof_dump(); }
#endif
        host_exit_group((int)a[0]);

    case C_execve: {
        if (is_proc_self_exe(a[0])) {
            a[0] = (uint64_t)(uintptr_t)g_xl_exe_path;
        }
        xl_signal_before_exec();
        long r = xl_host_syscall(t, nr, a);
        if (r == -XL_ENOEXEC && is_guest_elf((const char *)(uintptr_t)a[0])) {
            r = exec_through_self(t, a);
        }
        return r;
    }
    case C_execveat:
        if (is_proc_self_exe(a[1])) {
            a[1] = (uint64_t)(uintptr_t)g_xl_exe_path;
        }
        xl_signal_before_exec();
        break;
    case C_readlink:
        if (is_proc_self_exe(a[0])) {
            return readlink_exe(a[1], a[2]);
        }
        break;
    case C_readlinkat:
        if (is_proc_self_exe(a[1])) {
            return readlink_exe(a[2], a[3]);
        }
        break;

    case C_mmap:
    case C_mprotect:
        /* x86 has no execute-only pages; the interpreter reads code. */
        if ((a[2] & PROT_EXEC) != 0u) {
            a[2] |= PROT_READ;
        }
        break;

    case C_uname: {
        long r = xl_host_syscall(t, nr, a);
        if (r == 0) {
            guest_fix_uname((void *)(uintptr_t)a[0]);
        }
        return r;
    }

    case C_arch_prctl:
        return guest_arch_syscall(t, nr, a);
    case C_set_thread_area:
    case C_get_thread_area:
    case C_modify_ldt:
    case C_rseq:
    case C_bpf:
    case C_io_uring_setup:
    case C_io_uring_enter:
    case C_io_uring_register:
        return -XL_ENOSYS;
    case C_iopl:
    case C_ioperm:
    case C_ptrace:
        return -XL_EPERM;
    case C_seccomp:
        /* A filter would judge the translator's host calls by guest
         * numbers. */
        return -XL_EINVAL;
    case C_prctl:
        if ((int)a[0] == PR_SET_SECCOMP) {
            return -XL_EINVAL;
        }
        break;
    default:
        break;
    }
    uint64_t in[6];
    memcpy(in, a, sizeof(in));
    long r = xl_host_syscall(t, nr, a);
    if (r == -XL_EINTR && !never_restarted(nr) && xl_signal_should_restart(t)) {
        return XL_ERESTART;
    }
    switch (nr) {
    case C_mmap: case C_mprotect: case C_munmap: case C_mremap: case C_madvise:
    case C_shmat: case C_shmdt:
        track_mapping(nr, in, r);
        break;
    default:
        break;
    }
    return r;
}

long xl_syscall(gthread_t *t, long nr, uint64_t a[6])
{
    if (XL_LIKELY(!g_xl_strace)) {
        return sys_inner(t, nr, a);
    }
    uint64_t in[6];
    memcpy(in, a, sizeof(in));
    /* the argument that is a path, for the calls that take one */
    int pi = -1;
    switch (nr) {
    case 2: case 4: case 6: case 21: case 59: case 80: case 83: case 84:
    case 87: case 89: case 90: case 137:
        pi = 0;
        break;
    case 257: case 262: case 263: case 267: case 268: case 269: case 322:
    case 332: case 437: case 439:
        pi = 1;
        break;
    default:
        break;
    }
    char path[96];
    path[0] = '\0';
    if (pi >= 0 && in[pi] != 0u) {
        const char *src = (const char *)(uintptr_t)in[pi];
        size_t k = 0;
        while (k + 1u < sizeof(path) && src[k] != '\0') {
            path[k] = src[k];
            ++k;
        }
        path[k] = '\0';
    }
    if (g_xl_strace > 1) {
        xl_log("-> %s(%lx, %lx, %lx, %lx, %lx, %lx)%s%s%s", xl_syscall_name(nr),
               in[0], in[1], in[2], in[3], in[4], in[5],
               pi >= 0 ? " \"" : "", path, pi >= 0 ? "\"" : "");
    }
    long r = sys_inner(t, nr, a);
    xl_log("%s(%lx, %lx, %lx, %lx, %lx, %lx)%s%s%s = %ld", xl_syscall_name(nr),
           in[0], in[1], in[2], in[3], in[4], in[5],
           pi >= 0 ? " \"" : "", path, pi >= 0 ? "\"" : "", r);
    return r;
}

static const char *const k_names[] = {
    "read", "write", "open", "close", "stat", "fstat", "lstat", "poll",
    "lseek", "mmap", "mprotect", "munmap", "brk", "rt_sigaction",
    "rt_sigprocmask", "rt_sigreturn", "ioctl", "pread64", "pwrite64",
    "readv", "writev", "access", "pipe", "select", "sched_yield", "mremap",
    "msync", "mincore", "madvise", "shmget", "shmat", "shmctl", "dup",
    "dup2", "pause", "nanosleep", "getitimer", "alarm", "setitimer",
    "getpid", "sendfile", "socket", "connect", "accept", "sendto",
    "recvfrom", "sendmsg", "recvmsg", "shutdown", "bind", "listen",
    "getsockname", "getpeername", "socketpair", "setsockopt", "getsockopt",
    "clone", "fork", "vfork", "execve", "exit", "wait4", "kill", "uname",
    "semget", "semop", "semctl", "shmdt", "msgget", "msgsnd", "msgrcv",
    "msgctl", "fcntl", "flock", "fsync", "fdatasync", "truncate",
    "ftruncate", "getdents", "getcwd", "chdir", "fchdir", "rename", "mkdir",
    "rmdir", "creat", "link", "unlink", "symlink", "readlink", "chmod",
    "fchmod", "chown", "fchown", "lchown", "umask", "gettimeofday",
    "getrlimit", "getrusage", "sysinfo", "times", "ptrace", "getuid",
    "syslog", "getgid", "setuid", "setgid", "geteuid", "getegid",
    "setpgid", "getppid", "getpgrp", "setsid", "setreuid", "setregid",
    "getgroups", "setgroups", "setresuid", "getresuid", "setresgid",
    "getresgid", "getpgid", "setfsuid", "setfsgid", "getsid", "capget",
    "capset", "rt_sigpending", "rt_sigtimedwait", "rt_sigqueueinfo",
    "rt_sigsuspend", "sigaltstack", "utime", "mknod", "uselib",
    "personality", "ustat", "statfs", "fstatfs", "sysfs", "getpriority",
    "setpriority", "sched_setparam", "sched_getparam", "sched_setscheduler",
    "sched_getscheduler", "sched_get_priority_max", "sched_get_priority_min",
    "sched_rr_get_interval", "mlock", "munlock", "mlockall", "munlockall",
    "vhangup", "modify_ldt", "pivot_root", "_sysctl", "prctl", "arch_prctl",
    "adjtimex", "setrlimit", "chroot", "sync", "acct", "settimeofday",
    "mount", "umount2", "swapon", "swapoff", "reboot", "sethostname",
    "setdomainname", "iopl", "ioperm", "create_module", "init_module",
    "delete_module", "get_kernel_syms", "query_module", "quotactl",
    "nfsservctl", "getpmsg", "putpmsg", "afs_syscall", "tuxcall",
    "security", "gettid", "readahead", "setxattr", "lsetxattr", "fsetxattr",
    "getxattr", "lgetxattr", "fgetxattr", "listxattr", "llistxattr",
    "flistxattr", "removexattr", "lremovexattr", "fremovexattr", "tkill",
    "time", "futex", "sched_setaffinity", "sched_getaffinity",
    "set_thread_area", "io_setup", "io_destroy", "io_getevents",
    "io_submit", "io_cancel", "get_thread_area", "lookup_dcookie",
    "epoll_create", "epoll_ctl_old", "epoll_wait_old", "remap_file_pages",
    "getdents64", "set_tid_address", "restart_syscall", "semtimedop",
    "fadvise64", "timer_create", "timer_settime", "timer_gettime",
    "timer_getoverrun", "timer_delete", "clock_settime", "clock_gettime",
    "clock_getres", "clock_nanosleep", "exit_group", "epoll_wait",
    "epoll_ctl", "tgkill", "utimes", "vserver", "mbind", "set_mempolicy",
    "get_mempolicy", "mq_open", "mq_unlink", "mq_timedsend",
    "mq_timedreceive", "mq_notify", "mq_getsetattr", "kexec_load", "waitid",
    "add_key", "request_key", "keyctl", "ioprio_set", "ioprio_get",
    "inotify_init", "inotify_add_watch", "inotify_rm_watch",
    "migrate_pages", "openat", "mkdirat", "mknodat", "fchownat",
    "futimesat", "newfstatat", "unlinkat", "renameat", "linkat",
    "symlinkat", "readlinkat", "fchmodat", "faccessat", "pselect6", "ppoll",
    "unshare", "set_robust_list", "get_robust_list", "splice", "tee",
    "sync_file_range", "vmsplice", "move_pages", "utimensat", "epoll_pwait",
    "signalfd", "timerfd_create", "eventfd", "fallocate", "timerfd_settime",
    "timerfd_gettime", "accept4", "signalfd4", "eventfd2", "epoll_create1",
    "dup3", "pipe2", "inotify_init1", "preadv", "pwritev",
    "rt_tgsigqueueinfo", "perf_event_open", "recvmmsg", "fanotify_init",
    "fanotify_mark", "prlimit64", "name_to_handle_at", "open_by_handle_at",
    "clock_adjtime", "syncfs", "sendmmsg", "setns", "getcpu",
    "process_vm_readv", "process_vm_writev", "kcmp", "finit_module",
    "sched_setattr", "sched_getattr", "renameat2", "seccomp", "getrandom",
    "memfd_create", "kexec_file_load", "bpf", "execveat", "userfaultfd",
    "membarrier", "mlock2", "copy_file_range", "preadv2", "pwritev2",
    "pkey_mprotect", "pkey_alloc", "pkey_free", "statx", "io_pgetevents",
    "rseq",
};

const char *xl_syscall_name(long nr)
{
    if (nr >= 0 && (size_t)nr < XL_ARRAY_LEN(k_names)) {
        return k_names[nr];
    }
    switch (nr) {
    case 424: return "pidfd_send_signal";
    case 434: return "pidfd_open";
    case 435: return "clone3";
    case 436: return "close_range";
    case 437: return "openat2";
    case 439: return "faccessat2";
    case 441: return "epoll_pwait2";
    default: break;
    }
    static char buf[24];
    xl_snprintf(buf, sizeof(buf), "syscall_%ld", nr);
    return buf;
}
