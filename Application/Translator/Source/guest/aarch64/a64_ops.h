#pragma once

/*
 * The AArch64 guest's register, flag and operand helpers, shared by the
 * instruction-at-a-time interpreter (a64_cpu.c) and the block engine
 * (a64_block.c).
 */

#include "a64.h"

static inline uint64_t xr(const a64cpu_t *c, unsigned n)
{
    return n == 31u ? 0u : c->x[n];
}

static inline uint64_t xsp(const a64cpu_t *c, unsigned n)
{
    return n == 31u ? c->x[A64_SP] : c->x[n];
}

static inline void wx(a64cpu_t *c, unsigned n, uint64_t v, bool sf)
{
    if (n != 31u) {
        c->x[n] = sf ? v : (uint32_t)v;
    }
}

static inline void wsp(a64cpu_t *c, unsigned n, uint64_t v, bool sf)
{
    if (!sf) v = (uint32_t)v;
    if (n == 31u) c->x[A64_SP] = v; else c->x[n] = v;
}

static inline bool a64_cond(uint32_t f, unsigned cond)
{
    const bool n = (f & NZCV_N) != 0u, z = (f & NZCV_Z) != 0u;
    const bool cy = (f & NZCV_C) != 0u, v = (f & NZCV_V) != 0u;
    bool r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: return true;           /* AL, NV */
    }
    return (cond & 1u) ? !r : r;
}

static inline uint64_t add_carry(a64cpu_t *c, uint64_t a, uint64_t b, uint64_t carry,
                          bool sf, bool setflags)
{
    uint64_t r;
    if (sf) {
        r = a + b + carry;
        if (setflags) {
            unsigned __int128 u = (unsigned __int128)a + b + carry;
            uint32_t f = 0;
            if (r >> 63) f |= NZCV_N;
            if (r == 0u) f |= NZCV_Z;
            if ((u >> 64) != 0u) f |= NZCV_C;
            if (((a ^ r) & (b ^ r)) >> 63) f |= NZCV_V;
            c->nzcv = f;
        }
    } else {
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        uint64_t u = (uint64_t)a32 + b32 + carry;
        r = (uint32_t)u;
        if (setflags) {
            uint32_t f = 0;
            if (r >> 31) f |= NZCV_N;
            if (r == 0u) f |= NZCV_Z;
            if (u >> 32) f |= NZCV_C;
            if (((a32 ^ (uint32_t)r) & (b32 ^ (uint32_t)r)) >> 31) f |= NZCV_V;
            c->nzcv = f;
        }
    }
    return r;
}

static inline void logic_flags(a64cpu_t *c, uint64_t r, bool sf)
{
    uint32_t f = 0;
    if (sf ? (r >> 63) : ((r >> 31) & 1u)) f |= NZCV_N;
    if ((sf ? r : (uint32_t)r) == 0u) f |= NZCV_Z;
    c->nzcv = f;
}

static inline uint64_t shift_reg(uint64_t v, unsigned type, unsigned amount, bool sf)
{
    const unsigned bits = sf ? 64u : 32u;
    if (!sf) v = (uint32_t)v;
    if (amount == 0u) return v;
    uint64_t r;
    switch (type) {
    case 0: r = v << amount; break;
    case 1: r = v >> amount; break;
    case 2: r = sf ? (uint64_t)((int64_t)v >> amount) : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> amount); break;
    default: r = (v >> amount) | (v << (bits - amount)); break;
    }
    return sf ? r : (uint32_t)r;
}

static inline uint64_t extend_reg(uint64_t v, unsigned option, unsigned shift)
{
    switch (option) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    default: break;
    }
    return v << shift;
}

static inline uint64_t ones(unsigned n)
{
    return n >= 64u ? ~0ULL : (1ULL << n) - 1ULL;
}

static inline bool decode_bitmask(unsigned n, unsigned imms, unsigned immr, bool sf, uint64_t *out)
{
    unsigned combined = (n << 6) | (~imms & 0x3Fu);
    if (combined == 0u) return false;
    unsigned len = 31u - (unsigned)__builtin_clz(combined);
    if (len < 1u || (!sf && len > 5u)) return false;
    unsigned esize = 1u << len, levels = esize - 1u;
    unsigned s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    uint64_t elem = ones(s + 1u);
    if (r != 0u) {
        elem = ((elem >> r) | (elem << (esize - r))) & ones(esize);
    }
    uint64_t m = 0;
    for (unsigned k = 0; k < 64u; k += esize) {
        m |= elem << k;
    }
    *out = sf ? m : (uint32_t)m;
    return true;
}


int a64_step(gthread_t *t, a64cpu_t *c);
int a64_run_interp(gthread_t *t);
