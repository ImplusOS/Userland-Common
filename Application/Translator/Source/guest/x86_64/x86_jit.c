/*
 * The x86_64 guest's JIT: x86-64 code translated into AArch64 machine code
 * (on an arm64 host; elsewhere guest_run() falls back to the block engine).
 *
 * Translation unit. A block is a run of guest instructions entered at its
 * first one: it continues past conditional branches (the taken side
 * leaves through a "side exit") and ends at an unconditional transfer, a
 * system call, an instruction that may branch in a way the translator
 * does not follow, a page boundary, or JX_MAX_INSNS.
 *
 * Registers. The sixteen guest integer registers live in host registers
 * for as long as generated code runs (HR[]); x20 holds the guest CPU
 * structure and x29 the gthread_t. The trampolines load them on entry and
 * store them on every exit to the dispatcher, and around calls into C.
 * XMM, x87 and MMX state stays in the CPU structure.
 *
 * Flags. The interpreter's lazy flags (cc_op/cc_a/cc_b/cc_r/cc_cin) are
 * the canonical state at block boundaries. Inside a block the translator
 * knows which instruction last set the flags ("pending" state, pf_t), and
 * where its operands and result are: a guest register that nothing
 * overwrites before the flags are used, an immediate, or a copy in one of
 * four flag temporaries. A backward liveness pass drops flags nobody
 * reads; a consumer (Jcc, SETcc, CMOVcc, ADC...) recomputes the host
 * condition flags from the pending operands -- CMP + Jcc becomes CMP +
 * B.cond -- and the pending state is written out to the CPU structure
 * only where it must be: at exits, before calls into C, and for the rare
 * conditions that need the interpreter's x86_cond().
 *
 * Faults. Generated code does not store the guest RIP before each memory
 * access. Each access instead has a "fault site" (host offset -> guest
 * instruction, and the pending flags there); guest_jit_fault() maps the
 * faulting host PC back, copies the guest registers out of the host
 * signal context and rebuilds the lazy flags, and the fault is then raised
 * on the guest instruction exactly as the interpreter would. Every
 * instruction does all its memory accesses before it writes a register.
 *
 * Memory order. Once the process has a second thread, loads are followed
 * by DMB ISHLD and stores preceded by DMB ISHST (x86's TSO, as the
 * interpreter does); a cache translated single-threaded is flushed when
 * that changes.
 *
 * Linking. A direct exit first jumps to the dispatcher, which patches the
 * exit's branch to the target translation; indirect jumps, calls and
 * returns probe the cache's indirect-branch table inline. Every block
 * checks for pending signals on entry, so a chained loop still stops.
 *
 * Anything the translator has no native code for runs through the
 * interpreter (x86_step) from generated code, with the registers spilled;
 * SSE instructions through the interpreter's x86_sse_body with their
 * operands predecoded.
 *
 * XL_NOJIT=1 selects the block engine (x86_block.c), XL_NOBLOCKS=1 the
 * plain interpreter.
 */

#include "x86_ops.h"
#include "host.h"
#include "jit.h"

/* ---- engine selection ------------------------------------------------------ */

static int g_engine = -1;   /* 0 JIT, 1 block engine, 2 interpreter */
/* JIT debugging (XL_JIT_DEBUG=<letters>, for telling which part of the JIT
 * a misbehaving program depends on): s SSE through the interpreter, f flags
 * written out after every instruction that sets them, a LOCKed instructions
 * through the interpreter, c no chaining or indirect-branch table, 1 one
 * instruction per block. */
static unsigned g_jx_dbg;
static int g_jx_fbk_lo = -1, g_jx_fbk_hi = -1;  /* XL_JIT_FBK=lo-hi: these kinds via the interpreter */
static int g_jx_sp_lo = -1, g_jx_sp_hi = -1;    /* XL_JIT_SP=lo-hi: these SSE plans via the helper */
static uint64_t g_jx_fpc_lo, g_jx_fpc_hi;       /* XL_JIT_FPC=lo-hi (hex, from the program's load
                                                 * base): "f" for the blocks that start there */
#define JXD_NOSSE   1u
#define JXD_FLAGS   2u
#define JXD_ATOMIC  4u
#define JXD_NOCHAIN 8u
#define JXD_ONE     16u
#define JXD_VERIFY  32u     /* v: each instruction also run by the interpreter, compared */

static bool env_has(const char *kv)
{
    for (char **p = g_xl_envp; p != NULL && *p != NULL; ++p) {
        if (strcmp(*p, kv) == 0) {
            return true;
        }
    }
    return false;
}

int guest_run(gthread_t *t)
{
    if (XL_UNLIKELY(g_engine < 0)) {
#if defined(__aarch64__)
        g_engine = 0;
#else
        g_engine = 1;
#endif
        if (env_has("XL_NOJIT=1")) {
            g_engine = 1;
        }
        if (env_has("XL_NOBLOCKS=1")) {
            g_engine = 2;
        }
        for (char **p = g_xl_envp; p != NULL && *p != NULL; ++p) {
            const bool fbk = strncmp(*p, "XL_JIT_FBK=", 11) == 0, spk = strncmp(*p, "XL_JIT_SP=", 10) == 0;
            if (fbk || spk) {
                const char *c = *p + (fbk ? 11 : 10);
                int lo = 0, hi = 0;
                while (*c >= '0' && *c <= '9') lo = lo * 10 + (*c++ - '0');
                if (*c == '-') ++c;
                while (*c >= '0' && *c <= '9') hi = hi * 10 + (*c++ - '0');
                if (fbk) { g_jx_fbk_lo = lo; g_jx_fbk_hi = hi; } else { g_jx_sp_lo = lo; g_jx_sp_hi = hi; }
            }
            if (strncmp(*p, "XL_JIT_FPC=", 11) == 0) {
                uint64_t v[2] = { 0, 0 };
                const char *c = *p + 11;
                for (int k = 0; k < 2; ++k) {
                    for (;; ++c) {
                        const unsigned h = *c >= '0' && *c <= '9' ? (unsigned)(*c - '0') :
                                           *c >= 'a' && *c <= 'f' ? (unsigned)(*c - 'a' + 10) : 16u;
                        if (h == 16u) break;
                        v[k] = v[k] << 4 | h;
                    }
                    if (*c == '-') ++c;
                }
                g_jx_fpc_lo = v[0];
                g_jx_fpc_hi = v[1];
            }
            if (strncmp(*p, "XL_JIT_DEBUG=", 13) == 0) {
                for (const char *c = *p + 13; *c != '\0'; ++c) {
                    if (*c == 's') g_jx_dbg |= JXD_NOSSE;
                    if (*c == 'f') g_jx_dbg |= JXD_FLAGS;
                    if (*c == 'a') g_jx_dbg |= JXD_ATOMIC;
                    if (*c == 'c') g_jx_dbg |= JXD_NOCHAIN;
                    if (*c == '1') g_jx_dbg |= JXD_ONE;
                    if (*c == 'v') g_jx_dbg |= JXD_VERIFY | JXD_ONE | JXD_NOCHAIN;
                }
            }
        }
    }
    switch (g_engine) {
    case 0: return x86_run_jit(t);
    case 1: return x86_run_blocks(t);
    default: return x86_run_interp(t);
    }
}

#if !defined(__aarch64__)

int x86_run_jit(gthread_t *t)
{
    return x86_run_blocks(t);
}

bool guest_jit_fault(gthread_t *t, void *uc)
{
    (void)t;
    (void)uc;
    return false;
}

#else /* __aarch64__ */

#include "a64emit.h"

#define JX_MAX_INSNS   96
#ifndef JX_CODE_SIZE                    /* (overridable to test cache flushes) */
#define JX_CODE_SIZE   (64u << 20)
#define JX_META_SIZE   (32u << 20)
#endif
#define JX_BLOCK_CODE  (160u * 1024u)
#define JX_BLOCK_META  (48u * 1024u)
#define JX_MAX_SITES   640
#define JX_MAX_COLD    320
#define JX_MAX_LITS    256

/* what generated code returns to the dispatcher besides XL_EV_* */
enum { JX_CHAIN = 64, JX_LOOKUP, JX_INVAL, JX_STEP };

/* host registers */
#define RC   20u    /* x86cpu_t * */
#define RT   29u    /* gthread_t * */
#define FA   5u     /* flag temporaries */
#define FB   6u
#define FR   7u
#define FC   8u
#define S0   0u     /* scratch */
#define S1   1u
#define S2   2u
#define S3   3u
#define S4   4u     /* indirect branch targets */
#define X16  16u
#define X17  17u
#define ZR   31u
#define NOREG 16u

static const uint8_t HR[16] = { 9, 10, 11, 12, 13, 14, 15, 19, 21, 22, 23, 24, 25, 26, 27, 28 };

#define CPU_OFF(f) ((int64_t)__builtin_offsetof(x86cpu_t, f))
#define OFF_R(i)   (CPU_OFF(r) + 8 * (int64_t)(i))

_Static_assert(__builtin_offsetof(x86cpu_t, r) == 0, "guest registers first");
_Static_assert(__builtin_offsetof(x86cpu_t, cc_op) % 8 == 0, "cc_op/cc_size stored as one word");
_Static_assert(__builtin_offsetof(x86cpu_t, cc_size) == __builtin_offsetof(x86cpu_t, cc_op) + 4, "cc_size");
_Static_assert(__builtin_offsetof(x86cpu_t, cc_b) == __builtin_offsetof(x86cpu_t, cc_a) + 8, "cc_a/cc_b pair");
_Static_assert(__builtin_offsetof(x86cpu_t, cc_a) <= 496, "STP reach");

/* ---- decoded instructions ---------------------------------------------------- */

enum {
    K_FALLBACK = 0, K_SSE,
    K_ALU_RR, K_ALU_RI, K_ALU_RM, K_ALU_MR, K_ALU_MI,
    K_MOV_RR, K_MOV_RI, K_MOV_LD, K_MOV_ST, K_MOV_MI,
    K_LEA, K_MOVX_R, K_MOVX_M,
    K_PUSH_R, K_PUSH_I, K_POP_R,
    K_INCDEC_R, K_INCDEC_M, K_NOT_R, K_NEG_R, K_NOT_M, K_NEG_M,
    K_SHIFT_RI, K_SHIFT_RC,
    K_IMUL_RR, K_IMUL_RRI, K_IMUL_RM, K_IMUL_RMI,
    K_MUL1_R, K_MUL1_M, K_DIV1_R, K_DIV1_M,
    K_CDQ, K_SETCC_R, K_CMOV_R, K_CMOV_M,
    K_XCHG_RR, K_BSWAP, K_BSX_R, K_BSX_M, K_BT_RR, K_BT_RI,
    K_LEAVE, K_NOP,
    K_JCC, K_JMP, K_CALL, K_RET, K_CALL_R, K_CALL_M, K_JMP_R, K_JMP_M,
    K_SYSCALL,
    K_SETCC_M, K_PUSH_M,
    K_ATOM_ALU, K_ATOM_INCDEC, K_CMPXCHG, K_XADD, K_XCHG_M,    /* memory, atomic */
};

/* flag effects */
enum { FR_NONE = 0, FR_COND, FR_CF, FR_ALL };
enum { FW_NONE = 0, FW_FULL, FW_PASS };

typedef struct {
    uint64_t pc, next;
    int64_t disp;           /* EA displacement (RIP-relative: absolute) */
    uint64_t imm;           /* immediate / branch target / SSE imm8 */
    uint8_t kind;
    uint8_t sz;             /* operand size */
    uint8_t op;             /* ALU op, condition, shift kind, source size... */
    uint8_t dst, src;       /* guest registers */
    uint8_t rex;            /* a REX prefix was present */
    uint8_t base, index, scale, seg;
    uint8_t rexb, p66, rep, mod;    /* SSE operand decoding */
    uint8_t plan;           /* K_SSE: SP_* (0: through the interpreter) */
    uint8_t fr, fw;
    uint8_t live;           /* flags read after this instruction */
    uint8_t need;           /* producer: N_* components kept */
    uint8_t ends;
    uint16_t wmask;         /* guest registers written */
    uint16_t wwin;          /* producer: registers written while its flags matter */
} ji_t;

/* ---- pending flags -------------------------------------------------------------- */

enum { PK_NONE = 0, PK_MEM, PK_ADD, PK_SUB, PK_ADC, PK_SBB, PK_LOGIC, PK_TEST,
       PK_INC, PK_DEC, PK_SHL, PK_SHR, PK_SAR, PK_MUL,
       PK_EFL };    /* EFL: the arithmetic EFLAGS bits themselves, in lr */

static const uint8_t k_pk_cc[] = {
    [PK_ADD] = CC_ADD, [PK_SUB] = CC_SUB, [PK_ADC] = CC_ADC, [PK_SBB] = CC_SBB,
    [PK_LOGIC] = CC_LOGIC, [PK_TEST] = CC_LOGIC, [PK_INC] = CC_INC, [PK_DEC] = CC_DEC,
    [PK_SHL] = CC_SHL, [PK_SHR] = CC_SHR, [PK_SAR] = CC_SAR, [PK_MUL] = CC_MUL,
};

/* component locations: a host register (31 = the constant zero), or */
#define LOC_NONE 0xFFu
#define LOC_IMM  0xFEu

enum { N_A = 1, N_B = 2, N_R = 4, N_C = 8 };

typedef struct {
    uint8_t kind, sz;
    uint8_t la, lb, lr, lc;     /* a, b, result, carry in */
    uint8_t _pad[2];
    uint64_t imm;               /* b when lb == LOC_IMM */
} pf_t;

typedef struct {
    uint32_t off;               /* host offset of the access in the block */
    uint32_t _pad;
    uint64_t pc;                /* guest instruction */
    pf_t pf;
} jsite_t;

/* SSE instruction for the interpreter, operands predecoded */
typedef struct {
    uint64_t pc, next;
    int64_t disp;
    uint8_t op, imm, rexb, p66, rep, mod, reg, rm, seg, base, index, scale;
} jsse_t;

/* ---- translation workspace -------------------------------------------------------- */

enum { CS_SIDE, CS_EXIT_NOSPILL, CS_LOOKUP_NOSPILL, CS_SIGNAL, CS_INVAL, CS_STEPEXIT, CS_SSESLOW };

typedef struct {
    uint32_t *at;
    uint32_t *back;
    uint8_t type;
    int16_t insn;
    pf_t pf;
    uint64_t target;
} jcold_t;

typedef struct {
    jit_cache_t *jc;
    a64_t a;
    uint32_t *entry;
    uint64_t block_pc;
    int n, cur;
    bool mt, done, fail, entry_live, dbg_flags;
    pf_t pf;
    int nsites, ncold, nlits;
    ji_t ins[JX_MAX_INSNS];
    jsite_t sites[JX_MAX_SITES];
    jcold_t cold[JX_MAX_COLD];
    struct { uint32_t *at; uint64_t v; } lits[JX_MAX_LITS];
} jx_t;

#define A (&x->a)

#ifdef XL_PROFILE
extern uint64_t g_xb_entries, g_xb_trans, g_xb_uops, g_xb_fallbacks, g_xb_fb_op[512];
#endif

/* ---- helpers called from generated code --------------------------------------------- */

#ifdef XL_PROFILE
/* the guest instructions that went to the interpreter most often */
static struct { uint64_t pc, n; } g_jx_fbpc[512];
static void fb_note(uint64_t pc)
{
    unsigned h = (unsigned)((pc ^ (pc >> 9)) & 511u);
    for (unsigned k = 0; k < 8u; ++k) {
        const unsigned i = (h + k) & 511u;
        if (g_jx_fbpc[i].pc == pc || g_jx_fbpc[i].n == 0u) {
            g_jx_fbpc[i].pc = pc;
            g_jx_fbpc[i].n++;
            return;
        }
    }
}

void x86_jit_prof_dump(void);
void x86_jit_prof_dump(void)
{
    for (int n = 0; n < 12; ++n) {
        int best = -1;
        for (int k = 0; k < 512; ++k) {
            if (g_jx_fbpc[k].n != 0u && (best < 0 || g_jx_fbpc[k].n > g_jx_fbpc[best].n)) best = k;
        }
        if (best < 0) break;
        const uint8_t *p = (const uint8_t *)(uintptr_t)g_jx_fbpc[best].pc;
        xl_log("  pc %lx x%lu: %02x %02x %02x %02x %02x %02x", g_jx_fbpc[best].pc,
               g_jx_fbpc[best].n, p[0], p[1], p[2], p[3], p[4], p[5]);
        g_jx_fbpc[best].n = 0;
    }
}
#endif

static int jit_h_step(gthread_t *t)
{
    x86cpu_t *c = X86(t);
#ifdef XL_PROFILE
    fb_note(c->rip);
    {
        const uint8_t *p = (const uint8_t *)(uintptr_t)c->rip;
        while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0xF0 || *p == 0x64 ||
               *p == 0x65 || *p == 0x2E || *p == 0x3E || *p == 0x67) ++p;
        if ((*p & 0xF0) == 0x40) ++p;
        ++g_xb_fallbacks;
        ++g_xb_fb_op[*p == 0x0F ? 256 + p[1] : *p];
    }
#endif
    return x86_step(t, c);
}

static int jit_h_sse(gthread_t *t, const jsse_t *d)
{
    x86cpu_t *c = X86(t);
#ifdef XL_PROFILE
    ++g_xb_fallbacks;
    ++g_xb_fb_op[256 + d->op];
#endif
    insn_t i;
    i.t = t;
    i.c = c;
    i.start = d->pc;
    i.pc = d->next;
    i.rex = d->rexb;
    i.opsz = (d->rexb & 8u) ? 8 : (d->p66 ? 2 : 4);
    i.adsz32 = 0;
    i.rep = d->rep;
    i.p66 = d->p66;
    i.lock = 0;
    i.seg = d->seg;
    i.modrm = 0;
    i.mod = d->mod;
    i.reg = d->reg;
    i.rm = d->rm;
    i.riprel = 0;
    i.ea_base = d->base == NOREG ? (int8_t)-1 : (int8_t)d->base;
    i.ea_index = d->index == NOREG ? (int8_t)-1 : (int8_t)d->index;
    i.ea_scale = d->scale;
    i.ea_disp = d->disp;
    return x86_sse_body(&i, d->op, d->imm);
}

static uint64_t jit_h_cond(x86cpu_t *c, uint64_t cc)
{
    return x86_cond(c, (unsigned)cc) ? 1u : 0u;
}

static uint64_t jit_h_get_cf(x86cpu_t *c)
{
    return get_cf(c);
}

static uint64_t jit_h_flags(x86cpu_t *c)
{
    return x86_get_flags(c);
}

/* ---- decoder ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    uint8_t rex, p66, seg, riprel;
    uint8_t mod, reg, rm;
} jdec_t;

static void jmodrm(jdec_t *s, ji_t *d)
{
    const uint8_t m = *s->p++;
    s->mod = m >> 6;
    s->reg = (uint8_t)(((m >> 3) & 7u) | ((s->rex & 4u) ? 8u : 0u));
    const uint8_t rm = m & 7u;
    d->base = NOREG;
    d->index = NOREG;
    d->scale = 0;
    d->disp = 0;
    s->riprel = 0;
    if (s->mod == 3u) {
        s->rm = (uint8_t)(rm | ((s->rex & 1u) ? 8u : 0u));
        return;
    }
    s->rm = 0;
    if (rm == 4u) {
        const uint8_t sib = *s->p++;
        const uint8_t base = sib & 7u;
        const uint8_t idx = (uint8_t)(((sib >> 3) & 7u) | ((s->rex & 2u) ? 8u : 0u));
        d->scale = sib >> 6;
        if (idx != 4u) {
            d->index = idx;
        }
        if (base == 5u && s->mod == 0u) {
            int32_t v;
            __builtin_memcpy(&v, s->p, 4);
            s->p += 4;
            d->disp = v;
        } else {
            d->base = (uint8_t)(base | ((s->rex & 1u) ? 8u : 0u));
        }
    } else if (rm == 5u && s->mod == 0u) {
        int32_t v;
        __builtin_memcpy(&v, s->p, 4);
        s->p += 4;
        d->disp = v;
        s->riprel = 1;
        return;
    } else {
        d->base = (uint8_t)(rm | ((s->rex & 1u) ? 8u : 0u));
    }
    if (s->mod == 1u) {
        d->disp += (int8_t)*s->p++;
    } else if (s->mod == 2u) {
        int32_t v;
        __builtin_memcpy(&v, s->p, 4);
        s->p += 4;
        d->disp += v;
    }
}

static uint64_t jimm(jdec_t *s, int n)
{
    uint64_t v = 0;
    switch (n) {
    case 1: v = (uint64_t)(int64_t)(int8_t)s->p[0]; break;
    case 2: { int16_t q; __builtin_memcpy(&q, s->p, 2); v = (uint64_t)(int64_t)q; break; }
    case 4: { int32_t q; __builtin_memcpy(&q, s->p, 4); v = (uint64_t)(int64_t)q; break; }
    default: __builtin_memcpy(&v, s->p, 8); break;
    }
    s->p += n;
    return v;
}

static inline uint8_t greg(unsigned low3, uint8_t rex)
{
    return (uint8_t)(low3 | ((rex & 1u) ? 8u : 0u));
}

/* Decode the instruction at p into d. Returns its length, or 0 when the
 * translator has no native form (it then runs through the interpreter). */
static int jdecode(const uint8_t *p0, uint64_t pc, ji_t *d)
{
    jdec_t s;
    s.p = p0;
    s.rex = 0;
    s.p66 = 0;
    s.seg = 0;
    s.riprel = 0;
    s.mod = s.reg = s.rm = 0;
    uint8_t rep = 0, lock = 0;
    for (int k = 0; k < 6; ++k) {
        const uint8_t b = *s.p;
        if (b == 0x66) { s.p66 = 1; ++s.p; continue; }
        if (b == 0x64) { s.seg = 4; ++s.p; continue; }
        if (b == 0x65) { s.seg = 5; ++s.p; continue; }
        if (b == 0x2E || b == 0x3E) { ++s.p; continue; }
        if (b == 0xF3 || b == 0xF2) { rep = b; ++s.p; continue; }
        if (b == 0xF0) { lock = 1; ++s.p; continue; }
        if (b == 0x67 || b == 0x36 || b == 0x26) return 0;
        break;
    }
    if ((*s.p & 0xF0u) == 0x40u) {
        s.rex = *s.p++;
    }
    const uint8_t op = *s.p++;
    const int sz = (s.rex & 8u) ? 8 : (s.p66 ? 2 : 4);
    memset(d, 0, sizeof(*d));
    d->pc = pc;
    d->sz = (uint8_t)sz;
    d->rex = s.rex != 0u;
    d->seg = s.seg;
    d->base = NOREG;
    d->index = NOREG;
    d->dst = NOREG;
    d->src = NOREG;

#define CUR_PC() (pc + (uint64_t)(s.p - p0))

    if (op == 0x0F) {
        const uint8_t o2 = s.p[0];
        const bool sse = (o2 >= 0x10 && o2 <= 0x17) || (o2 >= 0x28 && o2 <= 0x2F) ||
                         (o2 >= 0x50 && o2 <= 0x7F && o2 != 0x77 && (o2 < 0x78 || o2 > 0x7B)) ||
                         (o2 >= 0xC2 && o2 <= 0xC6) || (o2 >= 0xD0 && o2 <= 0xFE);
        if (sse) {
            ++s.p;
            jmodrm(&s, d);
            d->kind = K_SSE;
            d->rexb = s.rex;
            d->p66 = s.p66;
            d->rep = rep;
            d->mod = s.mod;
            d->dst = s.reg;
            d->src = s.rm;
            d->op = o2;
            d->imm = x86_sse_has_imm8(o2) ? *s.p++ : 0u;
            goto done;
        }
    }

    if (lock) {
        /* LOCK: the read-modify-write forms that have an atomic translation */
        if (rep || (g_jx_dbg & JXD_ATOMIC)) return 0;
        if (op < 0x40 && (op & 7u) <= 1u && (op >> 3) != 7u) {
            jmodrm(&s, d);
            if (s.mod == 3u) return 0;
            d->kind = K_ATOM_ALU; d->op = op >> 3; d->src = s.reg;
            d->sz = (uint8_t)((op & 1u) ? sz : 1);
            goto done;
        }
        if (op == 0x80 || op == 0x81 || op == 0x83) {
            d->sz = (uint8_t)(op == 0x80 ? 1 : sz);
            jmodrm(&s, d);
            if (s.mod == 3u || (s.reg & 7u) == 7u) return 0;
            d->kind = K_ATOM_ALU; d->op = s.reg & 7u;
            d->imm = jimm(&s, op == 0x81 ? (d->sz == 2 ? 2 : 4) : 1);
            goto done;
        }
        if (op == 0xFE || op == 0xFF) {
            d->sz = (uint8_t)(op == 0xFE ? 1 : sz);
            jmodrm(&s, d);
            if (s.mod == 3u || (s.reg & 7u) > 1u) return 0;
            d->kind = K_ATOM_INCDEC; d->op = s.reg & 1u;
            goto done;
        }
        if (op == 0x86 || op == 0x87) {
            /* XCHG is atomic with or without the prefix: below */
        } else if (op == 0x0F && (s.p[0] == 0xB0 || s.p[0] == 0xB1 || s.p[0] == 0xC0 || s.p[0] == 0xC1)) {
            /* below */
        } else {
            return 0;
        }
    }

    if (rep) {
        /* F3 only in its NOP-like uses -- ENDBR64, PAUSE, REP RET -- and as
         * REP BSF/BSR, which compilers emit for TZCNT/LZCNT: CPUID reports
         * no BMI1/ABM, so they are BSF/BSR, as on such CPUs */
        if (rep == 0xF3) {
            if (op == 0x0F && s.p[0] == 0x1E && s.p[1] == 0xFA) { s.p += 2; d->kind = K_NOP; goto done; }
            if (op == 0x90) { d->kind = K_NOP; goto done; }
            if (op == 0xC3) { d->kind = K_RET; d->imm = 0; d->ends = 1; goto done; }
            if (op == 0x0F && (s.p[0] == 0xBC || s.p[0] == 0xBD) && sz >= 4) {
                const uint8_t o2 = *s.p++;
                jmodrm(&s, d);
                d->dst = s.reg; d->op = o2 == 0xBD;
                if (s.mod == 3u) { d->kind = K_BSX_R; d->src = s.rm; } else d->kind = K_BSX_M;
                goto done;
            }
        }
        return 0;
    }

    if (op < 0x40 && (op & 7u) < 6u) {
        const unsigned aop = op >> 3;
        const int sz1 = (op & 1u) ? sz : 1;
        d->sz = (uint8_t)sz1;
        d->op = (uint8_t)aop;
        switch (op & 7u) {
        case 0: case 1:
            jmodrm(&s, d);
            if (s.mod == 3u) { d->kind = K_ALU_RR; d->dst = s.rm; d->src = s.reg; }
            else { d->kind = K_ALU_MR; d->src = s.reg; }
            break;
        case 2: case 3:
            jmodrm(&s, d);
            d->dst = s.reg;
            if (s.mod == 3u) { d->kind = K_ALU_RR; d->src = s.rm; }
            else d->kind = K_ALU_RM;
            break;
        case 4:
            d->kind = K_ALU_RI; d->dst = RAX; d->imm = jimm(&s, 1);
            break;
        default:
            d->kind = K_ALU_RI; d->dst = RAX; d->imm = jimm(&s, sz1 == 2 ? 2 : 4);
            break;
        }
        goto done;
    }

    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (s.p66) return 0;
        d->kind = K_PUSH_R; d->src = greg(op & 7u, s.rex);
        goto done;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (s.p66) return 0;
        d->kind = K_POP_R; d->dst = greg(op & 7u, s.rex);
        goto done;
    case 0x63:
        jmodrm(&s, d);
        d->dst = s.reg; d->op = 4; d->imm = (s.rex & 8u) ? 1u : 0u;
        if (s.mod == 3u) { d->kind = K_MOVX_R; d->src = s.rm; } else d->kind = K_MOVX_M;
        goto done;
    case 0x68: case 0x6A:
        if (s.p66) return 0;
        d->kind = K_PUSH_I; d->imm = jimm(&s, op == 0x6A ? 1 : 4);
        goto done;
    case 0x69: case 0x6B:
        jmodrm(&s, d);
        d->dst = s.reg;
        d->imm = jimm(&s, op == 0x6B ? 1 : (sz == 2 ? 2 : 4));
        if (s.mod == 3u) { d->kind = K_IMUL_RRI; d->src = s.rm; } else d->kind = K_IMUL_RMI;
        goto done;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        const int64_t rel = (int8_t)*s.p++;
        d->kind = K_JCC; d->op = op & 15u;
        d->imm = CUR_PC() + (uint64_t)rel;
        goto done;
    }
    case 0x80: case 0x81: case 0x83: {
        const int sz1 = op == 0x80 ? 1 : sz;
        d->sz = (uint8_t)sz1;
        jmodrm(&s, d);
        d->op = s.reg & 7u;
        d->imm = jimm(&s, op == 0x81 ? (sz1 == 2 ? 2 : 4) : 1);
        if (s.mod == 3u) { d->kind = K_ALU_RI; d->dst = s.rm; } else d->kind = K_ALU_MI;
        goto done;
    }
    case 0x84: case 0x85:
        d->sz = (uint8_t)(op == 0x84 ? 1 : sz);
        d->op = 8;
        jmodrm(&s, d);
        if (s.mod == 3u) { d->kind = K_ALU_RR; d->dst = s.rm; d->src = s.reg; }
        else { d->kind = K_ALU_MR; d->src = s.reg; }
        goto done;
    case 0x86: case 0x87:
        d->sz = (uint8_t)(op == 0x86 ? 1 : sz);
        jmodrm(&s, d);
        if (s.mod != 3u) {                  /* memory: implicitly locked */
            if (g_jx_dbg & JXD_ATOMIC) return 0;
            d->kind = K_XCHG_M; d->src = s.reg;
            goto done;
        }
        d->kind = K_XCHG_RR; d->dst = s.rm; d->src = s.reg;
        goto done;
    case 0x88: case 0x89: case 0x8A: case 0x8B:
        d->sz = (uint8_t)((op & 1u) ? sz : 1);
        jmodrm(&s, d);
        if (op <= 0x89) {
            if (s.mod == 3u) { d->kind = K_MOV_RR; d->dst = s.rm; d->src = s.reg; }
            else { d->kind = K_MOV_ST; d->src = s.reg; }
        } else {
            d->dst = s.reg;
            if (s.mod == 3u) { d->kind = K_MOV_RR; d->src = s.rm; } else d->kind = K_MOV_LD;
        }
        goto done;
    case 0x8D:
        jmodrm(&s, d);
        if (s.mod == 3u) return 0;
        d->kind = K_LEA; d->dst = s.reg; d->seg = 0;
        goto done;
    case 0x90:
        if (s.rex & 1u) { d->kind = K_XCHG_RR; d->dst = RAX; d->src = R8; goto done; }
        d->kind = K_NOP;
        goto done;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        d->kind = K_XCHG_RR; d->dst = RAX; d->src = greg(op & 7u, s.rex);
        goto done;
    case 0x98: case 0x99:
        d->kind = K_CDQ; d->op = op & 1u;
        goto done;
    case 0xA8: case 0xA9:
        d->sz = (uint8_t)(op == 0xA8 ? 1 : sz);
        d->kind = K_ALU_RI; d->op = 8; d->dst = RAX;
        d->imm = jimm(&s, op == 0xA8 ? 1 : (sz == 2 ? 2 : 4));
        goto done;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        d->sz = 1;
        d->kind = K_MOV_RI; d->dst = greg(op & 7u, s.rex);
        d->imm = (uint8_t)jimm(&s, 1);
        goto done;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        d->kind = K_MOV_RI; d->dst = greg(op & 7u, s.rex);
        d->imm = jimm(&s, sz);
        if (sz == 4) d->imm = (uint32_t)d->imm;
        if (sz == 2) d->imm = (uint16_t)d->imm;
        goto done;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const int sz1 = (op & 1u) ? sz : 1;
        d->sz = (uint8_t)sz1;
        jmodrm(&s, d);
        if (s.mod != 3u) return 0;
        const unsigned kind = s.reg & 7u;
        if (kind == 2u || kind == 3u) return 0;               /* RCL/RCR */
        if (sz1 < 4 && kind < 2u) return 0;                   /* narrow rotates */
        d->dst = s.rm; d->op = (uint8_t)kind;
        if (op >= 0xD2) {
            d->kind = K_SHIFT_RC;
        } else {
            const unsigned cnt = (op <= 0xC1 ? (unsigned)*s.p++ : 1u) & (sz1 == 8 ? 63u : 31u);
            d->kind = cnt == 0u ? K_NOP : K_SHIFT_RI;
            d->imm = cnt;
        }
        goto done;
    }
    case 0xC2:
        d->kind = K_RET; d->imm = (uint16_t)jimm(&s, 2); d->ends = 1;
        goto done;
    case 0xC3:
        d->kind = K_RET; d->imm = 0; d->ends = 1;
        goto done;
    case 0xC6: case 0xC7: {
        const int sz1 = op == 0xC6 ? 1 : sz;
        d->sz = (uint8_t)sz1;
        jmodrm(&s, d);
        if ((s.reg & 7u) != 0u) return 0;
        d->imm = jimm(&s, sz1 == 8 ? 4 : sz1);
        if (s.mod == 3u) { d->kind = K_MOV_RI; d->dst = s.rm; } else d->kind = K_MOV_MI;
        if (sz1 == 4) d->imm = (uint32_t)d->imm;
        goto done;
    }
    case 0xC9:
        if (s.p66) return 0;
        d->kind = K_LEAVE;
        goto done;
    case 0xE8: case 0xE9: case 0xEB: {
        const int64_t rel = op == 0xEB ? (int8_t)*s.p++ : (int64_t)jimm(&s, 4);
        d->kind = op == 0xE8 ? K_CALL : K_JMP;
        d->imm = CUR_PC() + (uint64_t)rel;
        d->ends = 1;
        goto done;
    }
    case 0xF6: case 0xF7: {
        const int sz1 = op == 0xF6 ? 1 : sz;
        d->sz = (uint8_t)sz1;
        jmodrm(&s, d);
        const unsigned sub = s.reg & 7u;
        if (sub <= 1u) {
            d->op = 8;
            d->imm = jimm(&s, sz1 == 1 ? 1 : (sz1 == 2 ? 2 : 4));
            if (s.mod == 3u) { d->kind = K_ALU_RI; d->dst = s.rm; } else d->kind = K_ALU_MI;
            goto done;
        }
        if (sub == 2u) {
            if (s.mod == 3u) { d->kind = K_NOT_R; d->dst = s.rm; } else d->kind = K_NOT_M;
            goto done;
        }
        if (sub == 3u) {
            if (s.mod == 3u) { d->kind = K_NEG_R; d->dst = s.rm; } else d->kind = K_NEG_M;
            goto done;
        }
        if (sz1 < 4) return 0;
        d->op = (uint8_t)sub;
        if (sub <= 5u) {
            if (s.mod == 3u) { d->kind = K_MUL1_R; d->src = s.rm; } else d->kind = K_MUL1_M;
        } else {
            if (s.mod == 3u) { d->kind = K_DIV1_R; d->src = s.rm; } else d->kind = K_DIV1_M;
        }
        goto done;
    }
    case 0xFE: case 0xFF: {
        const int sz1 = op == 0xFE ? 1 : sz;
        d->sz = (uint8_t)sz1;
        jmodrm(&s, d);
        const unsigned sub = s.reg & 7u;
        if (sub <= 1u) {
            d->op = (uint8_t)sub;
            if (s.mod == 3u) { d->kind = K_INCDEC_R; d->dst = s.rm; } else d->kind = K_INCDEC_M;
            goto done;
        }
        if (op == 0xFE || s.p66) return 0;
        if (sub == 6u) {
            if (s.mod == 3u) {
                d->kind = K_PUSH_R; d->src = s.rm;
            } else {
                d->kind = K_PUSH_M; d->sz = 8;
            }
            goto done;
        }
        if (sub == 2u) {
            d->sz = 8;
            if (s.mod == 3u) { d->kind = K_CALL_R; d->src = s.rm; } else d->kind = K_CALL_M;
            d->ends = 1;
            goto done;
        }
        if (sub == 4u) {
            d->sz = 8;
            if (s.mod == 3u) { d->kind = K_JMP_R; d->src = s.rm; } else d->kind = K_JMP_M;
            d->ends = 1;
            goto done;
        }
        return 0;
    }
    case 0x0F: {
        const uint8_t o2 = *s.p++;
        if (o2 >= 0x80 && o2 <= 0x8F) {
            const int64_t rel = (int64_t)jimm(&s, 4);
            d->kind = K_JCC; d->op = o2 & 15u;
            d->imm = CUR_PC() + (uint64_t)rel;
            goto done;
        }
        if (o2 >= 0x40 && o2 <= 0x4F) {
            jmodrm(&s, d);
            d->dst = s.reg; d->op = o2 & 15u;
            if (s.mod == 3u) { d->kind = K_CMOV_R; d->src = s.rm; } else d->kind = K_CMOV_M;
            goto done;
        }
        if (o2 >= 0x90 && o2 <= 0x9F) {
            jmodrm(&s, d);
            d->sz = 1;
            d->op = o2 & 15u;
            if (s.mod != 3u) {
                d->kind = K_SETCC_M;
                goto done;
            }
            d->kind = K_SETCC_R; d->dst = s.rm;
            goto done;
        }
        if (o2 >= 0xC8 && o2 <= 0xCF) {
            if (sz < 4) return 0;
            d->kind = K_BSWAP; d->dst = greg(o2 & 7u, s.rex);
            goto done;
        }
        switch (o2) {
        case 0x05:
            d->kind = K_SYSCALL; d->ends = 1;
            goto done;
        case 0xB0: case 0xB1: case 0xC0: case 0xC1:
            d->sz = (uint8_t)((o2 & 1u) ? sz : 1);
            jmodrm(&s, d);
            if (s.mod == 3u || (g_jx_dbg & JXD_ATOMIC)) return 0;
            d->kind = (o2 & 0xF0u) == 0xB0u ? K_CMPXCHG : K_XADD;
            d->src = s.reg;
            goto done;
        case 0x1E: case 0x1F: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
            jmodrm(&s, d);
            d->kind = K_NOP;
            d->base = d->index = NOREG;
            goto done;
        case 0xA3: case 0xAB: case 0xB3: case 0xBB:
            if (sz < 4) return 0;
            jmodrm(&s, d);
            if (s.mod != 3u) return 0;
            d->kind = K_BT_RR; d->dst = s.rm; d->src = s.reg; d->op = (o2 >> 3) & 3u;
            goto done;
        case 0xBA: {
            if (sz < 4) return 0;
            jmodrm(&s, d);
            const unsigned sub = s.reg & 7u;
            if (s.mod != 3u || sub < 4u) return 0;
            d->kind = K_BT_RI; d->dst = s.rm; d->op = (uint8_t)(sub - 4u);
            d->imm = *s.p++;
            goto done;
        }
        case 0xBC: case 0xBD:
            if (sz < 4) return 0;
            jmodrm(&s, d);
            d->dst = s.reg; d->op = o2 == 0xBD;
            if (s.mod == 3u) { d->kind = K_BSX_R; d->src = s.rm; } else d->kind = K_BSX_M;
            goto done;
        case 0xAF:
            jmodrm(&s, d);
            d->dst = s.reg;
            if (s.mod == 3u) { d->kind = K_IMUL_RR; d->src = s.rm; } else d->kind = K_IMUL_RM;
            goto done;
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:
            jmodrm(&s, d);
            d->dst = s.reg;
            d->op = (o2 & 1u) ? 2 : 1;
            d->imm = o2 >= 0xBE ? 1u : 0u;
            if (s.mod == 3u) { d->kind = K_MOVX_R; d->src = s.rm; } else d->kind = K_MOVX_M;
            goto done;
        default:
            return 0;
        }
    }
    default:
        return 0;
    }

done:
    {
        const int len = (int)(s.p - p0);
        d->next = pc + (uint64_t)len;
        if (s.riprel) {
            d->disp += (int64_t)d->next;
        }
        return len;
    }
#undef CUR_PC
}

/* ---- SSE: what runs natively ------------------------------------------------------------ */

enum {
    SP_HELPER = 0,
    SP_MOV16_LD, SP_MOV16_ST, SP_MOVS_LD, SP_MOVS_ST, SP_MOVLP_LD, SP_MOVLP_ST, SP_MOVHP_LD,
    SP_MOVHP_ST, SP_MOVDDUP, SP_UNPCK, SP_CVTSI2F, SP_CVTF2SI, SP_COMIS, SP_MOVMSK, SP_FSQRT,
    SP_FLOGIC, SP_FARITH, SP_CVTFF, SP_CVTDQ2PS, SP_FMINMAX, SP_PACK, SP_IARITH, SP_MULH,
    SP_PMULUDQ, SP_PMADDWD, SP_PSADBW, SP_MOVD_TOX, SP_MOVD_FROMX, SP_MOVQ_TOX, SP_MOVQ_FROMX,
    SP_PSHUF, SP_SHIFTI, SP_CMPF, SP_PINSRW, SP_PEXTRW, SP_SHUFP, SP_PMOVMSKB, SP_CVTDQ2PD,
};

/* mandatory prefix: 0 none, 1 66, 2 F2, 3 F3 */
static inline unsigned sse_pfx(const ji_t *d)
{
    return d->rep == 0xF3 ? 3u : d->rep == 0xF2 ? 2u : d->p66 ? 1u : 0u;
}

/* integer SIMD ops with a single NEON equivalent: opcode, element size */
static bool sse_iop(uint8_t op, uint32_t *opc, unsigned *size)
{
    switch (op) {
    case 0xD4: *opc = A64_V_ADD; *size = 3; return true;
    case 0xFC: *opc = A64_V_ADD; *size = 0; return true;
    case 0xFD: *opc = A64_V_ADD; *size = 1; return true;
    case 0xFE: *opc = A64_V_ADD; *size = 2; return true;
    case 0xF8: *opc = A64_V_SUB; *size = 0; return true;
    case 0xF9: *opc = A64_V_SUB; *size = 1; return true;
    case 0xFA: *opc = A64_V_SUB; *size = 2; return true;
    case 0xFB: *opc = A64_V_SUB; *size = 3; return true;
    case 0x74: *opc = A64_V_CMEQ; *size = 0; return true;
    case 0x75: *opc = A64_V_CMEQ; *size = 1; return true;
    case 0x76: *opc = A64_V_CMEQ; *size = 2; return true;
    case 0x64: *opc = A64_V_CMGT; *size = 0; return true;
    case 0x65: *opc = A64_V_CMGT; *size = 1; return true;
    case 0x66: *opc = A64_V_CMGT; *size = 2; return true;
    case 0xDA: *opc = A64_V_UMIN; *size = 0; return true;
    case 0xDE: *opc = A64_V_UMAX; *size = 0; return true;
    case 0xEA: *opc = A64_V_SMIN; *size = 1; return true;
    case 0xEE: *opc = A64_V_SMAX; *size = 1; return true;
    case 0xE0: *opc = A64_V_URHADD; *size = 0; return true;
    case 0xE3: *opc = A64_V_URHADD; *size = 1; return true;
    case 0xD8: *opc = A64_V_UQSUB; *size = 0; return true;
    case 0xD9: *opc = A64_V_UQSUB; *size = 1; return true;
    case 0xDC: *opc = A64_V_UQADD; *size = 0; return true;
    case 0xDD: *opc = A64_V_UQADD; *size = 1; return true;
    case 0xE8: *opc = A64_V_SQSUB; *size = 0; return true;
    case 0xE9: *opc = A64_V_SQSUB; *size = 1; return true;
    case 0xEC: *opc = A64_V_SQADD; *size = 0; return true;
    case 0xED: *opc = A64_V_SQADD; *size = 1; return true;
    case 0xD5: *opc = A64_V_MUL; *size = 1; return true;
    case 0x60: *opc = A64_V_ZIP1; *size = 0; return true;
    case 0x61: *opc = A64_V_ZIP1; *size = 1; return true;
    case 0x62: *opc = A64_V_ZIP1; *size = 2; return true;
    case 0x6C: *opc = A64_V_ZIP1; *size = 3; return true;
    case 0x68: *opc = A64_V_ZIP2; *size = 0; return true;
    case 0x69: *opc = A64_V_ZIP2; *size = 1; return true;
    case 0x6A: *opc = A64_V_ZIP2; *size = 2; return true;
    case 0x6D: *opc = A64_V_ZIP2; *size = 3; return true;
    default: return false;
    }
}

static unsigned sse_plan(const ji_t *d)
{
    const unsigned pf = sse_pfx(d), op = d->op;
    const bool reg = d->mod == 3u;
    uint32_t opc;
    unsigned size;
    switch (op) {
    case 0x10: return pf <= 1u ? SP_MOV16_LD : SP_MOVS_LD;
    case 0x11: return pf <= 1u ? SP_MOV16_ST : SP_MOVS_ST;
    case 0x28: return pf <= 1u ? SP_MOV16_LD : SP_HELPER;
    case 0x29: return pf <= 1u ? SP_MOV16_ST : SP_HELPER;
    case 0x2B: return pf <= 1u && !reg ? SP_MOV16_ST : SP_HELPER;
    case 0x6F: return pf == 1u || pf == 3u ? SP_MOV16_LD : SP_HELPER;
    case 0x7F: return pf == 1u || pf == 3u ? SP_MOV16_ST : SP_HELPER;
    case 0xE7: return pf == 1u && !reg ? SP_MOV16_ST : SP_HELPER;
    case 0xF0: return pf == 2u && !reg ? SP_MOV16_LD : SP_HELPER;
    case 0x12:
        if (pf == 2u) return SP_MOVDDUP;
        return (pf == 0u || (pf == 1u && !reg)) ? SP_MOVLP_LD : SP_HELPER;
    case 0x13: return pf <= 1u && !reg ? SP_MOVLP_ST : SP_HELPER;
    case 0x16: return (pf == 0u || (pf == 1u && !reg)) ? SP_MOVHP_LD : SP_HELPER;
    case 0x17: return pf <= 1u && !reg ? SP_MOVHP_ST : SP_HELPER;
    case 0x14: case 0x15: return pf <= 1u ? SP_UNPCK : SP_HELPER;
    case 0x2A: return pf >= 2u ? SP_CVTSI2F : SP_HELPER;
    case 0x2C: case 0x2D: return pf >= 2u ? SP_CVTF2SI : SP_HELPER;
    case 0x2E: case 0x2F: return pf <= 1u ? SP_COMIS : SP_HELPER;
    case 0x50: return pf <= 1u && reg ? SP_MOVMSK : SP_HELPER;
    case 0x51: return SP_FSQRT;
    case 0x54: case 0x55: case 0x56: case 0x57: return pf <= 1u ? SP_FLOGIC : SP_HELPER;
    case 0xDB: case 0xDF: case 0xEB: case 0xEF: return pf == 1u ? SP_FLOGIC : SP_HELPER;
    case 0x58: case 0x59: case 0x5C: case 0x5E: return SP_FARITH;
    case 0x5A: return pf >= 2u ? SP_CVTFF : SP_HELPER;
    case 0x5B: return pf == 0u ? SP_CVTDQ2PS : SP_HELPER;
    case 0x5D: case 0x5F: return SP_FMINMAX;
    case 0x63: case 0x67: case 0x6B: return pf == 1u ? SP_PACK : SP_HELPER;
    case 0xE4: case 0xE5: return pf == 1u ? SP_MULH : SP_HELPER;
    case 0xF4: return pf == 1u ? SP_PMULUDQ : SP_HELPER;
    case 0xF5: return pf == 1u ? SP_PMADDWD : SP_HELPER;
    case 0xF6: return pf == 1u ? SP_PSADBW : SP_HELPER;
    case 0x6E: return pf == 1u ? SP_MOVD_TOX : SP_HELPER;
    case 0x7E: return pf == 1u ? SP_MOVD_FROMX : pf == 3u ? SP_MOVQ_TOX : SP_HELPER;
    case 0xD6: return pf == 1u ? SP_MOVQ_FROMX : SP_HELPER;
    case 0x70: return pf >= 1u ? SP_PSHUF : SP_HELPER;
    case 0x71: case 0x72: case 0x73: {
        if (pf != 1u || !reg) return SP_HELPER;
        const unsigned sub = d->dst & 7u;
        if (op == 0x73) return (sub == 2u || sub == 3u || sub == 6u || sub == 7u) ? SP_SHIFTI : SP_HELPER;
        return (sub == 2u || sub == 4u || sub == 6u) ? SP_SHIFTI : SP_HELPER;
    }
    case 0xC2: return SP_CMPF;
    case 0xC4: return pf == 1u ? SP_PINSRW : SP_HELPER;
    case 0xC5: return pf == 1u && reg ? SP_PEXTRW : SP_HELPER;
    case 0xC6: return pf <= 1u ? SP_SHUFP : SP_HELPER;
    case 0xD7: return pf == 1u && reg ? SP_PMOVMSKB : SP_HELPER;
    case 0xE6: return pf == 3u ? SP_CVTDQ2PD : SP_HELPER;
    default:
        return pf == 1u && sse_iop((uint8_t)op, &opc, &size) ? SP_IARITH : SP_HELPER;
    }
}

/* the general register an SSE instruction writes, or NOREG */
static unsigned sse_gpr_dst(const ji_t *d)
{
    switch (d->plan) {
    case SP_CVTF2SI: case SP_MOVMSK: case SP_PEXTRW: case SP_PMOVMSKB:
        return d->dst;
    case SP_MOVD_FROMX:
        return d->mod == 3u ? d->src : NOREG;
    default:
        return NOREG;
    }
}

/* ---- analysis -------------------------------------------------------------------------- */

static inline bool is_hi8(unsigned g, unsigned sz, bool rex)
{
    return sz == 1u && !rex && g >= 4u && g < 8u;
}

static inline uint16_t regbit(unsigned g, unsigned sz, bool rex)
{
    if (g >= 16u) return 0;
    return (uint16_t)(1u << (is_hi8(g, sz, rex) ? g - 4u : g));
}

static void set_effects(ji_t *d)
{
    const uint16_t D = regbit(d->dst, d->sz, d->rex != 0u);
    d->fr = FR_NONE;
    d->fw = FW_NONE;
    d->wmask = 0;
    switch (d->kind) {
    case K_ALU_RR: case K_ALU_RI: case K_ALU_RM:
        d->fw = FW_FULL;
        if (d->op == 2u || d->op == 3u) d->fr = FR_CF;
        if (d->op != 7u && d->op != 8u) d->wmask = D;
        break;
    case K_ALU_MR: case K_ALU_MI:
        d->fw = FW_FULL;
        if (d->op == 2u || d->op == 3u) d->fr = FR_CF;
        break;
    case K_MOV_RR: case K_MOV_RI: case K_MOV_LD: case K_LEA: case K_MOVX_R: case K_MOVX_M:
    case K_BSWAP:
        d->wmask = D;
        break;
    case K_PUSH_R: case K_PUSH_I: case K_CALL: case K_CALL_R: case K_CALL_M: case K_RET:
        d->wmask = 1u << RSP;
        break;
    case K_POP_R:
        d->wmask = (uint16_t)((1u << RSP) | (1u << d->dst));
        break;
    case K_INCDEC_R:
        d->fw = FW_FULL; d->fr = FR_CF; d->wmask = D;
        break;
    case K_INCDEC_M:
        d->fw = FW_FULL; d->fr = FR_CF;
        break;
    case K_NOT_R:
        d->wmask = D;
        break;
    case K_NEG_R:
        d->fw = FW_FULL; d->wmask = D;
        break;
    case K_NEG_M:
        d->fw = FW_FULL;
        break;
    case K_SHIFT_RI:
        d->fw = d->op < 2u ? FW_PASS : FW_FULL;
        d->wmask = D;
        break;
    case K_SHIFT_RC: case K_BSX_R: case K_BSX_M:
        d->fw = FW_PASS; d->wmask = D;
        break;
    case K_BT_RR: case K_BT_RI:
        d->fw = FW_PASS; d->wmask = d->op != 0u ? D : 0;
        break;
    case K_IMUL_RR: case K_IMUL_RRI: case K_IMUL_RM: case K_IMUL_RMI:
        d->fw = FW_FULL; d->wmask = D;
        break;
    case K_MUL1_R: case K_MUL1_M:
        d->fw = FW_FULL; d->wmask = (1u << RAX) | (1u << RDX);
        break;
    case K_DIV1_R: case K_DIV1_M:
        d->wmask = (1u << RAX) | (1u << RDX);
        break;
    case K_CDQ:
        d->wmask = (uint16_t)(1u << (d->op ? RDX : RAX));
        break;
    case K_SETCC_R: case K_CMOV_R: case K_CMOV_M:
        d->fr = FR_COND; d->wmask = D;
        break;
    case K_SETCC_M:
        d->fr = FR_COND;
        break;
    case K_PUSH_M:
        d->wmask = 1u << RSP;
        break;
    case K_ATOM_ALU:
        d->fw = FW_FULL;
        if (d->op == 2u || d->op == 3u) d->fr = FR_CF;
        break;
    case K_ATOM_INCDEC:
        d->fw = FW_FULL; d->fr = FR_CF;
        break;
    case K_CMPXCHG:
        d->fw = FW_FULL; d->wmask = 1u << RAX;
        break;
    case K_XADD:
        d->fw = FW_FULL; d->wmask = regbit(d->src, d->sz, d->rex != 0u);
        break;
    case K_XCHG_M:
        d->wmask = regbit(d->src, d->sz, d->rex != 0u);
        break;
    case K_XCHG_RR:
        d->wmask = (uint16_t)(D | regbit(d->src, d->sz, d->rex != 0u));
        break;
    case K_LEAVE:
        d->wmask = (1u << RSP) | (1u << RBP);
        break;
    case K_JCC:
        d->fr = FR_COND;
        break;
    case K_SSE:
        d->plan = (g_jx_dbg & JXD_NOSSE) ? (uint8_t)SP_HELPER : (uint8_t)sse_plan(d);
        if ((int)d->plan >= g_jx_sp_lo && (int)d->plan <= g_jx_sp_hi) d->plan = SP_HELPER;
        if (d->plan == SP_HELPER) {
            d->fr = FR_ALL; d->fw = FW_FULL; d->wmask = 0xFFFFu;
        } else {
            const unsigned g = sse_gpr_dst(d);
            d->wmask = g < 16u ? (uint16_t)(1u << g) : 0u;
            if (d->plan == SP_COMIS) d->fw = FW_FULL;
        }
        break;
    case K_SYSCALL: case K_FALLBACK:
        d->fr = FR_ALL; d->fw = FW_FULL; d->wmask = 0xFFFFu;
        break;
    default:
        break;
    }
}

static unsigned pk_of(const ji_t *d)
{
    switch (d->kind) {
    case K_ALU_RR: case K_ALU_RI: case K_ALU_RM: case K_ALU_MR: case K_ALU_MI:
        switch (d->op) {
        case 0: return PK_ADD;
        case 2: return PK_ADC;
        case 3: return PK_SBB;
        case 5: case 7: return PK_SUB;
        case 8: return PK_TEST;
        default: return PK_LOGIC;
        }
    case K_ATOM_ALU:
        switch (d->op) {
        case 0: return PK_ADD;
        case 2: return PK_ADC;
        case 3: return PK_SBB;
        case 5: return PK_SUB;
        default: return PK_LOGIC;
        }
    case K_INCDEC_R: case K_INCDEC_M: case K_ATOM_INCDEC: return d->op ? PK_DEC : PK_INC;
    case K_NEG_R: case K_NEG_M: case K_CMPXCHG: return PK_SUB;
    case K_XADD: return PK_ADD;
    case K_SHIFT_RI:
        if (d->op < 2u) return d->fw == FW_FULL ? PK_EFL : PK_NONE;    /* live ROL/ROR */
        return d->op == 5u ? PK_SHR : d->op == 7u ? PK_SAR : PK_SHL;
    case K_BT_RR: case K_BT_RI: return d->fw == FW_FULL ? PK_EFL : PK_NONE;
    case K_BSX_R: case K_BSX_M: return d->fw == FW_FULL ? PK_LOGIC : PK_NONE;
    case K_IMUL_RR: case K_IMUL_RRI: case K_IMUL_RM: case K_IMUL_RMI:
    case K_MUL1_R: case K_MUL1_M: return PK_MUL;
    case K_SSE: return d->plan == SP_COMIS ? PK_EFL : PK_NONE;
    default: return PK_NONE;
    }
}

static unsigned cc_need(unsigned pk, unsigned cc)
{
    const unsigned c2 = cc >> 1;
    const bool zsp = c2 == 2u || c2 == 4u || c2 == 5u;
    switch (pk) {
    case PK_ADD: case PK_SUB: return zsp ? N_R : (N_A | N_B);
    case PK_LOGIC: return N_R;
    case PK_TEST: return N_A | N_B;
    case PK_INC: case PK_DEC: return (c2 == 1u || c2 == 3u) ? (N_R | N_C) : N_R;
    case PK_SHL: case PK_SHR: case PK_SAR: return zsp ? N_R : c2 == 1u ? N_A : (N_A | N_R);
    case PK_MUL: return zsp ? N_R : c2 <= 1u ? N_B : (N_R | N_B);
    case PK_EFL: return N_R;
    default: return N_A | N_B | N_C;
    }
}

static unsigned all_need(unsigned pk)
{
    switch (pk) {
    case PK_ADD: case PK_SUB: case PK_TEST: return N_A | N_B;
    case PK_LOGIC: return N_R;
    case PK_INC: case PK_DEC: return N_R | N_C;
    case PK_SHL: case PK_SHR: case PK_SAR: return N_A | N_R;
    case PK_MUL: return N_R | N_B;
    case PK_EFL: return N_R;
    default: return N_A | N_B | N_C;
    }
}

static unsigned cf_need(unsigned pk)
{
    switch (pk) {
    case PK_ADD: case PK_SUB: return N_A | N_B;
    case PK_LOGIC: case PK_TEST: return 0;
    case PK_INC: case PK_DEC: return N_C;
    case PK_SHL: case PK_SHR: case PK_SAR: return N_A;
    case PK_MUL: return N_B;
    case PK_EFL: return N_R;
    default: return N_A | N_B | N_C;
    }
}

static void analyze(jx_t *x)
{
    /* liveness, backwards; flags are live at the end of the block */
    bool live = true;
    for (int i = x->n - 1; i >= 0; --i) {
        ji_t *d = &x->ins[i];
        d->live = live;
        if (d->fw == FW_PASS) {
            if (!live) {
                continue;   /* no flag effect at all */
            }
            /* its flags matter */
            if (d->kind == K_BSX_R || d->kind == K_BSX_M) {
                d->fw = FW_FULL;                /* ZF only defined: as a logic op */
            } else if (d->kind == K_SHIFT_RI || d->kind == K_BT_RR || d->kind == K_BT_RI) {
                d->fr = FR_ALL;                 /* CF (and OF) over the old flags */
                d->fw = FW_FULL;
            } else {
                d->kind = K_FALLBACK;           /* a shift by CL */
                d->fr = FR_ALL;
                d->fw = FW_FULL;
                d->wmask = 0xFFFFu;
            }
        }
        if (d->fr == FR_ALL) {
            live = true;
        } else if (d->fw == FW_FULL) {
            if (d->fr == FR_CF) {
                live = (d->kind == K_ALU_RR || d->kind == K_ALU_RI || d->kind == K_ALU_RM ||
                        d->kind == K_ALU_MR || d->kind == K_ALU_MI || d->kind == K_ATOM_ALU) ? true : d->live;
            } else {
                live = false;
            }
        } else if (d->fr == FR_COND) {
            live = true;
        }
    }
    x->entry_live = live;

    /* per producer: which pending components are used, and what is written
     * while they are; from the last producer back (a carry-reading producer
     * decides what its predecessor must keep) */
    for (int i = x->n - 1; i >= 0; --i) {
        ji_t *d = &x->ins[i];
        d->need = 0;
        d->wwin = 0;
        const unsigned pk = pk_of(d);
        if (pk == PK_NONE || d->fw != FW_FULL || !d->live) {
            continue;
        }
        unsigned need = 0, wwin = 0;
        int k = i + 1;
        for (; k < x->n; ++k) {
            const ji_t *e = &x->ins[k];
            if (e->fr == FR_ALL) {
                need |= all_need(pk);
                break;
            }
            if (e->fr == FR_COND) {
                need |= cc_need(pk, e->op);
                if (e->kind == K_JCC) {
                    need |= all_need(pk);       /* the side exit writes the flags out */
                }
            }
            if (e->fw == FW_FULL) {
                const bool alu = (e->kind >= K_ALU_RR && e->kind <= K_ALU_MI) || e->kind == K_ATOM_ALU;
                if (e->fr == FR_CF && (alu || (e->need & N_C) != 0u)) {
                    need |= cf_need(pk);
                }
                break;
            }
            wwin |= e->wmask;
        }
        if (k == x->n) {
            need |= all_need(pk);               /* the block's exit */
        }
        /* The emitters read need == 0 as "these flags are dead" and drop
         * the pending state. A live producer can still need no component:
         * after a logic op the carry is the constant 0, so a carry-only
         * reader asks for nothing (cf_need). Dropped, the reader took CF
         * from whatever flags were last written to memory -- and "xor
         * reg,reg; ... sbb" is how BoringSSL's bn_mul_mont clears the borrow
         * for its final subtraction, which then went wrong at random and
         * failed every RSA and ECDSA signature check (Chromium:
         * ERR_CERT_INVALID, ERR_SSL_PROTOCOL_ERROR). Keep what the kind
         * needs to stand on its own. */
        if (need == 0u) {
            need = all_need(pk);
        }
        d->need = (uint8_t)need;
        d->wwin = (uint16_t)wwin;
    }
}

/* ---- emission basics --------------------------------------------------------------------- */

static void site(jx_t *x)
{
    if (x->nsites == JX_MAX_SITES) {
        x->fail = true;
        return;
    }
    jsite_t *s = &x->sites[x->nsites++];
    s->off = (uint32_t)((uint8_t *)x->a.p - (uint8_t *)x->entry);
    s->_pad = 0;
    s->pc = x->cur >= 0 ? x->ins[x->cur].pc : x->block_pc;
    s->pf = x->pf;
}

/* a branch (cond: A64_AL for B) to a cold stub emitted after the body */
static jcold_t *br_cold(jx_t *x, unsigned cond, int type)
{
    if (x->ncold == JX_MAX_COLD) {
        x->fail = true;
        x->ncold = 0;
    }
    jcold_t *c = &x->cold[x->ncold++];
    c->at = x->a.p;
    c->type = (uint8_t)type;
    c->pf = x->pf;
    c->insn = (int16_t)x->cur;
    c->target = 0;
    c->back = NULL;
    if (cond == A64_AL) {
        a64_b(A, x->a.p);
    } else {
        a64_bcond(A, cond, x->a.p);
    }
    return c;
}

static jcold_t *cbz_cold(jx_t *x, bool sf, bool nz, unsigned rt, int type)
{
    jcold_t *c = br_cold(x, A64_AL, type);
    if (!x->a.overflow) {
        x->a.p--;
        a64_cbz(A, sf, nz, rt, x->a.p);
    }
    return c;
}

static void lit(jx_t *x, unsigned rt, uint64_t v)
{
    if (x->nlits == JX_MAX_LITS) {
        x->fail = true;
        x->nlits = 0;
    }
    x->lits[x->nlits].at = x->a.p;
    x->lits[x->nlits].v = v;
    x->nlits++;
    a64_ldr_lit(A, true, rt, x->a.p);
}

static inline void mov_imm(jx_t *x, unsigned rd, uint64_t v)
{
    a64_mov_imm(A, true, rd, v);
}

/* rd = rn + v (64-bit, no flags); rn is a general register */
static void addi(jx_t *x, unsigned rd, unsigned rn, int64_t v)
{
    if (v == 0) {
        if (rd != rn) a64_mov(A, true, rd, rn);
        return;
    }
    const bool sub = v < 0;
    const uint64_t u = sub ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (u <= 0xFFFu) {
        a64_addsub_imm(A, true, sub, false, rd, rn, (uint32_t)u, false);
    } else if (u <= 0xFFFFFFu) {
        a64_addsub_imm(A, true, sub, false, rd, rn, (uint32_t)(u >> 12), true);
        if ((u & 0xFFFu) != 0u) {
            a64_addsub_imm(A, true, sub, false, rd, rd, (uint32_t)(u & 0xFFFu), false);
        }
    } else {
        mov_imm(x, X17, (uint64_t)v);
        a64_addsub(A, true, false, false, rd, rn, X17, 0, 0);
    }
}

/* ADD/SUB(S) with an immediate that a64_imm12_ok() accepted */
static void addsub_imm12(jx_t *x, bool sf, bool sub, bool setf, unsigned rd, unsigned rn, uint64_t v)
{
    if (v <= 0xFFFu) {
        a64_addsub_imm(A, sf, sub, setf, rd, rn, (uint32_t)v, false);
    } else {
        a64_addsub_imm(A, sf, sub, setf, rd, rn, (uint32_t)(v >> 12), true);
    }
}

static void spill_all(a64_t *a)
{
    for (unsigned k = 0; k < 16u; k += 2u) {
        a64_stp(a, HR[k], HR[k + 1u], RC, OFF_R(k));
    }
}

static void reload_all(a64_t *a)
{
    for (unsigned k = 0; k < 16u; k += 2u) {
        a64_ldp(a, HR[k], HR[k + 1u], RC, OFF_R(k));
    }
}

/* the guest registers kept in caller-saved host registers (RAX..RSI) */
static void spill_cs(a64_t *a)
{
    a64_stp(a, HR[0], HR[1], RC, OFF_R(0));
    a64_stp(a, HR[2], HR[3], RC, OFF_R(2));
    a64_stp(a, HR[4], HR[5], RC, OFF_R(4));
    a64_ls_imm(a, 8, A64_ST, HR[6], RC, OFF_R(6));
}

static void reload_cs(a64_t *a)
{
    a64_ldp(a, HR[0], HR[1], RC, OFF_R(0));
    a64_ldp(a, HR[2], HR[3], RC, OFF_R(2));
    a64_ldp(a, HR[4], HR[5], RC, OFF_R(4));
    a64_ls_imm(a, 8, A64_LDZ, HR[6], RC, OFF_R(6));
}

/* Call a C helper that touches neither guest memory nor guest registers;
 * arguments already in x0/x1. The pending flags must be in memory. */
static void call_partial(jx_t *x, const void *fn)
{
    spill_cs(A);
    mov_imm(x, X16, (uint64_t)(uintptr_t)fn);
    a64_blr(A, X16);
    reload_cs(A);
}

/* ---- guest registers ----------------------------------------------------------------------- */

/* A host register whose low sz bytes are guest register g (AH..BH are
 * extracted into tmp). */
static unsigned rd_reg(jx_t *x, unsigned g, unsigned sz, bool rex, unsigned tmp)
{
    if (is_hi8(g, sz, rex)) {
        a64_ubfx(A, false, tmp, HR[g - 4u], 8, 8);
        return tmp;
    }
    return HR[g];
}

/* Write the low sz bytes of src to guest register g, x86 style (32-bit
 * writes zero-extend, 8/16-bit ones merge). */
static void wr_reg(jx_t *x, unsigned g, unsigned sz, bool rex, unsigned src)
{
    switch (sz) {
    case 8:
        if (HR[g] != src) a64_mov(A, true, HR[g], src);
        break;
    case 4:
        a64_mov(A, false, HR[g], src);
        break;
    case 2:
        a64_bfi(A, true, HR[g], src, 0, 16);
        break;
    default:
        if (is_hi8(g, sz, rex)) a64_bfi(A, true, HR[g - 4u], src, 8, 8);
        else a64_bfi(A, true, HR[g], src, 0, 8);
        break;
    }
}

static inline bool gfree(const ji_t *d, unsigned g)
{
    return g < 16u && (d->wwin & (1u << g)) == 0u;
}

/* ---- memory operands ----------------------------------------------------------------------- */

typedef struct {
    uint8_t base, index, scaled;
    int64_t off;
} mref_t;

#define MR_NOIDX 0xFFu

static bool ls_off_ok(unsigned sz, int64_t off)
{
    if (sz == 16u) {
        return (off >= 0 && (off & 15) == 0 && (off >> 4) <= 0xFFF) || (off >= -256 && off <= 255);
    }
    return a64_ls_imm_ok(sz, off);
}

/* The full address (no segment) into rd. */
static void emit_addr(jx_t *x, const ji_t *d, unsigned rd)
{
    const bool hb = d->base != NOREG, hi = d->index != NOREG;
    if (hb && hi) {
        a64_addsub(A, true, false, false, rd, HR[d->base], HR[d->index], A64_LSL, d->scale);
        addi(x, rd, rd, d->disp);
    } else if (hb) {
        addi(x, rd, HR[d->base], d->disp);
    } else if (hi) {
        a64_lsl_imm(A, true, rd, HR[d->index], d->scale);
        addi(x, rd, rd, d->disp);
    } else {
        mov_imm(x, rd, (uint64_t)d->disp);
    }
}

/* The memory operand of d, for an access of sz bytes (X16 as needed). */
static mref_t ea_mem(jx_t *x, const ji_t *d, unsigned sz)
{
    mref_t m = { X16, MR_NOIDX, 0, 0 };
    const bool hb = d->base != NOREG, hi = d->index != NOREG;
    if (d->seg != 0u) {
        emit_addr(x, d, X16);
        a64_ls_imm(A, 8, A64_LDZ, X17, RC, d->seg == 4u ? CPU_OFF(fs_base) : CPU_OFF(gs_base));
        a64_addsub(A, true, false, false, X16, X16, X17, 0, 0);
        return m;
    }
    if (hb && !hi) {
        if (ls_off_ok(sz, d->disp)) {
            m.base = HR[d->base];
            m.off = d->disp;
        } else {
            addi(x, X16, HR[d->base], d->disp);
        }
        return m;
    }
    if (hb && hi) {
        if (d->disp == 0 && sz != 16u && (d->scale == 0u || (1u << d->scale) == sz)) {
            m.base = HR[d->base];
            m.index = HR[d->index];
            m.scaled = d->scale != 0u;
            return m;
        }
        a64_addsub(A, true, false, false, X16, HR[d->base], HR[d->index], A64_LSL, d->scale);
    } else if (hi) {
        a64_lsl_imm(A, true, X16, HR[d->index], d->scale);
    } else {
        mov_imm(x, X16, (uint64_t)d->disp);
        return m;
    }
    if (ls_off_ok(sz, d->disp)) {
        m.off = d->disp;
    } else {
        addi(x, X16, X16, d->disp);
    }
    return m;
}

/* one guest memory access (vec: rt is a SIMD&FP register) */
static void access(jx_t *x, unsigned sz, unsigned kind, unsigned rt, const mref_t *m, bool load,
                   bool vec)
{
    if (m->index != MR_NOIDX && (!vec || !m->scaled)) {
        site(x);
        if (vec) a64_fls_reg(A, sz, load, rt, m->base, m->index);
        else a64_ls_reg(A, sz, kind, rt, m->base, m->index, m->scaled != 0u);
        return;
    }
    if (m->index != MR_NOIDX) {     /* vector, scaled index: no such form here */
        a64_addsub(A, true, false, false, X17, m->base, m->index, A64_LSL, a64_log2sz(sz));
        site(x);
        (void)a64_fls_imm(A, sz, load, rt, X17, 0);
        return;
    }
    const bool ok = vec ? ls_off_ok(sz == 16u ? 16u : sz, m->off) : ls_off_ok(sz, m->off);
    if (!ok) {
        mov_imm(x, X17, (uint64_t)m->off);
        site(x);
        if (vec) a64_fls_reg(A, sz, load, rt, m->base, X17);
        else a64_ls_reg(A, sz, kind, rt, m->base, X17, false);
        return;
    }
    site(x);
    if (vec) (void)a64_fls_imm(A, sz, load, rt, m->base, m->off);
    else (void)a64_ls_imm(A, sz, kind, rt, m->base, m->off);
}

static void emit_load(jx_t *x, unsigned sz, unsigned kind, unsigned rt, const mref_t *m)
{
    access(x, sz, kind, rt, m, true, sz == 16u);
    if (x->mt) a64_dmb_ishld(A);
}

static void emit_store(jx_t *x, unsigned sz, unsigned rt, const mref_t *m)
{
    if (x->mt) a64_dmb_ishst(A);
    access(x, sz, A64_ST, rt, m, false, sz == 16u);
}

static void emit_vload(jx_t *x, unsigned sz, unsigned v, const mref_t *m)
{
    access(x, sz, A64_LDZ, v, m, true, true);
    if (x->mt) a64_dmb_ishld(A);
}

static void emit_vstore(jx_t *x, unsigned sz, unsigned v, const mref_t *m)
{
    if (x->mt) a64_dmb_ishst(A);
    access(x, sz, A64_ST, v, m, false, true);
}

/* ---- flags ------------------------------------------------------------------------------------- */

#define JC_TRUE  16u
#define JC_FALSE 17u

static unsigned cond_neg(unsigned c, unsigned neg)
{
    if (!neg) return c;
    if (c == JC_TRUE) return JC_FALSE;
    if (c == JC_FALSE) return JC_TRUE;
    return c ^ 1u;
}

static unsigned loc_get(jx_t *x, unsigned loc, uint64_t imm, unsigned tmp)
{
    if (loc == LOC_IMM) {
        mov_imm(x, tmp, imm);
        return tmp;
    }
    if (loc == LOC_NONE) {
        x->fail = true;         /* the analysis kept too little: a bug */
        return ZR;
    }
    return loc;
}

static void mask_into(jx_t *x, unsigned dst, unsigned src, unsigned sz)
{
    switch (sz) {
    case 8: if (dst != src) a64_mov(A, true, dst, src); break;
    case 4: a64_mov(A, false, dst, src); break;
    case 2: a64_uxt(A, dst, src, 16); break;
    default: a64_uxt(A, dst, src, 8); break;
    }
}

/* NZCV of the sz-byte a +/- b */
static void flags_arith(jx_t *x, bool add, unsigned sz, unsigned la, unsigned lb, uint64_t imm)
{
    if (la == ZR && lb == LOC_IMM) {
        a64_movz(A, true, X16, 0, 0);
        la = X16;
    }
    if (sz >= 4u) {
        const bool sf = sz == 8u;
        if (lb == LOC_IMM) {
            const uint64_t v = sf ? imm : (uint32_t)imm;
            const uint64_t nv = sf ? (uint64_t)0 - v : (uint32_t)(0u - (uint32_t)v);
            if (a64_imm12_ok(v)) {
                addsub_imm12(x, sf, !add, true, ZR, la, v);
            } else if (v != 0u && a64_imm12_ok(nv)) {
                addsub_imm12(x, sf, add, true, ZR, la, nv);
            } else {
                a64_mov_imm(A, sf, X17, v);
                a64_addsub(A, sf, !add, true, ZR, la, X17, 0, 0);
            }
        } else {
            a64_addsub(A, sf, !add, true, ZR, la, lb, 0, 0);
        }
        return;
    }
    const unsigned s = 32u - 8u * sz;
    a64_lsl_imm(A, false, X16, la, s);
    if (lb == LOC_IMM) {
        a64_mov_imm(A, false, X17, (uint64_t)(uint32_t)(imm << s));
        a64_addsub(A, false, !add, true, ZR, X16, X17, 0, 0);
    } else {
        a64_addsub(A, false, !add, true, ZR, X16, lb, A64_LSL, s);
    }
}

/* N and Z of the sz-byte value r; C = V = 0 */
static void flags_test(jx_t *x, unsigned sz, unsigned r)
{
    if (sz >= 4u) {
        a64_tst(A, sz == 8u, r, r);
        return;
    }
    a64_lsl_imm(A, false, X16, r, 32u - 8u * sz);
    a64_tst(A, false, X16, X16);
}

/* N and Z of a & b; C = V = 0 */
static void flags_and(jx_t *x, unsigned sz, unsigned la, unsigned lb, uint64_t imm)
{
    uint32_t enc = 0;
    if (sz >= 4u) {
        const bool sf = sz == 8u;
        if (lb == LOC_IMM) {
            const uint64_t v = sf ? imm : (uint32_t)imm;
            if (a64_logical_imm(v, sf, &enc) == 0) {
                a64_logic_imm_enc(A, sf, A64_ANDS, ZR, la, enc);
                return;
            }
            a64_mov_imm(A, sf, X17, v);
            lb = X17;
        }
        a64_tst(A, sf, la, lb);
        return;
    }
    if (lb == LOC_IMM) {
        const uint64_t v = imm & szmask((int)sz);
        if (a64_logical_imm(v, false, &enc) == 0) {
            a64_logic_imm_enc(A, false, A64_AND, X16, la, enc);
        } else {
            a64_mov_imm(A, false, X17, v);
            a64_logic(A, false, A64_L_AND, X16, la, X17, 0, 0);
        }
    } else {
        a64_logic(A, false, A64_L_AND, X16, la, lb, 0, 0);
    }
    a64_lsl_imm(A, false, X16, X16, 32u - 8u * sz);
    a64_tst(A, false, X16, X16);
}

/* Z = 1 (EQ) when the low byte of r has even parity (x86 PF = 1) */
static unsigned parity_of(jx_t *x, unsigned r)
{
    uint32_t enc = 0;
    a64_logic(A, false, A64_L_EOR, X16, r, r, A64_LSR, 4);
    a64_logic(A, false, A64_L_EOR, X16, X16, X16, A64_LSR, 2);
    a64_logic(A, false, A64_L_EOR, X16, X16, X16, A64_LSR, 1);
    (void)a64_logical_imm(1, false, &enc);
    a64_logic_imm_enc(A, false, A64_ANDS, ZR, X16, enc);
    return A64_EQ;
}

static void st_ccop(jx_t *x, unsigned cc, unsigned sz)
{
    mov_imm(x, X16, (uint64_t)cc | ((uint64_t)sz << 32));
    a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(cc_op));
}

/* Write the pending flags to the CPU structure (lazy form). Uses X16,
 * X17, S2, S3. */
static void materialize(jx_t *x)
{
    pf_t *p = &x->pf;
    const unsigned sz = p->sz;
    switch (p->kind) {
    case PK_NONE: case PK_MEM:
        return;
    case PK_ADD: case PK_SUB: case PK_ADC: case PK_SBB: case PK_TEST:
        mask_into(x, S2, loc_get(x, p->la, 0, S2), sz);
        mask_into(x, S3, loc_get(x, p->lb, p->imm, S3), sz);
        switch (p->kind) {
        case PK_ADD: a64_addsub(A, true, false, false, X16, S2, S3, 0, 0); break;
        case PK_SUB: a64_addsub(A, true, true, false, X16, S2, S3, 0, 0); break;
        case PK_ADC:
            a64_addsub(A, true, false, false, X16, S2, S3, 0, 0);
            a64_addsub(A, true, false, false, X16, X16, p->lc, 0, 0);
            break;
        case PK_SBB:
            a64_addsub(A, true, true, false, X16, S2, S3, 0, 0);
            a64_addsub(A, true, true, false, X16, X16, p->lc, 0, 0);
            break;
        default:
            a64_logic(A, true, A64_L_AND, X16, S2, S3, 0, 0);
            break;
        }
        mask_into(x, X16, X16, sz);
        a64_stp(A, S2, S3, RC, CPU_OFF(cc_a));
        a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(cc_r));
        if (p->kind == PK_ADC || p->kind == PK_SBB) {
            a64_ls_imm(A, 8, A64_ST, p->lc, RC, CPU_OFF(cc_cin));
        }
        st_ccop(x, k_pk_cc[p->kind], sz);
        break;
    case PK_LOGIC:
        mask_into(x, X16, loc_get(x, p->lr, 0, X16), sz);
        a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(cc_r));
        st_ccop(x, CC_LOGIC, sz);
        break;
    case PK_INC: case PK_DEC:
        mask_into(x, S3, loc_get(x, p->lr, 0, S3), sz);
        a64_addsub_imm(A, true, p->kind == PK_INC, false, S2, S3, 1, false);
        mask_into(x, S2, S2, sz);
        a64_movz(A, true, X16, 1, 0);
        a64_stp(A, S2, X16, RC, CPU_OFF(cc_a));
        a64_ls_imm(A, 8, A64_ST, S3, RC, CPU_OFF(cc_r));
        a64_ls_imm(A, 8, A64_ST, p->lc == LOC_NONE ? ZR : p->lc, RC, CPU_OFF(cc_cin));
        st_ccop(x, k_pk_cc[p->kind], sz);
        break;
    case PK_SHL: case PK_SHR: case PK_SAR:
        mask_into(x, S2, loc_get(x, p->la, 0, S2), sz);
        a64_movz(A, true, X16, (uint32_t)p->imm, 0);
        a64_stp(A, S2, X16, RC, CPU_OFF(cc_a));
        mask_into(x, S3, loc_get(x, p->lr, 0, S3), sz);
        a64_ls_imm(A, 8, A64_ST, S3, RC, CPU_OFF(cc_r));
        st_ccop(x, k_pk_cc[p->kind], sz);
        break;
    case PK_EFL:
        a64_ls_imm(A, 8, A64_LDZ, X16, RC, CPU_OFF(eflags));
        a64_movz(A, true, X17, F_ARITH, 0);
        a64_logic(A, true, A64_L_BIC, X16, X16, X17, 0, 0);
        a64_logic(A, true, A64_L_ORR, X16, X16, loc_get(x, p->lr, 0, S3), 0, 0);
        a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(eflags));
        a64_ls_imm(A, 4, A64_ST, ZR, RC, CPU_OFF(cc_op));     /* CC_EFLAGS */
        break;
    case PK_MUL:
        a64_stp(A, ZR, loc_get(x, p->lb, 0, S2), RC, CPU_OFF(cc_a));
        mask_into(x, S3, loc_get(x, p->lr, 0, S3), sz);
        a64_ls_imm(A, 8, A64_ST, S3, RC, CPU_OFF(cc_r));
        st_ccop(x, CC_MUL, sz);
        break;
    default:
        break;
    }
    p->kind = PK_MEM;
}

/* Set the host condition flags for x86 condition cc and return the A64
 * condition that holds iff it is true (or JC_TRUE/JC_FALSE). May call C
 * (clobbering x0-x8, x16-x17), so nothing may be live in scratch. */
static unsigned emit_cond(jx_t *x, unsigned cc)
{
    pf_t *p = &x->pf;
    const unsigned c2 = cc >> 1, neg = cc & 1u, sz = p->sz;
    unsigned r;
    switch (p->kind) {
    case PK_ADD: case PK_SUB: {
        const bool add = p->kind == PK_ADD;
        const bool ab = p->la != LOC_NONE && p->lb != LOC_NONE;
        if (c2 == 2u || c2 == 4u || c2 == 5u) {
            if (p->lr != LOC_NONE) {
                if (c2 == 5u) return cond_neg(parity_of(x, p->lr), neg);
                flags_test(x, sz, p->lr);
                return cond_neg(c2 == 2u ? A64_EQ : A64_MI, neg);
            }
            if (!ab) break;
            if (c2 == 5u) {
                const unsigned ra = loc_get(x, p->la, 0, S2), rb = loc_get(x, p->lb, p->imm, S3);
                a64_addsub(A, true, !add, false, X16, ra, rb, 0, 0);
                return cond_neg(parity_of(x, X16), neg);
            }
            flags_arith(x, add, sz, p->la, p->lb, p->imm);
            return cond_neg(c2 == 2u ? A64_EQ : A64_MI, neg);
        }
        if (!ab) break;
        flags_arith(x, add, sz, p->la, p->lb, p->imm);
        switch (c2) {
        case 0: r = A64_VS; break;
        case 1: r = add ? A64_CS : A64_LO; break;
        case 3:
            if (add) {          /* CF | ZF after an addition */
                a64_cset(A, false, X16, A64_CS);
                a64_csel_op(A, false, 1, X16, X16, ZR, A64_NE);
                a64_addsub_imm(A, false, true, true, ZR, X16, 0, false);
                r = A64_NE;
            } else {
                r = A64_LS;
            }
            break;
        case 6: r = A64_LT; break;
        default: r = A64_LE; break;
        }
        return cond_neg(r, neg);
    }
    case PK_LOGIC: case PK_TEST:
        if (c2 == 0u || c2 == 1u) return cond_neg(JC_FALSE, neg);
        if (c2 == 5u) {
            unsigned rr = p->lr;
            if (p->kind == PK_TEST) {
                const unsigned ra = loc_get(x, p->la, 0, S2), rb = loc_get(x, p->lb, p->imm, S3);
                a64_logic(A, true, A64_L_AND, X17, ra, rb, 0, 0);
                rr = X17;
            }
            return cond_neg(parity_of(x, rr), neg);
        }
        if (p->kind == PK_LOGIC) flags_test(x, sz, p->lr);
        else flags_and(x, sz, p->la, p->lb, p->imm);
        switch (c2) {
        case 2: case 3: r = A64_EQ; break;
        case 4: r = A64_MI; break;
        case 6: r = A64_LT; break;
        default: r = A64_LE; break;
        }
        return cond_neg(r, neg);
    case PK_INC: case PK_DEC:
        if (c2 == 2u || c2 == 4u) {
            flags_test(x, sz, p->lr);
            return cond_neg(c2 == 2u ? A64_EQ : A64_MI, neg);
        }
        if (c2 == 5u) return cond_neg(parity_of(x, p->lr), neg);
        if (c2 == 1u && p->lc != LOC_NONE) {
            a64_addsub_imm(A, false, true, true, ZR, p->lc, 0, false);
            return cond_neg(A64_NE, neg);
        }
        if (c2 == 0u || c2 == 6u || c2 == 7u) {
            a64_addsub_imm(A, true, p->kind == PK_INC, false, X16, p->lr, 1, false);
            flags_arith(x, p->kind == PK_INC, sz, X16, LOC_IMM, 1);
            return cond_neg(c2 == 0u ? A64_VS : c2 == 6u ? A64_LT : A64_LE, neg);
        }
        break;
    case PK_SHL: case PK_SHR: case PK_SAR:
        if (c2 == 2u || c2 == 4u) {
            flags_test(x, sz, p->lr);
            return cond_neg(c2 == 2u ? A64_EQ : A64_MI, neg);
        }
        if (c2 == 5u) return cond_neg(parity_of(x, p->lr), neg);
        if (c2 == 1u && p->la != LOC_NONE) {
            const unsigned bits = 8u * sz, n = (unsigned)p->imm;
            unsigned pos;
            if (p->kind == PK_SAR) {
                pos = n - 1u > bits - 1u ? bits - 1u : n - 1u;
            } else {
                if (n > bits) return cond_neg(JC_FALSE, neg);
                pos = p->kind == PK_SHL ? bits - n : n - 1u;
            }
            uint32_t enc = 0;
            (void)a64_logical_imm(1ULL << pos, true, &enc);
            a64_logic_imm_enc(A, true, A64_ANDS, ZR, p->la, enc);
            return cond_neg(A64_NE, neg);
        }
        break;
    case PK_MUL:
        if (c2 == 2u || c2 == 4u) {
            flags_test(x, sz, p->lr);
            return cond_neg(c2 == 2u ? A64_EQ : A64_MI, neg);
        }
        if (c2 == 5u) return cond_neg(parity_of(x, p->lr), neg);
        if (c2 <= 1u && p->lb != LOC_NONE) {
            a64_addsub_imm(A, false, true, true, ZR, p->lb, 0, false);
            return cond_neg(A64_NE, neg);
        }
        break;
    case PK_EFL: {
        /* one bit to test (CF|ZF is not a logical immediate: ZF is
         * folded onto CF first) */
        static const uint16_t k_bit[8] = { F_OF, F_CF, F_ZF, F_CF, F_SF, F_PF, 0, 0 };
        uint32_t enc = 0;
        unsigned v = p->lr;
        if (c2 >= 6u) {     /* SF != OF (, or ZF) */
            a64_logic(A, false, A64_L_EOR, X16, v, v, A64_LSR, 4);
            if (c2 == 7u) a64_logic(A, false, A64_L_ORR, X16, X16, v, A64_LSL, 1);
            v = X16;
        } else if (c2 == 3u) {  /* CF or ZF */
            a64_logic(A, false, A64_L_ORR, X16, v, v, A64_LSR, 6);
            v = X16;
        }
        if (a64_logical_imm(c2 >= 6u ? F_SF : k_bit[c2], false, &enc) != 0) {
            xl_fatal("JIT: condition %u has no logical immediate", c2);
        }
        a64_logic_imm_enc(A, false, A64_ANDS, ZR, v, enc);
        return cond_neg(A64_NE, neg);
    }
    default:
        break;
    }
    /* anything else: the interpreter's x86_cond on the materialized state */
    materialize(x);
    a64_mov(A, true, S0, RC);
    a64_movz(A, false, S1, cc, 0);
    call_partial(x, (const void *)jit_h_cond);
    a64_addsub_imm(A, false, true, true, ZR, S0, 0, false);
    return A64_NE;
}

/* The carry flag (0/1) into dst; reads the pending flags. May call C. */
static void emit_cf(jx_t *x, unsigned dst)
{
    pf_t *p = &x->pf;
    const unsigned sz = p->sz, bits = 8u * sz;
    switch (p->kind) {
    case PK_SUB:
        flags_arith(x, false, sz, p->la, p->lb, p->imm);
        a64_cset(A, true, dst, A64_LO);
        return;
    case PK_ADD:
        flags_arith(x, true, sz, p->la, p->lb, p->imm);
        a64_cset(A, true, dst, A64_CS);
        return;
    case PK_LOGIC: case PK_TEST:
        a64_movz(A, true, dst, 0, 0);
        return;
    case PK_INC: case PK_DEC:
        if (p->lc != LOC_NONE) {
            if (p->lc != dst) a64_mov(A, true, dst, p->lc);
            return;
        }
        break;
    case PK_SHL: case PK_SHR: case PK_SAR: {
        const unsigned n = (unsigned)p->imm;
        if (p->la == LOC_NONE) break;
        if (p->kind != PK_SAR && n > bits) {
            a64_movz(A, true, dst, 0, 0);
            return;
        }
        unsigned pos = p->kind == PK_SHL ? bits - n : n - 1u;
        if (p->kind == PK_SAR && pos > bits - 1u) pos = bits - 1u;
        a64_ubfx(A, true, dst, p->la, pos, 1);
        return;
    }
    case PK_MUL:
        if (p->lb != LOC_NONE) {
            if (p->lb != dst) a64_mov(A, true, dst, p->lb);
            return;
        }
        break;
    case PK_EFL:
        a64_ubfx(A, true, dst, p->lr, 0, 1);
        return;
    case PK_ADC: case PK_SBB:
        mask_into(x, S2, loc_get(x, p->la, 0, S2), sz);
        mask_into(x, S3, loc_get(x, p->lb, p->imm, S3), sz);
        if (p->kind == PK_ADC) {
            /* CF = cin ? r <= a : r < a */
            a64_addsub(A, true, false, false, X16, S2, S3, 0, 0);
            a64_addsub(A, true, false, false, X16, X16, p->lc, 0, 0);
            mask_into(x, X16, X16, sz);
            a64_cmp(A, true, X16, S2);
        } else {
            /* CF = cin ? a <= b : a < b */
            a64_cmp(A, true, S2, S3);
        }
        a64_cset(A, true, X17, A64_LS);
        a64_cset(A, true, X16, A64_LO);
        a64_addsub_imm(A, true, true, true, ZR, p->lc, 0, false);
        a64_csel(A, true, dst, X17, X16, A64_NE);
        return;
    default:
        break;
    }
    materialize(x);
    a64_mov(A, true, S0, RC);
    call_partial(x, (const void *)jit_h_get_cf);
    a64_mov(A, true, dst, S0);
}

/* The arithmetic EFLAGS bits of the pending state into FR (pending
 * becomes "in memory"; calls C). */
static void flags_to_fr(jx_t *x)
{
    materialize(x);
    a64_mov(A, true, S0, RC);
    call_partial(x, (const void *)jit_h_flags);
    a64_mov(A, true, FR, S0);
}

/* ---- exits ------------------------------------------------------------------------------------- */

static void exit_direct(jx_t *x, uint64_t target)
{
    if (target == x->block_pc) {
        if (x->entry_live) materialize(x);
        x->pf.kind = PK_MEM;
        a64_b(A, x->entry);
        return;
    }
    materialize(x);
    uint32_t *site_p = x->a.p;
    a64_b(A, x->a.p + 1);                   /* patched to the target translation */
    mov_imm(x, S0, target);
    a64_adr(A, 2, ((uint8_t *)site_p - (uint8_t *)x->a.p));
    a64_movz(A, false, S1, JX_CHAIN, 0);
    a64_b(A, x->jc->tramp[1]);
}

/* target in S4 */
static void exit_indirect(jx_t *x)
{
    materialize(x);
    a64_ls_imm(A, 8, A64_LDZ, X17, RT, (int64_t)__builtin_offsetof(gthread_t, engine));
    a64_logic(A, true, A64_L_EOR, X16, S4, S4, A64_LSR, 12);
    a64_ubfiz(A, true, X16, X16, 4, JIT_IBTC_BITS);
    a64_addsub(A, true, false, false, X17, X17, X16, 0, 0);
    a64_put(A, 0xA9400000u | ((JIT_IBTC_OFF / 8u) << 15) | (X17 << 10) | (X17 << 5) | X16);  /* ldp x16, x17, [x17, #64] */
    a64_cmp(A, true, X16, S4);
    a64_bcond(A, A64_NE, x->a.p + 2);
    a64_br(A, X17);
    a64_mov(A, true, S0, S4);
    a64_movz(A, false, S1, JX_LOOKUP, 0);
    a64_b(A, x->jc->tramp[2]);
}

/* The instruction through the interpreter (registers spilled, the flags
 * in memory). On return: an event or a changed RIP leaves the block. */
static void emit_step_call(jx_t *x, const ji_t *d)
{
    spill_all(A);
    mov_imm(x, X16, d->pc);
    a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(rip));
    a64_mov(A, true, S0, RT);
    mov_imm(x, X16, (uint64_t)(uintptr_t)jit_h_step);
    a64_blr(A, X16);
}

static void e_fallback(jx_t *x, const ji_t *d)
{
    materialize(x);
    emit_step_call(x, d);
    cbz_cold(x, false, true, S0, CS_EXIT_NOSPILL);
    a64_ls_imm(A, 8, A64_LDZ, X16, RC, CPU_OFF(rip));
    mov_imm(x, X17, d->next);
    a64_cmp(A, true, X16, X17);
    br_cold(x, A64_NE, CS_LOOKUP_NOSPILL);
    reload_all(A);
    x->pf.kind = PK_MEM;
}

static jsse_t *sse_desc(jx_t *x, const ji_t *d)
{
    jsse_t *s = jit_meta_alloc(x->jc, sizeof(jsse_t));
    if (s == NULL) {
        x->fail = true;
        return NULL;
    }
    s->pc = d->pc;
    s->next = d->next;
    s->disp = d->disp;
    s->op = d->op;
    s->imm = (uint8_t)d->imm;
    s->rexb = d->rexb;
    s->p66 = d->p66;
    s->rep = d->rep;
    s->mod = d->mod;
    s->reg = d->dst;
    s->rm = d->src;
    s->seg = d->seg;
    s->base = d->base;
    s->index = d->index;
    s->scale = d->scale;
    return s;
}

static void e_sse(jx_t *x, const ji_t *d)
{
    jsse_t *s = sse_desc(x, d);
    if (s == NULL) {
        return;
    }
    materialize(x);
    spill_all(A);
    mov_imm(x, X16, d->pc);
    a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(rip));
    a64_mov(A, true, S0, RT);
    mov_imm(x, S1, (uint64_t)(uintptr_t)s);
    mov_imm(x, X16, (uint64_t)(uintptr_t)jit_h_sse);
    a64_blr(A, X16);
    cbz_cold(x, false, true, S0, CS_EXIT_NOSPILL);
    reload_all(A);
    x->pf.kind = PK_MEM;
}

/* ---- SSE natively ----------------------------------------------------------------------------------
 * XMM registers stay in the CPU structure; v0-v3 are scratch. */

#define OFF_X(n) (CPU_OFF(xmm) + 16 * (int64_t)(n))
_Static_assert(__builtin_offsetof(x86cpu_t, xmm) % 16 == 0, "xmm alignment");
_Static_assert(__builtin_offsetof(x86cpu_t, xmm) + 16 * 15 + 8 <= 504, "LDP/STP reach of xmm[]");

/* AdvSIMD encodings used only here */
#define V_SQXTN    0x0E214800u
#define V_SQXTN2   0x4E214800u
#define V_SQXTUN   0x2E212800u
#define V_SQXTUN2  0x6E212800u
#define V_XTN_2S   0x0EA12800u
#define V_UMULL_2D 0x2EA0C000u
#define V_SMULL_4S 0x0E60C000u
#define V_SMULL2_4S 0x4E60C000u
#define V_UMULL_4S 0x2E60C000u
#define V_UMULL2_4S 0x6E60C000u
#define V_ADDP_4S  0x4EA0BC00u
#define V_UABD_16B 0x6E207400u
#define V_UADDLP   0x6E202800u
#define V_SSHLL_2D 0x0F20A400u
#define V_SCVTF    0x4E21D800u
#define V_FSQRT    0x6EA1F800u
#define V_NOT      0x6E205800u

static inline void v2op(jx_t *x, uint32_t opc, unsigned vd, unsigned vn)
{
    a64_put(A, opc | (vn << 5) | vd);
}

static inline void xld(jx_t *x, unsigned sz, unsigned v, unsigned n, unsigned off)
{
    (void)a64_fls_imm(A, sz, true, v, RC, OFF_X(n) + off);
}

static inline void xst(jx_t *x, unsigned sz, unsigned v, unsigned n, unsigned off)
{
    (void)a64_fls_imm(A, sz, false, v, RC, OFF_X(n) + off);
}

/* the r/m operand (an XMM register or sz bytes of memory) into v */
static void sse_src(jx_t *x, const ji_t *d, unsigned v, unsigned sz)
{
    if (d->mod == 3u) {
        xld(x, sz, v, d->src, 0);
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_vload(x, sz, v, &m);
    }
}

/* a NaN result: the interpreter redoes the instruction (x86 NaN rules) */
static void sse_nan_check(jx_t *x, const ji_t *d, bool packed, bool dbl, unsigned v, jcold_t **out)
{
    jcold_t *c;
    if (!packed) {
        a64_fcmp(A, dbl, v, v);
        c = br_cold(x, A64_VS, CS_SSESLOW);
    } else {
        a64_vf3(A, A64_V_FCMEQ, dbl, 3, v, v);
        a64_uminv(A, 2, 3, 3);
        a64_fmov_from_s(A, X16, 3);
        c = cbz_cold(x, false, false, X16, CS_SSESLOW);
    }
    c->target = (uint64_t)(uintptr_t)sse_desc(x, d);
    *out = c;
}

static void e_sse_native(jx_t *x, const ji_t *d)
{
    const unsigned pf = sse_pfx(d), op = d->op, G = d->dst, E = d->src;
    const bool reg = d->mod == 3u, w64 = (d->rexb & 8u) != 0u;
    const unsigned imm = (unsigned)d->imm;
    jcold_t *slow = NULL;
    uint32_t opc = 0;
    unsigned size = 0;
    mref_t m;
    switch (d->plan) {
    case SP_MOV16_LD:
        sse_src(x, d, 0, 16);
        xst(x, 16, 0, G, 0);
        break;
    case SP_MOV16_ST:
        xld(x, 16, 0, G, 0);
        if (reg) {
            xst(x, 16, 0, E, 0);
        } else {
            m = ea_mem(x, d, 16);
            emit_vstore(x, 16, 0, &m);
        }
        break;
    case SP_MOVS_LD: {          /* MOVSS/MOVSD xmm, xmm/m */
        const unsigned sz = pf == 3u ? 4u : 8u;
        if (reg) {
            a64_ls_imm(A, sz, A64_LDZ, S0, RC, OFF_X(E));
            a64_ls_imm(A, sz, A64_ST, S0, RC, OFF_X(G));
        } else {
            m = ea_mem(x, d, sz);
            emit_vload(x, sz, 0, &m);       /* zero-extends to 128 bits */
            xst(x, 16, 0, G, 0);
        }
        break;
    }
    case SP_MOVS_ST: {
        const unsigned sz = pf == 3u ? 4u : 8u;
        a64_ls_imm(A, sz, A64_LDZ, S0, RC, OFF_X(G));
        if (reg) {
            a64_ls_imm(A, sz, A64_ST, S0, RC, OFF_X(E));
        } else {
            m = ea_mem(x, d, sz);
            emit_store(x, sz, S0, &m);
        }
        break;
    }
    case SP_MOVLP_LD:           /* MOVLPS/MOVLPD; MOVHLPS */
        if (reg) {
            a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(E) + 8);
        } else {
            m = ea_mem(x, d, 8);
            emit_load(x, 8, A64_LDZ, S0, &m);
        }
        a64_ls_imm(A, 8, A64_ST, S0, RC, OFF_X(G));
        break;
    case SP_MOVLP_ST:
        a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(G));
        m = ea_mem(x, d, 8);
        emit_store(x, 8, S0, &m);
        break;
    case SP_MOVHP_LD:           /* MOVHPS/MOVHPD; MOVLHPS */
        if (reg) {
            a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(E));
        } else {
            m = ea_mem(x, d, 8);
            emit_load(x, 8, A64_LDZ, S0, &m);
        }
        a64_ls_imm(A, 8, A64_ST, S0, RC, OFF_X(G) + 8);
        break;
    case SP_MOVHP_ST:
        a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(G) + 8);
        m = ea_mem(x, d, 8);
        emit_store(x, 8, S0, &m);
        break;
    case SP_MOVDDUP:
        if (reg) {
            a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(E));
        } else {
            m = ea_mem(x, d, 8);
            emit_load(x, 8, A64_LDZ, S0, &m);
        }
        a64_stp(A, S0, S0, RC, OFF_X(G));
        break;
    case SP_UNPCK: case SP_IARITH:
        if (op == 0x14u || op == 0x15u) {
            opc = op == 0x14u ? A64_V_ZIP1 : A64_V_ZIP2;
            size = pf == 1u ? 3u : 2u;
        } else {
            (void)sse_iop((uint8_t)op, &opc, &size);
        }
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        a64_v3s(A, opc, size, 0, 0, 1);
        xst(x, 16, 0, G, 0);
        break;
    case SP_FLOGIC:
        if (reg && E == G && (op == 0x57u || op == 0xEFu || op == 0x55u || op == 0xDFu)) {
            a64_stp(A, ZR, ZR, RC, OFF_X(G));       /* the zeroing idiom */
            break;
        }
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        switch (op) {
        case 0x54: case 0xDB: a64_v3(A, A64_V_AND, 0, 0, 1); break;
        case 0x55: case 0xDF: a64_v3(A, A64_V_BIC, 0, 1, 0); break;
        case 0x56: case 0xEB: a64_v3(A, A64_V_ORR, 0, 0, 1); break;
        default: a64_v3(A, A64_V_EOR, 0, 0, 1); break;
        }
        xst(x, 16, 0, G, 0);
        break;
    case SP_PACK:
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        if (op == 0x67u) {
            v2op(x, V_SQXTUN, 2, 0);
            v2op(x, V_SQXTUN2, 2, 1);
        } else {
            const uint32_t sz1 = op == 0x6Bu ? (1u << 22) : 0u;
            v2op(x, V_SQXTN | sz1, 2, 0);
            v2op(x, V_SQXTN2 | sz1, 2, 1);
        }
        xst(x, 16, 2, G, 0);
        break;
    case SP_MULH:
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        a64_v3(A, op == 0xE5u ? V_SMULL_4S : V_UMULL_4S, 2, 0, 1);
        a64_v3(A, op == 0xE5u ? V_SMULL2_4S : V_UMULL2_4S, 3, 0, 1);
        a64_v3s(A, A64_V_UZP2, 1, 0, 2, 3);
        xst(x, 16, 0, G, 0);
        break;
    case SP_PMULUDQ:
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        v2op(x, V_XTN_2S, 2, 0);
        v2op(x, V_XTN_2S, 3, 1);
        a64_v3(A, V_UMULL_2D, 0, 2, 3);
        xst(x, 16, 0, G, 0);
        break;
    case SP_PMADDWD:
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        a64_v3(A, V_SMULL_4S, 2, 0, 1);
        a64_v3(A, V_SMULL2_4S, 3, 0, 1);
        a64_v3(A, V_ADDP_4S, 0, 2, 3);
        xst(x, 16, 0, G, 0);
        break;
    case SP_PSADBW:
        xld(x, 16, 0, G, 0);
        sse_src(x, d, 1, 16);
        a64_v3(A, V_UABD_16B, 2, 0, 1);
        v2op(x, V_UADDLP, 2, 2);
        v2op(x, V_UADDLP | (1u << 22), 2, 2);
        v2op(x, V_UADDLP | (2u << 22), 2, 2);
        xst(x, 16, 2, G, 0);
        break;
    case SP_MOVD_TOX:           /* MOVD/MOVQ xmm, r/m */
        if (reg) {
            if (w64) {
                a64_stp(A, HR[E], ZR, RC, OFF_X(G));
            } else {
                a64_mov(A, false, S0, HR[E]);
                a64_stp(A, S0, ZR, RC, OFF_X(G));
            }
        } else {
            m = ea_mem(x, d, w64 ? 8u : 4u);
            emit_load(x, w64 ? 8u : 4u, A64_LDZ, S0, &m);
            a64_stp(A, S0, ZR, RC, OFF_X(G));
        }
        break;
    case SP_MOVD_FROMX:         /* MOVD/MOVQ r/m, xmm */
        if (reg) {
            a64_ls_imm(A, w64 ? 8u : 4u, A64_LDZ, HR[E], RC, OFF_X(G));
        } else {
            a64_ls_imm(A, w64 ? 8u : 4u, A64_LDZ, S0, RC, OFF_X(G));
            m = ea_mem(x, d, w64 ? 8u : 4u);
            emit_store(x, w64 ? 8u : 4u, S0, &m);
        }
        break;
    case SP_MOVQ_TOX:           /* MOVQ xmm, xmm/m64 */
        if (reg) {
            a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(E));
        } else {
            m = ea_mem(x, d, 8);
            emit_load(x, 8, A64_LDZ, S0, &m);
        }
        a64_stp(A, S0, ZR, RC, OFF_X(G));
        break;
    case SP_MOVQ_FROMX:         /* MOVQ xmm/m64, xmm */
        a64_ls_imm(A, 8, A64_LDZ, S0, RC, OFF_X(G));
        if (reg) {
            a64_stp(A, S0, ZR, RC, OFF_X(E));
        } else {
            m = ea_mem(x, d, 8);
            emit_store(x, 8, S0, &m);
        }
        break;
    case SP_PSHUF:
        sse_src(x, d, 1, 16);
        if (pf == 1u) {             /* PSHUFD */
            const unsigned s0 = imm & 3u;
            if (imm == s0 * 0x55u) {
                a64_dup_elt(A, 32, 0, 1, s0);
            } else {
                for (unsigned k = 0; k < 4u; ++k) a64_ins_elt(A, 32, 0, k, 1, (imm >> (2u * k)) & 3u);
            }
        } else {                    /* PSHUFLW (F2), PSHUFHW (F3) */
            const unsigned base = pf == 2u ? 0u : 4u;
            a64_vmov(A, 0, 1);
            for (unsigned k = 0; k < 4u; ++k) a64_ins_elt(A, 16, 0, base + k, 1, base + ((imm >> (2u * k)) & 3u));
        }
        xst(x, 16, 0, G, 0);
        break;
    case SP_SHIFTI: {
        const unsigned sub = G & 7u;
        xld(x, 16, 0, E, 0);
        if (op == 0x73u && (sub == 3u || sub == 7u)) {     /* PSRLDQ / PSLLDQ (bytes) */
            if (imm == 0u) break;
            if (imm >= 16u) {
                a64_stp(A, ZR, ZR, RC, OFF_X(E));
                break;
            }
            a64_vzero(A, 1);
            if (sub == 3u) a64_ext(A, 0, 0, 1, imm);
            else a64_ext(A, 0, 1, 0, 16u - imm);
        } else {
            const unsigned es = op == 0x71u ? 16u : op == 0x72u ? 32u : 64u;
            if (imm == 0u) break;
            if (sub == 4u) {
                a64_vsshr(A, es, 0, 0, imm > es ? es : imm);
            } else if (imm >= es) {
                a64_stp(A, ZR, ZR, RC, OFF_X(E));
                break;
            } else if (sub == 2u) {
                a64_vushr(A, es, 0, 0, imm);
            } else {
                a64_vshl_imm(A, es, 0, 0, imm);
            }
        }
        xst(x, 16, 0, E, 0);
        break;
    }
    case SP_PINSRW: {
        unsigned v = HR[E < 16u ? E : 0];
        if (!reg) {
            m = ea_mem(x, d, 2);
            emit_load(x, 2, A64_LDZ, S0, &m);
            v = S0;
        }
        a64_ls_imm(A, 2, A64_ST, v, RC, OFF_X(G) + 2u * (imm & 7u));
        break;
    }
    case SP_PEXTRW:
        a64_ls_imm(A, 2, A64_LDZ, HR[G], RC, OFF_X(E) + 2u * (imm & 7u));
        break;
    case SP_SHUFP:
        xld(x, 16, 0, G, 0);
        a64_vmov(A, 2, 0);
        sse_src(x, d, 1, 16);
        if (pf == 0u) {
            a64_ins_elt(A, 32, 0, 0, 2, imm & 3u);
            a64_ins_elt(A, 32, 0, 1, 2, (imm >> 2) & 3u);
            a64_ins_elt(A, 32, 0, 2, 1, (imm >> 4) & 3u);
            a64_ins_elt(A, 32, 0, 3, 1, (imm >> 6) & 3u);
        } else {
            a64_ins_elt(A, 64, 0, 0, 2, imm & 1u);
            a64_ins_elt(A, 64, 0, 1, 1, (imm >> 1) & 1u);
        }
        xst(x, 16, 0, G, 0);
        break;
    case SP_PMOVMSKB: {
        uint32_t enc = 0;
        a64_ldp(A, X16, X17, RC, OFF_X(E));
        (void)a64_logical_imm(0x8080808080808080ULL, true, &enc);
        a64_logic_imm_enc(A, true, A64_AND, X16, X16, enc);
        a64_logic_imm_enc(A, true, A64_AND, X17, X17, enc);
        mov_imm(x, S0, 0x0002040810204081ULL);
        a64_mul(A, true, X16, X16, S0);
        a64_mul(A, true, X17, X17, S0);
        a64_lsr_imm(A, true, X16, X16, 56);
        a64_lsr_imm(A, true, X17, X17, 56);
        a64_logic(A, false, A64_L_ORR, HR[G], X16, X17, A64_LSL, 8);
        break;
    }
    case SP_MOVMSK:
        a64_ldp(A, X16, X17, RC, OFF_X(E));
        if (pf == 0u) {
            a64_ubfx(A, true, S0, X16, 31, 1);
            a64_lsr_imm(A, true, S1, X16, 63);
            a64_logic(A, true, A64_L_ORR, S0, S0, S1, A64_LSL, 1);
            a64_ubfx(A, true, S1, X17, 31, 1);
            a64_logic(A, true, A64_L_ORR, S0, S0, S1, A64_LSL, 2);
            a64_lsr_imm(A, true, S1, X17, 63);
            a64_logic(A, true, A64_L_ORR, HR[G], S0, S1, A64_LSL, 3);
        } else {
            a64_lsr_imm(A, true, S0, X16, 63);
            a64_lsr_imm(A, true, S1, X17, 63);
            a64_logic(A, true, A64_L_ORR, HR[G], S0, S1, A64_LSL, 1);
        }
        break;
    case SP_FARITH: case SP_FSQRT: {
        const bool scalar = pf >= 2u, dbl = pf == 1u || pf == 2u;
        const unsigned fop = op == 0x58u ? A64_FADD : op == 0x59u ? A64_FMUL :
                             op == 0x5Cu ? A64_FSUB : A64_FDIV;
        static const uint32_t k_vop[4] = { A64_V_FADD, A64_V_FMUL, A64_V_FSUB, A64_V_FDIV };
        const unsigned sz = scalar ? (dbl ? 8u : 4u) : 16u;
        if (op != 0x51u) xld(x, sz, 0, G, 0);
        sse_src(x, d, 1, sz);
        if (scalar) {
            if (op == 0x51u) a64_fsqrt(A, dbl, 0, 1);
            else a64_fp2(A, dbl, fop, 0, 0, 1);
        } else if (op == 0x51u) {
            v2op(x, V_FSQRT | (dbl ? (1u << 22) : 0u), 0, 1);
        } else {
            a64_vf3(A, k_vop[fop == A64_FADD ? 0 : fop == A64_FMUL ? 1 : fop == A64_FSUB ? 2 : 3],
                    dbl, 0, 0, 1);
        }
        sse_nan_check(x, d, !scalar, dbl, 0, &slow);
        xst(x, sz, 0, G, 0);
        break;
    }
    case SP_FMINMAX: {
        const bool scalar = pf >= 2u, dbl = pf == 1u || pf == 2u, max = op == 0x5Fu;
        const unsigned sz = scalar ? (dbl ? 8u : 4u) : 16u;
        xld(x, sz, 0, G, 0);
        sse_src(x, d, 1, sz);
        if (scalar) {
            a64_put(A, (dbl ? 0x1E602010u : 0x1E202010u) | (1u << 16));     /* fcmpe v0, v1 */
            a64_fcsel(A, dbl, 0, 0, 1, max ? A64_GT : A64_MI);
            xst(x, sz, 0, G, 0);
        } else {
            if (max) a64_vf3(A, A64_V_FCMGT, dbl, 2, 0, 1);
            else a64_vf3(A, A64_V_FCMGT, dbl, 2, 1, 0);
            a64_v3(A, A64_V_BSL, 2, 0, 1);
            xst(x, 16, 2, G, 0);
        }
        break;
    }
    case SP_COMIS: {
        const bool dbl = pf == 1u;
        const unsigned sz = dbl ? 8u : 4u;
        xld(x, sz, 0, G, 0);
        sse_src(x, d, 1, sz);
        a64_put(A, (dbl ? 0x1E602000u : 0x1E202000u) | (op == 0x2Fu ? 0x10u : 0u) | (1u << 16));
        if (!d->live || d->need == 0u) {
            x->pf.kind = PK_NONE;
            break;
        }
        /* ZF PF CF: unordered 111, less 001, equal 100, greater 000 */
        a64_cset(A, false, X16, A64_VS);
        a64_cset(A, false, X17, A64_EQ);
        a64_logic(A, false, A64_L_ORR, X17, X17, X16, 0, 0);
        a64_cset(A, false, FR, A64_LT);
        a64_logic(A, false, A64_L_ORR, FR, FR, X16, A64_LSL, 2);
        a64_logic(A, false, A64_L_ORR, FR, FR, X17, A64_LSL, 6);
        pf_t np = { PK_EFL, 8, LOC_NONE, LOC_NONE, FR, LOC_NONE, { 0, 0 }, 0 };
        x->pf = np;
        break;
    }
    case SP_CVTSI2F: {          /* CVTSI2SS/SD */
        const bool dbl = pf == 2u;
        unsigned r = HR[E < 16u ? E : 0];
        if (!reg) {
            m = ea_mem(x, d, w64 ? 8u : 4u);
            emit_load(x, w64 ? 8u : 4u, A64_LDZ, S0, &m);
            r = S0;
        }
        a64_scvtf(A, dbl, w64, 0, r);
        xst(x, dbl ? 8u : 4u, 0, G, 0);
        break;
    }
    case SP_CVTF2SI: {          /* CVT(T)SS/SD2SI */
        const bool dbl = pf == 2u;
        sse_src(x, d, 0, dbl ? 8u : 4u);
        if (op == 0x2Du) a64_frintx(A, dbl, 0, 0);
        a64_fcvtzs(A, dbl, w64, S0, 0);
        /* out of range or NaN: the "integer indefinite" 0x80..0 */
        mov_imm(x, S1, w64 ? 0x8000000000000000ULL : 0x80000000ULL);
        a64_addsub_imm(A, w64, false, true, ZR, S0, 1, false);      /* V: S0 == INT_MAX */
        a64_csel(A, w64, S0, S1, S0, A64_VS);
        a64_fcmp(A, dbl, 0, 0);
        a64_csel(A, w64, HR[G], S1, S0, A64_VS);
        break;
    }
    case SP_CVTFF:
        if (pf == 3u) {
            sse_src(x, d, 1, 4);
            a64_fcvt_d_s(A, 0, 1);
            xst(x, 8, 0, G, 0);
        } else {
            sse_src(x, d, 1, 8);
            a64_fcvt_s_d(A, 0, 1);
            xst(x, 4, 0, G, 0);
        }
        break;
    case SP_CVTDQ2PS:
        sse_src(x, d, 1, 16);
        v2op(x, V_SCVTF, 0, 1);
        xst(x, 16, 0, G, 0);
        break;
    case SP_CVTDQ2PD:
        sse_src(x, d, 1, 8);
        v2op(x, V_SSHLL_2D, 1, 1);
        v2op(x, V_SCVTF | (1u << 22), 0, 1);
        xst(x, 16, 0, G, 0);
        break;
    case SP_CMPF: {
        const bool scalar = pf >= 2u, dbl = pf == 1u || pf == 2u;
        const unsigned sz = scalar ? (dbl ? 8u : 4u) : 16u, p = imm & 7u;
        xld(x, sz, 0, G, 0);
        sse_src(x, d, 1, sz);
        switch (p & 3u) {
        case 0: a64_vf3(A, A64_V_FCMEQ, dbl, 2, 0, 1); break;
        case 1: a64_vf3(A, A64_V_FCMGT, dbl, 2, 1, 0); break;
        case 2: a64_vf3(A, A64_V_FCMGE, dbl, 2, 1, 0); break;
        default:    /* ordered */
            a64_vf3(A, A64_V_FCMEQ, dbl, 2, 0, 0);
            a64_vf3(A, A64_V_FCMEQ, dbl, 3, 1, 1);
            a64_v3(A, A64_V_AND, 2, 2, 3);
            break;
        }
        /* 3 UNORD and 4..6 are the complements of 7 ORD and 0..2 */
        if (p == 3u || (p >= 4u && p <= 6u)) v2op(x, V_NOT, 2, 2);
        xst(x, sz, 2, G, 0);
        break;
    }
    default:
        e_sse(x, d);
        return;
    }
    if (slow != NULL) {
        slow->back = x->a.p;
    }
}

/* ---- integer instructions ------------------------------------------------------------------------ */

/* rd = rn <op> rm/imm, sized (sf: 64-bit); op as x86 0..6 */
static void alu_op(jx_t *x, unsigned op, bool sf, unsigned rd, unsigned rn, unsigned rm, uint64_t imm)
{
    uint32_t enc = 0;
    if (rm == LOC_IMM) {
        const uint64_t v = sf ? imm : (uint32_t)imm;
        switch (op) {
        case 0: case 5: {
            const bool sub = op == 5u;
            const uint64_t nv = sf ? (uint64_t)0 - v : (uint32_t)(0u - (uint32_t)v);
            if (a64_imm12_ok(v)) { addsub_imm12(x, sf, sub, false, rd, rn, v); return; }
            if (a64_imm12_ok(nv)) { addsub_imm12(x, sf, !sub, false, rd, rn, nv); return; }
            break;
        }
        case 1: case 4: case 6:
            if (a64_logical_imm(v, sf, &enc) == 0) {
                a64_logic_imm_enc(A, sf, op == 1u ? A64_ORR : op == 4u ? A64_AND : A64_EOR, rd, rn, enc);
                return;
            }
            break;
        default:
            break;
        }
        a64_mov_imm(A, sf, X17, v);
        rm = X17;
    }
    switch (op) {
    case 0: a64_addsub(A, sf, false, false, rd, rn, rm, 0, 0); break;
    case 1: a64_logic(A, sf, A64_L_ORR, rd, rn, rm, 0, 0); break;
    case 2: a64_adc(A, sf, false, false, rd, rn, rm); break;
    case 3: a64_adc(A, sf, true, false, rd, rn, rm); break;
    case 4: a64_logic(A, sf, A64_L_AND, rd, rn, rm, 0, 0); break;
    case 5: a64_addsub(A, sf, true, false, rd, rn, rm, 0, 0); break;
    default: a64_logic(A, sf, A64_L_EOR, rd, rn, rm, 0, 0); break;
    }
}

static void e_alu(jx_t *x, const ji_t *d)
{
    const unsigned op = d->op, sz = d->sz, k = d->kind;
    const bool sf = sz == 8u, wide = sz >= 4u, rex = d->rex != 0u;
    const bool writes = op != 7u && op != 8u;
    const bool regdst = k == K_ALU_RR || k == K_ALU_RI || k == K_ALU_RM;
    const bool live = d->live && d->need != 0u;
    unsigned need = live ? d->need : 0u;
    const unsigned pk = pk_of(d);
    mref_t mr = { X16, MR_NOIDX, 0, 0 };

    /* the carry in stays in S4 until the last memory access is done: a
     * fault there must still find the previous flags where they were */
    if (op == 2u || op == 3u) {
        emit_cf(x, S4);
    }
    if (op == 7u && (need & N_R)) {
        need = (need & ~(unsigned)N_R) | N_A | N_B;
    }

    unsigned ra;
    if (regdst) {
        ra = rd_reg(x, d->dst, sz, rex, S0);
    } else {
        mr = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &mr);
        ra = S0;
    }
    unsigned rb = LOC_IMM;
    uint64_t bimm = 0;
    if (k == K_ALU_RR || k == K_ALU_MR) {
        rb = rd_reg(x, d->src, sz, rex, S1);
    } else if (k == K_ALU_RM) {
        mref_t m2 = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S1, &m2);
        rb = S1;
    } else {
        bimm = d->imm & szmask((int)sz);
    }
    const bool same = k == K_ALU_RR && d->dst == d->src;

    pf_t np = { (uint8_t)pk, (uint8_t)sz, LOC_NONE, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    if (pk == PK_TEST && same) {
        np.kind = PK_LOGIC;
    }
    /* a register destination: keep a and b before the result overwrites
     * them (there is no memory access after this); a memory one: after the
     * store */
    unsigned rr = LOC_NONE;
    if (!regdst && writes) {
        rr = S2;
        if (op == 2u) a64_addsub_imm(A, false, true, true, ZR, S4, 1, false);   /* C = carry in */
        else if (op == 3u) a64_addsub(A, false, true, true, ZR, ZR, S4, 0, 0); /* C = !borrow in */
        alu_op(x, op, wide ? sf : false, rr, ra, rb, bimm);
        emit_store(x, sz, rr, &mr);
    }
    if (np.kind != PK_LOGIC) {
        if (need & N_A) {
            const bool afree = regdst && !writes && !is_hi8(d->dst, sz, rex) && gfree(d, d->dst);
            if (afree) {
                np.la = (uint8_t)ra;
            } else {
                a64_mov(A, true, FA, ra);
                np.la = FA;
            }
        }
        if (need & N_B) {
            if (rb == LOC_IMM) {
                np.lb = LOC_IMM;
                np.imm = bimm;
            } else {
                const bool bfree = (k == K_ALU_RR || k == K_ALU_MR) && !is_hi8(d->src, sz, rex) &&
                                   gfree(d, d->src) && !(writes && regdst && d->src == d->dst);
                if (bfree) {
                    np.lb = (uint8_t)rb;
                } else {
                    a64_mov(A, true, FB, rb);
                    np.lb = FB;
                }
            }
        }
    }

    if (regdst && writes) {
        rr = wide ? HR[d->dst] : S2;
        if (op == 2u) a64_addsub_imm(A, false, true, true, ZR, S4, 1, false);
        else if (op == 3u) a64_addsub(A, false, true, true, ZR, ZR, S4, 0, 0);
        alu_op(x, op, wide ? sf : false, rr, ra, rb, bimm);
        if (!(wide && rr == HR[d->dst])) {
            wr_reg(x, d->dst, sz, rex, rr);
        }
    }
    if (op == 2u || op == 3u) {
        a64_mov(A, true, FC, S4);
    }

    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    if (np.kind == PK_LOGIC) {
        if (pk == PK_TEST) {
            if (!is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
                np.lr = (uint8_t)ra;
            } else {
                a64_mov(A, true, FR, ra);
                np.lr = FR;
            }
        } else if (regdst && !is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
            np.lr = HR[d->dst];
        } else {
            a64_mov(A, true, FR, rr);
            np.lr = FR;
        }
    } else if (pk == PK_ADD || pk == PK_SUB) {
        if (writes && regdst && !is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
            np.lr = HR[d->dst];
        } else if (writes && (need & N_R)) {
            a64_mov(A, true, FR, rr);
            np.lr = FR;
        }
    } else if (pk == PK_ADC || pk == PK_SBB) {
        np.lc = FC;
    }
    x->pf = np;
}

static void e_mov(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool rex = d->rex != 0u;
    mref_t m;
    switch (d->kind) {
    case K_MOV_RR:
        wr_reg(x, d->dst, sz, rex, rd_reg(x, d->src, sz, rex, S0));
        break;
    case K_MOV_RI:
        if (sz == 8u) {
            mov_imm(x, HR[d->dst], d->imm);
        } else if (sz == 4u) {
            a64_mov_imm(A, false, HR[d->dst], d->imm);
        } else {
            a64_mov_imm(A, false, S0, d->imm);
            wr_reg(x, d->dst, sz, rex, S0);
        }
        break;
    case K_MOV_LD:
        m = ea_mem(x, d, sz);
        if (sz >= 4u) {
            emit_load(x, sz, A64_LDZ, HR[d->dst], &m);
        } else {
            emit_load(x, sz, A64_LDZ, S0, &m);
            wr_reg(x, d->dst, sz, rex, S0);
        }
        break;
    case K_MOV_ST: {
        const unsigned v = rd_reg(x, d->src, sz, rex, S0);
        m = ea_mem(x, d, sz);
        emit_store(x, sz, v, &m);
        break;
    }
    default: {   /* K_MOV_MI */
        const uint64_t v = d->imm & szmask((int)sz);
        unsigned r = ZR;
        if (v != 0u) {
            mov_imm(x, S0, v);
            r = S0;
        }
        m = ea_mem(x, d, sz);
        emit_store(x, sz, r, &m);
        break;
    }
    }
}

static void e_lea(jx_t *x, const ji_t *d)
{
    if (d->sz == 8u) {
        emit_addr(x, d, HR[d->dst]);
    } else {
        emit_addr(x, d, X16);
        wr_reg(x, d->dst, d->sz, d->rex != 0u, X16);
    }
}

static void e_movx(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz, ssz = d->op;
    const bool sgn = d->imm != 0u, rex = d->rex != 0u;
    const unsigned dst = sz >= 4u ? HR[d->dst] : S1;
    if (d->kind == K_MOVX_R) {
        const unsigned v = rd_reg(x, d->src, ssz, rex, S0);
        if (ssz == 4u) {
            if (sgn) a64_sxt(A, true, dst, v, 32);
            else a64_mov(A, false, dst, v);
        } else if (sgn) {
            a64_sxt(A, sz == 8u, dst, v, 8u * ssz);
        } else {
            a64_uxt(A, dst, v, 8u * ssz);
        }
    } else {
        mref_t m = ea_mem(x, d, ssz);
        unsigned kind = A64_LDZ;
        if (sgn) kind = (sz == 8u || ssz == 4u) ? A64_LDSX : A64_LDSW;
        emit_load(x, ssz, kind, dst, &m);
    }
    if (sz < 4u) {
        wr_reg(x, d->dst, sz, rex, S1);
    }
}

static void e_push_pop(jx_t *x, const ji_t *d)
{
    const unsigned sp = HR[RSP];
    switch (d->kind) {
    case K_PUSH_R:
        if (d->src == RSP) {
            a64_addsub_imm(A, true, true, false, X16, sp, 8, false);
            if (x->mt) a64_dmb_ishst(A);
            site(x);
            a64_ls_imm(A, 8, A64_ST, sp, X16, 0);
            a64_mov(A, true, sp, X16);
        } else {
            if (x->mt) a64_dmb_ishst(A);
            site(x);
            a64_ls_wb(A, 8, A64_ST, HR[d->src], sp, -8, true);
        }
        break;
    case K_PUSH_I: {
        unsigned r = ZR;
        if (d->imm != 0u) {
            mov_imm(x, S0, d->imm);
            r = S0;
        }
        if (x->mt) a64_dmb_ishst(A);
        site(x);
        a64_ls_wb(A, 8, A64_ST, r, sp, -8, true);
        break;
    }
    default:   /* K_POP_R */
        if (d->dst == RSP) {
            site(x);
            a64_ls_imm(A, 8, A64_LDZ, X16, sp, 0);
            if (x->mt) a64_dmb_ishld(A);
            a64_mov(A, true, sp, X16);
        } else {
            site(x);
            a64_ls_wb(A, 8, A64_LDZ, HR[d->dst], sp, 8, false);
            if (x->mt) a64_dmb_ishld(A);
        }
        break;
    }
}

static void push_imm(jx_t *x, uint64_t v)
{
    mov_imm(x, S0, v);
    if (x->mt) a64_dmb_ishst(A);
    site(x);
    a64_ls_wb(A, 8, A64_ST, S0, HR[RSP], -8, true);
}

static void e_incdec(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool rex = d->rex != 0u, dec = d->op != 0u;
    const bool live = d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    unsigned rr;
    if (need & N_C) {
        emit_cf(x, S4);     /* into FC only after the store (see e_alu) */
    }
    if (d->kind == K_INCDEC_R) {
        if (sz >= 4u) {
            a64_addsub_imm(A, sz == 8u, dec, false, HR[d->dst], HR[d->dst], 1, false);
            rr = HR[d->dst];
        } else {
            const unsigned v = rd_reg(x, d->dst, sz, rex, S0);
            a64_addsub_imm(A, false, dec, false, S2, v, 1, false);
            wr_reg(x, d->dst, sz, rex, S2);
            rr = S2;
        }
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &m);
        a64_addsub_imm(A, sz == 8u, dec, false, S2, S0, 1, false);
        emit_store(x, sz, S2, &m);
        rr = S2;
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    pf_t np = { (uint8_t)(dec ? PK_DEC : PK_INC), (uint8_t)sz, LOC_NONE, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    if (d->kind == K_INCDEC_R && !is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
        np.lr = HR[d->dst];
    } else {
        a64_mov(A, true, FR, rr);
        np.lr = FR;
    }
    if (need & N_C) {
        a64_mov(A, true, FC, S4);
        np.lc = FC;
    }
    x->pf = np;
}

static void e_notneg(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool rex = d->rex != 0u, wide = sz >= 4u, sf = sz == 8u;
    const bool neg = d->kind == K_NEG_R || d->kind == K_NEG_M;
    const bool reg = d->kind == K_NOT_R || d->kind == K_NEG_R;
    const bool live = neg && d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    mref_t m = { X16, MR_NOIDX, 0, 0 };
    unsigned v, rr;
    if (reg) {
        v = rd_reg(x, d->dst, sz, rex, S0);
    } else {
        m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &m);
        v = S0;
    }
    if ((need & N_B) && reg) {
        a64_mov(A, true, FB, v);
    }
    rr = (reg && wide) ? HR[d->dst] : S2;
    if (neg) {
        a64_addsub(A, wide ? sf : false, true, false, rr, ZR, v, 0, 0);
    } else {
        a64_logic(A, wide ? sf : false, A64_L_ORN, rr, ZR, v, 0, 0);
    }
    if (reg) {
        if (!wide) wr_reg(x, d->dst, sz, rex, rr);
    } else {
        emit_store(x, sz, rr, &m);
        if (need & N_B) a64_mov(A, true, FB, v);
    }
    if (!neg) {
        return;
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    pf_t np = { PK_SUB, (uint8_t)sz, ZR, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    if (need & N_B) np.lb = FB;
    if (reg && !is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
        np.lr = HR[d->dst];
    } else if (need & N_R) {
        a64_mov(A, true, FR, rr);
        np.lr = FR;
    }
    x->pf = np;
}

static void e_shift_ri(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz, bits = 8u * sz, cnt = (unsigned)d->imm, kind = d->op;
    const bool rex = d->rex != 0u, wide = sz >= 4u, sf = sz == 8u;
    const bool live = kind >= 2u && d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    const bool rotfl = kind < 2u && d->fw == FW_FULL;
    if (rotfl) {
        flags_to_fr(x);     /* ROL/ROR change CF and OF only */
    }
    const unsigned v = rd_reg(x, d->dst, sz, rex, S0);
    const unsigned rr = wide ? HR[d->dst] : S2;
    if (need & N_A) {
        a64_mov(A, true, FA, v);
    }
    switch (kind) {
    case 0:     /* ROL (wide only) */
        if (cnt % bits == 0u) a64_mov(A, sf, rr, v);
        else a64_ror_imm(A, sf, rr, v, bits - cnt % bits);
        break;
    case 1:     /* ROR */
        if (cnt % bits == 0u) a64_mov(A, sf, rr, v);
        else a64_ror_imm(A, sf, rr, v, cnt % bits);
        break;
    case 5:     /* SHR */
        if (wide) a64_lsr_imm(A, sf, rr, v, cnt);
        else if (cnt >= bits) a64_movz(A, false, rr, 0, 0);
        else a64_ubfx(A, false, rr, v, cnt, bits - cnt);
        break;
    case 7: {   /* SAR */
        if (wide) {
            a64_asr_imm(A, sf, rr, v, cnt);
        } else {
            const unsigned c = cnt > bits - 1u ? bits - 1u : cnt;
            a64_sbfx(A, false, rr, v, c, bits - c);
        }
        break;
    }
    default:    /* SHL/SAL */
        a64_lsl_imm(A, wide ? sf : false, rr, v, cnt);
        break;
    }
    if (!wide) {
        wr_reg(x, d->dst, sz, rex, rr);
    }
    if (rotfl) {
        /* ROL: CF = r<0>, OF = r<msb> ^ CF; ROR: CF = r<msb>, OF = r<msb> ^ r<msb-1> */
        if (kind == 0u) {
            a64_ubfx(A, true, X16, rr, 0, 1);
            a64_ubfx(A, true, X17, rr, bits - 1u, 1);
            a64_logic(A, false, A64_L_EOR, X17, X17, X16, 0, 0);
        } else {
            a64_ubfx(A, true, X16, rr, bits - 1u, 1);
            a64_ubfx(A, true, X17, rr, bits - 2u, 1);
            a64_logic(A, false, A64_L_EOR, X17, X17, X16, 0, 0);
        }
        a64_bfi(A, true, FR, X16, 0, 1);
        a64_bfi(A, true, FR, X17, 11, 1);
        if (d->live && d->need != 0u) {
            pf_t np = { PK_EFL, 8, LOC_NONE, LOC_NONE, FR, LOC_NONE, { 0, 0 }, 0 };
            x->pf = np;
        } else {
            x->pf.kind = PK_NONE;
        }
        return;
    }
    if (kind < 2u) {
        return;
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    pf_t np = { (uint8_t)pk_of(d), (uint8_t)sz, LOC_NONE, LOC_IMM, LOC_NONE, LOC_NONE, { 0, 0 }, cnt };
    if (need & N_A) np.la = FA;
    if (!is_hi8(d->dst, sz, rex) && gfree(d, d->dst)) {
        np.lr = HR[d->dst];
    } else if (need & N_R) {
        a64_mov(A, true, FR, rr);
        np.lr = FR;
    }
    x->pf = np;
}

/* shift by CL, flags not needed */
static void e_shift_rc(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz, kind = d->op;
    const bool rex = d->rex != 0u;
    const unsigned dst = HR[d->dst], cl = HR[RCX];
    uint32_t enc31 = 0;
    (void)a64_logical_imm(31, false, &enc31);
    static const unsigned k_op[8] = { A64_RORV, A64_RORV, 0, 0, A64_LSLV, A64_LSRV, A64_LSLV, A64_ASRV };
    if (sz == 8u) {
        unsigned cnt = cl;
        if (kind == 0u) {
            a64_addsub(A, true, true, false, X17, ZR, cl, 0, 0);
            cnt = X17;
        }
        a64_dp2(A, true, k_op[kind], dst, dst, cnt);
        return;
    }
    if (sz == 4u) {
        /* a zero count leaves the register as it was (no zero extension) */
        a64_logic_imm_enc(A, false, A64_ANDS, X17, cl, enc31);
        unsigned cnt = X17;
        if (kind == 0u) {
            a64_addsub(A, false, true, false, X16, ZR, X17, 0, 0);
            cnt = X16;
        }
        a64_dp2(A, false, k_op[kind], X16, dst, cnt);
        a64_csel(A, true, dst, X16, dst, A64_NE);
        return;
    }
    const unsigned bits = 8u * sz;
    a64_logic_imm_enc(A, false, A64_ANDS, X17, cl, enc31);
    uint32_t *skip = x->a.p;
    a64_bcond(A, A64_EQ, x->a.p);
    const unsigned v = rd_reg(x, d->dst, sz, rex, S0);
    if (kind == 5u) {
        a64_uxt(A, S2, v, bits);
        a64_dp2(A, false, A64_LSRV, X16, S2, X17);
    } else if (kind == 7u) {
        a64_sxt(A, false, S2, v, bits);
        a64_dp2(A, false, A64_ASRV, X16, S2, X17);
    } else {
        a64_dp2(A, false, A64_LSLV, X16, v, X17);
    }
    wr_reg(x, d->dst, sz, rex, X16);
    a64_patch_b(skip, x->a.p);
}

static void e_imul(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool rex = d->rex != 0u;
    const bool live = d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    unsigned ra, rb;
    switch (d->kind) {
    case K_IMUL_RR:
        ra = rd_reg(x, d->dst, sz, rex, S0);
        rb = rd_reg(x, d->src, sz, rex, S1);
        break;
    case K_IMUL_RM: {
        ra = rd_reg(x, d->dst, sz, rex, S0);
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S1, &m);
        rb = S1;
        break;
    }
    case K_IMUL_RRI:
        ra = rd_reg(x, d->src, sz, rex, S0);
        mov_imm(x, S1, d->imm);
        rb = S1;
        break;
    default: {  /* K_IMUL_RMI */
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &m);
        ra = S0;
        mov_imm(x, S1, d->imm);
        rb = S1;
        break;
    }
    }
    if (sz == 8u) {
        a64_mul(A, true, S2, ra, rb);
        if (need & N_B) {
            a64_smulh(A, S3, ra, rb);
            a64_addsub(A, true, true, true, ZR, S3, S2, A64_ASR, 63);
            a64_cset(A, true, FB, A64_NE);
        }
        a64_mov(A, true, HR[d->dst], S2);
    } else if (sz == 4u) {
        a64_smull(A, S2, ra, rb);
        if (need & N_B) {
            a64_sxt(A, true, S3, S2, 32);
            a64_cmp(A, true, S2, S3);
            a64_cset(A, true, FB, A64_NE);
        }
        a64_mov(A, false, HR[d->dst], S2);
    } else {
        a64_sxt(A, false, S2, ra, 16);
        a64_sxt(A, false, S3, rb, 16);
        a64_mul(A, false, S2, S2, S3);
        if (need & N_B) {
            a64_sxt(A, false, S3, S2, 16);
            a64_cmp(A, false, S2, S3);
            a64_cset(A, true, FB, A64_NE);
        }
        wr_reg(x, d->dst, 2, rex, S2);
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    pf_t np = { PK_MUL, (uint8_t)sz, LOC_NONE, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    if (need & N_B) np.lb = FB;
    if (gfree(d, d->dst)) {
        np.lr = HR[d->dst];
    } else {
        a64_mov(A, true, FR, S2);
        np.lr = FR;
    }
    x->pf = np;
}

static void e_mul1(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool sgn = d->op == 5u;
    const bool live = d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    unsigned v;
    if (d->kind == K_MUL1_R) {
        v = HR[d->src];
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &m);
        v = S0;
    }
    const unsigned rax = HR[RAX], rdx = HR[RDX];
    if (sz == 8u) {
        a64_mul(A, true, S2, rax, v);
        if (sgn) a64_smulh(A, S3, rax, v);
        else a64_umulh(A, S3, rax, v);
        if (need & N_B) {
            if (sgn) a64_addsub(A, true, true, true, ZR, S3, S2, A64_ASR, 63);
            else a64_addsub_imm(A, true, true, true, ZR, S3, 0, false);
            a64_cset(A, true, FB, A64_NE);
        }
        a64_mov(A, true, rax, S2);
        a64_mov(A, true, rdx, S3);
    } else {
        if (sgn) a64_smull(A, S2, rax, v);
        else a64_umull(A, S2, rax, v);
        a64_lsr_imm(A, true, S3, S2, 32);
        if (need & N_B) {
            if (sgn) {
                a64_sxt(A, true, X16, S2, 32);
                a64_cmp(A, true, S2, X16);
            } else {
                a64_addsub_imm(A, true, true, true, ZR, S3, 0, false);
            }
            a64_cset(A, true, FB, A64_NE);
        }
        a64_mov(A, false, rax, S2);
        a64_mov(A, false, rdx, S3);
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    pf_t np = { PK_MUL, (uint8_t)sz, LOC_NONE, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    if (need & N_B) np.lb = FB;
    if (gfree(d, RAX)) {
        np.lr = HR[RAX];
    } else {
        a64_mov(A, true, FR, S2);
        np.lr = FR;
    }
    x->pf = np;
}

/* DIV/IDIV r/m32, r/m64: the common cases natively; the rest (a divide
 * error, a 128-bit dividend) leave the block and the interpreter runs the
 * instruction */
static void e_div1(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool sgn = d->op == 7u;
    const unsigned rax = HR[RAX], rdx = HR[RDX];
    unsigned v;
    if (d->kind == K_DIV1_R) {
        v = HR[d->src];
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S0, &m);
        v = S0;
    }
    if (sz == 8u) {
        if (sgn) {
            a64_addsub(A, true, true, true, ZR, rdx, rax, A64_ASR, 63);
            (void)br_cold(x, A64_NE, CS_STEPEXIT);
        } else {
            (void)cbz_cold(x, true, true, rdx, CS_STEPEXIT);
        }
        (void)cbz_cold(x, true, false, v, CS_STEPEXIT);
        if (sgn) {
            /* INT64_MIN / -1 overflows */
            mov_imm(x, X17, 0x8000000000000000ULL);
            a64_addsub_imm(A, true, false, true, ZR, v, 1, false);       /* cmn v, #1 */
            a64_ccmp(A, true, rax, X17, 0u, A64_EQ);    /* v == -1: rax == INT64_MIN? */
            (void)br_cold(x, A64_EQ, CS_STEPEXIT);
            a64_dp2(A, true, A64_SDIV, S2, rax, v);
        } else {
            a64_dp2(A, true, A64_UDIV, S2, rax, v);
        }
        a64_msub(A, true, S3, S2, v, rax);
        a64_mov(A, true, rax, S2);
        a64_mov(A, true, rdx, S3);
    } else {
        (void)cbz_cold(x, false, false, v, CS_STEPEXIT);
        if (sgn) a64_sxt(A, true, S1, v, 32);
        else a64_mov(A, false, S1, v);
        a64_lsl_imm(A, true, X16, rdx, 32);
        a64_mov(A, false, S3, rax);
        a64_logic(A, true, A64_L_ORR, X16, X16, S3, 0, 0);
        if (sgn) {
            a64_dp2(A, true, A64_SDIV, S2, X16, S1);
            a64_sxt(A, true, X17, S2, 32);
            a64_cmp(A, true, X17, S2);
            (void)br_cold(x, A64_NE, CS_STEPEXIT);
        } else {
            a64_dp2(A, true, A64_UDIV, S2, X16, S1);
            a64_lsr_imm(A, true, X17, S2, 32);
            (void)cbz_cold(x, true, true, X17, CS_STEPEXIT);
        }
        a64_msub(A, true, S3, S2, S1, X16);
        a64_mov(A, false, rax, S2);
        a64_mov(A, false, rdx, S3);
    }
}

static void e_cdq(jx_t *x, const ji_t *d)
{
    const unsigned rax = HR[RAX], rdx = HR[RDX], sz = d->sz;
    if (d->op == 0u) {
        if (sz == 8u) a64_sxt(A, true, rax, rax, 32);
        else if (sz == 4u) a64_sxt(A, false, rax, rax, 16);
        else {
            a64_sxt(A, false, S0, rax, 8);
            wr_reg(x, RAX, 2, false, S0);
        }
    } else {
        if (sz == 8u) a64_asr_imm(A, true, rdx, rax, 63);
        else if (sz == 4u) a64_asr_imm(A, false, rdx, rax, 31);
        else {
            a64_sbfx(A, false, S0, rax, 15, 1);
            wr_reg(x, RDX, 2, false, S0);
        }
    }
}

static void e_setcc(jx_t *x, const ji_t *d)
{
    const unsigned c = emit_cond(x, d->op);
    if (c == JC_TRUE) a64_movz(A, false, S0, 1, 0);
    else if (c == JC_FALSE) a64_movz(A, false, S0, 0, 0);
    else a64_cset(A, false, S0, c);
    wr_reg(x, d->dst, 1, d->rex != 0u, S0);
}

static void e_cmov(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const unsigned c = emit_cond(x, d->op);
    unsigned v;
    if (d->kind == K_CMOV_R) {
        v = HR[d->src];
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S1, &m);
        v = S1;
    }
    const unsigned dst = HR[d->dst];
    if (sz == 8u) {
        if (c == JC_TRUE) a64_mov(A, true, dst, v);
        else if (c != JC_FALSE) a64_csel(A, true, dst, v, dst, c);
    } else if (sz == 4u) {
        if (c == JC_TRUE) a64_mov(A, false, dst, v);
        else if (c == JC_FALSE) a64_mov(A, false, dst, dst);
        else a64_csel(A, false, dst, v, dst, c);
    } else {
        if (c == JC_TRUE) wr_reg(x, d->dst, 2, false, v);
        else if (c != JC_FALSE) {
            a64_csel(A, false, S2, v, dst, c);
            wr_reg(x, d->dst, 2, false, S2);
        }
    }
}

static void e_xchg(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool rex = d->rex != 0u;
    if (sz >= 4u) {
        const bool sf = sz == 8u;
        const unsigned ra = HR[d->dst], rb = HR[d->src];
        if (ra == rb) {
            if (!sf) a64_mov(A, false, ra, ra);
            return;
        }
        a64_mov(A, sf, X16, ra);
        a64_mov(A, sf, ra, rb);
        a64_mov(A, sf, rb, X16);
        return;
    }
    const unsigned va = rd_reg(x, d->dst, sz, rex, S0);
    if (va != S0) a64_mov(A, true, S0, va);
    const unsigned vb = rd_reg(x, d->src, sz, rex, S1);
    if (vb != S1) a64_mov(A, true, S1, vb);
    wr_reg(x, d->dst, sz, rex, S1);
    wr_reg(x, d->src, sz, rex, S0);
}

static void e_bsx(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool sf = sz == 8u;
    unsigned v;
    if (d->kind == K_BSX_R) {
        v = HR[d->src];
    } else {
        mref_t m = ea_mem(x, d, sz);
        emit_load(x, sz, A64_LDZ, S1, &m);
        v = S1;
    }
    if (d->fw == FW_FULL) {
        /* flags wanted: ZF = (source == 0), the rest undefined -- as a
         * logic op on the source */
        a64_mov(A, true, FR, v);
        if (d->live && d->need != 0u) {
            pf_t np = { PK_LOGIC, (uint8_t)sz, LOC_NONE, LOC_NONE, FR, LOC_NONE, { 0, 0 }, 0 };
            x->pf = np;
        } else {
            x->pf.kind = PK_NONE;
        }
        v = FR;
    }
    uint32_t *skip = x->a.p;
    a64_cbz(A, sf, false, v, x->a.p);
    if (d->op == 0u) {
        a64_dp1(A, sf, A64_RBIT, X16, v);
        a64_dp1(A, sf, A64_CLZ, X16, X16);
    } else {
        uint32_t enc = 0;
        a64_dp1(A, sf, A64_CLZ, X16, v);
        (void)a64_logical_imm(sf ? 63u : 31u, sf, &enc);
        a64_logic_imm_enc(A, sf, A64_EOR, X16, X16, enc);
    }
    a64_mov(A, sf, HR[d->dst], X16);
    a64_patch_b(skip, x->a.p);
}

static void e_bt(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz;
    const bool sf = sz == 8u;
    const unsigned dst = HR[d->dst];
    static const unsigned k_lop[4] = { 0, A64_L_ORR, A64_L_BIC, A64_L_EOR };
    if (d->fw == FW_FULL) {
        /* CF = the bit; ZF kept, OF SF AF PF undefined (kept) */
        flags_to_fr(x);
        if (d->kind == K_BT_RI) {
            a64_ubfx(A, true, X16, dst, d->imm & (sf ? 63u : 31u), 1);
        } else {
            a64_dp2(A, sf, A64_LSRV, X16, dst, HR[d->src]);
        }
        a64_bfi(A, true, FR, X16, 0, 1);
        if (d->live && d->need != 0u) {
            pf_t np = { PK_EFL, 8, LOC_NONE, LOC_NONE, FR, LOC_NONE, { 0, 0 }, 0 };
            x->pf = np;
        } else {
            x->pf.kind = PK_NONE;
        }
    }
    if (d->op == 0u) {
        return;             /* BT */
    }
    if (d->kind == K_BT_RI) {
        mov_imm(x, X17, 1ULL << (d->imm & (sf ? 63u : 31u)));
    } else {
        a64_movz(A, sf, X17, 1, 0);
        a64_dp2(A, sf, A64_LSLV, X17, X17, HR[d->src]);
    }
    a64_logic(A, sf, k_lop[d->op], dst, dst, X17, 0, 0);
}

static void e_setcc_m(jx_t *x, const ji_t *d)
{
    const unsigned c = emit_cond(x, d->op);
    if (c == JC_TRUE) a64_movz(A, false, S0, 1, 0);
    else if (c == JC_FALSE) a64_movz(A, false, S0, 0, 0);
    else a64_cset(A, false, S0, c);
    mref_t m = ea_mem(x, d, 1);
    emit_store(x, 1, S0, &m);
}

static void e_push_m(jx_t *x, const ji_t *d)
{
    mref_t m = ea_mem(x, d, 8);
    emit_load(x, 8, A64_LDZ, S0, &m);
    if (x->mt) a64_dmb_ishst(A);
    site(x);
    a64_ls_wb(A, 8, A64_ST, S0, HR[RSP], -8, true);
}

/* The memory operand's full address into X16; a misaligned one leaves the
 * block for the interpreter (exclusive accesses must be aligned). */
static void atom_addr(jx_t *x, const ji_t *d, unsigned sz)
{
    mref_t m = ea_mem(x, d, sz);
    if (m.index != MR_NOIDX) {
        a64_addsub(A, true, false, false, X16, m.base, m.index, A64_LSL, m.scaled ? a64_log2sz(sz) : 0u);
    } else {
        addi(x, X16, m.base, m.off);
    }
    if (sz > 1u) {
        uint32_t enc = 0;
        (void)a64_logical_imm(sz - 1u, true, &enc);
        a64_logic_imm_enc(A, true, A64_ANDS, ZR, X16, enc);
        br_cold(x, A64_NE, CS_STEPEXIT);
    }
}

/* LOCKed read-modify-write: LDAXR/STLXR, then a full barrier (x86 LOCK
 * orders everything) */
static void e_atomic(jx_t *x, const ji_t *d)
{
    const unsigned sz = d->sz, k = d->kind, op = d->op;
    const bool rex = d->rex != 0u, wide = sz >= 4u, sf = sz == 8u;
    const bool live = d->live && d->need != 0u;
    const unsigned need = live ? d->need : 0u;
    const unsigned ws = wide ? sf : false;
    bool carry = false;

    if (k == K_ATOM_ALU && (op == 2u || op == 3u)) {
        emit_cf(x, S4);
        carry = true;
    } else if (k == K_ATOM_INCDEC && (need & N_C)) {
        emit_cf(x, S4);
        carry = true;
    }
    /* the register operand (or immediate) into S1, the accumulator for
     * CMPXCHG into S3 */
    unsigned rb = S1;
    if (k == K_ATOM_ALU && d->src == NOREG) {
        mov_imm(x, S1, d->imm & szmask((int)sz));
    } else if (k != K_ATOM_INCDEC) {
        const unsigned v = rd_reg(x, d->src, sz, rex, S1);
        if (v != S1) a64_mov(A, true, S1, v);
    }
    if (k == K_CMPXCHG) {
        mask_into(x, S3, rd_reg(x, RAX, sz, rex, S3), sz);
    }
    atom_addr(x, d, sz);
    if (k == K_ATOM_ALU && op == 2u) a64_addsub_imm(A, false, true, true, ZR, S4, 1, false);
    if (k == K_ATOM_ALU && op == 3u) a64_addsub(A, false, true, true, ZR, ZR, S4, 0, 0);

    uint32_t *retry = x->a.p;
    site(x);
    a64_ldaxr(A, sz, S2, X16);
    uint32_t *fail = NULL;
    unsigned nv = S0;
    switch (k) {
    case K_ATOM_ALU:
        alu_op(x, op, ws, S0, S2, rb, 0);
        break;
    case K_ATOM_INCDEC:
        a64_addsub_imm(A, ws, op != 0u, false, S0, S2, 1, false);
        break;
    case K_XADD:
        a64_addsub(A, ws, false, false, S0, S2, S1, 0, 0);
        break;
    case K_XCHG_M:
        nv = S1;
        break;
    default:    /* K_CMPXCHG */
        a64_cmp(A, sz == 8u, S2, S3);
        fail = x->a.p;
        a64_bcond(A, A64_NE, x->a.p);
        nv = S1;
        break;
    }
    site(x);
    a64_stlxr(A, sz, X17, nv, X16);
    a64_cbz(A, false, true, X17, retry);
    if (fail != NULL) {
        uint32_t *done = x->a.p;
        a64_b(A, x->a.p);
        a64_patch_b(fail, x->a.p);
        a64_put(A, 0xD503305Fu);            /* CLREX */
        a64_patch_b(done, x->a.p);
    }
    a64_dmb_ish(A);

    /* registers (all memory accesses are done) */
    if (k == K_CMPXCHG) {
        /* NZCV still holds the comparison: on failure RAX = the old value */
        if (sz >= 4u) {
            a64_csel(A, true, HR[RAX], HR[RAX], S2, A64_EQ);
        } else {
            uint32_t *skip = x->a.p;
            a64_bcond(A, A64_EQ, x->a.p);
            a64_bfi(A, true, HR[RAX], S2, 0, 8u * sz);
            a64_patch_b(skip, x->a.p);
        }
    } else if (k == K_XADD || k == K_XCHG_M) {
        if (k == K_XADD && live) a64_mov(A, true, FB, S1);
        wr_reg(x, d->src, sz, rex, S2);
    }
    if (carry) {
        a64_mov(A, true, FC, S4);
    }
    if (k == K_XCHG_M) {
        return;
    }
    if (!live) {
        x->pf.kind = PK_NONE;
        return;
    }
    const unsigned pk = pk_of(d);
    pf_t np = { (uint8_t)pk, (uint8_t)sz, LOC_NONE, LOC_NONE, LOC_NONE, LOC_NONE, { 0, 0 }, 0 };
    switch (pk) {
    case PK_INC: case PK_DEC:
        a64_mov(A, true, FR, S0);
        np.lr = FR;
        if (need & N_C) np.lc = FC;
        break;
    case PK_LOGIC:
        a64_mov(A, true, FR, S0);
        np.lr = FR;
        break;
    default:
        if (k == K_CMPXCHG) {           /* CMP accumulator, old */
            a64_mov(A, true, FA, S3);
            a64_mov(A, true, FB, S2);
        } else {                        /* old op operand */
            a64_mov(A, true, FA, S2);
            if (k != K_XADD) a64_mov(A, true, FB, S1);
            a64_mov(A, true, FR, S0);
            np.lr = FR;
        }
        np.la = FA;
        np.lb = FB;
        if (pk == PK_ADC || pk == PK_SBB) np.lc = FC;
        break;
    }
    x->pf = np;
}

static void e_leave(jx_t *x)
{
    site(x);
    a64_ls_imm(A, 8, A64_LDZ, X16, HR[RBP], 0);
    if (x->mt) a64_dmb_ishld(A);
    a64_addsub_imm(A, true, false, false, HR[RSP], HR[RBP], 8, false);
    a64_mov(A, true, HR[RBP], X16);
}

static void e_jcc(jx_t *x, const ji_t *d)
{
    const unsigned c = emit_cond(x, d->op);
    if (c == JC_FALSE) {
        return;
    }
    if (c == JC_TRUE) {
        exit_direct(x, d->imm);
        x->done = true;
        return;
    }
    if (d->imm == x->block_pc && !x->entry_live) {
        a64_bcond(A, c, x->entry);          /* a loop: straight back to the top */
        return;
    }
    jcold_t *cs = br_cold(x, c, CS_SIDE);
    cs->target = d->imm;
}

static void e_branch(jx_t *x, const ji_t *d)
{
    switch (d->kind) {
    case K_JMP:
        exit_direct(x, d->imm);
        break;
    case K_CALL:
        push_imm(x, d->next);
        exit_direct(x, d->imm);
        break;
    case K_RET: {
        const int64_t inc = 8 + (int64_t)d->imm;
        site(x);
        if (inc <= 255) {
            a64_ls_wb(A, 8, A64_LDZ, S4, HR[RSP], inc, false);
        } else {
            a64_ls_imm(A, 8, A64_LDZ, S4, HR[RSP], 0);
            addi(x, HR[RSP], HR[RSP], inc);
        }
        if (x->mt) a64_dmb_ishld(A);
        exit_indirect(x);
        break;
    }
    case K_CALL_R:
        a64_mov(A, true, S4, HR[d->src]);
        push_imm(x, d->next);
        exit_indirect(x);
        break;
    case K_CALL_M: {
        mref_t m = ea_mem(x, d, 8);
        emit_load(x, 8, A64_LDZ, S4, &m);
        push_imm(x, d->next);
        exit_indirect(x);
        break;
    }
    case K_JMP_R:
        a64_mov(A, true, S4, HR[d->src]);
        exit_indirect(x);
        break;
    case K_JMP_M: {
        mref_t m = ea_mem(x, d, 8);
        emit_load(x, 8, A64_LDZ, S4, &m);
        exit_indirect(x);
        break;
    }
    default:    /* K_SYSCALL */
        materialize(x);
        mov_imm(x, S0, d->next);
        a64_movz(A, false, S1, XL_EV_SYSCALL, 0);
        a64_b(A, x->jc->tramp[2]);
        break;
    }
    x->done = true;
}

static void emit_insn_body(jx_t *x, ji_t *d);

static void emit_insn(jx_t *x, ji_t *d)
{
    emit_insn_body(x, d);
    if (x->dbg_flags && !x->done) {
        materialize(x);
    }
}

static void emit_insn_body(jx_t *x, ji_t *d)
{
    switch (d->kind) {
    case K_ALU_RR: case K_ALU_RI: case K_ALU_RM: case K_ALU_MR: case K_ALU_MI:
        e_alu(x, d);
        break;
    case K_MOV_RR: case K_MOV_RI: case K_MOV_LD: case K_MOV_ST: case K_MOV_MI:
        e_mov(x, d);
        break;
    case K_LEA:
        e_lea(x, d);
        break;
    case K_MOVX_R: case K_MOVX_M:
        e_movx(x, d);
        break;
    case K_PUSH_R: case K_PUSH_I: case K_POP_R:
        e_push_pop(x, d);
        break;
    case K_INCDEC_R: case K_INCDEC_M:
        e_incdec(x, d);
        break;
    case K_NOT_R: case K_NOT_M: case K_NEG_R: case K_NEG_M:
        e_notneg(x, d);
        break;
    case K_SHIFT_RI:
        e_shift_ri(x, d);
        break;
    case K_SHIFT_RC:
        e_shift_rc(x, d);
        break;
    case K_IMUL_RR: case K_IMUL_RRI: case K_IMUL_RM: case K_IMUL_RMI:
        e_imul(x, d);
        break;
    case K_MUL1_R: case K_MUL1_M:
        e_mul1(x, d);
        break;
    case K_DIV1_R: case K_DIV1_M:
        e_div1(x, d);
        break;
    case K_CDQ:
        e_cdq(x, d);
        break;
    case K_SETCC_R:
        e_setcc(x, d);
        break;
    case K_SETCC_M:
        e_setcc_m(x, d);
        break;
    case K_PUSH_M:
        e_push_m(x, d);
        break;
    case K_ATOM_ALU: case K_ATOM_INCDEC: case K_CMPXCHG: case K_XADD: case K_XCHG_M:
        e_atomic(x, d);
        break;
    case K_CMOV_R: case K_CMOV_M:
        e_cmov(x, d);
        break;
    case K_XCHG_RR:
        e_xchg(x, d);
        break;
    case K_BSWAP:
        a64_dp1(A, d->sz == 8u, d->sz == 8u ? A64_REV64 : A64_REV32, HR[d->dst], HR[d->dst]);
        break;
    case K_BSX_R: case K_BSX_M:
        e_bsx(x, d);
        break;
    case K_BT_RR: case K_BT_RI:
        e_bt(x, d);
        break;
    case K_LEAVE:
        e_leave(x);
        break;
    case K_NOP:
        break;
    case K_JCC:
        e_jcc(x, d);
        break;
    case K_JMP: case K_CALL: case K_RET: case K_CALL_R: case K_CALL_M: case K_JMP_R:
    case K_JMP_M: case K_SYSCALL:
        e_branch(x, d);
        break;
    case K_SSE:
        if (d->plan != SP_HELPER) e_sse_native(x, d);
        else e_sse(x, d);
        break;
    default:
        e_fallback(x, d);
        break;
    }
}

/* ---- cold code ------------------------------------------------------------------------------------- */

static void emit_cold(jx_t *x)
{
    for (int k = 0; k < x->ncold; ++k) {
        jcold_t *c = &x->cold[k];
        a64_patch_b(c->at, x->a.p);
        switch (c->type) {
        case CS_SIDE: {
            const pf_t save = x->pf;
            x->pf = c->pf;
            exit_direct(x, c->target);
            x->pf = save;
            break;
        }
        case CS_EXIT_NOSPILL:
            a64_b(A, x->jc->tramp[3]);
            break;
        case CS_LOOKUP_NOSPILL:
            a64_movz(A, false, S0, JX_LOOKUP, 0);
            a64_b(A, x->jc->tramp[3]);
            break;
        case CS_SIGNAL:
        case CS_INVAL:
            mov_imm(x, S0, x->block_pc);
            a64_movz(A, false, S1, c->type == CS_SIGNAL ? XL_EV_SIGNAL : JX_INVAL, 0);
            a64_b(A, x->jc->tramp[2]);
            break;
        case CS_SSESLOW: {      /* the SSE instruction through the interpreter, then back */
            const ji_t *d = &x->ins[c->insn];
            const pf_t save = x->pf;
            x->pf = c->pf;
            materialize(x);
            x->pf = save;
            a64_stp_pre(A, FA, FB, A64_SP, -32);
            a64_stp(A, FR, FC, A64_SP, 16);
            spill_all(A);
            mov_imm(x, X16, d->pc);
            a64_ls_imm(A, 8, A64_ST, X16, RC, CPU_OFF(rip));
            a64_mov(A, true, S0, RT);
            mov_imm(x, S1, c->target);
            mov_imm(x, X16, (uint64_t)(uintptr_t)jit_h_sse);
            a64_blr(A, X16);
            uint32_t *ev = x->a.p;
            a64_cbz(A, false, true, S0, x->a.p);
            reload_all(A);
            a64_ldp(A, FR, FC, A64_SP, 16);
            a64_ldp_post(A, FA, FB, A64_SP, 32);
            a64_b(A, c->back);
            a64_patch_b(ev, x->a.p);
            a64_addsub_imm(A, true, false, false, A64_SP, A64_SP, 32, false);
            a64_b(A, x->jc->tramp[3]);
            break;
        }
        default: {  /* CS_STEPEXIT: leave; the dispatcher runs the instruction in the interpreter */
            const pf_t save = x->pf;
            x->pf = c->pf;
            materialize(x);
            x->pf = save;
            mov_imm(x, S0, x->ins[c->insn].pc);
            a64_movz(A, false, S1, JX_STEP, 0);
            a64_b(A, x->jc->tramp[2]);
            break;
        }
        }
    }
}

/* ---- blocks ------------------------------------------------------------------------------------------ */

static void emit_tramp(jit_cache_t *jc)
{
    a64_t aa = { (uint32_t *)(void *)jc->code_cur, (uint32_t *)(void *)jc->code_cur,
                 (uint32_t *)(void *)jc->code_end, false };
    a64_t *a = &aa;
    /* int enter(x86cpu_t *c, gthread_t *t, void *code) */
    jc->tramp[0] = a->p;
    a64_stp_pre(a, 29, 30, A64_SP, -112);
    a64_stp(a, 19, 20, A64_SP, 16);
    a64_stp(a, 21, 22, A64_SP, 32);
    a64_stp(a, 23, 24, A64_SP, 48);
    a64_stp(a, 25, 26, A64_SP, 64);
    a64_stp(a, 27, 28, A64_SP, 80);
    a64_mov(a, true, RC, 0);
    a64_mov(a, true, RT, 1);
    reload_all(a);
    a64_br(a, 2);
    /* exit, chainable: x0 = rip, w1 = event, x2 = the branch to patch */
    jc->tramp[1] = a->p;
    a64_ls_imm(a, 8, A64_LDZ, X16, RT, (int64_t)__builtin_offsetof(gthread_t, engine));
    a64_ls_imm(a, 8, A64_ST, 2, X16, 0);
    /* exit: x0 = rip, w1 = event */
    jc->tramp[2] = a->p;
    spill_all(a);
    a64_ls_imm(a, 8, A64_ST, 0, RC, CPU_OFF(rip));
    a64_mov(a, false, 0, 1);
    /* exit, registers already in memory: w0 = event */
    jc->tramp[3] = a->p;
    a64_ldp(a, 19, 20, A64_SP, 16);
    a64_ldp(a, 21, 22, A64_SP, 32);
    a64_ldp(a, 23, 24, A64_SP, 48);
    a64_ldp(a, 25, 26, A64_SP, 64);
    a64_ldp(a, 27, 28, A64_SP, 80);
    a64_ldp_post(a, 29, 30, A64_SP, 112);
    a64_ret(a);
    jc->code_cur = (uint8_t *)a->p;
    if (jc->scratch == NULL) {
        jc->scratch = xl_map(sizeof(jx_t));
        if (jc->scratch == NULL) {
            xl_fatal("out of memory for the JIT");
        }
    }
}

/* compare the block's guest code against the bytes it was translated from */
static void emit_self_check(jx_t *x, uint64_t start, uint64_t len)
{
    const uint8_t *g = (const uint8_t *)(uintptr_t)start;
    mov_imm(x, X16, start);
    if (len < 8u) {
        for (uint64_t off = 0; off < len; ++off) {
            site(x);
            (void)a64_ls_imm(A, 1, A64_LDZ, X17, X16, (int64_t)off);
            a64_addsub_imm(A, false, true, true, ZR, X17, g[off], false);
            br_cold(x, A64_NE, CS_INVAL);
        }
        return;
    }
    for (uint64_t off = 0; off < len; off += 8u) {
        const uint64_t o = off + 8u <= len ? off : len - 8u;   /* the last chunk may overlap */
        uint64_t v;
        memcpy(&v, g + o, 8);
        unsigned base = X16;
        int64_t disp = (int64_t)o;
        if (!a64_ls_imm_ok(8, disp)) {
            addi(x, X17, X16, disp);
            base = X17;
            disp = 0;
        }
        site(x);
        (void)a64_ls_imm(A, 8, A64_LDZ, X17, base, disp);
        lit(x, S0, v);
        a64_cmp(A, true, X17, S0);
        br_cold(x, A64_NE, CS_INVAL);
    }
}

static jit_block_t *translate(jit_cache_t *jc, uint64_t pc)
{
    jx_t *x = jc->scratch;
    int max = (g_jx_dbg & JXD_ONE) ? 1 : JX_MAX_INSNS;
retry:
    (void)jit_room(jc, JX_BLOCK_CODE, JX_BLOCK_META);
    x->jc = jc;
    x->n = 0;
    uint64_t cur = pc;
    const uint64_t page = XL_PAGE_DOWN(pc);
    while (x->n < max) {
        ji_t *d = &x->ins[x->n];
        const uint8_t *p = (const uint8_t *)(uintptr_t)cur;
        int len = jdecode(p, cur, d);
        if (len > 0 && g_jx_fbk_lo >= 0 && (int)d->kind >= g_jx_fbk_lo && (int)d->kind <= g_jx_fbk_hi) {
            len = 0;
        }
        if (len <= 0) {
            bool ends = false;
            len = x86_insn_len(p, &ends);
            memset(d, 0, sizeof(*d));
            d->kind = K_FALLBACK;
            d->pc = cur;
            d->base = d->index = d->dst = d->src = NOREG;
            d->ends = ends;
            if (len <= 0) {
                d->next = cur + 1u;     /* the interpreter decides (SIGILL) */
                d->ends = 1;
            } else {
                d->next = cur + (uint64_t)len;
            }
        }
        set_effects(d);
        cur = d->next;
        x->n++;
        if (d->ends || XL_PAGE_DOWN(cur) != page) {
            break;
        }
    }
    analyze(x);

    x->a.start = (uint32_t *)(void *)jc->code_cur;
    x->a.p = x->a.start;
    x->a.end = x->a.start + JX_BLOCK_CODE / 4u;
    x->a.overflow = false;
    x->entry = x->a.p;
    x->block_pc = pc;
    x->dbg_flags = (g_jx_dbg & JXD_FLAGS) != 0u ||
                   (pc - g_xl_prog_base >= g_jx_fpc_lo && pc - g_xl_prog_base < g_jx_fpc_hi);
    x->mt = jc->mt != 0;
    x->done = false;
    x->fail = false;
    x->nsites = x->ncold = x->nlits = 0;
    x->pf.kind = PK_MEM;
    x->cur = -1;
    const bool ro = xl_code_is_readonly(pc, cur);

    /* entry: pending signals, then (code that may change) the self check */
    a64_ls_imm(A, 4, A64_LDZ, X16, RT, 8);
    cbz_cold(x, false, true, X16, CS_SIGNAL);
    if (!ro) {
        emit_self_check(x, pc, cur - pc);
    }
    for (int i = 0; i < x->n && !x->done; ++i) {
        x->cur = i;
        emit_insn(x, &x->ins[i]);
    }
    if (!x->done) {
        x->cur = x->n - 1;
        exit_direct(x, x->ins[x->n - 1].next);
    }
    x->cur = -1;
    emit_cold(x);
    uint8_t *redirect = (uint8_t *)x->a.p;
    mov_imm(x, S0, pc);
    a64_movz(A, false, S1, JX_LOOKUP, 0);
    a64_b(A, jc->tramp[2]);
    if (x->nlits > 0) {
        if (((uintptr_t)x->a.p & 7u) != 0u) a64_nop(A);
        for (int k = 0; k < x->nlits; ++k) {
            uint32_t *at = x->lits[k].at;
            const int64_t off = ((uint8_t *)x->a.p - (uint8_t *)at) / 4;
            *at = (*at & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFFu) << 5);
            a64_put(A, (uint32_t)x->lits[k].v);
            a64_put(A, (uint32_t)(x->lits[k].v >> 32));
        }
    }
    if (x->a.overflow || x->fail) {
        if (max <= 1) {
            xl_fatal("JIT: cannot translate the block at %lx", pc);
        }
        max /= 2;
        goto retry;
    }

    jit_block_t *b = jit_meta_alloc(jc, sizeof(jit_block_t) + (size_t)x->nsites * sizeof(jsite_t));
    if (b == NULL) {
        jit_flush(jc);
        goto retry;
    }
    memset(b, 0, sizeof(*b));
    jsite_t *sites = (jsite_t *)(void *)(b + 1);
    memcpy(sites, x->sites, (size_t)x->nsites * sizeof(jsite_t));
    b->pc = pc;
    b->code = (uint8_t *)x->entry;
    b->code_size = (uint32_t)((uint8_t *)x->a.p - (uint8_t *)x->entry);
    b->ro = ro;
    b->nsegs = 1;
    b->seg[0].start = pc;
    b->seg[0].end = cur;
    b->nsites = (uint32_t)x->nsites;
    b->site_size = sizeof(jsite_t);
    b->sites = sites;
    b->redirect = redirect;
    jc->code_cur = (uint8_t *)(((uintptr_t)x->a.p + 15u) & ~(uintptr_t)15u);
    jit_host_sync(x->entry, x->a.p);
    jit_insert(jc, b);
#ifdef XL_PROFILE
    ++g_xb_trans;
    g_xb_uops += (uint64_t)x->n;
#endif
    return b;
}

/* ---- XL_JIT_DEBUG=v: every instruction checked against the interpreter ---------------------------------- */

typedef struct {
    x86cpu_t pre;
    bool ok;
    uint64_t mask;                  /* flags defined after the instruction */
} jverify_t;
#define g_vpre (VER(t)->pre)
#define g_vok (VER(t)->ok)
#define g_vmask (VER(t)->mask)
static jverify_t *VER(gthread_t *t)
{
    jit_cache_t *jc = t->engine;
    if (jc->tramp[7] == NULL) {
        jc->tramp[7] = xl_map(sizeof(jverify_t));
    }
    return jc->tramp[7];
}
static uint64_t g_vseen[64];
static unsigned g_vnseen;

static void verify_prepare(gthread_t *t, x86cpu_t *c, uint64_t pc)
{
    ji_t d;
    g_vok = false;
    if (jdecode((const uint8_t *)(uintptr_t)pc, pc, &d) <= 0) {
        return;                 /* the interpreter runs it anyway */
    }
    set_effects(&d);
    switch (d.kind) {
    case K_ALU_MR: case K_ALU_MI:
        if (d.op == 7u || d.op == 8u) break;
        return;                 /* read-modify-write: not repeatable */
    case K_INCDEC_M: case K_NOT_M: case K_NEG_M: case K_ATOM_ALU: case K_ATOM_INCDEC:
    case K_CMPXCHG: case K_XADD: case K_XCHG_M: case K_SYSCALL: case K_FALLBACK:
        return;
    case K_SSE:
        if (d.plan == SP_HELPER) return;
        break;
    default:
        break;
    }
    g_vmask = F_CF | F_ZF | F_SF | F_OF | F_PF;
    switch (d.kind) {
    case K_BSX_R: case K_BSX_M: g_vmask = F_ZF; break;
    case K_IMUL_RR: case K_IMUL_RRI: case K_IMUL_RM: case K_IMUL_RMI:
    case K_MUL1_R: case K_MUL1_M: g_vmask = F_CF | F_OF; break;
    case K_SHIFT_RI: if (d.imm != 1u) g_vmask &= ~(uint64_t)F_OF; if (d.op < 2u) g_vmask &= F_CF | F_OF; break;
    case K_SHIFT_RC: g_vmask = 0; break;
    case K_BT_RR: case K_BT_RI: g_vmask = F_CF; break;
    default: break;
    }
    g_vpre = *c;
    g_vok = true;
}

static void verify_check(gthread_t *t, x86cpu_t *c, uint64_t pc)
{
    if (!g_vok) {
        return;
    }
    x86cpu_t jit = *c;
    *c = g_vpre;
    const int e = x86_step(t, c);
    if (e != 0) {
        return;
    }
    const uint64_t fj = x86_get_flags(&jit) & g_vmask;
    const uint64_t fi = x86_get_flags(c) & g_vmask;
    bool bad = jit.rip != c->rip || memcmp(jit.r, c->r, sizeof(uint64_t) * 16u) != 0 ||
               memcmp(jit.xmm, c->xmm, sizeof(jit.xmm)) != 0;
    const bool flagbad = fj != fi;
    if (!bad && !flagbad) {
        return;
    }
    for (unsigned k = 0; k < g_vnseen; ++k) {
        if (g_vseen[k] == pc) return;
    }
    if (g_vnseen < 64u) g_vseen[g_vnseen++] = pc;
    const uint8_t *p = (const uint8_t *)(uintptr_t)pc;
    xl_log("JIT verify: %s mismatch at %lx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
           bad ? "state" : "flags", pc, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
    for (unsigned k = 0; k < 16u; ++k) {
        if (jit.r[k] != c->r[k]) xl_log("  r%u jit %lx interp %lx (before %lx)", k, jit.r[k], c->r[k], g_vpre.r[k]);
    }
    if (jit.rip != c->rip) xl_log("  rip jit %lx interp %lx", jit.rip, c->rip);
    for (unsigned k = 0; k < 16u; ++k) {
        if (memcmp(&jit.xmm[k], &c->xmm[k], 16) != 0) {
            xl_log("  xmm%u jit %lx:%lx interp %lx:%lx", k, jit.xmm[k].q[1], jit.xmm[k].q[0],
                   c->xmm[k].q[1], c->xmm[k].q[0]);
        }
    }
    if (flagbad) xl_log("  flags jit %lx interp %lx", fj, fi);
    /* carry on with the interpreter's result */
}

/* ---- the dispatcher ------------------------------------------------------------------------------------ */

int x86_run_jit(gthread_t *t)
{
    x86cpu_t *c = X86(t);
    jit_cache_t *jc = jit_cache_get(t, JX_CODE_SIZE, JX_META_SIZE, emit_tramp);
    if (XL_UNLIKELY(jc->mt != g_xl_mt)) {
        jit_flush(jc);
        jc->mt = g_xl_mt;
    }
    int (*enter)(x86cpu_t *, gthread_t *, void *) =
        (int (*)(x86cpu_t *, gthread_t *, void *))jc->tramp[0];
    uint64_t patch = 0;
    for (;;) {
        if (XL_UNLIKELY(t->sig_pending_flag)) {
            return XL_EV_SIGNAL;
        }
        if (XL_UNLIKELY(jc->inval_seen != g_xl_inval_seq)) {
            const uint32_t f = jc->flushes;
            jit_replay_inval(jc);
            if (jc->flushes != f) patch = 0;
        }
        const uint64_t pc = c->rip;
        jit_block_t *b = jit_lookup(jc, pc);
        if (b == NULL) {
            const uint32_t f = jc->flushes;
            b = translate(jc, pc);
            if (jc->flushes != f) patch = 0;
        }
        if (patch != 0u && (g_jx_dbg & JXD_NOCHAIN) == 0u) {
            jit_host_patch_jump((uint8_t *)(uintptr_t)patch, b->code);
            patch = 0;
        }
        if ((g_jx_dbg & JXD_NOCHAIN) == 0u) {
            jit_ibtc_set(jc, pc, b->code);
        }
#ifdef XL_PROFILE
        ++g_xb_entries;
#endif
        if (XL_UNLIKELY(g_jx_dbg & JXD_VERIFY)) {
            verify_prepare(t, c, pc);
        }
        const int ev = enter(c, t, b->code);
        if (XL_UNLIKELY(g_jx_dbg & JXD_VERIFY) && (ev == JX_CHAIN || ev == JX_LOOKUP)) {
            verify_check(t, c, pc);
        }
        switch (ev) {
        case JX_CHAIN:
            patch = jc->patch_site;
            break;
        case JX_LOOKUP:
            break;
        case JX_STEP: {
            const int e = x86_step(t, c);
            if (e != 0) {
                return e;
            }
            break;
        }
        case JX_INVAL: {
            jit_block_t *ob = jit_lookup(jc, c->rip);
            if (ob != NULL) {
                jit_kill(jc, ob);
            }
            break;
        }
        case XL_EV_SYSCALL:
            c->r[RCX] = c->rip;
            c->r[R11] = x86_get_flags(c) | (c->eflags & ~(uint64_t)F_ARITH) | 0x202u;
            return ev;
        default:
            return ev;
        }
    }
}

/* ---- faults in generated code ---------------------------------------------------------------------------- */

static uint64_t uc_loc(void *uc, unsigned loc, uint64_t imm)
{
    if (loc == LOC_IMM) return imm;
    if (loc == LOC_NONE || loc == ZR) return 0;
    return *host_uc_reg(uc, loc);
}

static void rebuild_flags(x86cpu_t *c, const pf_t *p, void *uc)
{
    const int sz = p->sz;
    const uint64_t m = szmask(sz);
    if (p->kind == PK_NONE || p->kind == PK_MEM) {
        return;
    }
    uint64_t a = uc_loc(uc, p->la, 0) & m, b = uc_loc(uc, p->lb, p->imm) & m;
    uint64_t r = uc_loc(uc, p->lr, 0) & m, cin = uc_loc(uc, p->lc, 0);
    switch (p->kind) {
    case PK_ADD: case PK_SUB: case PK_ADC: case PK_SBB:
        if (p->la == LOC_NONE || p->lb == LOC_NONE) {
            /* only the result was kept: enough for ZF/SF/PF */
            setcc(c, CC_SUB, sz, r, 0, r);
            return;
        }
        if (p->kind == PK_ADD) r = (a + b) & m;
        else if (p->kind == PK_SUB) r = (a - b) & m;
        else if (p->kind == PK_ADC) r = (a + b + cin) & m;
        else r = (a - b - cin) & m;
        setcc(c, k_pk_cc[p->kind], sz, a, b, r);
        c->cc_cin = cin;
        return;
    case PK_TEST:
        setcc(c, CC_LOGIC, sz, a, b, a & b);
        return;
    case PK_LOGIC:
        setcc(c, CC_LOGIC, sz, 0, 0, r);
        return;
    case PK_INC: case PK_DEC:
        a = (p->kind == PK_INC ? r - 1u : r + 1u) & m;
        setcc(c, k_pk_cc[p->kind], sz, a, 1, r);
        c->cc_cin = p->lc == LOC_NONE ? 0 : cin;
        return;
    case PK_SHL: case PK_SHR: case PK_SAR:
        setcc(c, k_pk_cc[p->kind], sz, a, p->imm, r);
        return;
    case PK_MUL:
        setcc(c, CC_MUL, sz, 0, b != 0u, r);
        return;
    case PK_EFL:
        x86_set_flags(c, uc_loc(uc, p->lr, 0));
        return;
    default:
        return;     /* PK_MEM: already there; PK_NONE: dead */
    }
}

bool guest_jit_fault(gthread_t *t, void *uc)
{
    if (g_engine != 0) {
        return false;
    }
    jit_cache_t *jc = t->engine;
    const uint64_t hpc = *host_uc_pc_ptr(uc);
    if (jc == NULL || hpc < (uint64_t)(uintptr_t)jc->code ||
        hpc >= (uint64_t)(uintptr_t)jc->code_end) {
        return false;
    }
    const jit_block_t *b = jit_find_host(jc, hpc);
    const jsite_t *s = NULL;
    if (b != NULL) {
        const uint32_t off = (uint32_t)(hpc - (uint64_t)(uintptr_t)b->code);
        const jsite_t *sites = b->sites;
        uint32_t lo = 0, hi = b->nsites;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2u;
            if (sites[mid].off < off) lo = mid + 1u;
            else hi = mid;
        }
        if (lo < b->nsites && sites[lo].off == off) {
            s = &sites[lo];
        }
    }
    if (s == NULL) {
        xl_log("JIT: fault in generated code at %lx (block %lx), not at a memory access",
               hpc, b != NULL ? b->pc : 0UL);
        xl_signal_die(XL_SIGSEGV);
    }
    x86cpu_t *c = X86(t);
    for (unsigned k = 0; k < 16u; ++k) {
        c->r[k] = *host_uc_reg(uc, HR[k]);
    }
    c->rip = s->pc;
    rebuild_flags(c, &s->pf, uc);
    return true;
}

#endif /* __aarch64__ */
