/*
 * Guest threads.
 *
 * Every guest thread is a host thread of the translator. Its gthread_t and
 * the host stack the translator runs it on share one mapping:
 *
 *     [guard page][host stack ............][gthread_t + guest CPU]
 *
 * and the host thread pointer points at the gthread_t, so the host signal
 * handler and the run loop find it without a lookup.
 *
 * clone() of a thread (CLONE_VM) becomes a host clone() onto a fresh
 * mapping whose guest CPU is a copy of the caller's; anything without
 * CLONE_VM (fork) is a host fork, after which the child's copy of the
 * caller's gthread_t simply carries on. vfork (CLONE_VM | CLONE_VFORK
 * without CLONE_THREAD) is run as a fork: the child cannot share the
 * parent's host stack.
 */

#include "xl.h"
#include "host.h"

#define HOST_STACK_SIZE (512u * 1024u)

#define CLONE_VM        0x00000100ULL
#define CLONE_VFORK     0x00004000ULL
#define CLONE_THREAD    0x00010000ULL
#define CLONE_SETTLS    0x00080000ULL

static xl_lock_t g_free_lock;
static gthread_t *g_free_threads;
volatile int g_xl_mt;

gthread_t *xl_current(void)
{
    return xl_host_tp();
}

static bool thread_is_gone(int32_t tid)
{
    return host_tgkill(host_getpid(), tid, 0) == -XL_ESRCH;
}

gthread_t *xl_thread_alloc(void)
{
    gthread_t *t = NULL;
    xl_lock(&g_free_lock);
    for (gthread_t **pp = &g_free_threads; *pp != NULL; pp = &(*pp)->next_free) {
        if (thread_is_gone((*pp)->tid)) {
            t = *pp;
            *pp = t->next_free;
            break;
        }
    }
    xl_unlock(&g_free_lock);

    if (t == NULL) {
        size_t head = XL_PAGE_UP(sizeof(gthread_t) + g_xl_guest.cpu_size);
        size_t size = XL_PAGE_SIZE + HOST_STACK_SIZE + head;
        uint8_t *m = xl_map(size);
        if (m == NULL) {
            return NULL;
        }
        (void)host_mprotect((uint64_t)(uintptr_t)m, XL_PAGE_SIZE, 0);
        t = (gthread_t *)(void *)(m + XL_PAGE_SIZE + HOST_STACK_SIZE);
        t->mapping = m;
        t->mapping_size = size;
        t->host_stack_top = (uint64_t)(uintptr_t)t;
    }
    void *mapping = t->mapping;
    size_t mapping_size = t->mapping_size;
    uint64_t top = t->host_stack_top;
    void *engine = t->engine;
    memset(t, 0, sizeof(gthread_t) + g_xl_guest.cpu_size);
    t->self = t;
    t->mapping = mapping;
    t->mapping_size = mapping_size;
    t->host_stack_top = top;
    t->engine = engine;
    t->ss_flags = XL_SS_DISABLE;
    return t;
}

void xl_thread_free(gthread_t *t)
{
    xl_lock(&g_free_lock);
    t->next_free = g_free_threads;
    g_free_threads = t;
    xl_unlock(&g_free_lock);
}

XL_NORETURN void xl_fault_resume(gthread_t *t)
{
    host_longjmp(&t->jb, XL_EV_FAULT);
}

XL_NORETURN void xl_thread_loop(gthread_t *self)
{
    guest_thread_enter(self);
    for (;;) {
        /* Re-read after a longjmp lands here: nothing local survives it. */
        int ev = host_setjmp(&xl_current()->jb);
        gthread_t *t = xl_current();
        if (ev == 0) {
            ev = guest_run(t);
        }
        switch (ev) {
        case XL_EV_SYSCALL:
            guest_syscall(t);
            break;
        case XL_EV_FAULT:
            if (t->in_frame_setup) {
                /* The handler's frame could not be written: Linux kills
                 * the process with SIGSEGV. */
                xl_log("cannot write a signal frame at sp %lx", guest_sp(t));
                xl_signal_die(XL_SIGSEGV);
            } else {
                xl_signal_raise_sync(t, t->fault_sig, t->fault_code, t->fault_addr);
            }
            break;
        default:
            break;
        }
        xl_signal_deliver(t);
    }
}

static void thread_start(void *arg)
{
    gthread_t *t = arg;
    t->tid = (int32_t)host_gettid();
    xl_thread_loop(t);
}

bool xl_is_multithreaded(void);
bool xl_is_multithreaded(void)
{
    return g_xl_mt != 0;
}

long xl_sys_clone(gthread_t *t, uint64_t flags, uint64_t newsp, uint64_t ptid,
                  uint64_t ctid, uint64_t tls);
long xl_sys_clone(gthread_t *t, uint64_t flags, uint64_t newsp, uint64_t ptid,
                  uint64_t ctid, uint64_t tls)
{
    const bool settls = (flags & CLONE_SETTLS) != 0u;

    if ((flags & CLONE_VM) != 0u &&
        !((flags & CLONE_VFORK) != 0u && (flags & CLONE_THREAD) == 0u)) {
        gthread_t *c = xl_thread_alloc();
        if (c == NULL) {
            return -XL_EAGAIN;
        }
        memcpy(c->cpu, t->cpu, g_xl_guest.cpu_size);
        c->guest_mask = t->guest_mask;
        guest_clone_child(c, newsp, settls, tls);
        g_xl_mt = 1;
        long r = host_clone(flags | CLONE_SETTLS, c->host_stack_top, ptid, ctid,
                            (uint64_t)(uintptr_t)c, thread_start, c);
        if (r < 0) {
            c->tid = 0;
            xl_thread_free(c);
        }
        return r;
    }

    /* fork, or vfork run as one.
     *
     * Only the calling thread exists in the child, so a translator lock that
     * another thread holds at the moment of the fork would stay held there
     * forever, and the child would spin on it at its first mmap or signal
     * (the code-range lock is taken on every mapping change). Take every
     * lock first, the way pthread_atfork handlers do, and let go of them on
     * both sides once the fork is done -- the child then also inherits the
     * tables they protect in a consistent state. */
    xl_signal_fork_locks(1);
    xl_code_fork_locks(1);
    guest_fork_locks(1);
    xl_lock(&g_free_lock);
    uint64_t hflags = flags & ~(CLONE_SETTLS | CLONE_VM | CLONE_VFORK);
    uint64_t a[6] = { hflags, 0, ptid, ctid, 0, 0 };
    long r = xl_host_syscall_plain(56 /* clone */, a);
    if (r == 0) {
        t->tid = (int32_t)host_gettid();
        /* Only this thread exists in the child. */
        g_free_threads = NULL;
    }
    xl_unlock(&g_free_lock);
    guest_fork_locks(0);
    xl_code_fork_locks(0);
    xl_signal_fork_locks(0);
    if (r == 0) {
        guest_clone_child(t, newsp, settls, tls);
        xl_signal_after_fork_child(t);
    }
    return r;
}

XL_NORETURN void xl_thread_exit(gthread_t *t, int code);
XL_NORETURN void xl_thread_exit(gthread_t *t, int code)
{
    xl_thread_free(t);
    host_exit_thread(code);
}
