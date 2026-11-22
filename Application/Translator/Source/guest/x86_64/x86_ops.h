#pragma once

/*
 * The x86 guest's arithmetic, flags and register helpers, shared by the
 * instruction-at-a-time interpreter (x86_cpu.c) and the block engine
 * (x86_block.c) so both compute exactly the same results.
 */

#include "x86.h"

static inline uint64_t szmask(int sz)
{
    return sz == 8 ? ~0ULL : (1ULL << (sz * 8)) - 1ULL;
}

static inline uint64_t sbit(int sz)
{
    return 1ULL << (sz * 8 - 1);
}

static inline int64_t sx(uint64_t v, int sz)
{
    switch (sz) {
    case 1: return (int8_t)v;
    case 2: return (int16_t)v;
    case 4: return (int32_t)v;
    default: return (int64_t)v;
    }
}

static inline uint64_t parity_flag(uint64_t r)
{
    return (__builtin_popcountll(r & 0xFFu) & 1) == 0 ? F_PF : 0u;
}

static inline void setcc(x86cpu_t *c, uint32_t op, int sz, uint64_t a,
                         uint64_t b, uint64_t r)
{
    c->cc_op = op;
    c->cc_size = (uint32_t)sz;
    c->cc_a = a;
    c->cc_b = b;
    c->cc_r = r;
}


static inline uint64_t get_cf(x86cpu_t *c)
{
    switch (c->cc_op) {
    case CC_SUB: return c->cc_a < c->cc_b;
    case CC_ADD: return c->cc_r < c->cc_a;
    case CC_LOGIC: return 0;
    default: return x86_get_flags(c) & F_CF;
    }
}

static inline bool x86_cond(x86cpu_t *c, unsigned cc)
{
    bool r;
    if (c->cc_op == CC_SUB) {
        const int sz = (int)c->cc_size;
        const uint64_t a = c->cc_a, b = c->cc_b;
        switch (cc >> 1) {
        case 0: r = ((a ^ b) & (a ^ c->cc_r) & sbit(sz)) != 0u; break;
        case 1: r = a < b; break;
        case 2: r = a == b; break;
        case 3: r = a <= b; break;
        case 4: r = (c->cc_r & sbit(sz)) != 0u; break;
        case 5: r = parity_flag(c->cc_r) != 0u; break;
        case 6: r = sx(a, sz) < sx(b, sz); break;
        default: r = sx(a, sz) <= sx(b, sz); break;
        }
        return r ^ (cc & 1u);
    }
    if (c->cc_op == CC_LOGIC) {
        const uint64_t v = c->cc_r;
        const bool s = (v & sbit((int)c->cc_size)) != 0u;
        switch (cc >> 1) {
        case 0: r = false; break;
        case 1: r = false; break;
        case 2: r = v == 0u; break;
        case 3: r = v == 0u; break;
        case 4: r = s; break;
        case 5: r = parity_flag(v) != 0u; break;
        case 6: r = s; break;
        default: r = v == 0u || s; break;
        }
        return r ^ (cc & 1u);
    }
    const uint64_t f = x86_get_flags(c);
    const bool of = (f & F_OF) != 0u, sf = (f & F_SF) != 0u;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = (f & F_CF) != 0u; break;
    case 2: r = (f & F_ZF) != 0u; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0u; break;
    case 4: r = sf; break;
    case 5: r = (f & F_PF) != 0u; break;
    case 6: r = sf != of; break;
    default: r = (f & F_ZF) != 0u || sf != of; break;
    }
    return r ^ (cc & 1u);
}


/* ---- registers ---- */

static inline uint64_t x86_rd(const x86cpu_t *c, int r, int sz, bool rex)
{
    switch (sz) {
    case 1:
        if (rex || r < 4 || r >= 8) {
            return (uint8_t)c->r[r];
        }
        return (uint8_t)(c->r[r - 4] >> 8);
    case 2: return (uint16_t)c->r[r];
    case 4: return (uint32_t)c->r[r];
    default: return c->r[r];
    }
}

static inline void x86_wr(x86cpu_t *c, int r, int sz, bool rex, uint64_t v)
{
    switch (sz) {
    case 1:
        if (rex || r < 4 || r >= 8) {
            c->r[r] = (c->r[r] & ~0xFFULL) | (v & 0xFFu);
        } else {
            c->r[r - 4] = (c->r[r - 4] & ~0xFF00ULL) | ((v & 0xFFu) << 8);
        }
        break;
    case 2: c->r[r] = (c->r[r] & ~0xFFFFULL) | (v & 0xFFFFu); break;
    case 4: c->r[r] = (uint32_t)v; break;
    default: c->r[r] = v; break;
    }
}

/* ---- ALU ---- */

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP; cin for ADC/SBB */
static inline uint64_t x86_alu(x86cpu_t *c, int op, int sz, uint64_t a, uint64_t b, uint64_t cin)
{
    const uint64_t m = szmask(sz);
    a &= m;
    b &= m;
    uint64_t r;
    switch (op) {
    case 0: r = (a + b) & m; setcc(c, CC_ADD, sz, a, b, r); break;
    case 1: r = a | b; setcc(c, CC_LOGIC, sz, a, b, r); break;
    case 2: r = (a + b + cin) & m; setcc(c, CC_ADC, sz, a, b, r); c->cc_cin = cin; break;
    case 3: r = (a - b - cin) & m; setcc(c, CC_SBB, sz, a, b, r); c->cc_cin = cin; break;
    case 4: r = a & b; setcc(c, CC_LOGIC, sz, a, b, r); break;
    case 6: r = a ^ b; setcc(c, CC_LOGIC, sz, a, b, r); break;
    default: r = (a - b) & m; setcc(c, CC_SUB, sz, a, b, r); break;
    }
    return r;
}

static inline uint64_t x86_inc_dec(x86cpu_t *c, int sz, uint64_t a, bool dec)
{
    uint64_t cin = get_cf(c);
    a &= szmask(sz);
    uint64_t r = (dec ? a - 1u : a + 1u) & szmask(sz);
    setcc(c, dec ? CC_DEC : CC_INC, sz, a, 1u, r);
    c->cc_cin = cin;
    return r;
}


uint64_t x86_shift(x86cpu_t *c, int kind, int sz, uint64_t v, unsigned count);
int x86_step(gthread_t *t, x86cpu_t *c);
/* The three engines guest_run() (x86_jit.c) chooses between. */
int x86_run_interp(gthread_t *t);
int x86_run_blocks(gthread_t *t);
int x86_run_jit(gthread_t *t);
/* Length of the instruction at p (0: not decodable); *ends: it may
 * transfer control. */
int x86_insn_len(const uint8_t *p, bool *ends);
