#pragma once

/*
 * ImplusOS Translator -- runs a Linux program built for another CPU.
 *
 * The translator is itself an ordinary Linux program of the host
 * architecture (static, no libc). The kernel starts it in place of a foreign
 * ELF (Core/process/ProcessManager_Create.c, "foreign-architecture
 * programs"), passing
 *
 *     argv = { translator, "-0", <guest argv[0]>, <guest path>, <guest argv[1..]> }
 *
 * and it then does what the kernel and the CPU would have done for that
 * program: maps the program and its dynamic linker, builds the initial stack
 * and auxiliary vector, interprets the guest instructions, and turns the
 * guest's Linux system calls into the host's.
 *
 * Guest and host share one address space: a guest pointer is a host pointer.
 * That keeps every system call whose arguments are plain memory (read,
 * write, futex, mmap, ...) a renumbering at most.
 *
 * Layers (each file says more):
 *
 *   main.c             entry, command line, ELF loading, initial stack
 *   thread.c           the per-thread run loop, clone/fork
 *   signal.c           guest signal dispositions, masks, delivery
 *   syscall.c          the guest-independent system call layer; it speaks
 *                      Linux x86_64 numbering and structure layouts (the
 *                      "canonical" ABI, as Kernel/Source/Compat/Linux does)
 *   hostsys_*.c        canonical ABI -> the host's Linux ABI
 *   host_*.S, host.h   host traps, signal context access, TLS, setjmp
 *   guest/<isa>/       one guest CPU: interpreter, its system call front end,
 *                      its signal frames
 *
 * Exactly one guest is linked into a build: the x86_64 guest into the arm64
 * translator, the AArch64 guest into the x86_64 one.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define XL_NORETURN   __attribute__((noreturn))
#define XL_UNUSED     __attribute__((unused))
#define XL_LIKELY(x)   __builtin_expect(!!(x), 1)
#define XL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define XL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define XL_PAGE_SIZE 4096ULL
#define XL_PAGE_DOWN(x) ((uint64_t)(x) & ~(XL_PAGE_SIZE - 1ULL))
#define XL_PAGE_UP(x)   (((uint64_t)(x) + XL_PAGE_SIZE - 1ULL) & ~(XL_PAGE_SIZE - 1ULL))

/* ---- util.c ---------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strrchr(const char *s, int c);

int xl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int xl_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* "translator: <message>\n" on fd 2. */
void xl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
XL_NORETURN void xl_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Diagnostics. XL_DEBUG=1 (or -d) logs faults and unusual events,
 * XL_STRACE=1 (or -strace) every guest system call. */
extern int g_xl_debug;
extern int g_xl_strace;
#define XL_DEBUG(...) do { if (XL_UNLIKELY(g_xl_debug)) xl_log(__VA_ARGS__); } while (0)

/* Anonymous private memory for the translator's own use. */
void *xl_map(size_t size);
void xl_unmap(void *p, size_t size);

typedef struct { volatile uint32_t v; } xl_lock_t;
void xl_lock(xl_lock_t *l);
void xl_unlock(xl_lock_t *l);

/* ---- Linux ABI pieces every layer shares ------------------------------ */

#define XL_NSIG 64

/* siginfo_t is 128 bytes with the same layout on x86_64 and AArch64. */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t __pad;
    union {
        uint64_t addr;                  /* SIGSEGV/SIGBUS/SIGILL/SIGFPE */
        struct { int32_t pid; uint32_t uid; } kill;
        uint8_t raw[112];
    } u;
} xl_siginfo_t;
_Static_assert(sizeof(xl_siginfo_t) == 128, "siginfo_t");

/* The kernel's struct sigaction -- identical on x86_64 and AArch64 (both
 * define SA_RESTORER). */
typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} xl_ksigaction_t;

#define XL_SIG_DFL 0ULL
#define XL_SIG_IGN 1ULL

#define XL_SA_NOCLDSTOP 0x00000001ULL
#define XL_SA_NOCLDWAIT 0x00000002ULL
#define XL_SA_SIGINFO   0x00000004ULL
#define XL_SA_RESTORER  0x04000000ULL
#define XL_SA_ONSTACK   0x08000000ULL
#define XL_SA_RESTART   0x10000000ULL
#define XL_SA_NODEFER   0x40000000ULL
#define XL_SA_RESETHAND 0x80000000ULL

#define XL_SS_ONSTACK 1
#define XL_SS_DISABLE 2

#define XL_SIGHUP  1
#define XL_SIGINT  2
#define XL_SIGQUIT 3
#define XL_SIGILL  4
#define XL_SIGTRAP 5
#define XL_SIGABRT 6
#define XL_SIGBUS  7
#define XL_SIGFPE  8
#define XL_SIGKILL 9
#define XL_SIGUSR1 10
#define XL_SIGSEGV 11
#define XL_SIGUSR2 12
#define XL_SIGPIPE 13
#define XL_SIGALRM 14
#define XL_SIGTERM 15
#define XL_SIGCHLD 17
#define XL_SIGCONT 18
#define XL_SIGSTOP 19
#define XL_SIGTSTP 20
#define XL_SIGTTIN 21
#define XL_SIGTTOU 22
#define XL_SIGURG  23
#define XL_SIGWINCH 28
#define XL_SIGSYS  31

#define XL_SIGBIT(s) (1ULL << ((s) - 1))
/* Raised by the CPU for the instruction that caused them; never blocked at
 * the host, so a guest fault always reaches the translator. */
#define XL_SYNC_SIGS (XL_SIGBIT(XL_SIGSEGV) | XL_SIGBIT(XL_SIGBUS) | \
                      XL_SIGBIT(XL_SIGILL) | XL_SIGBIT(XL_SIGFPE) | \
                      XL_SIGBIT(XL_SIGTRAP))

/* si_code values the interpreters report */
#define XL_SI_USER     0
#define XL_SI_KERNEL   0x80
#define XL_ILL_ILLOPC  1
#define XL_ILL_ILLOPN  2
#define XL_ILL_PRVOPC  5
#define XL_FPE_INTDIV  1
#define XL_FPE_INTOVF  2
#define XL_SEGV_MAPERR 1
#define XL_SEGV_ACCERR 2
#define XL_BUS_ADRALN  1
#define XL_TRAP_BRKPT  1

/* errno values used by the translator itself (same on both ABIs) */
#define XL_EPERM   1
#define XL_ENOENT  2
#define XL_ESRCH   3
#define XL_EINTR   4
#define XL_EIO     5
#define XL_ENOEXEC 8
#define XL_EBADF   9
#define XL_EAGAIN  11
#define XL_ENOMEM  12
#define XL_EFAULT  14
#define XL_EINVAL  22
#define XL_ENOTTY  25
#define XL_ERANGE  34
#define XL_ENAMETOOLONG 36
#define XL_ENOSYS  38
#define XL_ELOOP   40

/* ---- threads ---------------------------------------------------------- */

/* Saved by host_setjmp: enough callee-saved state for either host. */
typedef struct { uint64_t r[24]; } xl_jmpbuf_t;

/*
 * One guest thread. Lives in its own anonymous mapping together with the
 * host stack the translator runs that thread on. The host's thread pointer
 * (TPIDR_EL0 on arm64, FS base on x86_64) points here.
 */
typedef struct gthread {
    struct gthread *self;           /* first: the x86_64 host reads %fs:0 */
    /* Set by the host signal handler when it queued a signal. Read by
     * host_safe_syscall (fixed offset 8) and by the interpreters. */
    volatile uint32_t sig_pending_flag;
    int32_t tid;
    volatile uint64_t sig_pending;  /* XL_SIGBIT() set */
    uint64_t guest_mask;            /* signals the guest has blocked */
    uint64_t suspend_mask;          /* rt_sigsuspend's temporary mask */
    int32_t suspend_active;
    int32_t _pad0;
    uint64_t ss_sp;                 /* guest sigaltstack */
    uint64_t ss_size;
    int32_t ss_flags;
    /* A synchronous fault the run loop is turning into a guest signal. */
    int32_t fault_sig;
    int32_t fault_code;
    int32_t in_frame_setup;
    uint64_t fault_addr;
    uint64_t fault_host_pc;
    xl_jmpbuf_t jb;                 /* the run loop, for fault recovery */
    void *mapping;                  /* this struct + host stack */
    size_t mapping_size;
    uint64_t host_stack_top;
    struct gthread *next_free;
    /* The guest CPU's per-thread translation cache, if it keeps one
     * (survives the gthread_t being recycled for a new thread). */
    void *engine;
    xl_siginfo_t sig_info[XL_NSIG + 1];
    /* The guest CPU; guest_cpu_size bytes. */
    _Alignas(64) uint8_t cpu[];
} gthread_t;

_Static_assert(__builtin_offsetof(gthread_t, sig_pending_flag) == 8,
               "host_safe_syscall reads sig_pending_flag at offset 8");

gthread_t *xl_current(void);           /* host.h implementation */

gthread_t *xl_thread_alloc(void);
void xl_thread_free(gthread_t *t);
XL_NORETURN void xl_thread_loop(gthread_t *t);

/* Run-loop events: what made guest_run() return. */
enum {
    XL_EV_NONE = 0,
    XL_EV_SYSCALL,      /* the guest executed its system call instruction */
    XL_EV_SIGNAL,       /* a signal is pending (or the guest raised one) */
    XL_EV_FAULT,        /* host fault while touching guest memory (longjmp) */
};

/* ---- system calls ----------------------------------------------------- */

/* Return values of xl_syscall() that are not results. */
#define XL_ERESTART     (-512L)   /* re-execute the system call instruction */
#define XL_EJUSTRETURN  (-514L)   /* registers already set (sigreturn) */

/* Canonical (x86_64-numbered) system call on behalf of thread t. */
long xl_syscall(gthread_t *t, long nr, uint64_t a[6]);
/* The canonical call made on the host, no translator-level interception. */
long xl_host_syscall(gthread_t *t, long nr, uint64_t a[6]);
/* Same, but for the translator's own needs: not interruptible. */
long xl_host_syscall_plain(long nr, uint64_t a[6]);
const char *xl_syscall_name(long nr);

/* The guest program the translator is running (for /proc/self/exe). */
extern char g_xl_exe_path[512];
/* The guest's initial stack, which this translator maps (build_stack()). */
extern uint64_t g_xl_guest_stack_lo, g_xl_guest_stack_hi;
/* How the translator itself was started (its argv[0]). */
extern char g_xl_self_path[512];
/* The environment the translator was started with. */
extern char **g_xl_envp;
/* Where the guest program was loaded (its load bias; 0 for a fixed one). */
extern uint64_t g_xl_prog_base;

/* Read-only code (syscall.c): ranges mapped executable and not writable,
 * and a counter bumped on every change to the address space. */
extern volatile uint64_t g_xl_code_epoch;
void xl_code_map_update(uint64_t addr, uint64_t len, int prot);   /* prot -1: unmapped */
bool xl_code_is_readonly(uint64_t start, uint64_t end);
/* Ranges that stopped being read-only code (unmapped, remapped, made
 * writable), as a log: g_xl_inval_seq counts the entries. A JIT replays
 * the ones after *seen (fn per range) and kills its translations of them;
 * false if the log no longer holds them all (the caller then flushes). */
extern volatile uint64_t g_xl_inval_seq;
bool xl_code_inval_since(uint64_t *seen, void (*fn)(uint64_t start, uint64_t end, void *arg),
                         void *arg);

/* ---- signals (signal.c) ----------------------------------------------- */

void xl_signal_init(void);
long xl_sys_rt_sigaction(gthread_t *t, int sig, uint64_t act, uint64_t oact,
                         uint64_t sigsetsize);
long xl_sys_rt_sigprocmask(gthread_t *t, int how, uint64_t set, uint64_t oset,
                           uint64_t sigsetsize);
long xl_sys_sigaltstack(gthread_t *t, uint64_t ss, uint64_t oss);
long xl_sys_rt_sigsuspend(gthread_t *t, uint64_t mask, uint64_t sigsetsize);
/* Called by the run loop: deliver what is pending and unblocked. */
void xl_signal_deliver(gthread_t *t);
/* The guest raised a synchronous signal (illegal instruction, divide
 * error, breakpoint, fault). */
void xl_signal_raise_sync(gthread_t *t, int sig, int code, uint64_t addr);
/* After a host system call came back EINTR: re-execute it (true), or hand
 * the guest EINTR (false)? */
bool xl_signal_should_restart(gthread_t *t);
/* Set the host's mask to the guest's (minus the synchronous signals). */
void xl_signal_sync_host_mask(gthread_t *t);
/* For the guest signal frame code. */
bool xl_signal_on_altstack(gthread_t *t, uint64_t sp);
uint64_t xl_signal_frame_top(gthread_t *t, int sig, uint64_t flags, uint64_t sp);
void xl_signal_get_action(int sig, xl_ksigaction_t *out);
/* The guest returned from a handler: install the mask it saved. */
void xl_signal_set_guest_mask(gthread_t *t, uint64_t mask);
/* Before the process is replaced: make the host forget our handlers'
 * blocking side effects. */
void xl_signal_before_exec(void);
/* fork child: per-thread state of the new single-threaded process */
void xl_signal_after_fork_child(gthread_t *t);
/* Terminate the process by `sig` with its default action (core dump /
 * "killed by signal" status for the parent). */
XL_NORETURN void xl_signal_die(int sig);

/* thread.c */
long xl_sys_clone(gthread_t *t, uint64_t flags, uint64_t newsp, uint64_t ptid,
                  uint64_t ctid, uint64_t tls);
XL_NORETURN void xl_thread_exit(gthread_t *t, int code);
bool xl_is_multithreaded(void);
/* Set once a second guest thread exists: the x86 guest then orders its
 * memory accesses as x86 does (TSO). */
extern volatile int g_xl_mt;

/* Host fault handler hook: called on the host signal stack. */
void xl_host_signal(int sig, xl_siginfo_t *info, void *uc);
XL_NORETURN void xl_fault_resume(gthread_t *t);

/* ---- the guest CPU (guest/<isa>/) -------------------------------------- */

typedef struct {
    const char *name;           /* "x86_64" */
    uint16_t elf_machine;       /* EM_X86_64 */
    const char *platform;       /* AT_PLATFORM */
    uint64_t hwcap;             /* AT_HWCAP */
    uint64_t hwcap2;            /* AT_HWCAP2 */
    uint64_t minsigstksz;       /* AT_MINSIGSTKSZ (0: omit) */
    size_t cpu_size;
    const char *lib_path;       /* LD_LIBRARY_PATH default for this ISA */
} xl_guest_desc_t;

extern const xl_guest_desc_t g_xl_guest;

void guest_cpu_init(gthread_t *t, uint64_t pc, uint64_t sp);
int  guest_run(gthread_t *t);
void guest_syscall(gthread_t *t);
uint64_t guest_pc(gthread_t *t);
uint64_t guest_sp(gthread_t *t);
void guest_dump(gthread_t *t);
/* The register state of a new thread or process: the call returns 0 there,
 * on stack `sp` (0: the parent's), with thread pointer `tls` if set_tls. */
void guest_clone_child(gthread_t *child, uint64_t sp, bool set_tls, uint64_t tls);
/* fork() from a multi-threaded guest: every translator lock is taken before
 * the host fork and let go after it, in the parent and in the child (see
 * xl_sys_clone). acquire != 0 takes them, 0 releases. */
void guest_fork_locks(int acquire);
void xl_code_fork_locks(int acquire);
void xl_signal_fork_locks(int acquire);
/* Build a signal frame on the guest stack and enter the handler. Returns
 * false if the frame could not be written. */
bool guest_signal_setup(gthread_t *t, int sig, const xl_siginfo_t *info,
                        const xl_ksigaction_t *act, uint64_t saved_mask);
long guest_rt_sigreturn(gthread_t *t);
/* Guest-specific system calls the guest front end did not handle itself
 * but which need the CPU (arch_prctl). XL_ENOSYS if none. */
long guest_arch_syscall(gthread_t *t, long nr, uint64_t a[6]);
/* Rewrite the uname "machine" the host reported. */
void guest_fix_uname(void *utsname);
/* The guest is about to run in a new thread: anything per-thread host
 * state (rounding modes) the CPU state implies. */
void guest_thread_enter(gthread_t *t);
/* A synchronous host fault interrupted the thread at the host context uc.
 * If that was generated code (a JIT translation), write the guest state
 * of the faulting guest instruction back into the CPU structure and
 * return true; false if the fault was elsewhere (the CPU structure is
 * then already up to date). */
bool guest_jit_fault(gthread_t *t, void *uc);
