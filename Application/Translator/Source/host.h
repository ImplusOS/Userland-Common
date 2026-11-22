#pragma once

/*
 * The host: its Linux system call numbers for the calls the translator
 * makes for itself, its trap stubs (host_<arch>.S), its thread pointer and
 * the layout of the ucontext its kernel hands a signal handler.
 *
 * Guest system calls do not come through here by number; they go through
 * the canonical layer (syscall.c) and hostsys_<abi>.c.
 */

#include <stdint.h>
#include <stddef.h>

struct gthread;

/* host_<arch>.S */
long host_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
/* A system call that a queued guest signal interrupts: returns -EINTR
 * without entering the kernel if t->sig_pending_flag is set, and the host
 * signal handler moves an interrupted call to the -EINTR exit (see
 * xl_host_signal). */
long host_safe_syscall(struct gthread *t, long nr, long a0, long a1, long a2,
                       long a3, long a4, long a5);
extern const char host_safe_syscall_start[];
extern const char host_safe_syscall_end[];
extern const char host_safe_syscall_eintr[];
/* clone() for a new thread: the child calls fn(arg) on `stack` and exits
 * the thread when it returns. Arguments in x86_64 order. */
long host_clone(uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid,
                uint64_t tls, void (*fn)(void *), void *arg);
int host_setjmp(void *buf) __attribute__((returns_twice));
__attribute__((noreturn)) void host_longjmp(void *buf, int val);
extern const char host_sigreturn[];   /* SA_RESTORER for our handlers */

#define HOST_PROT_READ   1
#define HOST_PROT_WRITE  2
#define HOST_PROT_EXEC   4
#define HOST_MAP_SHARED    0x01
#define HOST_MAP_PRIVATE   0x02
#define HOST_MAP_FIXED     0x10
#define HOST_MAP_ANONYMOUS 0x20
#define HOST_MAP_NORESERVE 0x4000
#define HOST_AT_FDCWD (-100)
#define HOST_O_RDONLY 0
#define HOST_O_CLOEXEC 02000000

#if defined(__aarch64__)

/* include/uapi/asm-generic/unistd.h */
#define HNR_openat        56
#define HNR_close         57
#define HNR_read          63
#define HNR_write         64
#define HNR_pread64       67
#define HNR_readlinkat    78
#define HNR_exit          93
#define HNR_exit_group    94
#define HNR_futex         98
#define HNR_clock_gettime 113
#define HNR_sched_yield   124
#define HNR_kill          129
#define HNR_tgkill        131
#define HNR_rt_sigaction  134
#define HNR_rt_sigprocmask 135
#define HNR_rt_sigreturn  139
#define HNR_getpid        172
#define HNR_getuid        174
#define HNR_geteuid       175
#define HNR_getgid        176
#define HNR_getegid       177
#define HNR_gettid        178
#define HNR_munmap        215
#define HNR_clone         220
#define HNR_mmap          222
#define HNR_mprotect      226

static inline struct gthread *xl_host_tp(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return (struct gthread *)(uintptr_t)v;
}

static inline void xl_host_set_tp(struct gthread *t)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"((uint64_t)(uintptr_t)t) : "memory");
}

/* struct ucontext (arch/arm64/include/uapi/asm/ucontext.h): uc_mcontext
 * at 176; sigcontext = fault_address, regs[31], sp, pc, pstate. */
#define HOST_UC_MCONTEXT 176u
static inline uint64_t *host_uc_reg(void *uc, unsigned i)
{
    return (uint64_t *)((uint8_t *)uc + HOST_UC_MCONTEXT + 8u + 8u * i);
}
static inline uint64_t *host_uc_pc_ptr(void *uc)
{
    return (uint64_t *)((uint8_t *)uc + HOST_UC_MCONTEXT + 264u);
}
static inline uint64_t *host_uc_sp_ptr(void *uc)
{
    return (uint64_t *)((uint8_t *)uc + HOST_UC_MCONTEXT + 256u);
}
/* Resume the interrupted thread in fn(arg) on its own stack. */
static inline void host_uc_redirect(void *uc, uint64_t fn, uint64_t arg)
{
    *host_uc_pc_ptr(uc) = fn;
    *host_uc_reg(uc, 0) = arg;
    *host_uc_reg(uc, 30) = 0;
    uint64_t *sp = host_uc_sp_ptr(uc);
    *sp = (*sp - 256u) & ~0xFULL;
}

#elif defined(__x86_64__)

/* arch/x86/entry/syscalls/syscall_64.tbl */
#define HNR_read          0
#define HNR_write         1
#define HNR_close         3
#define HNR_mmap          9
#define HNR_mprotect      10
#define HNR_munmap        11
#define HNR_rt_sigaction  13
#define HNR_rt_sigprocmask 14
#define HNR_rt_sigreturn  15
#define HNR_pread64       17
#define HNR_sched_yield   24
#define HNR_getpid        39
#define HNR_clone         56
#define HNR_exit          60
#define HNR_kill          62
#define HNR_getuid        102
#define HNR_getgid        104
#define HNR_geteuid       107
#define HNR_getegid       108
#define HNR_arch_prctl    158
#define HNR_gettid        186
#define HNR_futex         202
#define HNR_clock_gettime 228
#define HNR_exit_group    231
#define HNR_tgkill        234
#define HNR_openat        257
#define HNR_readlinkat    267

static inline struct gthread *xl_host_tp(void)
{
    uint64_t v;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
    return (struct gthread *)(uintptr_t)v;
}

static inline void xl_host_set_tp(struct gthread *t)
{
    (void)host_syscall6(HNR_arch_prctl, 0x1002 /* ARCH_SET_FS */,
                        (long)(uintptr_t)t, 0, 0, 0, 0);
}

/* struct ucontext: uc_mcontext (struct sigcontext) at 40; gregs order
 * r8..r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags. */
#define HOST_UC_MCONTEXT 40u
static inline uint64_t *host_uc_greg(void *uc, unsigned i)
{
    return (uint64_t *)((uint8_t *)uc + HOST_UC_MCONTEXT + 8u * i);
}
static inline uint64_t *host_uc_pc_ptr(void *uc) { return host_uc_greg(uc, 16); }
static inline uint64_t *host_uc_sp_ptr(void *uc) { return host_uc_greg(uc, 15); }
static inline void host_uc_redirect(void *uc, uint64_t fn, uint64_t arg)
{
    *host_uc_pc_ptr(uc) = fn;
    *host_uc_greg(uc, 8) = arg;   /* rdi */
    uint64_t *sp = host_uc_sp_ptr(uc);
    /* below the red zone, and as if fn had just been called */
    *sp = ((*sp - 512u) & ~0xFULL) - 8u;
}

#else
#error "unsupported host"
#endif

static inline long host_sys(long nr, long a0, long a1, long a2)
{
    return host_syscall6(nr, a0, a1, a2, 0, 0, 0);
}

static inline long host_write(int fd, const void *buf, size_t n)
{
    return host_sys(HNR_write, fd, (long)(uintptr_t)buf, (long)n);
}

static inline long host_pread(int fd, void *buf, size_t n, uint64_t off)
{
    return host_syscall6(HNR_pread64, fd, (long)(uintptr_t)buf, (long)n,
                         (long)off, 0, 0);
}

static inline long host_openat(int dfd, const char *path, int flags, int mode)
{
    return host_syscall6(HNR_openat, dfd, (long)(uintptr_t)path, flags, mode, 0, 0);
}

static inline long host_close(int fd)
{
    return host_sys(HNR_close, fd, 0, 0);
}

static inline long host_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                             int fd, uint64_t off)
{
    return host_syscall6(HNR_mmap, (long)addr, (long)len, prot, flags, fd,
                         (long)off);
}

static inline long host_munmap(uint64_t addr, uint64_t len)
{
    return host_sys(HNR_munmap, (long)addr, (long)len, 0);
}

static inline long host_mprotect(uint64_t addr, uint64_t len, int prot)
{
    return host_sys(HNR_mprotect, (long)addr, (long)len, prot);
}

__attribute__((noreturn)) static inline void host_exit_group(int code)
{
    for (;;) {
        (void)host_sys(HNR_exit_group, code, 0, 0);
    }
}

__attribute__((noreturn)) static inline void host_exit_thread(int code)
{
    for (;;) {
        (void)host_sys(HNR_exit, code, 0, 0);
    }
}

static inline long host_gettid(void)  { return host_sys(HNR_gettid, 0, 0, 0); }
static inline long host_getpid(void)  { return host_sys(HNR_getpid, 0, 0, 0); }
static inline long host_sched_yield(void) { return host_sys(HNR_sched_yield, 0, 0, 0); }

static inline long host_tgkill(long pid, long tid, int sig)
{
    return host_sys(HNR_tgkill, pid, tid, sig);
}

static inline long host_rt_sigaction(int sig, const void *act, void *oact)
{
    return host_syscall6(HNR_rt_sigaction, sig, (long)(uintptr_t)act,
                         (long)(uintptr_t)oact, 8, 0, 0);
}

static inline long host_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oset)
{
    return host_syscall6(HNR_rt_sigprocmask, how, (long)(uintptr_t)set,
                         (long)(uintptr_t)oset, 8, 0, 0);
}

static inline long host_clock_gettime(int clk, void *ts)
{
    return host_sys(HNR_clock_gettime, clk, (long)(uintptr_t)ts, 0);
}
