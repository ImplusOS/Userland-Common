/*
 * The x86_64 guest's interface to the rest of the translator: its
 * descriptor, CPUID, the system call front end (the canonical ABI is the
 * x86_64 one, so a call goes through as it is), signal frames in the
 * x86_64 rt_sigframe layout, arch_prctl and the state of a cloned thread.
 */

#include "x86.h"
#include "host.h"

/* CPUID.1:EDX -- FPU TSC MSR PSE PAE CX8 APIC SEP PGE CMOV PAT CLFSH MMX
 * FXSR SSE SSE2. Linux reports this word as AT_HWCAP. */
#define X86_CPUID1_EDX 0x0789AB79u

const xl_guest_desc_t g_xl_guest = {
    .name = "x86_64",
    .elf_machine = 62,          /* EM_X86_64 */
    .platform = "x86_64",
    .hwcap = X86_CPUID1_EDX,
    .hwcap2 = 0,
    .minsigstksz = 2048,
    .cpu_size = sizeof(x86cpu_t),
    .lib_path = "/lib64:/usr/lib/x86_64-linux-gnu:/usr/lib",
};

/* ---- CPUID ------------------------------------------------------------------ */

static void cpuid_str(const char *s, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t w[4] = { 0, 0, 0, 0 };
    memcpy(w, s, 16);
    *a = w[0]; *b = w[1]; *c = w[2]; *d = w[3];
}

/* deterministic cache parameters (leaf 4) */
static void cache_leaf(unsigned type, unsigned level, unsigned ways, unsigned size_kb,
                       unsigned sharing, uint32_t *a, uint32_t *b, uint32_t *c)
{
    unsigned sets = size_kb * 1024u / (ways * 64u);
    *a = type | (level << 5) | (1u << 8) | ((sharing - 1u) << 14);
    *b = ((ways - 1u) << 22) | 63u;
    *c = sets - 1u;
}

void x86_cpuid(x86cpu_t *c)
{
    const uint32_t leaf = (uint32_t)c->r[RAX];
    const uint32_t sub = (uint32_t)c->r[RCX];
    uint32_t a = 0, b = 0, cx = 0, d = 0;
    static const char brand[48] = "ImplusOS x86-64 translator (interpreted)";
    switch (leaf) {
    case 0:
        a = 7;
        b = 0x756E6547u;    /* "GenuineIntel" */
        d = 0x49656E69u;
        cx = 0x6C65746Eu;
        break;
    case 1:
        a = 0x000206A7u;    /* family 6, model 0x2A */
        b = 0x00010800u;    /* 64-byte CLFLUSH line, 1 logical processor */
        cx = 1u;            /* SSE3; no SSSE3 and up */
        d = X86_CPUID1_EDX;
        break;
    case 2:
        a = 0x0000FF01u;    /* "see leaf 4" */
        break;
    case 4:
        switch (sub) {
        case 0: cache_leaf(1, 1, 8, 32, 1, &a, &b, &cx); break;
        case 1: cache_leaf(2, 1, 8, 32, 1, &a, &b, &cx); break;
        case 2: cache_leaf(3, 2, 16, 1024, 1, &a, &b, &cx); break;
        case 3: cache_leaf(3, 3, 16, 8192, 16, &a, &b, &cx); break;
        default: break;
        }
        break;
    case 0x80000000u:
        a = 0x80000008u;
        break;
    case 0x80000001u:
        cx = 1u;                                    /* LAHF/SAHF */
        d = (1u << 11) | (1u << 20) | (1u << 29);   /* SYSCALL, NX, LM */
        break;
    case 0x80000002u: case 0x80000003u: case 0x80000004u:
        cpuid_str(brand + 16u * (leaf - 0x80000002u), &a, &b, &cx, &d);
        break;
    case 0x80000006u:
        cx = (1024u << 16) | (6u << 12) | 64u;
        break;
    case 0x80000008u:
        a = 0x3030u;
        break;
    default:
        break;
    }
    c->r[RAX] = a;
    c->r[RBX] = b;
    c->r[RCX] = cx;
    c->r[RDX] = d;
}

/* ---- CPU state ------------------------------------------------------------------- */

void guest_cpu_init(gthread_t *t, uint64_t pc, uint64_t sp)
{
    x86cpu_t *c = X86(t);
    memset(c, 0, sizeof(*c));
    c->rip = pc;
    c->r[RSP] = sp;
    c->eflags = 0x202u;
    c->cc_op = CC_EFLAGS;
    c->mxcsr = 0x1F80u;
    x86_fpu_reset(c);
}

void guest_thread_enter(gthread_t *t)
{
    x86_mxcsr_apply(X86(t));
}

uint64_t guest_pc(gthread_t *t) { return X86(t)->rip; }
uint64_t guest_sp(gthread_t *t) { return X86(t)->r[RSP]; }

void guest_dump(gthread_t *t)
{
    x86cpu_t *c = X86(t);
    static const char *const n[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                       "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    for (int k = 0; k < 16; k += 4) {
        xl_log("  %-3s %016lx  %-3s %016lx  %-3s %016lx  %-3s %016lx",
               n[k], c->r[k], n[k + 1], c->r[k + 1], n[k + 2], c->r[k + 2], n[k + 3], c->r[k + 3]);
    }
    xl_log("  rip %016lx  flags %lx  fs %lx", c->rip,
           x86_get_flags(c) | (c->eflags & ~(uint64_t)F_ARITH), c->fs_base);
}

void guest_clone_child(gthread_t *child, uint64_t sp, bool set_tls, uint64_t tls)
{
    x86cpu_t *c = X86(child);
    c->r[RAX] = 0;
    if (sp != 0u) {
        c->r[RSP] = sp;
    }
    if (set_tls) {
        c->fs_base = tls;
    }
}

void guest_fix_uname(void *utsname)
{
    char *machine = (char *)utsname + 4 * 65;
    memset(machine, 0, 65);
    memcpy(machine, "x86_64", 7);
}

long guest_arch_syscall(gthread_t *t, long nr, uint64_t a[6])
{
    x86cpu_t *c = X86(t);
    (void)nr;
    switch (a[0]) {
    case 0x1001: c->gs_base = a[1]; return 0;                       /* ARCH_SET_GS */
    case 0x1002: c->fs_base = a[1]; return 0;                       /* ARCH_SET_FS */
    case 0x1003: mwr(a[1], 8, c->fs_base); return 0;                /* ARCH_GET_FS */
    case 0x1004: mwr(a[1], 8, c->gs_base); return 0;                /* ARCH_GET_GS */
    case 0x1011: return 1;                                          /* ARCH_GET_CPUID */
    default: return -XL_EINVAL;
    }
}

/* ---- system calls ------------------------------------------------------------------ */

void guest_syscall(gthread_t *t)
{
    x86cpu_t *c = X86(t);
    long nr = (long)c->r[RAX];
    uint64_t a[6] = { c->r[RDI], c->r[RSI], c->r[RDX], c->r[R10], c->r[R8], c->r[R9] };
    long r = xl_syscall(t, nr, a);
    if (r == XL_ERESTART) {
        c->rip -= 2;            /* back onto the SYSCALL; RAX still holds nr */
        return;
    }
    if (r == XL_EJUSTRETURN) {
        return;
    }
    c->r[RAX] = (uint64_t)r;
}

/* ---- signal frames ------------------------------------------------------------------- */

/* struct rt_sigframe (arch/x86/include/asm/sigframe.h):
 *   char *pretcode; struct ucontext uc; struct siginfo info;
 * struct ucontext: flags, link, stack_t (24), sigcontext (256), sigmask (8). */
#define UC_OFF        8u
#define UC_STACK      16u
#define UC_MCONTEXT   40u
#define UC_SIGMASK    296u
#define UC_SIZE       304u
#define INFO_OFF      (UC_OFF + UC_SIZE)
#define FRAME_SIZE    (INFO_OFF + 128u)

/* sigcontext slots (u64) */
enum { SC_R8, SC_R9, SC_R10, SC_R11, SC_R12, SC_R13, SC_R14, SC_R15, SC_RDI, SC_RSI,
       SC_RBP, SC_RBX, SC_RDX, SC_RAX, SC_RCX, SC_RSP, SC_RIP, SC_EFLAGS, SC_SEGS,
       SC_ERR, SC_TRAPNO, SC_OLDMASK, SC_CR2, SC_FPSTATE };

static uint64_t g_trampoline;

static uint64_t default_restorer(void)
{
    if (g_trampoline == 0u) {
        static const uint8_t code[] = { 0xB8, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05 };
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
    x86cpu_t *c = X86(t);
    const uint64_t old_sp = c->r[RSP];
    uint64_t sp = old_sp - 128u;    /* the red zone */
    uint64_t top = xl_signal_frame_top(t, sig, act->flags, sp);
    const bool on_alt = top != sp || xl_signal_on_altstack(t, old_sp);
    sp = top;
    const uint64_t fp = (sp - 512u) & ~63ULL;
    const uint64_t frame = ((fp - FRAME_SIZE) & ~15ULL) - 8u;

    insn_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.t = t;
    dummy.c = c;
    (void)x86_fxsave(&dummy, fp);

    uint8_t buf[FRAME_SIZE];
    memset(buf, 0, sizeof(buf));
    uint64_t restorer = (act->flags & XL_SA_RESTORER) != 0u ? act->restorer : default_restorer();
    memcpy(buf, &restorer, 8);

    uint8_t *uc = buf + UC_OFF;
    struct { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; } st = {
        t->ss_sp,
        (t->ss_flags & XL_SS_DISABLE) != 0 ? XL_SS_DISABLE : (on_alt ? XL_SS_ONSTACK : 0),
        0, t->ss_size,
    };
    memcpy(uc + UC_STACK, &st, sizeof(st));
    uint64_t sc[32];
    memset(sc, 0, sizeof(sc));
    sc[SC_R8] = c->r[R8];   sc[SC_R9] = c->r[R9];   sc[SC_R10] = c->r[R10];
    sc[SC_R11] = c->r[R11]; sc[SC_R12] = c->r[R12]; sc[SC_R13] = c->r[R13];
    sc[SC_R14] = c->r[R14]; sc[SC_R15] = c->r[R15]; sc[SC_RDI] = c->r[RDI];
    sc[SC_RSI] = c->r[RSI]; sc[SC_RBP] = c->r[RBP]; sc[SC_RBX] = c->r[RBX];
    sc[SC_RDX] = c->r[RDX]; sc[SC_RAX] = c->r[RAX]; sc[SC_RCX] = c->r[RCX];
    sc[SC_RSP] = old_sp;    sc[SC_RIP] = c->rip;
    sc[SC_EFLAGS] = x86_get_flags(c) | (c->eflags & ~(uint64_t)F_ARITH) | 0x202u;
    sc[SC_SEGS] = 0x33u | (0x2BULL << 48);   /* cs, gs, fs, ss */
    switch (sig) {
    case XL_SIGSEGV: case XL_SIGBUS:
        sc[SC_TRAPNO] = 14;
        sc[SC_ERR] = 4;
        sc[SC_CR2] = info->u.addr;
        break;
    case XL_SIGILL: sc[SC_TRAPNO] = 6; break;
    case XL_SIGFPE: sc[SC_TRAPNO] = 0; break;
    case XL_SIGTRAP: sc[SC_TRAPNO] = 3; break;
    default: break;
    }
    sc[SC_OLDMASK] = saved_mask;
    sc[SC_FPSTATE] = fp;
    memcpy(uc + UC_MCONTEXT, sc, 256);
    memcpy(uc + UC_SIGMASK, &saved_mask, 8);
    memcpy(buf + INFO_OFF, info, 128);
    mwr_n(frame, buf, sizeof(buf));

    c->r[RSP] = frame;
    c->rip = act->handler;
    c->r[RDI] = (uint64_t)sig;
    c->r[RSI] = frame + INFO_OFF;
    c->r[RDX] = frame + UC_OFF;
    c->r[RAX] = 0;
    c->eflags &= ~(uint64_t)(F_DF | F_TF);
    return true;
}

long guest_rt_sigreturn(gthread_t *t)
{
    x86cpu_t *c = X86(t);
    const uint64_t frame = c->r[RSP] - 8u;
    uint64_t sc[32];
    uint64_t mask;
    mrd_n(frame + UC_OFF + UC_MCONTEXT, sc, 256);
    mrd_n(frame + UC_OFF + UC_SIGMASK, &mask, 8);
    c->r[R8] = sc[SC_R8];   c->r[R9] = sc[SC_R9];   c->r[R10] = sc[SC_R10];
    c->r[R11] = sc[SC_R11]; c->r[R12] = sc[SC_R12]; c->r[R13] = sc[SC_R13];
    c->r[R14] = sc[SC_R14]; c->r[R15] = sc[SC_R15]; c->r[RDI] = sc[SC_RDI];
    c->r[RSI] = sc[SC_RSI]; c->r[RBP] = sc[SC_RBP]; c->r[RBX] = sc[SC_RBX];
    c->r[RDX] = sc[SC_RDX]; c->r[RAX] = sc[SC_RAX]; c->r[RCX] = sc[SC_RCX];
    c->r[RSP] = sc[SC_RSP];
    c->rip = sc[SC_RIP];
    const uint64_t keep = F_DF | F_AC | F_ID;
    c->eflags = (c->eflags & ~keep) | (sc[SC_EFLAGS] & keep);
    x86_set_flags(c, sc[SC_EFLAGS]);
    if (sc[SC_FPSTATE] != 0u) {
        insn_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.t = t;
        dummy.c = c;
        (void)x86_fxrstor(&dummy, sc[SC_FPSTATE]);
    }
    xl_signal_set_guest_mask(t, mask);
    return XL_EJUSTRETURN;
}
