/*
 * Guest signals.
 *
 * The guest's dispositions live here (g_actions); the host only ever sees
 * one of three things for a signal: SIG_DFL, SIG_IGN, or host_handler(),
 * which runs whenever the guest has a handler of its own.
 *
 * host_handler() does not run guest code -- the interrupted host thread may
 * be anywhere in the translator. It queues the signal on the thread's
 * gthread_t and sets sig_pending_flag; the interpreter checks that flag
 * between instructions and returns to the run loop, which builds the
 * guest's signal frame (guest_signal_setup) at an instruction boundary.
 * If the thread was about to make, or was blocked in, a guest system call
 * (host_safe_syscall), the handler moves it to that call's -EINTR exit so
 * the guest handler is not held up until the call completes; whether the
 * call is then re-run follows the guest handler's SA_RESTART
 * (xl_signal_should_restart).
 *
 * The host's blocked mask mirrors the guest's, so a signal the guest blocks
 * stays pending in the kernel (sigwait, signalfd and sigpending see it as
 * they would natively) -- except the synchronous signals, which the host
 * never blocks: a guest fault must always reach the translator.
 *
 * A fault (SIGSEGV/SIGBUS) raised by the kernel while the translator
 * touched guest memory is not queued: the handler sends the thread to
 * xl_fault_resume(), which returns to the run loop, and the loop raises the
 * signal on the guest instruction being executed.
 */

#include "xl.h"
#include "host.h"

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define CANON_rt_sigsuspend 130

static xl_ksigaction_t g_actions[XL_NSIG + 1];
static xl_lock_t g_actions_lock;

static const uint64_t k_unblockable = XL_SIGBIT(XL_SIGKILL) | XL_SIGBIT(XL_SIGSTOP);
static const uint64_t k_default_ignore = XL_SIGBIT(XL_SIGCHLD) | XL_SIGBIT(XL_SIGCONT) |
                                         XL_SIGBIT(XL_SIGURG) | XL_SIGBIT(XL_SIGWINCH);
static const uint64_t k_default_stop = XL_SIGBIT(XL_SIGSTOP) | XL_SIGBIT(XL_SIGTSTP) |
                                       XL_SIGBIT(XL_SIGTTIN) | XL_SIGBIT(XL_SIGTTOU);

static void host_handler(int sig, xl_siginfo_t *info, void *uc)
{
    xl_host_signal(sig, info, uc);
}

void xl_host_signal(int sig, xl_siginfo_t *info, void *uc)
{
    gthread_t *t = xl_current();
    if (t == NULL || sig <= 0 || sig > XL_NSIG) {
        return;
    }
    uint64_t pc = *host_uc_pc_ptr(uc);
    if ((XL_SIGBIT(sig) & XL_SYNC_SIGS) != 0u && info->si_code > 0) {
        /* in generated code: put the guest state back first */
        (void)guest_jit_fault(t, uc);
        t->fault_sig = sig;
        t->fault_code = info->si_code;
        t->fault_addr = info->u.addr;
        t->fault_host_pc = pc;
        host_uc_redirect(uc, (uint64_t)(uintptr_t)xl_fault_resume,
                         (uint64_t)(uintptr_t)t);
        return;
    }
    t->sig_info[sig] = *info;
    __atomic_or_fetch(&t->sig_pending, XL_SIGBIT(sig), __ATOMIC_SEQ_CST);
    t->sig_pending_flag = 1;
    if (pc >= (uint64_t)(uintptr_t)host_safe_syscall_start &&
        pc < (uint64_t)(uintptr_t)host_safe_syscall_end) {
        *host_uc_pc_ptr(uc) = (uint64_t)(uintptr_t)host_safe_syscall_eintr;
    }
}

static void apply_host_action(int sig)
{
    if (sig == XL_SIGKILL || sig == XL_SIGSTOP) {
        return;
    }
    const xl_ksigaction_t *ga = &g_actions[sig];
    const bool sync = (XL_SIGBIT(sig) & XL_SYNC_SIGS) != 0u;
    xl_ksigaction_t h;
    memset(&h, 0, sizeof(h));
    if (!sync && ga->handler == XL_SIG_DFL) {
        h.handler = XL_SIG_DFL;
    } else if (!sync && ga->handler == XL_SIG_IGN) {
        h.handler = XL_SIG_IGN;
    } else {
        h.handler = (uint64_t)(uintptr_t)host_handler;
        h.flags = XL_SA_SIGINFO | XL_SA_RESTORER;
        h.restorer = (uint64_t)(uintptr_t)host_sigreturn;
        h.mask = sync ? 0u : ~XL_SYNC_SIGS;
        if (sync) {
            h.flags |= XL_SA_NODEFER;
        }
    }
    h.flags |= ga->flags & (XL_SA_NOCLDSTOP | XL_SA_NOCLDWAIT);
    (void)host_rt_sigaction(sig, &h, NULL);
}

void xl_signal_init(void)
{
    gthread_t *t = xl_current();
    /* What exec left us: ignored signals stay ignored, and the blocked
     * mask is inherited. */
    for (int sig = 1; sig <= XL_NSIG; ++sig) {
        xl_ksigaction_t old;
        memset(&old, 0, sizeof(old));
        if (host_rt_sigaction(sig, NULL, &old) == 0 && old.handler == XL_SIG_IGN) {
            g_actions[sig].handler = XL_SIG_IGN;
        }
    }
    uint64_t mask = 0;
    (void)host_rt_sigprocmask(SIG_BLOCK, NULL, &mask);
    t->guest_mask = mask & ~k_unblockable;
    for (int sig = 1; sig <= XL_NSIG; ++sig) {
        if ((XL_SIGBIT(sig) & XL_SYNC_SIGS) != 0u) {
            apply_host_action(sig);
        }
    }
    xl_signal_sync_host_mask(t);
}

void xl_signal_sync_host_mask(gthread_t *t)
{
    uint64_t m = t->guest_mask & ~XL_SYNC_SIGS;
    (void)host_rt_sigprocmask(SIG_SETMASK, &m, NULL);
}

void xl_signal_set_guest_mask(gthread_t *t, uint64_t mask)
{
    t->guest_mask = mask & ~k_unblockable;
    xl_signal_sync_host_mask(t);
}

void xl_signal_get_action(int sig, xl_ksigaction_t *out)
{
    *out = g_actions[sig];
}

long xl_sys_rt_sigaction(gthread_t *t, int sig, uint64_t act, uint64_t oact,
                         uint64_t sigsetsize)
{
    (void)t;
    if (sigsetsize != 8u || sig <= 0 || sig > XL_NSIG) {
        return -XL_EINVAL;
    }
    if (act != 0u && (sig == XL_SIGKILL || sig == XL_SIGSTOP)) {
        return -XL_EINVAL;
    }
    xl_ksigaction_t nact;
    if (act != 0u) {
        memcpy(&nact, (const void *)(uintptr_t)act, sizeof(nact));
    }
    xl_lock(&g_actions_lock);
    xl_ksigaction_t old = g_actions[sig];
    if (act != 0u) {
        nact.mask &= ~k_unblockable;
        g_actions[sig] = nact;
        apply_host_action(sig);
    }
    xl_unlock(&g_actions_lock);
    if (oact != 0u) {
        memcpy((void *)(uintptr_t)oact, &old, sizeof(old));
    }
    return 0;
}

long xl_sys_rt_sigprocmask(gthread_t *t, int how, uint64_t set, uint64_t oset,
                           uint64_t sigsetsize)
{
    if (sigsetsize != 8u) {
        return -XL_EINVAL;
    }
    uint64_t old = t->guest_mask;
    if (set != 0u) {
        uint64_t s;
        memcpy(&s, (const void *)(uintptr_t)set, 8);
        uint64_t n;
        switch (how) {
        case SIG_BLOCK:   n = old | s; break;
        case SIG_UNBLOCK: n = old & ~s; break;
        case SIG_SETMASK: n = s; break;
        default: return -XL_EINVAL;
        }
        xl_signal_set_guest_mask(t, n);
        if ((t->sig_pending & ~t->guest_mask) != 0u) {
            t->sig_pending_flag = 1;
        }
    }
    if (oset != 0u) {
        memcpy((void *)(uintptr_t)oset, &old, 8);
    }
    return 0;
}

bool xl_signal_on_altstack(gthread_t *t, uint64_t sp)
{
    return (t->ss_flags & XL_SS_DISABLE) == 0 &&
           sp > t->ss_sp && sp - t->ss_sp <= t->ss_size;
}

long xl_sys_sigaltstack(gthread_t *t, uint64_t ss, uint64_t oss)
{
    struct { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; } cur, req;
    const bool on = xl_signal_on_altstack(t, guest_sp(t));
    cur.sp = t->ss_sp;
    cur.flags = (t->ss_flags & XL_SS_DISABLE) != 0 ? XL_SS_DISABLE :
                (on ? XL_SS_ONSTACK : 0);
    cur.pad = 0;
    cur.size = t->ss_size;
    if (ss != 0u) {
        memcpy(&req, (const void *)(uintptr_t)ss, sizeof(req));
        if (on) {
            return -XL_EPERM;
        }
        int32_t mode = req.flags & ~(int32_t)0x80000000; /* SS_AUTODISARM */
        if (mode != 0 && mode != XL_SS_DISABLE && mode != XL_SS_ONSTACK) {
            return -XL_EINVAL;
        }
        if (mode == XL_SS_DISABLE) {
            t->ss_flags = XL_SS_DISABLE;
            t->ss_sp = 0;
            t->ss_size = 0;
        } else {
            if (req.size < 2048u) {
                return -XL_ENOMEM;
            }
            t->ss_flags = 0;
            t->ss_sp = req.sp;
            t->ss_size = req.size;
        }
    }
    if (oss != 0u) {
        memcpy((void *)(uintptr_t)oss, &cur, sizeof(cur));
    }
    return 0;
}

long xl_sys_rt_sigsuspend(gthread_t *t, uint64_t mask, uint64_t sigsetsize)
{
    if (sigsetsize != 8u) {
        return -XL_EINVAL;
    }
    uint64_t m;
    memcpy(&m, (const void *)(uintptr_t)mask, 8);
    m &= ~k_unblockable;
    t->suspend_mask = m;
    t->suspend_active = 1;
    for (;;) {
        if ((t->sig_pending & ~m) != 0u) {
            break;
        }
        uint64_t hm = m & ~XL_SYNC_SIGS;
        uint64_t a[6] = { (uint64_t)(uintptr_t)&hm, 8, 0, 0, 0, 0 };
        long r = xl_host_syscall(t, CANON_rt_sigsuspend, a);
        if (r != -XL_EINTR) {
            t->suspend_active = 0;
            return r;
        }
    }
    t->sig_pending_flag = 1;
    return -XL_EINTR;
}

uint64_t xl_signal_frame_top(gthread_t *t, int sig, uint64_t flags, uint64_t sp)
{
    (void)sig;
    if ((flags & XL_SA_ONSTACK) != 0u && (t->ss_flags & XL_SS_DISABLE) == 0 &&
        !xl_signal_on_altstack(t, sp)) {
        return t->ss_sp + t->ss_size;
    }
    return sp;
}

XL_NORETURN void xl_signal_die(int sig)
{
    xl_ksigaction_t h;
    memset(&h, 0, sizeof(h));
    (void)host_rt_sigaction(sig, &h, NULL);
    uint64_t m = XL_SIGBIT(sig);
    (void)host_rt_sigprocmask(SIG_UNBLOCK, &m, NULL);
    (void)host_tgkill(host_getpid(), host_gettid(), sig);
    host_exit_group(128 + sig);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case XL_SIGSEGV: return "SIGSEGV";
    case XL_SIGBUS:  return "SIGBUS";
    case XL_SIGILL:  return "SIGILL";
    case XL_SIGFPE:  return "SIGFPE";
    case XL_SIGTRAP: return "SIGTRAP";
    default:         return "signal";
    }
}

void xl_signal_raise_sync(gthread_t *t, int sig, int code, uint64_t addr)
{
    const xl_ksigaction_t *act = &g_actions[sig];
    if (act->handler == XL_SIG_DFL || act->handler == XL_SIG_IGN ||
        (t->guest_mask & XL_SIGBIT(sig)) != 0u) {
        /* Nothing in the guest will catch it: the process dies of it, as
         * it would natively. Say where first -- an illegal instruction is
         * most likely one the interpreter does not know. */
        xl_log("%s %s (code %d) at pc %lx, address %lx%s",
               g_xl_exe_path, sig_name(sig), code, guest_pc(t), addr,
               sig == XL_SIGILL ? " -- instruction not supported?" : "");
        if (g_xl_debug && (sig == XL_SIGSEGV || sig == XL_SIGBUS)) {
            xl_log("host pc %lx", t->fault_host_pc);
        }
        guest_dump(t);
        xl_signal_die(sig);
    }
    xl_siginfo_t *info = &t->sig_info[sig];
    memset(info, 0, sizeof(*info));
    info->si_signo = sig;
    info->si_code = code;
    info->u.addr = addr;
    __atomic_or_fetch(&t->sig_pending, XL_SIGBIT(sig), __ATOMIC_SEQ_CST);
    t->sig_pending_flag = 1;
}

static uint64_t effective_mask(gthread_t *t)
{
    return t->suspend_active ? t->suspend_mask : t->guest_mask;
}

bool xl_signal_should_restart(gthread_t *t)
{
    uint64_t d = t->sig_pending & ~effective_mask(t);
    if (d == 0u) {
        return true;
    }
    int sig = __builtin_ctzll(d) + 1;
    const xl_ksigaction_t *act = &g_actions[sig];
    if (act->handler == XL_SIG_DFL || act->handler == XL_SIG_IGN) {
        return true;
    }
    return (act->flags & XL_SA_RESTART) != 0u;
}

void xl_signal_deliver(gthread_t *t)
{
    for (;;) {
        t->sig_pending_flag = 0;
        uint64_t d = __atomic_load_n(&t->sig_pending, __ATOMIC_SEQ_CST) &
                     ~effective_mask(t);
        if (d == 0u) {
            break;
        }
        uint64_t sync = d & XL_SYNC_SIGS;
        int sig = __builtin_ctzll(sync != 0u ? sync : d) + 1;
        __atomic_and_fetch(&t->sig_pending, ~XL_SIGBIT(sig), __ATOMIC_SEQ_CST);
        xl_siginfo_t info = t->sig_info[sig];
        xl_ksigaction_t act = g_actions[sig];

        if (act.handler == XL_SIG_IGN) {
            continue;
        }
        if (act.handler == XL_SIG_DFL) {
            if ((XL_SIGBIT(sig) & k_default_ignore) != 0u) {
                continue;
            }
            if ((XL_SIGBIT(sig) & k_default_stop) != 0u) {
                (void)host_sys(HNR_kill, host_getpid(), XL_SIGSTOP, 0);
                continue;
            }
            xl_signal_die(sig);
        }

        uint64_t saved = t->guest_mask;
        t->suspend_active = 0;
        t->in_frame_setup = 1;
        bool ok = guest_signal_setup(t, sig, &info, &act, saved);
        t->in_frame_setup = 0;
        if (!ok) {
            xl_log("cannot write the frame for signal %d", sig);
            xl_signal_die(XL_SIGSEGV);
        }
        uint64_t m = saved | act.mask;
        if ((act.flags & XL_SA_NODEFER) == 0u) {
            m |= XL_SIGBIT(sig);
        }
        if ((act.flags & XL_SA_RESETHAND) != 0u) {
            xl_lock(&g_actions_lock);
            memset(&g_actions[sig], 0, sizeof(g_actions[sig]));
            apply_host_action(sig);
            xl_unlock(&g_actions_lock);
        }
        xl_signal_set_guest_mask(t, m);
        /* One handler per pass; anything else still pending interrupts the
         * handler right away, as nested delivery would. */
        if ((t->sig_pending & ~t->guest_mask) != 0u) {
            t->sig_pending_flag = 1;
        }
        return;
    }
    t->suspend_active = 0;
}

void xl_signal_before_exec(void)
{
}

void xl_signal_after_fork_child(gthread_t *t)
{
    t->sig_pending = 0;
    t->sig_pending_flag = 0;
    t->suspend_active = 0;
}
