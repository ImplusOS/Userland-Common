/*
 * x86_64 interpreter: prefixes, ModRM, the integer instruction set, the
 * lazily computed arithmetic flags, and the run loop.
 *
 * One instruction at a time, straight from guest memory (no decode cache:
 * code the guest writes or maps later needs no invalidation). cpu->rip
 * still holds the instruction's address until the instruction completes,
 * so a fault anywhere inside it (xl_fault_resume) is reported at the
 * instruction, as the CPU would. Memory is written before registers where
 * an instruction does both.
 */

#include "x86.h"
#include "x86_ops.h"
#include "host.h"

/* ---- flags ------------------------------------------------------------ */

uint64_t x86_get_flags(x86cpu_t *c)
{
    if (c->cc_op == CC_EFLAGS) {
        return c->eflags & F_ARITH;
    }
    const int sz = (int)c->cc_size;
    const unsigned bits = (unsigned)sz * 8u;
    const uint64_t sign = sbit(sz);
    const uint64_t a = c->cc_a, b = c->cc_b, r = c->cc_r;
    uint64_t cf = 0, of = 0, af = 0;
    switch (c->cc_op) {
    case CC_ADD:
        cf = r < a;
        of = ((a ^ ~b) & (a ^ r) & sign) != 0u;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_ADC:
        cf = c->cc_cin ? r <= a : r < a;
        of = ((a ^ ~b) & (a ^ r) & sign) != 0u;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_SUB:
        cf = a < b;
        of = ((a ^ b) & (a ^ r) & sign) != 0u;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_SBB:
        cf = c->cc_cin ? a <= b : a < b;
        of = ((a ^ b) & (a ^ r) & sign) != 0u;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_LOGIC:
        break;
    case CC_INC:
        cf = c->cc_cin;
        of = r == sign;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_DEC:
        cf = c->cc_cin;
        of = r == sign - 1u;
        af = (a ^ b ^ r) & 0x10u;
        break;
    case CC_SHL:
        cf = b <= bits ? (a >> (bits - b)) & 1u : 0u;
        of = ((r & sign) != 0u) ^ cf;
        break;
    case CC_SHR:
        cf = b <= bits ? (a >> (b - 1u)) & 1u : 0u;
        of = (a & sign) != 0u;
        break;
    case CC_SAR: {
        uint64_t s = b - 1u > 63u ? 63u : b - 1u;
        cf = ((uint64_t)(sx(a, sz) >> s)) & 1u;
        break;
    }
    case CC_MUL:
        cf = of = b != 0u;
        break;
    default:
        break;
    }
    return (cf ? F_CF : 0u) | parity_flag(r) | (af ? F_AF : 0u) |
           (r == 0u ? F_ZF : 0u) | ((r & sign) ? F_SF : 0u) | (of ? F_OF : 0u);
}

void x86_set_flags(x86cpu_t *c, uint64_t f)
{
    c->eflags = (c->eflags & ~(uint64_t)F_ARITH) | (f & F_ARITH);
    c->cc_op = CC_EFLAGS;
}

/* ---- decoding ---------------------------------------------------------- */

void x86_modrm(insn_t *i)
{
    uint8_t m = fetch8(i);
    i->modrm = m;
    i->mod = m >> 6;
    i->reg = (uint8_t)(((m >> 3) & 7u) | (REX_R(i) ? 8u : 0u));
    uint8_t rm = m & 7u;
    i->riprel = 0;
    i->ea_base = -1;
    i->ea_index = -1;
    i->ea_scale = 0;
    i->ea_disp = 0;
    if (i->mod == 3) {
        i->rm = (uint8_t)(rm | (REX_B(i) ? 8u : 0u));
        return;
    }
    i->rm = rm;
    if (rm == 4) {
        uint8_t sib = fetch8(i);
        uint8_t base = sib & 7u;
        uint8_t idx = (uint8_t)(((sib >> 3) & 7u) | (REX_X(i) ? 8u : 0u));
        i->ea_scale = sib >> 6;
        if (idx != 4) {
            i->ea_index = (int8_t)idx;
        }
        if (base == 5 && i->mod == 0) {
            i->ea_disp = (int32_t)fetch32(i);
        } else {
            i->ea_base = (int8_t)(base | (REX_B(i) ? 8u : 0u));
        }
    } else if (rm == 5 && i->mod == 0) {
        i->riprel = 1;
        i->ea_disp = (int32_t)fetch32(i);
        return;
    } else {
        i->ea_base = (int8_t)(rm | (REX_B(i) ? 8u : 0u));
    }
    if (i->mod == 1) {
        i->ea_disp += (int8_t)fetch8(i);
    } else if (i->mod == 2) {
        i->ea_disp += (int32_t)fetch32(i);
    }
}

static uint64_t ea_offset(insn_t *i)
{
    uint64_t a;
    if (i->riprel) {
        a = i->pc + (uint64_t)i->ea_disp;
    } else {
        a = (uint64_t)i->ea_disp;
        if (i->ea_base >= 0) {
            a += i->c->r[i->ea_base];
        }
        if (i->ea_index >= 0) {
            a += i->c->r[i->ea_index] << i->ea_scale;
        }
    }
    if (i->adsz32) {
        a = (uint32_t)a;
    }
    return a;
}

uint64_t x86_ea(insn_t *i)
{
    uint64_t a = ea_offset(i);
    if (i->seg == 4) {
        a += i->c->fs_base;
    } else if (i->seg == 5) {
        a += i->c->gs_base;
    }
    return a;
}

int x86_fault(insn_t *i, int sig, int code, uint64_t addr)
{
    xl_signal_raise_sync(i->t, sig, code, addr);
    return XL_EV_SIGNAL;
}

/* ---- registers and operands --------------------------------------------- */

static inline uint64_t getreg(insn_t *i, int r, int sz)
{
    return x86_rd(i->c, r, sz, i->rex != 0u);
}

static inline void setreg(insn_t *i, int r, int sz, uint64_t v)
{
    x86_wr(i->c, r, sz, i->rex != 0u, v);
}

typedef struct {
    uint64_t addr;
    int reg;        /* >= 0: a register */
} loc_t;

static inline loc_t rm_loc(insn_t *i)
{
    loc_t l;
    if (i->mod == 3) {
        l.reg = i->rm;
        l.addr = 0;
    } else {
        l.reg = -1;
        l.addr = x86_ea(i);
    }
    return l;
}

static inline uint64_t ld(insn_t *i, const loc_t *l, int sz)
{
    return l->reg >= 0 ? getreg(i, l->reg, sz) : mrd(l->addr, sz);
}

static inline void st(insn_t *i, const loc_t *l, int sz, uint64_t v)
{
    if (l->reg >= 0) {
        setreg(i, l->reg, sz, v);
    } else {
        mwr(l->addr, sz, v);
    }
}

static inline uint64_t fetch_imm(insn_t *i, int sz)
{
    switch (sz) {
    case 1: return (uint64_t)(int64_t)(int8_t)fetch8(i);
    case 2: return (uint64_t)(int64_t)(int16_t)fetch16(i);
    default: return (uint64_t)(int64_t)(int32_t)fetch32(i);   /* Iz */
    }
}

static inline void push(insn_t *i, int sz, uint64_t v)
{
    uint64_t sp = i->c->r[RSP] - (uint64_t)sz;
    mwr(sp, sz, v);
    i->c->r[RSP] = sp;
}

static inline uint64_t pop(insn_t *i, int sz)
{
    uint64_t v = mrd(i->c->r[RSP], sz);
    i->c->r[RSP] += (uint64_t)sz;
    return v;
}

static inline int stack_sz(insn_t *i)
{
    return i->p66 ? 2 : 8;
}

/* ---- atomics for LOCK-prefixed and implicitly locked instructions -------- */

static xl_lock_t g_misaligned_atomic;

static bool cas(uint64_t addr, int sz, uint64_t *expected, uint64_t desired)
{
    if ((addr & (uint64_t)(sz - 1)) == 0u) {
        switch (sz) {
        case 1: {
            uint8_t e = (uint8_t)*expected;
            bool ok = __atomic_compare_exchange_n((uint8_t *)(uintptr_t)addr, &e,
                (uint8_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        case 2: {
            uint16_t e = (uint16_t)*expected;
            bool ok = __atomic_compare_exchange_n((uint16_t *)(uintptr_t)addr, &e,
                (uint16_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        case 4: {
            uint32_t e = (uint32_t)*expected;
            bool ok = __atomic_compare_exchange_n((uint32_t *)(uintptr_t)addr, &e,
                (uint32_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        default: {
            uint64_t e = *expected;
            bool ok = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)addr, &e,
                desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        }
    }
    xl_lock(&g_misaligned_atomic);
    uint64_t cur = mrd(addr, sz);
    bool ok = cur == *expected;
    if (ok) {
        mwr(addr, sz, desired);
    } else {
        *expected = cur;
    }
    xl_unlock(&g_misaligned_atomic);
    return ok;
}

static uint64_t xchg_mem(uint64_t addr, int sz, uint64_t v)
{
    uint64_t old = mrd(addr, sz);
    while (!cas(addr, sz, &old, v)) {
    }
    return old;
}

/* ---- ALU ----------------------------------------------------------------- */

/* Read-modify-write of an ALU op on `l`, atomically under LOCK. */
static void alu_rmw(insn_t *i, const loc_t *l, int op, int sz, uint64_t b)
{
    x86cpu_t *c = i->c;
    uint64_t cin = (op == 2 || op == 3) ? get_cf(c) : 0u;
    if (op == 7) {
        (void)x86_alu(c, 7, sz, ld(i, l, sz), b, 0);
        return;
    }
    if (i->lock && l->reg < 0) {
        uint64_t old = mrd(l->addr, sz);
        for (;;) {
            uint64_t r = x86_alu(c, op, sz, old, b, cin);
            if (cas(l->addr, sz, &old, r)) {
                return;
            }
        }
    }
    st(i, l, sz, x86_alu(c, op, sz, ld(i, l, sz), b, cin));
}

uint64_t x86_shift(x86cpu_t *c, int kind, int sz, uint64_t v, unsigned count)
{
    const unsigned bits = (unsigned)sz * 8u;
    const uint64_t m = szmask(sz);
    count &= sz == 8 ? 63u : 31u;
    v &= m;
    if (count == 0u) {
        return v;
    }
    uint64_t r;
    switch (kind) {
    case 0: case 1: {   /* ROL, ROR */
        unsigned n = count % bits;
        if (kind == 0) {
            r = n == 0u ? v : ((v << n) | (v >> (bits - n))) & m;
        } else {
            r = n == 0u ? v : ((v >> n) | (v << (bits - n))) & m;
        }
        uint64_t f = x86_get_flags(c) & ~(uint64_t)(F_CF | F_OF);
        uint64_t cf, of;
        if (kind == 0) {
            cf = r & 1u;
            of = ((r >> (bits - 1u)) & 1u) ^ cf;
        } else {
            cf = (r >> (bits - 1u)) & 1u;
            of = cf ^ ((r >> (bits - 2u)) & 1u);
        }
        x86_set_flags(c, f | (cf ? F_CF : 0u) | (of ? F_OF : 0u));
        return r;
    }
    case 2: case 3: {   /* RCL, RCR */
        unsigned n = sz < 4 ? count % (bits + 1u) : count;
        uint64_t f = x86_get_flags(c);
        uint64_t cf = f & F_CF;
        uint64_t of = 0;
        r = v;
        if (kind == 3) {
            of = ((r >> (bits - 1u)) & 1u) ^ cf;
        }
        for (unsigned k = 0; k < n; ++k) {
            if (kind == 2) {
                uint64_t ncf = (r >> (bits - 1u)) & 1u;
                r = ((r << 1) | cf) & m;
                cf = ncf;
            } else {
                uint64_t ncf = r & 1u;
                r = (r >> 1) | (cf << (bits - 1u));
                cf = ncf;
            }
        }
        if (kind == 2) {
            of = ((r >> (bits - 1u)) & 1u) ^ cf;
        }
        f &= ~(uint64_t)(F_CF | F_OF);
        x86_set_flags(c, f | (cf ? F_CF : 0u) | (of ? F_OF : 0u));
        return r;
    }
    case 4: case 6:     /* SHL/SAL */
        r = count >= 64u ? 0u : (v << count) & m;
        setcc(c, CC_SHL, sz, v, count, r);
        return r;
    case 5:             /* SHR */
        r = count >= 64u ? 0u : v >> count;
        setcc(c, CC_SHR, sz, v, count, r);
        return r;
    default:            /* SAR */
        r = (uint64_t)(sx(v, sz) >> (count > 63u ? 63u : count)) & m;
        setcc(c, CC_SAR, sz, v, count, r);
        return r;
    }
}

/* ---- string instructions --------------------------------------------------- */

static int string_op(insn_t *i, uint8_t op)
{
    x86cpu_t *c = i->c;
    const int sz = (op & 1u) ? i->opsz : 1;
    const bool down = (c->eflags & F_DF) != 0u;
    const int64_t step = down ? -(int64_t)sz : (int64_t)sz;
    const uint64_t amask = i->adsz32 ? 0xFFFFFFFFULL : ~0ULL;
    const uint64_t sbase = i->seg == 4 ? c->fs_base : (i->seg == 5 ? c->gs_base : 0u);
    uint64_t si = c->r[RSI] & amask, di = c->r[RDI] & amask;
    const bool rep = i->rep != 0u;
    uint64_t cnt = rep ? (c->r[RCX] & amask) : 1u;
    const uint8_t base = op & 0xFEu;

    if (cnt == 0u) {
        return 0;
    }
    switch (base) {
    case 0xA4: /* MOVS */
        if (!down && cnt > 1u) {
            uint64_t n = cnt * (uint64_t)sz;
            uint64_t s = sbase + si;
            if (di <= s || di >= s + n) {
                if (XL_UNLIKELY(g_xl_mt)) X86_STORE_FENCE();
                memmove((void *)(uintptr_t)di, (const void *)(uintptr_t)s, n);
                si += n;
                di += n;
                cnt = 0;
                break;
            }
        }
        while (cnt != 0u) {
            mwr(di, sz, mrd(sbase + si, sz));
            si = (si + (uint64_t)step) & amask;
            di = (di + (uint64_t)step) & amask;
            --cnt;
        }
        break;
    case 0xAA: { /* STOS */
        uint64_t v = c->r[RAX] & szmask(sz);
        if (!down && sz == 1 && cnt > 1u) {
            if (XL_UNLIKELY(g_xl_mt)) X86_STORE_FENCE();
            memset((void *)(uintptr_t)di, (int)v, cnt);
            di += cnt;
            cnt = 0;
            break;
        }
        while (cnt != 0u) {
            mwr(di, sz, v);
            di = (di + (uint64_t)step) & amask;
            --cnt;
        }
        break;
    }
    case 0xAC: /* LODS */
        while (cnt != 0u) {
            setreg(i, RAX, sz, mrd(sbase + si, sz));
            si = (si + (uint64_t)step) & amask;
            --cnt;
        }
        break;
    case 0xA6:   /* CMPS */
    case 0xAE: { /* SCAS */
        while (cnt != 0u) {
            uint64_t a, b;
            if (base == 0xA6) {
                a = mrd(sbase + si, sz);
                b = mrd(di, sz);
                si = (si + (uint64_t)step) & amask;
            } else {
                a = c->r[RAX];
                b = mrd(di, sz);
            }
            di = (di + (uint64_t)step) & amask;
            --cnt;
            uint64_t r = x86_alu(c, 7, sz, a, b, 0);
            if (rep) {
                if (i->rep == 0xF3 && r != 0u) break;   /* REPE: stop on difference */
                if (i->rep == 0xF2 && r == 0u) break;   /* REPNE: stop on equality */
            }
        }
        break;
    }
    default:
        return X86_UD(i);
    }
    if (base == 0xA4 || base == 0xAC || base == 0xA6) {
        c->r[RSI] = i->adsz32 ? (uint32_t)si : si;
    }
    if (base != 0xAC) {
        c->r[RDI] = i->adsz32 ? (uint32_t)di : di;
    }
    if (rep) {
        c->r[RCX] = i->adsz32 ? (uint32_t)cnt : cnt;
    }
    return 0;
}

/* ---- group 3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV ------------------------------ */

static int group3(insn_t *i, int sz)
{
    x86cpu_t *c = i->c;
    const unsigned sub = i->reg & 7u;
    uint64_t imm = 0;
    if (sub <= 1u) {
        imm = fetch_imm(i, sz == 8 ? 4 : sz);
    }
    loc_t l = rm_loc(i);
    const uint64_t m = szmask(sz);
    switch (sub) {
    case 0: case 1:
        (void)x86_alu(c, 4, sz, ld(i, &l, sz), imm, 0);
        return 0;
    case 2:
        if (i->lock && l.reg < 0) {
            uint64_t old = mrd(l.addr, sz);
            while (!cas(l.addr, sz, &old, ~old & m)) {
            }
        } else {
            st(i, &l, sz, ~ld(i, &l, sz) & m);
        }
        return 0;
    case 3:
        if (i->lock && l.reg < 0) {
            uint64_t old = mrd(l.addr, sz);
            for (;;) {
                uint64_t r = x86_alu(c, 5, sz, 0, old, 0);
                if (cas(l.addr, sz, &old, r)) break;
            }
        } else {
            st(i, &l, sz, x86_alu(c, 5, sz, 0, ld(i, &l, sz), 0));
        }
        return 0;
    default:
        break;
    }
    const uint64_t v = ld(i, &l, sz);
    switch (sub) {
    case 4: { /* MUL */
        if (sz == 8) {
            unsigned __int128 p = (unsigned __int128)c->r[RAX] * v;
            uint64_t lo = (uint64_t)p, hi = (uint64_t)(p >> 64);
            c->r[RAX] = lo;
            c->r[RDX] = hi;
            setcc(c, CC_MUL, 8, 0, hi != 0u, lo);
        } else {
            uint64_t p = (c->r[RAX] & m) * v;
            uint64_t lo = p & m, hi = (p >> (sz * 8)) & m;
            if (sz == 1) {
                setreg(i, RAX, 2, p & 0xFFFFu);
            } else {
                setreg(i, RAX, sz, lo);
                setreg(i, RDX, sz, hi);
            }
            setcc(c, CC_MUL, sz, 0, hi != 0u, lo);
        }
        return 0;
    }
    case 5: { /* IMUL */
        if (sz == 8) {
            __int128 p = (__int128)(int64_t)c->r[RAX] * (int64_t)v;
            uint64_t lo = (uint64_t)p, hi = (uint64_t)((unsigned __int128)p >> 64);
            c->r[RAX] = lo;
            c->r[RDX] = hi;
            setcc(c, CC_MUL, 8, 0, p != (__int128)(int64_t)lo, lo);
        } else {
            int64_t p = sx(c->r[RAX], sz) * sx(v, sz);
            uint64_t lo = (uint64_t)p & m, hi = ((uint64_t)p >> (sz * 8)) & m;
            if (sz == 1) {
                setreg(i, RAX, 2, (uint64_t)p & 0xFFFFu);
            } else {
                setreg(i, RAX, sz, lo);
                setreg(i, RDX, sz, hi);
            }
            setcc(c, CC_MUL, sz, 0, p != sx(lo, sz), lo);
        }
        return 0;
    }
    case 6: { /* DIV */
        if ((v & m) == 0u) {
            return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
        }
        if (sz == 8) {
            if (c->r[RDX] == 0u) {
                uint64_t q = c->r[RAX] / v, r = c->r[RAX] % v;
                c->r[RAX] = q;
                c->r[RDX] = r;
            } else {
                unsigned __int128 n = ((unsigned __int128)c->r[RDX] << 64) | c->r[RAX];
                unsigned __int128 q = n / v;
                if ((q >> 64) != 0u) {
                    return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
                }
                c->r[RDX] = (uint64_t)(n % v);
                c->r[RAX] = (uint64_t)q;
            }
        } else {
            uint64_t n = sz == 1 ? (c->r[RAX] & 0xFFFFu) :
                         (((c->r[RDX] & m) << (sz * 8)) | (c->r[RAX] & m));
            uint64_t q = n / v, r = n % v;
            if (q > m) {
                return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
            }
            if (sz == 1) {
                setreg(i, RAX, 2, (r << 8) | q);
            } else {
                setreg(i, RAX, sz, q);
                setreg(i, RDX, sz, r);
            }
        }
        return 0;
    }
    default: { /* IDIV */
        int64_t d = sx(v, sz);
        if (d == 0) {
            return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
        }
        if (sz == 8) {
            __int128 n = (__int128)(((unsigned __int128)c->r[RDX] << 64) | c->r[RAX]);
            if (d == -1 && n == (__int128)((unsigned __int128)1 << 127)) {
                return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
            }
            __int128 q = n / d, r = n % d;
            if (q > INT64_MAX || q < INT64_MIN) {
                return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
            }
            c->r[RAX] = (uint64_t)(int64_t)q;
            c->r[RDX] = (uint64_t)(int64_t)r;
        } else {
            int64_t n = sz == 1 ? (int16_t)c->r[RAX] :
                        (int64_t)(((uint64_t)sx(c->r[RDX], sz) << (sz * 8)) | (c->r[RAX] & m));
            int64_t q = n / d, r = n % d;
            if (q != sx((uint64_t)q, sz)) {
                return x86_fault(i, XL_SIGFPE, XL_FPE_INTDIV, i->start);
            }
            if (sz == 1) {
                setreg(i, RAX, 2, (((uint64_t)r & 0xFFu) << 8) | ((uint64_t)q & 0xFFu));
            } else {
                setreg(i, RAX, sz, (uint64_t)q);
                setreg(i, RDX, sz, (uint64_t)r);
            }
        }
        return 0;
    }
    }
}

/* ---- bit tests --------------------------------------------------------------- */

/* op: 0 BT, 1 BTS, 2 BTR, 3 BTC */
static void bit_op(insn_t *i, int op, int sz, uint64_t off, bool reg_off)
{
    x86cpu_t *c = i->c;
    const unsigned bits = (unsigned)sz * 8u;
    loc_t l = rm_loc(i);
    if (l.reg < 0 && reg_off) {
        int64_t so = sx(off, sz);
        l.addr += (uint64_t)((so >> (sz == 8 ? 6 : (sz == 4 ? 5 : 4))) * sz);
    }
    unsigned bit = (unsigned)(off & (bits - 1u));
    uint64_t f = x86_get_flags(c) & ~(uint64_t)F_CF;
    uint64_t mask = 1ULL << bit;
    uint64_t v;
    if (op != 0 && i->lock && l.reg < 0) {
        uint64_t old = mrd(l.addr, sz);
        for (;;) {
            uint64_t n = op == 1 ? old | mask : op == 2 ? old & ~mask : old ^ mask;
            if (cas(l.addr, sz, &old, n)) break;
        }
        v = old;
    } else {
        v = ld(i, &l, sz);
        if (op != 0) {
            st(i, &l, sz, op == 1 ? v | mask : op == 2 ? v & ~mask : v ^ mask);
        }
    }
    x86_set_flags(c, f | ((v & mask) ? F_CF : 0u));
}

/* ---- the 0F xx integer instructions ----------------------------------------- */

static uint64_t host_tsc(void)
{
    struct { int64_t s, ns; } ts = { 0, 0 };
    (void)host_clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return (uint64_t)ts.s * 1000000000ULL + (uint64_t)ts.ns;
}

static int op_0f(insn_t *i)
{
    x86cpu_t *c = i->c;
    const uint8_t op = fetch8(i);
    const int sz = i->opsz;

    switch (op) {
    case 0x05:  /* SYSCALL */
        c->r[RCX] = i->pc;
        c->r[R11] = x86_get_flags(c) | (c->eflags & ~(uint64_t)F_ARITH) | 0x202u;
        c->rip = i->pc;
        return XL_EV_SYSCALL;
    case 0x0B:  /* UD2 */
        return X86_UD(i);
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:   /* prefetch, hint NOPs, ENDBR */
        x86_modrm(i);
        break;
    case 0x01:
        x86_modrm(i);
        if (i->modrm == 0xF9) {  /* RDTSCP */
            uint64_t t = host_tsc();
            c->r[RAX] = (uint32_t)t;
            c->r[RDX] = t >> 32;
            c->r[RCX] = 0;
            break;
        }
        return X86_UD(i);
    case 0x31: { /* RDTSC */
        uint64_t t = host_tsc();
        c->r[RAX] = (uint32_t)t;
        c->r[RDX] = t >> 32;
        break;
    }
    case 0xA2:  /* CPUID */
        x86_cpuid(c);
        break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, sz);
        if (x86_cond(c, op & 15u)) {
            setreg(i, i->reg, sz, v);
        } else if (sz == 4) {
            c->r[i->reg] = (uint32_t)c->r[i->reg];
        }
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        int64_t rel = (int32_t)fetch32(i);
        c->rip = x86_cond(c, op & 15u) ? i->pc + (uint64_t)rel : i->pc;
        return 0;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        x86_modrm(i);
        loc_t l = rm_loc(i);
        st(i, &l, 1, x86_cond(c, op & 15u) ? 1u : 0u);
        break;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        x86_modrm(i);
        int bop = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        bit_op(i, bop, sz, getreg(i, i->reg, sz), true);
        break;
    }
    case 0xBA: {
        x86_modrm(i);
        unsigned sub = i->reg & 7u;
        uint64_t imm = fetch8(i);
        if (sub < 4u) {
            return X86_UD(i);
        }
        bit_op(i, (int)sub - 4, sz, imm, false);
        break;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { /* SHLD/SHRD */
        x86_modrm(i);
        unsigned cnt = (op & 1u) ? (unsigned)c->r[RCX] : fetch8(i);
        loc_t l = rm_loc(i);
        const unsigned bits = (unsigned)sz * 8u;
        cnt &= sz == 8 ? 63u : 31u;
        if (cnt == 0u) {
            break;
        }
        const uint64_t m = szmask(sz);
        uint64_t d = ld(i, &l, sz), s = getreg(i, i->reg, sz);
        uint64_t r, cf;
        if (op <= 0xA5) {
            r = cnt >= bits ? (s << (cnt - bits)) & m : ((d << cnt) | (s >> (bits - cnt))) & m;
            cf = cnt <= bits ? (d >> (bits - cnt)) & 1u : 0u;
        } else {
            r = cnt >= bits ? (s >> (cnt - bits)) & m : ((d >> cnt) | (s << (bits - cnt))) & m;
            cf = (d >> (cnt - 1u)) & 1u;
        }
        st(i, &l, sz, r);
        uint64_t of = ((r ^ d) & sbit(sz)) != 0u;
        x86_set_flags(c, (cf ? F_CF : 0u) | parity_flag(r) | (r == 0u ? F_ZF : 0u) |
                         ((r & sbit(sz)) ? F_SF : 0u) | (of ? F_OF : 0u));
        break;
    }
    case 0xAE: {
        x86_modrm(i);
        unsigned sub = i->reg & 7u;
        if (i->mod == 3) {
            if (i->rep != 0u) {
                return X86_UD(i);   /* RD/WRFSBASE: not reported by CPUID */
            }
            if (sub >= 5u) {        /* LFENCE, MFENCE, SFENCE */
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                break;
            }
            return X86_UD(i);
        }
        uint64_t a = x86_ea(i);
        switch (sub) {
        case 0: case 1: {   /* FXSAVE, FXRSTOR */
            int ev = sub == 0u ? x86_fxsave(i, a) : x86_fxrstor(i, a);
            if (ev != 0) {
                return ev;
            }
            break;
        }
        case 2: {
            uint32_t v = (uint32_t)mrd(a, 4);
            if ((v & 0xFFFF0000u) != 0u) {
                return x86_fault(i, XL_SIGSEGV, XL_SI_KERNEL, 0);
            }
            c->mxcsr = v;
            x86_mxcsr_apply(c);
            break;
        }
        case 3:
            x86_mxcsr_collect(c);
            mwr(a, 4, c->mxcsr);
            break;
        case 7:     /* CLFLUSH */
            break;
        default:
            return X86_UD(i);
        }
        break;
    }
    case 0xAF: {
        x86_modrm(i);
        loc_t l = rm_loc(i);
        int64_t a = sx(getreg(i, i->reg, sz), sz), b = sx(ld(i, &l, sz), sz);
        uint64_t r;
        bool ovf;
        if (sz == 8) {
            __int128 p = (__int128)a * b;
            r = (uint64_t)p;
            ovf = p != (__int128)(int64_t)r;
        } else {
            int64_t p = a * b;
            r = (uint64_t)p & szmask(sz);
            ovf = p != sx(r, sz);
        }
        setreg(i, i->reg, sz, r);
        setcc(c, CC_MUL, sz, 0, ovf, r);
        break;
    }
    case 0xB0: case 0xB1: { /* CMPXCHG */
        const int s = op == 0xB0 ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t acc = getreg(i, RAX, s);
        uint64_t src = getreg(i, i->reg, s);
        if (l.reg < 0) {
            uint64_t cur = acc;
            if (cas(l.addr, s, &cur, src)) {
                (void)x86_alu(c, 7, s, acc, acc, 0);
            } else {
                (void)x86_alu(c, 7, s, acc, cur, 0);
                setreg(i, RAX, s, cur);
            }
        } else {
            uint64_t cur = getreg(i, l.reg, s);
            (void)x86_alu(c, 7, s, acc, cur, 0);
            if (cur == acc) {
                setreg(i, l.reg, s, src);
            } else {
                setreg(i, RAX, s, cur);
            }
        }
        break;
    }
    case 0xC0: case 0xC1: { /* XADD */
        const int s = op == 0xC0 ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t src = getreg(i, i->reg, s);
        uint64_t old;
        if (l.reg < 0) {
            old = mrd(l.addr, s);
            for (;;) {
                uint64_t r = x86_alu(c, 0, s, old, src, 0);
                if (cas(l.addr, s, &old, r)) break;
            }
        } else {
            old = getreg(i, l.reg, s);
            uint64_t r = x86_alu(c, 0, s, old, src, 0);
            setreg(i, i->reg, s, old);
            setreg(i, l.reg, s, r);
            break;
        }
        setreg(i, i->reg, s, old);
        break;
    }
    case 0xC7: {
        x86_modrm(i);
        unsigned sub = i->reg & 7u;
        if (sub != 1u || i->mod == 3) {
            return X86_UD(i);   /* RDRAND/RDSEED are not reported */
        }
        uint64_t a = x86_ea(i);
        if (REX_W(i)) {     /* CMPXCHG16B */
            if ((a & 15u) != 0u) {
                return x86_fault(i, XL_SIGSEGV, XL_SI_KERNEL, 0);
            }
            xl_lock(&g_misaligned_atomic);
            uint64_t lo = mrd(a, 8), hi = mrd(a + 8, 8);
            bool eq = lo == c->r[RAX] && hi == c->r[RDX];
            if (eq) {
                mwr(a, 8, c->r[RBX]);
                mwr(a + 8, 8, c->r[RCX]);
            }
            xl_unlock(&g_misaligned_atomic);
            if (!eq) {
                c->r[RAX] = lo;
                c->r[RDX] = hi;
            }
            uint64_t f = x86_get_flags(c) & ~(uint64_t)F_ZF;
            x86_set_flags(c, f | (eq ? F_ZF : 0u));
        } else {            /* CMPXCHG8B */
            uint64_t exp = (c->r[RDX] << 32) | (uint32_t)c->r[RAX];
            uint64_t cur = exp;
            bool eq = cas(a, 8, &cur, (c->r[RCX] << 32) | (uint32_t)c->r[RBX]);
            if (!eq) {
                c->r[RAX] = (uint32_t)cur;
                c->r[RDX] = cur >> 32;
            }
            uint64_t f = x86_get_flags(c) & ~(uint64_t)F_ZF;
            x86_set_flags(c, f | (eq ? F_ZF : 0u));
        }
        break;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        x86_modrm(i);
        const int s = (op & 1u) ? 2 : 1;
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, s);
        if (op >= 0xBE) {
            v = (uint64_t)sx(v, s);
        }
        setreg(i, i->reg, sz, v & szmask(sz));
        break;
    }
    case 0xB8: { /* POPCNT */
        if (i->rep != 0xF3) {
            return X86_UD(i);
        }
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, sz);
        uint64_t r = (uint64_t)__builtin_popcountll(v);
        setreg(i, i->reg, sz, r);
        x86_set_flags(c, v == 0u ? F_ZF : 0u);
        break;
    }
    case 0xBC: case 0xBD: { /* BSF/BSR (also F3-prefixed: no BMI here) */
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, sz);
        uint64_t f = x86_get_flags(c) & ~(uint64_t)F_ZF;
        if (v == 0u) {
            x86_set_flags(c, f | F_ZF);
            break;
        }
        uint64_t r = op == 0xBC ? (uint64_t)__builtin_ctzll(v) : (uint64_t)(63 - __builtin_clzll(v));
        setreg(i, i->reg, sz, r);
        x86_set_flags(c, f);
        break;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int r = (op & 7) | (REX_B(i) ? 8 : 0);
        if (sz == 8) {
            c->r[r] = __builtin_bswap64(c->r[r]);
        } else {
            c->r[r] = __builtin_bswap32((uint32_t)c->r[r]);
        }
        break;
    }
    default: {
        int ev = x86_sse(i, op);
        if (ev != 0) {
            return ev;
        }
        break;
    }
    }
    c->rip = i->pc;
    return 0;
}

/* ---- one instruction --------------------------------------------------------- */

int x86_step(gthread_t *t, x86cpu_t *c)
{
    insn_t I;
    insn_t *i = &I;
    i->t = t;
    i->c = c;
    i->start = c->rip;
    i->pc = c->rip;
    i->rex = 0;
    i->adsz32 = 0;
    i->rep = 0;
    i->p66 = 0;
    i->lock = 0;
    i->seg = 0;

    uint8_t op;
    for (;;) {
        op = fetch8(i);
        switch (op) {
        case 0x66: i->p66 = 1; continue;
        case 0x67: i->adsz32 = 1; continue;
        case 0xF2: case 0xF3: i->rep = op; continue;
        case 0xF0: i->lock = 1; continue;
        case 0x2E: case 0x36: case 0x3E: case 0x26: continue;
        case 0x64: i->seg = 4; continue;
        case 0x65: i->seg = 5; continue;
        default: break;
        }
        break;
    }
    if ((op & 0xF0u) == 0x40u) {
        i->rex = op;
        op = fetch8(i);
    }
    i->opsz = REX_W(i) ? 8 : (i->p66 ? 2 : 4);
    const int sz = i->opsz;

    if (op < 0x40u && (op & 7u) < 6u) {     /* ADD OR ADC SBB AND SUB XOR CMP */
        const int aop = op >> 3;
        const int s = (op & 1u) ? sz : 1;
        switch (op & 7u) {
        case 0: case 1: {
            x86_modrm(i);
            loc_t l = rm_loc(i);
            alu_rmw(i, &l, aop, s, getreg(i, i->reg, s));
            break;
        }
        case 2: case 3: {
            x86_modrm(i);
            loc_t l = rm_loc(i);
            uint64_t cin = get_cf(c);
            uint64_t r = x86_alu(c, aop, s, getreg(i, i->reg, s), ld(i, &l, s), cin);
            if (aop != 7) {
                setreg(i, i->reg, s, r);
            }
            break;
        }
        case 4: {
            uint64_t imm = fetch8(i);
            uint64_t r = x86_alu(c, aop, 1, c->r[RAX], imm, get_cf(c));
            if (aop != 7) setreg(i, RAX, 1, r);
            break;
        }
        default: {
            uint64_t imm = fetch_imm(i, sz == 8 ? 4 : sz);
            uint64_t r = x86_alu(c, aop, sz, c->r[RAX], imm, get_cf(c));
            if (aop != 7) setreg(i, RAX, sz, r);
            break;
        }
        }
        c->rip = i->pc;
        return 0;
    }

    switch (op) {
    case 0x0F:
        return op_0f(i);

    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int r = (op & 7) | (REX_B(i) ? 8 : 0);
        push(i, stack_sz(i), c->r[r]);
        break;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int r = (op & 7) | (REX_B(i) ? 8 : 0);
        uint64_t v = pop(i, stack_sz(i));
        if (i->p66) {
            setreg(i, r, 2, v);
        } else {
            c->r[r] = v;
        }
        break;
    }
    case 0x63: { /* MOVSXD */
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, 4);
        setreg(i, i->reg, sz, sz == 8 ? (uint64_t)(int64_t)(int32_t)v : v);
        break;
    }
    case 0x68:
        push(i, stack_sz(i), fetch_imm(i, i->p66 ? 2 : 4));
        break;
    case 0x6A:
        push(i, stack_sz(i), fetch_imm(i, 1));
        break;
    case 0x69: case 0x6B: {
        x86_modrm(i);
        uint64_t imm = fetch_imm(i, op == 0x6B ? 1 : (sz == 8 ? 4 : sz));
        loc_t l = rm_loc(i);
        int64_t a = sx(ld(i, &l, sz), sz), b = sx(imm, sz);
        uint64_t r;
        bool ovf;
        if (sz == 8) {
            __int128 p = (__int128)a * b;
            r = (uint64_t)p;
            ovf = p != (__int128)(int64_t)r;
        } else {
            int64_t p = a * b;
            r = (uint64_t)p & szmask(sz);
            ovf = p != sx(r, sz);
        }
        setreg(i, i->reg, sz, r);
        setcc(c, CC_MUL, sz, 0, ovf, r);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int64_t rel = (int8_t)fetch8(i);
        c->rip = x86_cond(c, op & 15u) ? i->pc + (uint64_t)rel : i->pc;
        return 0;
    }
    case 0x80: case 0x81: case 0x83: {
        const int s = op == 0x80 ? 1 : sz;
        x86_modrm(i);
        uint64_t imm = fetch_imm(i, op == 0x81 ? (sz == 8 ? 4 : sz) : 1);
        loc_t l = rm_loc(i);
        alu_rmw(i, &l, i->reg & 7, s, imm);
        break;
    }
    case 0x84: case 0x85: {
        const int s = op == 0x84 ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        (void)x86_alu(c, 4, s, ld(i, &l, s), getreg(i, i->reg, s), 0);
        break;
    }
    case 0x86: case 0x87: {
        const int s = op == 0x86 ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        uint64_t rv = getreg(i, i->reg, s);
        if (l.reg < 0) {
            setreg(i, i->reg, s, xchg_mem(l.addr, s, rv));
        } else {
            uint64_t mv = getreg(i, l.reg, s);
            setreg(i, l.reg, s, rv);
            setreg(i, i->reg, s, mv);
        }
        break;
    }
    case 0x88: case 0x89: {
        const int s = op == 0x88 ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        st(i, &l, s, getreg(i, i->reg, s));
        break;
    }
    case 0x8A: case 0x8B: {
        const int s = op == 0x8A ? 1 : sz;
        x86_modrm(i);
        loc_t l = rm_loc(i);
        setreg(i, i->reg, s, ld(i, &l, s));
        break;
    }
    case 0x8C: { /* MOV r/m, Sreg */
        x86_modrm(i);
        static const uint16_t sel[8] = { 0, 0x33, 0x2B, 0, 0, 0, 0, 0 };
        loc_t l = rm_loc(i);
        st(i, &l, l.reg >= 0 ? sz : 2, sel[i->reg & 7u]);
        break;
    }
    case 0x8D: { /* LEA: the offset, no segment base */
        x86_modrm(i);
        if (i->mod == 3) {
            return X86_UD(i);
        }
        setreg(i, i->reg, sz, ea_offset(i));
        break;
    }
    case 0x8E: /* MOV Sreg, r/m: segments are flat */
        x86_modrm(i);
        break;
    case 0x8F: {
        x86_modrm(i);
        int s = stack_sz(i);
        uint64_t v = mrd(c->r[RSP], s);
        /* the address uses RSP as already incremented */
        c->r[RSP] += (uint64_t)s;
        loc_t l = rm_loc(i);
        st(i, &l, s, v);
        break;
    }
    case 0x90:
        if (REX_B(i)) {
            uint64_t v = getreg(i, R8, sz);
            setreg(i, R8, sz, getreg(i, RAX, sz));
            setreg(i, RAX, sz, v);
        }
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        int r = (op & 7) | (REX_B(i) ? 8 : 0);
        uint64_t v = getreg(i, r, sz);
        setreg(i, r, sz, getreg(i, RAX, sz));
        setreg(i, RAX, sz, v);
        break;
    }
    case 0x98:
        if (sz == 8) c->r[RAX] = (uint64_t)(int64_t)(int32_t)c->r[RAX];
        else if (sz == 4) c->r[RAX] = (uint32_t)(int32_t)(int16_t)c->r[RAX];
        else setreg(i, RAX, 2, (uint64_t)(int64_t)(int8_t)c->r[RAX]);
        break;
    case 0x99:
        if (sz == 8) c->r[RDX] = (int64_t)c->r[RAX] < 0 ? ~0ULL : 0u;
        else if (sz == 4) c->r[RDX] = (int32_t)c->r[RAX] < 0 ? 0xFFFFFFFFu : 0u;
        else setreg(i, RDX, 2, (int16_t)c->r[RAX] < 0 ? 0xFFFFu : 0u);
        break;
    case 0x9B:  /* FWAIT */
        break;
    case 0x9C: {
        uint64_t f = x86_get_flags(c) | (c->eflags & ~(uint64_t)F_ARITH) | 0x202u;
        push(i, stack_sz(i), f & ~(uint64_t)(F_TF));
        break;
    }
    case 0x9D: {
        uint64_t f = pop(i, stack_sz(i));
        uint64_t keep = F_DF | F_AC | F_ID;
        c->eflags = (c->eflags & ~keep) | (f & keep);
        x86_set_flags(c, f);
        break;
    }
    case 0x9E: { /* SAHF */
        uint64_t f = x86_get_flags(c) & ~(uint64_t)0xD5u;
        x86_set_flags(c, f | ((c->r[RAX] >> 8) & 0xD5u));
        break;
    }
    case 0x9F: /* LAHF */
        c->r[RAX] = (c->r[RAX] & ~0xFF00ULL) | ((x86_get_flags(c) | 2u) & 0xFFu) << 8;
        break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        const int s = (op & 1u) ? sz : 1;
        uint64_t a = i->adsz32 ? fetch32(i) : fetch64(i);
        if (i->seg == 4) a += c->fs_base;
        if (i->seg == 5) a += c->gs_base;
        if (op < 0xA2) setreg(i, RAX, s, mrd(a, s));
        else mwr(a, s, getreg(i, RAX, s));
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        int ev = string_op(i, op);
        if (ev != 0) return ev;
        break;
    }
    case 0xA8:
        (void)x86_alu(c, 4, 1, c->r[RAX], fetch8(i), 0);
        break;
    case 0xA9:
        (void)x86_alu(c, 4, sz, c->r[RAX], fetch_imm(i, sz == 8 ? 4 : sz), 0);
        break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setreg(i, (op & 7) | (REX_B(i) ? 8 : 0), 1, fetch8(i));
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int r = (op & 7) | (REX_B(i) ? 8 : 0);
        uint64_t v = sz == 8 ? fetch64(i) : sz == 4 ? fetch32(i) : fetch16(i);
        setreg(i, r, sz, v);
        break;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const int s = (op & 1u) ? sz : 1;
        x86_modrm(i);
        unsigned cnt = op <= 0xC1 ? fetch8(i) : op <= 0xD1 ? 1u : (unsigned)c->r[RCX];
        loc_t l = rm_loc(i);
        uint64_t v = ld(i, &l, s);
        uint64_t r = x86_shift(c, i->reg & 7, s, v, cnt);
        if ((cnt & (s == 8 ? 63u : 31u)) != 0u) {
            st(i, &l, s, r);
        }
        break;
    }
    case 0xC2: {
        uint64_t n = fetch16(i);
        c->rip = pop(i, 8);
        c->r[RSP] += n;
        return 0;
    }
    case 0xC3:
        c->rip = pop(i, 8);
        return 0;
    case 0xC6: case 0xC7: {
        const int s = op == 0xC6 ? 1 : sz;
        x86_modrm(i);
        if ((i->reg & 7u) != 0u) {
            return X86_UD(i);   /* XABORT/XBEGIN: no TSX */
        }
        uint64_t imm = fetch_imm(i, s == 8 ? 4 : s);
        loc_t l = rm_loc(i);
        st(i, &l, s, imm);
        break;
    }
    case 0xC8: { /* ENTER */
        uint64_t frame = fetch16(i);
        unsigned level = fetch8(i) & 31u;
        push(i, 8, c->r[RBP]);
        uint64_t fp = c->r[RSP];
        for (unsigned k = 1; k < level; ++k) {
            push(i, 8, mrd(c->r[RBP] - 8u * k, 8));
        }
        if (level > 0u) {
            push(i, 8, fp);
        }
        c->r[RBP] = fp;
        c->r[RSP] -= frame;
        break;
    }
    case 0xC9: /* LEAVE */
        c->r[RSP] = c->r[RBP];
        c->r[RBP] = pop(i, 8);
        break;
    case 0xCA: case 0xCB: { /* RETF: back into the 64-bit code segment only */
        const unsigned s = REX_W(i) ? 8u : 4u;
        const uint64_t n = op == 0xCA ? fetch16(i) : 0u;
        const uint64_t rip = pop(i, s);
        const uint64_t cs = pop(i, s) & 0xFFFFu;
        if (cs == 0x23u) {
            return X86_UD(i);   /* the 32-bit segment: no compatibility mode */
        }
        c->r[RSP] += n;
        c->rip = rip;
        return 0;
    }
    case 0xCF: { /* IRETQ from user mode, to the same privilege level: pops
                  * RIP, CS, RFLAGS, RSP, SS. Wine's x86_64 syscall dispatcher
                  * returns to Windows code this way (a full context restore
                  * in one instruction), so without it no PE code ran at all. */
        if (!REX_W(i)) {
            return X86_UD(i);   /* IRETD/IRET: 32-bit frames, not here */
        }
        const uint64_t rip = pop(i, 8);
        const uint64_t cs = pop(i, 8) & 0xFFFFu;
        const uint64_t f = pop(i, 8);
        const uint64_t rsp = pop(i, 8);
        (void)pop(i, 8);        /* SS */
        if (cs == 0x23u) {
            return X86_UD(i);
        }
        const uint64_t keep = F_DF | F_AC | F_ID;
        c->eflags = (c->eflags & ~keep) | (f & keep);
        x86_set_flags(c, f);
        c->r[RSP] = rsp;
        c->rip = rip;
        return 0;
    }
    case 0xCC: /* INT3: reported after the instruction */
        c->rip = i->pc;
        return x86_fault(i, XL_SIGTRAP, XL_SI_KERNEL, 0);
    case 0xCD:
        (void)fetch8(i);
        return x86_fault(i, XL_SIGSEGV, XL_SI_KERNEL, 0);
    case 0xD7: { /* XLAT */
        uint64_t a = c->r[RBX] + (c->r[RAX] & 0xFFu);
        if (i->seg == 4) a += c->fs_base;
        if (i->seg == 5) a += c->gs_base;
        setreg(i, RAX, 1, mrd(a, 1));
        break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        int ev = x86_x87(i, op);
        if (ev != 0) return ev;
        break;
    }
    case 0xE0: case 0xE1: case 0xE2: { /* LOOPNE, LOOPE, LOOP */
        int64_t rel = (int8_t)fetch8(i);
        uint64_t cnt = (i->adsz32 ? (uint32_t)c->r[RCX] : c->r[RCX]) - 1u;
        c->r[RCX] = i->adsz32 ? (uint32_t)cnt : cnt;
        bool take = cnt != 0u;
        if (op == 0xE0) take = take && !x86_cond(c, 4);
        if (op == 0xE1) take = take && x86_cond(c, 4);
        c->rip = take ? i->pc + (uint64_t)rel : i->pc;
        return 0;
    }
    case 0xE3: {
        int64_t rel = (int8_t)fetch8(i);
        uint64_t cnt = i->adsz32 ? (uint32_t)c->r[RCX] : c->r[RCX];
        c->rip = cnt == 0u ? i->pc + (uint64_t)rel : i->pc;
        return 0;
    }
    case 0xE8: {
        int64_t rel = (int32_t)fetch32(i);
        push(i, 8, i->pc);
        c->rip = i->pc + (uint64_t)rel;
        return 0;
    }
    case 0xE9: {
        int64_t rel = (int32_t)fetch32(i);
        c->rip = i->pc + (uint64_t)rel;
        return 0;
    }
    case 0xEB: {
        int64_t rel = (int8_t)fetch8(i);
        c->rip = i->pc + (uint64_t)rel;
        return 0;
    }
    case 0xF4: case 0xFA: case 0xFB: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        /* HLT, CLI, STI, IN, OUT: privileged */
        return x86_fault(i, XL_SIGSEGV, XL_SI_KERNEL, 0);
    case 0xF5: {
        uint64_t f = x86_get_flags(c);
        x86_set_flags(c, f ^ F_CF);
        break;
    }
    case 0xF8: case 0xF9: {
        uint64_t f = x86_get_flags(c) & ~(uint64_t)F_CF;
        x86_set_flags(c, f | (op == 0xF9 ? F_CF : 0u));
        break;
    }
    case 0xFC:
        c->eflags &= ~(uint64_t)F_DF;
        break;
    case 0xFD:
        c->eflags |= F_DF;
        break;
    case 0xF6: case 0xF7: {
        x86_modrm(i);
        int ev = group3(i, op == 0xF6 ? 1 : sz);
        if (ev != 0) return ev;
        break;
    }
    case 0xFE: case 0xFF: {
        const int s = op == 0xFE ? 1 : sz;
        x86_modrm(i);
        unsigned sub = i->reg & 7u;
        if (sub <= 1u) {
            loc_t l = rm_loc(i);
            if (i->lock && l.reg < 0) {
                uint64_t old = mrd(l.addr, s);
                for (;;) {
                    uint64_t r = x86_inc_dec(c, s, old, sub == 1u);
                    if (cas(l.addr, s, &old, r)) break;
                }
            } else {
                st(i, &l, s, x86_inc_dec(c, s, ld(i, &l, s), sub == 1u));
            }
            break;
        }
        if (op == 0xFE) {
            return X86_UD(i);
        }
        switch (sub) {
        case 2: { /* CALL r/m64 */
            loc_t l = rm_loc(i);
            uint64_t target = ld(i, &l, 8);
            push(i, 8, i->pc);
            c->rip = target;
            return 0;
        }
        case 4: { /* JMP r/m64 */
            loc_t l = rm_loc(i);
            c->rip = ld(i, &l, 8);
            return 0;
        }
        case 6: { /* PUSH r/m */
            loc_t l = rm_loc(i);
            int ss = stack_sz(i);
            push(i, ss, ld(i, &l, ss));
            break;
        }
        default:
            return X86_UD(i);
        }
        break;
    }
    default:
        return X86_UD(i);
    }
    c->rip = i->pc;
    return 0;
}

#ifdef XL_PROFILE
uint64_t g_prof_count;
uint64_t g_prof_op[512];
void guest_prof_dump(void);
void guest_prof_dump(void)
{
    extern uint64_t g_xb_entries, g_xb_trans, g_xb_uops, g_xb_fallbacks, g_xb_fb_op[512];
    xl_log("blocks: %lu entries, %lu translations, %lu uops (%lu per block), %lu fallbacks",
           g_xb_entries, g_xb_trans, g_xb_uops, g_xb_entries ? g_xb_uops / g_xb_entries : 0,
           g_xb_fallbacks);
    for (int k = 0; k < 512; ++k) g_prof_op[k] += g_xb_fb_op[k];
    g_prof_count += g_xb_fallbacks;
    xl_log("instructions: %lu", g_prof_count);
    for (int n = 0; n < 40; ++n) {
        int best = -1;
        for (int k = 0; k < 512; ++k) {
            if (g_prof_op[k] != 0u && (best < 0 || g_prof_op[k] > g_prof_op[best])) best = k;
        }
        if (best < 0) break;
        xl_log("  %s%02x: %lu", best >= 256 ? "0f " : "", best & 255, g_prof_op[best]);
        g_prof_op[best] = 0;
    }
#if defined(__aarch64__)
    extern void x86_jit_prof_dump(void);
    x86_jit_prof_dump();
#endif
}
#endif

/* The plain interpreter: one decoded instruction at a time. guest_run()
 * (x86_block.c) runs this way when XL_NOBLOCKS=1. */
int x86_run_interp(gthread_t *t)
{
    x86cpu_t *c = X86(t);
#ifdef XL_WATCHDOG
    uint64_t spin = 0;
#endif
    for (;;) {
        if (XL_UNLIKELY(t->sig_pending_flag)) {
            return XL_EV_SIGNAL;
        }
#ifdef XL_WATCHDOG
        /* debugging aid: where is a guest that stopped making system calls? */
        if (XL_UNLIKELY(++spin % XL_WATCHDOG == 0u)) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)c->rip;
            xl_log("watchdog: %lu instructions without a system call; pc %lx: "
                   "%02x %02x %02x %02x %02x %02x %02x %02x", spin, c->rip,
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            guest_dump(t);
        }
#endif
#ifdef XL_PROFILE
        {
            const uint8_t *p = (const uint8_t *)(uintptr_t)c->rip;
            while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0xF0 || *p == 0x64 || *p == 0x65 || *p == 0x2E || *p == 0x3E || *p == 0x67) ++p;
            if ((*p & 0xF0) == 0x40) ++p;
            ++g_prof_count;
            ++g_prof_op[*p == 0x0F ? 256 + p[1] : *p];
        }
#endif
        int ev = x86_step(t, c);
        if (XL_UNLIKELY(ev != 0)) {
            return ev;
        }
    }
}
