/*
 * AArch64 scalar floating point and Advanced SIMD.
 *
 * Arithmetic runs on the host FP unit in the guest's rounding mode
 * (a64_fpcr_apply); what IEEE leaves open is done the ARM way explicitly,
 * so the same results come out on an x86_64 host:
 *   - NaN results: an SNaN operand wins over a QNaN, the first over the
 *     second, and an invalid operation gives the positive default NaN
 *     (always the default NaN with FPCR.DN);
 *   - FMAX/FMIN propagate NaNs and order -0 below +0; FMAXNM/FMINNM prefer
 *     a number to a quiet NaN;
 *   - float -> integer conversion saturates, and NaN converts to 0.
 * Rounding to integral (FRINT*) and FMA are computed without libm.
 */

#include "a64_ops.h"
#include "host.h"

static inline uint32_t f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; __builtin_memcpy(&d, &u, 8); return d; }

#define FPCR_DN (1u << 25)
#define FPCR_RMODE(c) (((c)->fpcr >> 22) & 3u)

/* ---- NaN handling ---------------------------------------------------------------- */

static inline bool snan_d(double x) { uint64_t u = d2u(x); return (u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (u & 0x000FFFFFFFFFFFFFULL) != 0u && (u & 0x0008000000000000ULL) == 0u; }
static inline bool snan_f(float x) { uint32_t u = f2u(x); return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0u && (u & 0x00400000u) == 0u; }

static double nan_d(const a64cpu_t *c, double r, double a, double b)
{
    if (r == r) return r;
    if (c->fpcr & FPCR_DN) return u2d(0x7FF8000000000000ULL);
    if (snan_d(a)) return u2d(d2u(a) | 0x0008000000000000ULL);
    if (snan_d(b)) return u2d(d2u(b) | 0x0008000000000000ULL);
    if (a != a) return a;
    if (b != b) return b;
    return u2d(0x7FF8000000000000ULL);
}

static float nan_f(const a64cpu_t *c, float r, float a, float b)
{
    if (r == r) return r;
    if (c->fpcr & FPCR_DN) return u2f(0x7FC00000u);
    if (snan_f(a)) return u2f(f2u(a) | 0x00400000u);
    if (snan_f(b)) return u2f(f2u(b) | 0x00400000u);
    if (a != a) return a;
    if (b != b) return b;
    return u2f(0x7FC00000u);
}

/* ---- rounding without libm ----------------------------------------------------------- */

static double cpsign(double x, double s) { return u2d((d2u(x) & ~(1ULL << 63)) | (d2u(s) & (1ULL << 63))); }

static double r_trunc(double x)
{
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;   /* NaN, inf, integral */
    return cpsign((double)(int64_t)x, x);
}

/* mode: 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away */
static double r_round(double x, unsigned mode)
{
    double t = r_trunc(x);
    if (t == x || x != x) return x;
    double d = x - t;           /* exact, |d| < 1, same sign as x */
    double one = x < 0 ? -1.0 : 1.0;
    switch (mode) {
    case 1: return x > 0 ? t + 1.0 : cpsign(t, x);
    case 2: return x < 0 ? t - 1.0 : cpsign(t, x);
    case 3: return cpsign(t, x);
    case 4: return (d >= 0.5 || d <= -0.5) ? t + one : cpsign(t, x);
    default: {
        double ad = d < 0 ? -d : d;
        if (ad > 0.5) return t + one;
        if (ad < 0.5) return cpsign(t, x);
        return ((int64_t)t & 1) ? t + one : cpsign(t, x);
    }
    }
}

static unsigned host_mode(unsigned rmode)
{
    /* FPCR.RMode: 0 RN, 1 RP, 2 RM, 3 RZ -- same numbering as r_round */
    return rmode;
}

static double fma_d(double a, double b, double c)
{
#if defined(__aarch64__)
    return __builtin_fma(a, b, c);
#else
    return (double)((__float128)a * (__float128)b + (__float128)c);
#endif
}

static float fma_f(float a, float b, float c)
{
#if defined(__aarch64__)
    return __builtin_fmaf(a, b, c);
#else
    return (float)((double)a * (double)b + (double)c);
#endif
}

/* saturating float -> integer, `bits` wide, signed or not */
static uint64_t f2int(double v, unsigned mode, bool is_signed, unsigned bits)
{
    if (v != v) return 0;
    double r = r_round(v, mode);
    if (is_signed) {
        double lim = bits == 64 ? 9223372036854775808.0 : 2147483648.0;
        if (r >= lim) return bits == 64 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
        if (r < -lim) return bits == 64 ? (uint64_t)INT64_MIN : (uint64_t)(uint32_t)INT32_MIN;
        int64_t i = (int64_t)r;
        return bits == 64 ? (uint64_t)i : (uint32_t)(int32_t)i;
    }
    double lim = bits == 64 ? 18446744073709551616.0 : 4294967296.0;
    if (r >= lim) return bits == 64 ? UINT64_MAX : UINT32_MAX;
    if (r <= 0) return 0;
    return (uint64_t)r;
}

/* ---- half precision (FCVT only) ---------------------------------------------------- */

static float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0x1Fu) return u2f(s | 0x7F800000u | (m << 13));
    if (e == 0u) {
        if (m == 0u) return u2f(s);
        float v = (float)m * (1.0f / 16777216.0f);    /* m * 2^-24 */
        return s ? -v : v;
    }
    return u2f(s | ((e + 112u) << 23) | (m << 13));
}

static uint16_t f2h(float f)
{
    uint32_t u = f2u(f);
    uint16_t s = (uint16_t)((u >> 16) & 0x8000u);
    int e = (int)((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t m = u & 0x7FFFFFu;
    if (((u >> 23) & 0xFFu) == 0xFFu) return (uint16_t)(s | 0x7C00u | (m ? 0x200u | (m >> 13) : 0u));
    if (e >= 31) return (uint16_t)(s | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return s;
        m |= 0x800000u;
        unsigned sh = (unsigned)(14 - e);
        uint32_t hm = m >> sh, rem = m & ((1u << sh) - 1u), half = 1u << (sh - 1u);
        if (rem > half || (rem == half && (hm & 1u))) ++hm;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1FFFu;
    uint16_t r = (uint16_t)(s | ((unsigned)e << 10) | hm);
    if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) ++r;
    return r;
}

/* ---- FPCR/FPSR <-> host ------------------------------------------------------------- */

void a64_fpcr_apply(a64cpu_t *c)
{
#if defined(__aarch64__)
    uint64_t v = c->fpcr & ((3u << 22) | (1u << 24) | (1u << 25));
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
    uint64_t s = c->fpsr & 0x9Fu;
    __asm__ volatile("msr fpsr, %0" :: "r"(s));
#elif defined(__x86_64__)
    static const uint32_t rc[4] = { 0u, 2u, 1u, 3u };   /* RN, RP, RM, RZ -> x86 RC */
    uint32_t m = 0x1F80u | (rc[FPCR_RMODE(c)] << 13);
    if (c->fpcr & (1u << 24)) m |= 0x8040u;
    const uint32_t f = c->fpsr;
    if (f & 0x01u) m |= 0x01u;
    if (f & 0x02u) m |= 0x04u;
    if (f & 0x04u) m |= 0x08u;
    if (f & 0x08u) m |= 0x10u;
    if (f & 0x10u) m |= 0x20u;
    if (f & 0x80u) m |= 0x02u;
    __asm__ volatile("ldmxcsr %0" :: "m"(m));
#else
    (void)c;
#endif
}

void a64_fpsr_collect(a64cpu_t *c)
{
#if defined(__aarch64__)
    uint64_t s;
    __asm__ volatile("mrs %0, fpsr" : "=r"(s));
    c->fpsr = (c->fpsr & ~0x9Fu) | (uint32_t)(s & 0x9Fu);
#elif defined(__x86_64__)
    uint32_t m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    uint32_t f = 0;
    if (m & 0x01u) f |= 0x01u;
    if (m & 0x04u) f |= 0x02u;
    if (m & 0x08u) f |= 0x04u;
    if (m & 0x10u) f |= 0x08u;
    if (m & 0x20u) f |= 0x10u;
    if (m & 0x02u) f |= 0x80u;
    c->fpsr = (c->fpsr & ~0x9Fu) | f;
#else
    (void)c;
#endif
}

/* ---- scalar FP helpers -------------------------------------------------------------- */

/* Write a scalar: the rest of the vector register is zeroed. */
static inline void set_s(a64cpu_t *c, unsigned r, float v) { memset(&c->v[r], 0, 16); c->v[r].f[0] = v; }
static inline void set_d(a64cpu_t *c, unsigned r, double v) { memset(&c->v[r], 0, 16); c->v[r].fd[0] = v; }

static double fmax_rule(const a64cpu_t *c, double a, double b, bool max, bool nm)
{
    if (nm) {
        bool qa = a != a && !snan_d(a), qb = b != b && !snan_d(b);
        if (qa && !(b != b)) return b;
        if (qb && !(a != a)) return a;
    }
    if (a != a || b != b) return nan_d(c, a + b, a, b);
    if (a == 0 && b == 0) {
        bool na = __builtin_signbit(a) != 0, nb = __builtin_signbit(b) != 0;
        return max ? ((na && nb) ? a : (na ? b : a)) : ((na || nb) ? (na ? a : b) : a);
    }
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

static float fmax_rule_f(const a64cpu_t *c, float a, float b, bool max, bool nm)
{
    if (nm) {
        bool qa = a != a && !snan_f(a), qb = b != b && !snan_f(b);
        if (qa && !(b != b)) return b;
        if (qb && !(a != a)) return a;
    }
    if (a != a || b != b) return nan_f(c, a + b, a, b);
    if (a == 0 && b == 0) {
        bool na = __builtin_signbit(a) != 0, nb = __builtin_signbit(b) != 0;
        return max ? ((na && nb) ? a : (na ? b : a)) : ((na || nb) ? (na ? a : b) : a);
    }
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

/* binary op, opcode as in "FP data-processing (2 source)" */
double a64_fop2_d(const a64cpu_t *c, unsigned op, double a, double b)
{
    double r;
    switch (op) {
    case 0: r = a * b; break;
    case 1: r = a / b; break;
    case 2: r = a + b; break;
    case 3: r = a - b; break;
    case 4: return fmax_rule(c, a, b, true, false);
    case 5: return fmax_rule(c, a, b, false, false);
    case 6: return fmax_rule(c, a, b, true, true);
    case 7: return fmax_rule(c, a, b, false, true);
    default: r = -(a * b); break;           /* 8: FNMUL */
    }
    return nan_d(c, r, a, b);
}

float a64_fop2_f(const a64cpu_t *c, unsigned op, float a, float b)
{
    float r;
    switch (op) {
    case 0: r = a * b; break;
    case 1: r = a / b; break;
    case 2: r = a + b; break;
    case 3: r = a - b; break;
    case 4: return fmax_rule_f(c, a, b, true, false);
    case 5: return fmax_rule_f(c, a, b, false, false);
    case 6: return fmax_rule_f(c, a, b, true, true);
    case 7: return fmax_rule_f(c, a, b, false, true);
    default: r = -(a * b); break;
    }
    return nan_f(c, r, a, b);
}

static uint32_t fcmp_flags(double a, double b)
{
    if (a != a || b != b) return NZCV_C | NZCV_V;
    if (a == b) return NZCV_Z | NZCV_C;
    if (a < b) return NZCV_N;
    return NZCV_C;
}

static double frint(const a64cpu_t *c, double v, unsigned op)
{
    /* opcode 001xxx: N P M Z A - X I */
    switch (op & 7u) {
    case 0: return r_round(v, 0);
    case 1: return r_round(v, 1);
    case 2: return r_round(v, 2);
    case 3: return r_round(v, 3);
    case 4: return r_round(v, 4);
    default: return r_round(v, host_mode(FPCR_RMODE(c)));  /* X, I */
    }
}

/* ---- scalar floating point ------------------------------------------------------------ */

static int fp_scalar(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const unsigned type = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);
    const bool dbl = type == 1u;

    if ((w & 0x5F20FC00u) == 0x1E200000u) {         /* FP <-> integer */
        const bool sf = BIT(w, 31);
        const unsigned rmode = BITS(w, 20, 19), opcode = BITS(w, 18, 16);
        if (opcode == 6u || opcode == 7u) {          /* FMOV general <-> FP */
            if (rmode == 0u) {
                if (opcode == 7u) {                  /* to FP */
                    uint64_t v = rn == 31u ? 0u : c->x[rn];
                    memset(&c->v[rd], 0, 16);
                    if (sf && type == 1u) c->v[rd].d[0] = v;
                    else if (!sf && type == 0u) c->v[rd].s[0] = (uint32_t)v;
                    else if (type == 3u) c->v[rd].h[0] = (uint16_t)v;
                    else return A64_UD(t);
                } else {                             /* to general */
                    uint64_t v;
                    if (sf && type == 1u) v = c->v[rn].d[0];
                    else if (!sf && type == 0u) v = c->v[rn].s[0];
                    else if (type == 3u) v = c->v[rn].h[0];
                    else return A64_UD(t);
                    if (rd != 31u) c->x[rd] = v;
                }
                return 0;
            }
            if (rmode == 1u && sf && type == 2u) {   /* FMOV Vd.D[1] <-> Xn */
                if (opcode == 7u) c->v[rd].d[1] = rn == 31u ? 0u : c->x[rn];
                else if (rd != 31u) c->x[rd] = c->v[rn].d[1];
                return 0;
            }
            return A64_UD(t);
        }
        if (type > 1u) return A64_UD(t);
        if (opcode == 2u || opcode == 3u) {          /* SCVTF, UCVTF */
            if (rmode != 0u) return A64_UD(t);
            uint64_t v = rn == 31u ? 0u : c->x[rn];
            if (!sf) v = opcode == 2u ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
            if (dbl) set_d(c, rd, opcode == 2u ? (double)(int64_t)v : (double)v);
            else set_s(c, rd, opcode == 2u ? (float)(int64_t)v : (float)v);
            return 0;
        }
        /* FCVT{N,P,M,Z}{S,U}, FCVTA{S,U} */
        unsigned mode;
        if (opcode == 0u || opcode == 1u) mode = rmode == 0u ? 0u : rmode == 1u ? 1u : rmode == 2u ? 2u : 3u;
        else if (opcode == 4u || opcode == 5u) { if (rmode != 0u) return A64_UD(t); mode = 4u; }
        else return A64_UD(t);
        double v = dbl ? c->v[rn].fd[0] : (double)c->v[rn].f[0];
        uint64_t r = f2int(v, mode, (opcode & 1u) == 0u, sf ? 64u : 32u);
        if (rd != 31u) c->x[rd] = sf ? r : (uint32_t)r;
        return 0;
    }
    if ((w & 0x5F200000u) == 0x1E000000u) {         /* FP <-> fixed point */
        const bool sf = BIT(w, 31);
        const unsigned rmode = BITS(w, 20, 19), opcode = BITS(w, 18, 16), scale = 64u - BITS(w, 15, 10);
        if (type > 1u || (!sf && scale > 32u)) return A64_UD(t);
        double f = 1.0;
        for (unsigned k = 0; k < scale; ++k) f *= 2.0;
        if (rmode == 0u && (opcode == 2u || opcode == 3u)) {
            uint64_t v = rn == 31u ? 0u : c->x[rn];
            if (!sf) v = opcode == 2u ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
            double r = (opcode == 2u ? (double)(int64_t)v : (double)v) / f;
            if (dbl) set_d(c, rd, r); else set_s(c, rd, (float)r);
            return 0;
        }
        if (rmode == 3u && (opcode == 0u || opcode == 1u)) {
            double v = (dbl ? c->v[rn].fd[0] : (double)c->v[rn].f[0]) * f;
            uint64_t r = f2int(v, 3u, opcode == 0u, sf ? 64u : 32u);
            if (rd != 31u) c->x[rd] = sf ? r : (uint32_t)r;
            return 0;
        }
        return A64_UD(t);
    }
    if ((w & 0xFF207C00u) == 0x1E204000u) {         /* FP data-processing, 1 source */
        const unsigned opcode = BITS(w, 20, 15);
        if (opcode >= 4u && opcode <= 7u) {          /* FCVT between precisions */
            unsigned to = opcode & 3u;
            double v;
            if (type == 0u) v = c->v[rn].f[0];
            else if (type == 1u) v = c->v[rn].fd[0];
            else if (type == 3u) v = h2f(c->v[rn].h[0]);
            else return A64_UD(t);
            if (v != v && !(c->fpcr & FPCR_DN)) {
                /* keep the payload, quietened (the conversion does that) */
            }
            if (to == 0u) set_s(c, rd, (float)v);
            else if (to == 1u) set_d(c, rd, v);
            else if (to == 3u) { memset(&c->v[rd], 0, 16); c->v[rd].h[0] = f2h((float)v); }
            else return A64_UD(t);
            return 0;
        }
        if (type > 1u) return A64_UD(t);
        double a = dbl ? c->v[rn].fd[0] : (double)c->v[rn].f[0];
        double r;
        switch (opcode) {
        case 0: r = a; break;                               /* FMOV */
        case 1: r = u2d(d2u(a) & ~(1ULL << 63)); break;      /* FABS */
        case 2: r = u2d(d2u(a) ^ (1ULL << 63)); break;       /* FNEG */
        case 3:
            if (dbl) { double s = __builtin_sqrt(a); set_d(c, rd, nan_d(c, s, a, a)); }
            else { float af = c->v[rn].f[0], s = __builtin_sqrtf(af); set_s(c, rd, nan_f(c, s, af, af)); }
            return 0;
        case 8: case 9: case 10: case 11: case 12: case 14: case 15:
            if (a != a) { r = dbl ? nan_d(c, a, a, a) : (double)nan_f(c, (float)a, (float)a, (float)a); break; }
            r = frint(c, a, opcode);
            break;
        default:
            return A64_UD(t);
        }
        if (dbl) {
            if (opcode <= 2u) { memset(&c->v[rd], 0, 16); c->v[rd].d[0] = opcode == 0u ? c->v[rn].d[0] : d2u(r); return 0; }
            set_d(c, rd, r);
        } else {
            if (opcode <= 2u) {
                uint32_t u = c->v[rn].s[0];
                if (opcode == 1u) u &= 0x7FFFFFFFu;
                if (opcode == 2u) u ^= 0x80000000u;
                memset(&c->v[rd], 0, 16);
                c->v[rd].s[0] = u;
                return 0;
            }
            set_s(c, rd, (float)r);
        }
        return 0;
    }
    if ((w & 0xFF203C00u) == 0x1E202000u) {         /* FCMP, FCMPE */
        if (type > 1u) return A64_UD(t);
        double a = dbl ? c->v[rn].fd[0] : (double)c->v[rn].f[0];
        double b = BIT(w, 3) ? 0.0 : (dbl ? c->v[rm].fd[0] : (double)c->v[rm].f[0]);
        c->nzcv = fcmp_flags(a, b);
        return 0;
    }
    if ((w & 0xFF201C00u) == 0x1E201000u) {         /* FMOV (immediate) */
        if (BITS(w, 9, 5) != 0u || type > 1u) return A64_UD(t);
        unsigned imm8 = BITS(w, 20, 13);
        /* VFPExpandImm */
        unsigned sgn = imm8 >> 7, b6 = (imm8 >> 6) & 1u, low = imm8 & 0x3Fu;
        if (dbl) {
            uint64_t e = ((uint64_t)(b6 ? 0xFFu : 0x100u)) << 2;   /* NOT(b):Replicate(b,8) */
            uint64_t exp = (((uint64_t)(!b6)) << 10) | ((b6 ? 0xFFULL : 0ULL) << 2) | ((low >> 4) & 3u);
            (void)e;
            uint64_t u = ((uint64_t)sgn << 63) | (exp << 52) | ((uint64_t)(low & 15u) << 48);
            set_d(c, rd, u2d(u));
        } else {
            uint32_t exp = (((uint32_t)(!b6)) << 7) | ((b6 ? 0x1Fu : 0u) << 2) | ((low >> 4) & 3u);
            uint32_t u = ((uint32_t)sgn << 31) | (exp << 23) | ((low & 15u) << 19);
            set_s(c, rd, u2f(u));
        }
        return 0;
    }
    if ((w & 0xFF200C00u) == 0x1E200400u) {         /* FCCMP, FCCMPE */
        if (type > 1u) return A64_UD(t);
        if (a64_cond(c->nzcv, BITS(w, 15, 12))) {
            double a = dbl ? c->v[rn].fd[0] : (double)c->v[rn].f[0];
            double b = dbl ? c->v[rm].fd[0] : (double)c->v[rm].f[0];
            c->nzcv = fcmp_flags(a, b);
        } else {
            c->nzcv = (w & 15u) << 28;
        }
        return 0;
    }
    if ((w & 0xFF200C00u) == 0x1E200800u) {         /* FP data-processing, 2 sources */
        unsigned op = BITS(w, 15, 12);
        if (type > 1u || op > 8u) return A64_UD(t);
        if (dbl) set_d(c, rd, a64_fop2_d(c, op, c->v[rn].fd[0], c->v[rm].fd[0]));
        else set_s(c, rd, a64_fop2_f(c, op, c->v[rn].f[0], c->v[rm].f[0]));
        return 0;
    }
    if ((w & 0xFF200C00u) == 0x1E200C00u) {         /* FCSEL */
        if (type > 1u) return A64_UD(t);
        unsigned src = a64_cond(c->nzcv, BITS(w, 15, 12)) ? rn : rm;
        if (dbl) { uint64_t u = c->v[src].d[0]; memset(&c->v[rd], 0, 16); c->v[rd].d[0] = u; }
        else { uint32_t u = c->v[src].s[0]; memset(&c->v[rd], 0, 16); c->v[rd].s[0] = u; }
        return 0;
    }
    if ((w & 0xFF000000u) == 0x1F000000u) {         /* FMADD, FMSUB, FNMADD, FNMSUB */
        if (type > 1u) return A64_UD(t);
        const unsigned ra = BITS(w, 14, 10);
        const bool o1 = BIT(w, 21), o0 = BIT(w, 15);
        if (dbl) {
            /* FMADD a+nm, FMSUB a-nm, FNMADD -a-nm, FNMSUB -a+nm */
            double a = c->v[ra].fd[0], n = c->v[rn].fd[0], m = c->v[rm].fd[0];
            if (o0 != o1) n = -n;
            if (o1) a = -a;
            double r = fma_d(n, m, a);
            if (r != r) r = nan_d(c, r, (a != a) ? a : n, m);
            set_d(c, rd, r);
        } else {
            float a = c->v[ra].f[0], n = c->v[rn].f[0], m = c->v[rm].f[0];
            if (o0 != o1) n = -n;
            if (o1) a = -a;
            float r = fma_f(n, m, a);
            if (r != r) r = nan_f(c, r, (a != a) ? a : n, m);
            set_s(c, rd, r);
        }
        return 0;
    }
    return A64_UD(t);
}

/* ---- Advanced SIMD helpers ------------------------------------------------------------ */

/* element e (size esize bits) of v, zero-extended */
static inline uint64_t el(const vreg_t *v, unsigned esize, unsigned e)
{
    switch (esize) {
    case 8: return v->b[e];
    case 16: return v->h[e];
    case 32: return v->s[e];
    default: return v->d[e];
    }
}

static inline void set_el(vreg_t *v, unsigned esize, unsigned e, uint64_t x)
{
    switch (esize) {
    case 8: v->b[e] = (uint8_t)x; break;
    case 16: v->h[e] = (uint16_t)x; break;
    case 32: v->s[e] = (uint32_t)x; break;
    default: v->d[e] = x; break;
    }
}

static inline int64_t sel(const vreg_t *v, unsigned esize, unsigned e)
{
    return sext(el(v, esize, e), esize);
}

static inline uint64_t emask(unsigned esize)
{
    return esize == 64 ? ~0ULL : (1ULL << esize) - 1ULL;
}

static inline void finish(a64cpu_t *c, unsigned rd, vreg_t *r, bool q)
{
    if (!q) r->d[1] = 0;
    c->v[rd] = *r;
}

static uint64_t sat_s(int64_t v, unsigned esize, bool *sat)
{
    int64_t hi = (int64_t)(emask(esize) >> 1), lo = -hi - 1;
    if (esize == 64) return (uint64_t)v;
    if (v > hi) { *sat = true; return (uint64_t)hi & emask(esize); }
    if (v < lo) { *sat = true; return (uint64_t)lo & emask(esize); }
    return (uint64_t)v & emask(esize);
}

static uint64_t sat_u(int64_t v, unsigned esize, bool *sat)
{
    if (esize == 64) return (uint64_t)v;
    if (v > (int64_t)emask(esize)) { *sat = true; return emask(esize); }
    if (v < 0) { *sat = true; return 0; }
    return (uint64_t)v;
}

/* ---- vector: three same --------------------------------------------------------------- */

static int simd_three_same(gthread_t *t, a64cpu_t *c, uint32_t w, bool scalar)
{
    const bool q = BIT(w, 30) || scalar, u = BIT(w, 29);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);
    const unsigned opcode = BITS(w, 15, 11);
    const vreg_t a = c->v[rn], b = c->v[rm];
    vreg_t r;
    memset(&r, 0, sizeof(r));

    if (opcode >= 0x18u) {                          /* floating point */
        const bool dbl = (size & 1u) != 0u;
        const unsigned hi = size >> 1;                /* bit 23 */
        const unsigned lanes = scalar ? 1u : (dbl ? (q ? 2u : 0u) : (q ? 4u : 2u));
        if (dbl && !q && !scalar) return A64_UD(t);
        const unsigned key = (u << 6) | (hi << 5) | opcode;
        for (unsigned e = 0; e < lanes; ++e) {
            double x = dbl ? a.fd[e] : a.f[e], y = dbl ? b.fd[e] : b.f[e];
            double res = 0;
            bool is_mask = false, m = false;
            switch (key) {
            case 0x18: res = dbl ? a64_fop2_d(c, 6, x, y) : a64_fop2_f(c, 6, (float)x, (float)y); break;   /* FMAXNM */
            case 0x19: {                                                                         /* FMLA */
                double acc = dbl ? c->v[rd].fd[e] : c->v[rd].f[e];
                res = dbl ? fma_d(x, y, acc) : fma_f((float)x, (float)y, (float)acc);
                break;
            }
            case 0x1A: res = dbl ? a64_fop2_d(c, 2, x, y) : a64_fop2_f(c, 2, (float)x, (float)y); break;   /* FADD */
            case 0x1B: res = dbl ? x * y : (float)x * (float)y; break;                            /* FMULX */
            case 0x1C: is_mask = true; m = x == y; break;                                          /* FCMEQ */
            case 0x1E: res = dbl ? a64_fop2_d(c, 4, x, y) : a64_fop2_f(c, 4, (float)x, (float)y); break;   /* FMAX */
            case 0x1F: res = dbl ? 2.0 - x * y : 2.0f - (float)x * (float)y; break;               /* FRECPS */
            case 0x38: res = dbl ? a64_fop2_d(c, 7, x, y) : a64_fop2_f(c, 7, (float)x, (float)y); break;   /* FMINNM */
            case 0x39: {                                                                         /* FMLS */
                double acc = dbl ? c->v[rd].fd[e] : c->v[rd].f[e];
                res = dbl ? fma_d(-x, y, acc) : fma_f(-(float)x, (float)y, (float)acc);
                break;
            }
            case 0x3A: res = dbl ? a64_fop2_d(c, 3, x, y) : a64_fop2_f(c, 3, (float)x, (float)y); break;   /* FSUB */
            case 0x3E: res = dbl ? a64_fop2_d(c, 5, x, y) : a64_fop2_f(c, 5, (float)x, (float)y); break;   /* FMIN */
            case 0x3F: res = dbl ? (3.0 - x * y) / 2.0 : (3.0f - (float)x * (float)y) / 2.0f; break; /* FRSQRTS */
            case 0x7A: res = dbl ? a64_fop2_d(c, 3, x, y) : a64_fop2_f(c, 3, (float)x, (float)y);         /* FABD */
                       res = res < 0 ? -res : res; break;
            case 0x5B: res = dbl ? a64_fop2_d(c, 0, x, y) : a64_fop2_f(c, 0, (float)x, (float)y); break;   /* FMUL */
            case 0x5C: is_mask = true; m = x >= y; break;                                          /* FCMGE */
            case 0x5D: is_mask = true; m = (x < 0 ? -x : x) >= (y < 0 ? -y : y); break;            /* FACGE */
            case 0x5F: res = dbl ? a64_fop2_d(c, 1, x, y) : a64_fop2_f(c, 1, (float)x, (float)y); break;   /* FDIV */
            case 0x7C: is_mask = true; m = x > y; break;                                           /* FCMGT */
            case 0x7D: is_mask = true; m = (x < 0 ? -x : x) > (y < 0 ? -y : y); break;             /* FACGT */
            case 0x58: case 0x5A: case 0x5E: case 0x78: case 0x7E: {                              /* pairwise */
                unsigned half = lanes / 2u;
                const vreg_t *src = e < half ? &a : &b;
                unsigned k = (e % (half ? half : 1u)) * 2u;
                double p0 = dbl ? src->fd[k] : src->f[k], p1 = dbl ? src->fd[k + 1] : src->f[k + 1];
                unsigned fop = key == 0x58 ? 6u : key == 0x5A ? 2u : key == 0x5E ? 4u : key == 0x78 ? 7u : 5u;
                if (scalar) { p0 = dbl ? a.fd[0] : a.f[0]; p1 = dbl ? a.fd[1] : a.f[1]; }
                res = dbl ? a64_fop2_d(c, fop, p0, p1) : a64_fop2_f(c, fop, (float)p0, (float)p1);
                break;
            }
            default:
                return A64_UD(t);
            }
            if (dbl) {
                if (is_mask) r.d[e] = m ? ~0ULL : 0u; else r.fd[e] = res;
            } else {
                if (is_mask) r.s[e] = m ? 0xFFFFFFFFu : 0u; else r.f[e] = (float)res;
            }
        }
        if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); if (dbl) z.d[0] = r.d[0]; else z.s[0] = r.s[0]; c->v[rd] = z; return 0; }
        finish(c, rd, &r, q);
        return 0;
    }

    /* integer */
    const unsigned esize = 8u << size;
    const unsigned lanes = scalar ? 1u : (q ? 128u : 64u) / esize;
    if (!scalar && size == 3u && !q && opcode != 0x03u) return A64_UD(t);
    if (opcode == 0x03u) {                          /* AND BIC ORR ORN EOR BSL BIT BIF */
        for (unsigned k = 0; k < 2u; ++k) {
            uint64_t x = a.d[k], y = b.d[k], d = c->v[rd].d[k], res;
            switch ((u << 2) | size) {
            case 0: res = x & y; break;
            case 1: res = x & ~y; break;
            case 2: res = x | y; break;
            case 3: res = x | ~y; break;
            case 4: res = x ^ y; break;
            case 5: res = (d & x) | (~d & y); break;        /* BSL */
            case 6: res = (y & x) | (~y & d); break;        /* BIT */
            default: res = (~y & x) | (y & d); break;       /* BIF */
            }
            r.d[k] = res;
        }
        finish(c, rd, &r, q);
        return 0;
    }
    bool sat = false;
    for (unsigned e = 0; e < lanes; ++e) {
        uint64_t x = el(&a, esize, e), y = el(&b, esize, e), res = 0;
        int64_t sx_ = sel(&a, esize, e), sy = sel(&b, esize, e);
        const uint64_t m = emask(esize);
        switch ((u << 5) | opcode) {
        case 0x01: res = sat_s(sx_ + sy, esize, &sat); break;                   /* SQADD */
        case 0x21: res = sat_u((int64_t)(x + y), esize, &sat);                  /* UQADD */
                   if (esize == 64 && x + y < x) { res = ~0ULL; sat = true; } break;
        case 0x05: res = sat_s(sx_ - sy, esize, &sat); break;                   /* SQSUB */
        case 0x25: res = x < y ? (sat = true, 0u) : x - y; break;               /* UQSUB */
        case 0x00: res = (uint64_t)((sx_ + sy) >> 1); break;                    /* SHADD */
        case 0x20: res = (x + y) >> 1; break;                                   /* UHADD */
        case 0x02: res = (uint64_t)((sx_ + sy + 1) >> 1); break;                /* SRHADD */
        case 0x22: res = (x + y + 1u) >> 1; break;                              /* URHADD */
        case 0x06: res = sx_ > sy ? m : 0u; break;                              /* CMGT */
        case 0x26: res = x > y ? m : 0u; break;                                 /* CMHI */
        case 0x07: res = sx_ >= sy ? m : 0u; break;                             /* CMGE */
        case 0x27: res = x >= y ? m : 0u; break;                                /* CMHS */
        case 0x08: case 0x28: {                                                 /* SSHL, USHL */
            int8_t s = (int8_t)y;
            if (s >= 0) res = s >= (int)esize ? 0u : x << s;
            else if (u) res = -s >= (int)esize ? 0u : x >> -s;
            else res = (uint64_t)(sx_ >> (-s >= (int)esize ? (int)esize - 1 : -s));
            break;
        }
        case 0x0C: res = sx_ > sy ? x : y; break;                               /* SMAX */
        case 0x2C: res = x > y ? x : y; break;                                  /* UMAX */
        case 0x0D: res = sx_ < sy ? x : y; break;                               /* SMIN */
        case 0x2D: res = x < y ? x : y; break;                                  /* UMIN */
        case 0x0E: res = (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_); break;     /* SABD */
        case 0x2E: res = x > y ? x - y : y - x; break;                          /* UABD */
        case 0x0F: res = el(&c->v[rd], esize, e) + (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_); break; /* SABA */
        case 0x2F: res = el(&c->v[rd], esize, e) + (x > y ? x - y : y - x); break; /* UABA */
        case 0x10: res = x + y; break;                                          /* ADD */
        case 0x30: res = x - y; break;                                          /* SUB */
        case 0x11: res = (x & y) != 0u ? m : 0u; break;                         /* CMTST */
        case 0x31: res = x == y ? m : 0u; break;                                /* CMEQ */
        case 0x12: res = el(&c->v[rd], esize, e) + x * y; break;                /* MLA */
        case 0x32: res = el(&c->v[rd], esize, e) - x * y; break;                /* MLS */
        case 0x13: res = x * y; break;                                          /* MUL */
        case 0x33: {                                                            /* PMUL */
            uint64_t p = 0;
            for (unsigned k = 0; k < esize; ++k) if ((y >> k) & 1u) p ^= x << k;
            res = p;
            break;
        }
        case 0x14: case 0x34: case 0x15: case 0x35: case 0x17: {               /* pairwise */
            unsigned half = lanes / 2u;
            const vreg_t *src = e < half ? &a : &b;
            unsigned k = (e % half) * 2u;
            uint64_t p0 = el(src, esize, k), p1 = el(src, esize, k + 1u);
            int64_t s0 = sel(src, esize, k), s1 = sel(src, esize, k + 1u);
            switch ((u << 5) | opcode) {
            case 0x14: res = s0 > s1 ? p0 : p1; break;       /* SMAXP */
            case 0x34: res = p0 > p1 ? p0 : p1; break;       /* UMAXP */
            case 0x15: res = s0 < s1 ? p0 : p1; break;       /* SMINP */
            case 0x35: res = p0 < p1 ? p0 : p1; break;       /* UMINP */
            default: res = p0 + p1; break;                   /* ADDP */
            }
            break;
        }
        case 0x16: case 0x36: {                                                 /* SQDMULH, SQRDMULH */
            if (esize != 16 && esize != 32) return A64_UD(t);
            int64_t p = 2 * sx_ * sy + (u ? (1LL << (esize - 1)) : 0);
            res = sat_s(p >> esize, esize, &sat);
            break;
        }
        default:
            return A64_UD(t);
        }
        set_el(&r, esize, e, res & m);
    }
    if (sat) c->fpsr |= 1u << 27;   /* QC */
    if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); set_el(&z, esize, 0, el(&r, esize, 0)); c->v[rd] = z; return 0; }
    finish(c, rd, &r, q);
    return 0;
}

/* ---- vector: two-register misc ------------------------------------------------------------ */

static int simd_two_misc(gthread_t *t, a64cpu_t *c, uint32_t w, bool scalar)
{
    const bool q = BIT(w, 30) || scalar, u = BIT(w, 29);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5);
    const unsigned opcode = BITS(w, 16, 12);
    const vreg_t a = c->v[rn];
    vreg_t r;
    memset(&r, 0, sizeof(r));
    const unsigned esize = 8u << size;
    const unsigned lanes = scalar ? 1u : (q ? 128u : 64u) / esize;
    const uint64_t m = emask(esize);
    const unsigned key = (u << 5) | opcode;

    switch (key) {
    case 0x00: case 0x20: case 0x01: {               /* REV64, REV32, REV16 */
        unsigned cont = key == 0x00 ? 64u : key == 0x20 ? 32u : 16u;
        if (esize >= cont) return A64_UD(t);
        unsigned per = cont / esize;
        for (unsigned e = 0; e < lanes; ++e) {
            unsigned base = (e / per) * per, idx = base + (per - 1u - (e - base));
            set_el(&r, esize, e, el(&a, esize, idx));
        }
        break;
    }
    case 0x02: case 0x22: case 0x06: case 0x26: {   /* SADDLP, UADDLP, SADALP, UADALP */
        if (size == 3u) return A64_UD(t);
        unsigned out = (q ? 128u : 64u) / (esize * 2u);
        for (unsigned e = 0; e < out; ++e) {
            uint64_t s = u ? el(&a, esize, 2 * e) + el(&a, esize, 2 * e + 1)
                           : (uint64_t)(sel(&a, esize, 2 * e) + sel(&a, esize, 2 * e + 1));
            if (opcode == 0x06u) s += el(&c->v[rd], esize * 2u, e);
            set_el(&r, esize * 2u, e, s & emask(esize * 2u));
        }
        break;
    }
    case 0x04: case 0x24:                            /* CLS, CLZ */
        for (unsigned e = 0; e < lanes; ++e) {
            uint64_t x = el(&a, esize, e);
            unsigned n = 0;
            if (key == 0x24) { while (n < esize && ((x >> (esize - 1u - n)) & 1u) == 0u) ++n; }
            else { unsigned sb = (unsigned)((x >> (esize - 1u)) & 1u); while (n + 1u < esize && ((x >> (esize - 2u - n)) & 1u) == sb) ++n; }
            set_el(&r, esize, e, n);
        }
        break;
    case 0x05:                                       /* CNT */
        if (size != 0u) return A64_UD(t);
        for (unsigned e = 0; e < lanes; ++e) r.b[e] = (uint8_t)__builtin_popcount(a.b[e]);
        break;
    case 0x25:                                       /* NOT (size 0), RBIT (size 1) */
        if (size == 0u) { r.d[0] = ~a.d[0]; r.d[1] = ~a.d[1]; }
        else if (size == 1u) {
            for (unsigned e = 0; e < (q ? 16u : 8u); ++e) {
                uint8_t x = a.b[e], y = 0;
                for (unsigned k = 0; k < 8u; ++k) y = (uint8_t)(y | (((x >> k) & 1u) << (7u - k)));
                r.b[e] = y;
            }
        } else return A64_UD(t);
        break;
    case 0x07: case 0x27: {                          /* SQABS, SQNEG */
        bool sat = false;
        for (unsigned e = 0; e < lanes; ++e) {
            int64_t x = sel(&a, esize, e);
            set_el(&r, esize, e, sat_s(key == 0x07 ? (x < 0 ? -x : x) : -x, esize, &sat));
        }
        if (sat) c->fpsr |= 1u << 27;
        break;
    }
    case 0x08: case 0x28: case 0x09: case 0x29: case 0x0A: case 0x0B: case 0x2B: {
        /* CMGT #0, CMGE #0, CMEQ #0, CMLE #0, CMLT #0, ABS, NEG */
        for (unsigned e = 0; e < lanes; ++e) {
            int64_t x = sel(&a, esize, e);
            uint64_t res;
            switch (key) {
            case 0x08: res = x > 0 ? m : 0u; break;
            case 0x28: res = x >= 0 ? m : 0u; break;
            case 0x09: res = x == 0 ? m : 0u; break;
            case 0x29: res = x <= 0 ? m : 0u; break;
            case 0x0A: res = x < 0 ? m : 0u; break;
            case 0x0B: res = (uint64_t)(x < 0 ? -x : x); break;
            default: res = (uint64_t)-x; break;
            }
            set_el(&r, esize, e, res & m);
        }
        break;
    }
    case 0x12: case 0x32: case 0x14: case 0x34: {   /* XTN, SQXTUN, SQXTN, UQXTN */
        if (size == 3u) return A64_UD(t);
        const unsigned de = esize;                    /* narrow element */
        const unsigned n = 64u / de;
        bool sat = false;
        vreg_t out = c->v[rd];
        if (!q) memset(&out, 0, sizeof(out));
        for (unsigned e = 0; e < n; ++e) {
            uint64_t x = el(&a, de * 2u, e);
            int64_t sx_ = sel(&a, de * 2u, e);
            uint64_t res;
            switch (key) {
            case 0x12: res = x & emask(de); break;
            case 0x32: res = sat_u(sx_, de, &sat); break;
            case 0x14: res = sat_s(sx_, de, &sat); break;
            default: res = x > emask(de) ? (sat = true, emask(de)) : x; break;
            }
            set_el(&out, de, (q ? n : 0u) + e, res);
        }
        if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); set_el(&z, de, 0, el(&out, de, 0)); out = z; }
        if (sat) c->fpsr |= 1u << 27;
        c->v[rd] = out;
        return 0;
    }
    case 0x13:                                       /* SHLL */
    case 0x33: {
        if (key == 0x13 || size == 3u) return A64_UD(t);
        unsigned n = 64u / esize;
        for (unsigned e = 0; e < n; ++e) set_el(&r, esize * 2u, e, el(&a, esize, (q ? n : 0u) + e) << esize);
        c->v[rd] = r;
        return 0;
    }
    default: {
        /* floating point two-reg misc */
        const bool dbl = (size & 1u) != 0u;
        const unsigned hi = size >> 1;
        const unsigned fl = scalar ? 1u : (dbl ? (q ? 2u : 0u) : (q ? 4u : 2u));
        if (dbl && !q && !scalar) return A64_UD(t);
        const unsigned fk = (u << 6) | (hi << 5) | opcode;
        if (fk == 0x16 || fk == 0x56) {              /* FCVTN, FCVTXN: double -> single, narrow */
            vreg_t out = c->v[rd];
            if (!q) memset(&out, 0, sizeof(out));
            if (size == 1u) {
                for (unsigned e = 0; e < 2u; ++e) out.f[(q ? 2u : 0u) + e] = (float)a.fd[e];
            } else {
                for (unsigned e = 0; e < 4u; ++e) out.h[(q ? 4u : 0u) + e] = f2h(a.f[e]);
            }
            c->v[rd] = out;
            return 0;
        }
        if (fk == 0x17) {                            /* FCVTL: single -> double, widen */
            if (size == 1u) {
                for (unsigned e = 0; e < 2u; ++e) r.fd[e] = (double)a.f[(q ? 2u : 0u) + e];
            } else {
                for (unsigned e = 0; e < 4u; ++e) r.f[e] = h2f(a.h[(q ? 4u : 0u) + e]);
            }
            c->v[rd] = r;
            return 0;
        }
        for (unsigned e = 0; e < fl; ++e) {
            double x = dbl ? a.fd[e] : a.f[e];
            double res = 0;
            bool is_int = false, is_mask = false, mk = false;
            uint64_t iv = 0;
            switch (fk) {
            case 0x18: case 0x19: case 0x38: case 0x39: case 0x58: case 0x59: case 0x79: {   /* FRINT N M P Z A X I */
                unsigned mode = fk == 0x18 ? 0u : fk == 0x19 ? 2u : fk == 0x38 ? 1u : fk == 0x39 ? 3u :
                                fk == 0x58 ? 4u : FPCR_RMODE(c);
                res = x != x ? x : r_round(x, mode);
                break;
            }
            case 0x1A: case 0x1B: case 0x3A: case 0x3B: case 0x1C:
            case 0x5A: case 0x5B: case 0x7A: case 0x7B: case 0x5C: {  /* FCVT{N,M,P,Z,A}{S,U} */
                unsigned mode = (fk & 0x3Fu) == 0x1A ? 0u : (fk & 0x3Fu) == 0x1B ? 2u : (fk & 0x3Fu) == 0x3A ? 1u :
                                (fk & 0x3Fu) == 0x3B ? 3u : 4u;
                is_int = true;
                iv = f2int(x, mode, (fk & 0x40u) == 0u, dbl ? 64u : 32u);
                break;
            }
            case 0x1D: case 0x5D: {                                   /* SCVTF, UCVTF */
                uint64_t v = dbl ? a.d[e] : a.s[e];
                if (fk == 0x1D) res = dbl ? (double)(int64_t)v : (double)(float)(int32_t)(uint32_t)v;
                else res = dbl ? (double)v : (double)(float)(uint32_t)v;
                break;
            }
            case 0x2C: is_mask = true; mk = x > 0; break;                     /* FCMGT #0 */
            case 0x2D: is_mask = true; mk = x == 0; break;                    /* FCMEQ #0 */
            case 0x2E: is_mask = true; mk = x < 0; break;                     /* FCMLT #0 */
            case 0x6C: is_mask = true; mk = x >= 0; break;                    /* FCMGE #0 */
            case 0x6D: is_mask = true; mk = x <= 0; break;                    /* FCMLE #0 */
            case 0x2F: res = u2d(d2u(x) & ~(1ULL << 63)); if (!dbl) { r.s[e] = a.s[e] & 0x7FFFFFFFu; continue; } break; /* FABS */
            case 0x6F: res = u2d(d2u(x) ^ (1ULL << 63)); if (!dbl) { r.s[e] = a.s[e] ^ 0x80000000u; continue; } break;  /* FNEG */
            case 0x7F: res = dbl ? nan_d(c, __builtin_sqrt(x), x, x) : nan_f(c, __builtin_sqrtf((float)x), (float)x, (float)x); break; /* FSQRT */
            case 0x3D: res = dbl ? 1.0 / x : 1.0f / (float)x; break;          /* FRECPE */
            case 0x7D: res = dbl ? 1.0 / __builtin_sqrt(x) : 1.0f / __builtin_sqrtf((float)x); break; /* FRSQRTE */
            default:
                return A64_UD(t);
            }
            if (is_int) { if (dbl) r.d[e] = iv; else r.s[e] = (uint32_t)iv; }
            else if (is_mask) { if (dbl) r.d[e] = mk ? ~0ULL : 0u; else r.s[e] = mk ? 0xFFFFFFFFu : 0u; }
            else if (dbl) r.fd[e] = res;
            else r.f[e] = (float)res;
        }
        if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); if (dbl) z.d[0] = r.d[0]; else z.s[0] = r.s[0]; c->v[rd] = z; return 0; }
        finish(c, rd, &r, q);
        return 0;
    }
    }
    if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); set_el(&z, esize, 0, el(&r, esize, 0)); c->v[rd] = z; return 0; }
    finish(c, rd, &r, q);
    return 0;
}

/* ---- across lanes ----------------------------------------------------------------------- */

static int simd_across(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30), u = BIT(w, 29);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5);
    const unsigned opcode = BITS(w, 16, 12);
    const vreg_t a = c->v[rn];
    vreg_t r;
    memset(&r, 0, sizeof(r));
    if (opcode == 0x0Cu || opcode == 0x0Fu) {        /* FMAXNMV, FMAXV, FMINNMV, FMINV (single) */
        if (!u || !q || (size & 1u)) return A64_UD(t);
        const bool min = (size & 2u) != 0u;
        unsigned fop = opcode == 0x0Cu ? (min ? 7u : 6u) : (min ? 5u : 4u);
        float v01 = a64_fop2_f(c, fop, a.f[0], a.f[1]), v23 = a64_fop2_f(c, fop, a.f[2], a.f[3]);
        r.f[0] = a64_fop2_f(c, fop, v01, v23);
        c->v[rd] = r;
        return 0;
    }
    const unsigned esize = 8u << size;
    const unsigned lanes = (q ? 128u : 64u) / esize;
    if (size == 3u || (size == 2u && !q)) return A64_UD(t);
    uint64_t acc = el(&a, esize, 0);
    int64_t sacc = sel(&a, esize, 0);
    switch ((u << 5) | opcode) {
    case 0x03: case 0x23: {                          /* SADDLV, UADDLV */
        int64_t s = 0;
        uint64_t us = 0;
        for (unsigned e = 0; e < lanes; ++e) { s += sel(&a, esize, e); us += el(&a, esize, e); }
        set_el(&r, esize * 2u, 0, (u ? us : (uint64_t)s) & emask(esize * 2u));
        c->v[rd] = r;
        return 0;
    }
    case 0x1B:                                       /* ADDV */
        for (unsigned e = 1; e < lanes; ++e) acc += el(&a, esize, e);
        break;
    case 0x0A:                                       /* SMAXV */
        for (unsigned e = 1; e < lanes; ++e) if (sel(&a, esize, e) > sacc) sacc = sel(&a, esize, e);
        acc = (uint64_t)sacc;
        break;
    case 0x2A:                                       /* UMAXV */
        for (unsigned e = 1; e < lanes; ++e) if (el(&a, esize, e) > acc) acc = el(&a, esize, e);
        break;
    case 0x1A:                                       /* SMINV */
        for (unsigned e = 1; e < lanes; ++e) if (sel(&a, esize, e) < sacc) sacc = sel(&a, esize, e);
        acc = (uint64_t)sacc;
        break;
    case 0x3A:                                       /* UMINV */
        for (unsigned e = 1; e < lanes; ++e) if (el(&a, esize, e) < acc) acc = el(&a, esize, e);
        break;
    default:
        return A64_UD(t);
    }
    set_el(&r, esize, 0, acc & emask(esize));
    c->v[rd] = r;
    return 0;
}

/* ---- copy: DUP, INS, UMOV, SMOV ------------------------------------------------------------- */

static int simd_copy(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30), op = BIT(w, 29);
    const unsigned imm5 = BITS(w, 20, 16), imm4 = BITS(w, 14, 11), rd = w & 31u, rn = BITS(w, 9, 5);
    if ((imm5 & 15u) == 0u) return A64_UD(t);
    const unsigned sz = (unsigned)__builtin_ctz(imm5);    /* 0 B, 1 H, 2 S, 3 D */
    const unsigned esize = 8u << sz;
    const unsigned idx = imm5 >> (sz + 1u);
    vreg_t r;
    memset(&r, 0, sizeof(r));
    if (op) {                                        /* INS (element) */
        r = c->v[rd];
        set_el(&r, esize, idx, el(&c->v[rn], esize, imm4 >> sz));
        c->v[rd] = r;
        return 0;
    }
    switch (imm4) {
    case 0: {                                        /* DUP (element) */
        uint64_t v = el(&c->v[rn], esize, idx);
        for (unsigned e = 0; e < (q ? 128u : 64u) / esize; ++e) set_el(&r, esize, e, v);
        c->v[rd] = r;
        return 0;
    }
    case 1: {                                        /* DUP (general) */
        uint64_t v = rn == 31u ? 0u : c->x[rn];
        for (unsigned e = 0; e < (q ? 128u : 64u) / esize; ++e) set_el(&r, esize, e, v & emask(esize));
        c->v[rd] = r;
        return 0;
    }
    case 3:                                          /* INS (general) */
        r = c->v[rd];
        set_el(&r, esize, idx, (rn == 31u ? 0u : c->x[rn]) & emask(esize));
        c->v[rd] = r;
        return 0;
    case 5: {                                        /* SMOV */
        int64_t v = sel(&c->v[rn], esize, idx);
        if (rd != 31u) c->x[rd] = q ? (uint64_t)v : (uint32_t)v;
        return 0;
    }
    case 7:                                          /* UMOV */
        if (rd != 31u) c->x[rd] = el(&c->v[rn], esize, idx);
        return 0;
    default:
        return A64_UD(t);
    }
}

/* ---- permutes, EXT, TBL ---------------------------------------------------------------------- */

static int simd_permute(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);
    const unsigned opcode = BITS(w, 14, 12);
    if (size == 3u && !q) return A64_UD(t);
    const unsigned esize = 8u << size, lanes = (q ? 128u : 64u) / esize, half = lanes / 2u;
    const vreg_t a = c->v[rn], b = c->v[rm];
    vreg_t r;
    memset(&r, 0, sizeof(r));
    for (unsigned e = 0; e < lanes; ++e) {
        uint64_t v;
        switch (opcode) {
        case 1: case 5: {                            /* UZP1, UZP2 */
            unsigned k = 2u * e + (opcode == 5u ? 1u : 0u);
            v = k < lanes ? el(&a, esize, k) : el(&b, esize, k - lanes);
            break;
        }
        case 2: case 6: {                            /* TRN1, TRN2 */
            unsigned p = (e & ~1u) + (opcode == 6u ? 1u : 0u);
            v = (e & 1u) ? el(&b, esize, p) : el(&a, esize, p);
            break;
        }
        case 3: case 7: {                            /* ZIP1, ZIP2 */
            unsigned base = opcode == 7u ? half : 0u;
            v = (e & 1u) ? el(&b, esize, base + e / 2u) : el(&a, esize, base + e / 2u);
            break;
        }
        default:
            return A64_UD(t);
        }
        set_el(&r, esize, e, v);
    }
    c->v[rd] = r;
    return 0;
}

static int simd_ext(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30);
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16), pos = BITS(w, 14, 11);
    const unsigned n = q ? 16u : 8u;
    if (!q && pos >= 8u) return A64_UD(t);
    uint8_t cat[32];
    memcpy(cat, c->v[rn].b, n);
    memcpy(cat + n, c->v[rm].b, n);
    vreg_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.b, cat + pos, n);
    c->v[rd] = r;
    return 0;
}

static int simd_tbl(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30), tbx = BIT(w, 12);
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16), len = BITS(w, 14, 13) + 1u;
    (void)t;
    uint8_t table[64];
    for (unsigned k = 0; k < len; ++k) memcpy(table + 16u * k, c->v[(rn + k) & 31u].b, 16);
    const vreg_t idx = c->v[rm];
    vreg_t r = tbx ? c->v[rd] : (vreg_t){ .d = { 0, 0 } };
    for (unsigned e = 0; e < (q ? 16u : 8u); ++e) {
        unsigned i = idx.b[e];
        if (i < 16u * len) r.b[e] = table[i];
        else if (!tbx) r.b[e] = 0;
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
    return 0;
}

/* ---- modified immediate (MOVI, MVNI, ORR, BIC, FMOV vector) -------------------------------- */

static int simd_modimm(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30), op = BIT(w, 29);
    const unsigned rd = w & 31u, cmode = BITS(w, 15, 12);
    const uint64_t imm8 = (BITS(w, 18, 16) << 5) | BITS(w, 9, 5);
    uint64_t imm;
    switch (cmode >> 1) {
    case 0: case 1: case 2: case 3: {                /* 32-bit shifted */
        uint64_t v = imm8 << (8u * (cmode >> 1));
        imm = v | (v << 32);
        break;
    }
    case 4: case 5: {                                /* 16-bit shifted */
        uint64_t v = imm8 << (8u * ((cmode >> 1) & 1u));
        imm = v | (v << 16) | (v << 32) | (v << 48);
        break;
    }
    case 6: {                                        /* 32-bit shifting ones */
        uint64_t v = (cmode & 1u) ? (imm8 << 16) | 0xFFFFu : (imm8 << 8) | 0xFFu;
        imm = v | (v << 32);
        break;
    }
    default:
        if (cmode == 14u && !op) {                   /* 8-bit */
            imm = imm8 * 0x0101010101010101ULL;
        } else if (cmode == 14u && op) {             /* 64-bit byte mask */
            imm = 0;
            for (unsigned k = 0; k < 8u; ++k) if ((imm8 >> k) & 1u) imm |= 0xFFULL << (8u * k);
        } else if (cmode == 15u && !op) {            /* FMOV single */
            uint32_t b6 = (imm8 >> 6) & 1u;
            uint32_t u = ((uint32_t)(imm8 >> 7) << 31) | ((uint32_t)!b6 << 30) |
                         ((b6 ? 0x1Fu : 0u) << 25) | ((uint32_t)(imm8 & 0x3Fu) << 19);
            imm = (uint64_t)u | ((uint64_t)u << 32);
        } else if (cmode == 15u && op && q) {        /* FMOV double */
            uint64_t b6 = (imm8 >> 6) & 1u;
            imm = ((imm8 >> 7) << 63) | ((uint64_t)!b6 << 62) | ((b6 ? 0xFFULL : 0u) << 54) |
                  ((imm8 & 0x3Fu) << 48);
        } else {
            return A64_UD(t);
        }
        break;
    }
    vreg_t r = c->v[rd];
    const bool is_orr_bic = (cmode < 12u) && (cmode & 1u);
    for (unsigned k = 0; k < 2u; ++k) {
        if (is_orr_bic) {
            r.d[k] = op ? r.d[k] & ~imm : r.d[k] | imm;  /* BIC / ORR */
        } else if (cmode < 14u && op) {
            r.d[k] = ~imm;                               /* MVNI */
        } else {
            r.d[k] = imm;                                /* MOVI, FMOV */
        }
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
    return 0;
}

/* ---- shift by immediate -------------------------------------------------------------------- */

static int simd_shift_imm(gthread_t *t, a64cpu_t *c, uint32_t w, bool scalar)
{
    const bool q = BIT(w, 30) || scalar, u = BIT(w, 29);
    const unsigned immh = BITS(w, 22, 19), immb = BITS(w, 18, 16);
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5), opcode = BITS(w, 15, 11);
    if (immh == 0u) return A64_UD(t);
    const unsigned hsb = 31u - (unsigned)__builtin_clz(immh);     /* 0..3 */
    const unsigned esize = 8u << hsb;
    const unsigned immhb = (immh << 3) | immb;
    const unsigned rshift = 2u * esize - immhb, lshift = immhb - esize;
    const vreg_t a = c->v[rn];
    vreg_t r;
    memset(&r, 0, sizeof(r));
    const unsigned lanes = scalar ? 1u : (q ? 128u : 64u) / esize;
    const uint64_t m = emask(esize);

    if (opcode == 0x14u) {                          /* SSHLL, USHLL (UXTL, SXTL) */
        if (hsb == 3u) return A64_UD(t);
        unsigned n = 64u / esize;
        for (unsigned e = 0; e < n; ++e) {
            uint64_t x = u ? el(&a, esize, (q ? n : 0u) + e) : (uint64_t)sel(&a, esize, (q ? n : 0u) + e);
            set_el(&r, esize * 2u, e, (x << lshift) & emask(esize * 2u));
        }
        c->v[rd] = r;
        return 0;
    }
    if (opcode == 0x10u || opcode == 0x11u || opcode == 0x12u || opcode == 0x13u) {
        /* SHRN, RSHRN (u=0); SQSHRUN, SQRSHRUN (u=1, 0x10/0x11); SQSHRN, UQSHRN (0x12) ... */
        /* immh gives the narrow (result) element size here */
        if (hsb == 3u) return A64_UD(t);
        const unsigned ne = esize;                    /* narrow element */
        const unsigned wide = 2u * ne;                /* source element */
        const unsigned shift = 2u * ne - immhb;
        vreg_t out = c->v[rd];
        if (!q || scalar) memset(&out, 0, sizeof(out));
        bool sat = false;
        unsigned n = scalar ? 1u : 64u / ne;
        for (unsigned e = 0; e < n; ++e) {
            uint64_t x = el(&a, wide, e);
            int64_t sx_ = sel(&a, wide, e);
            const bool round = (opcode & 1u) != 0u;
            uint64_t res;
            if (opcode <= 0x11u && !u) {            /* SHRN, RSHRN */
                res = ((x + (round ? (1ULL << (shift - 1u)) : 0u)) >> shift) & emask(ne);
            } else if (opcode <= 0x11u) {           /* SQSHRUN, SQRSHRUN */
                int64_t v = (sx_ + (round ? (1LL << (shift - 1u)) : 0)) >> shift;
                res = sat_u(v, ne, &sat);
            } else if (!u) {                        /* SQSHRN, SQRSHRN */
                int64_t v = (sx_ + (round ? (1LL << (shift - 1u)) : 0)) >> shift;
                res = sat_s(v, ne, &sat);
            } else {                                /* UQSHRN, UQRSHRN */
                uint64_t v = (x + (round ? (1ULL << (shift - 1u)) : 0u)) >> shift;
                res = v > emask(ne) ? (sat = true, emask(ne)) : v;
            }
            set_el(&out, ne, (q && !scalar ? n : 0u) + e, res);
        }
        if (sat) c->fpsr |= 1u << 27;
        c->v[rd] = out;
        return 0;
    }
    for (unsigned e = 0; e < lanes; ++e) {
        uint64_t x = el(&a, esize, e), d = el(&c->v[rd], esize, e), res;
        int64_t sx_ = sel(&a, esize, e);
        switch ((u << 5) | opcode) {
        case 0x00: res = (uint64_t)(sx_ >> (rshift >= esize ? esize - 1u : rshift)); break;      /* SSHR */
        case 0x20: res = rshift >= esize ? 0u : x >> rshift; break;                              /* USHR */
        case 0x02: res = d + (uint64_t)(sx_ >> (rshift >= esize ? esize - 1u : rshift)); break;  /* SSRA */
        case 0x22: res = d + (rshift >= esize ? 0u : x >> rshift); break;                        /* USRA */
        case 0x04: {                                                                             /* SRSHR */
            __int128 v = ((__int128)sx_ + ((__int128)1 << (rshift - 1u))) >> rshift;
            res = (uint64_t)v;
            break;
        }
        case 0x24: {                                                                             /* URSHR */
            unsigned __int128 v = ((unsigned __int128)x + ((unsigned __int128)1 << (rshift - 1u))) >> rshift;
            res = (uint64_t)v;
            break;
        }
        case 0x0A: res = x << lshift; break;                                                     /* SHL */
        case 0x28: {                                                                             /* SRI */
            uint64_t mask = rshift >= esize ? 0u : m >> rshift;
            res = (d & ~mask) | (rshift >= esize ? 0u : x >> rshift);
            break;
        }
        case 0x2A: {                                                                             /* SLI */
            uint64_t mask = (m << lshift) & m;
            res = (d & ~mask) | ((x << lshift) & mask);
            break;
        }
        case 0x1C: case 0x3C: {                                                                  /* SCVTF, UCVTF (fixed) */
            if (esize != 32 && esize != 64) return A64_UD(t);
            double f = 1.0;
            for (unsigned k = 0; k < rshift; ++k) f *= 2.0;
            double v = (u ? (double)x : (double)sx_) / f;
            if (esize == 64) { double dv = v; memcpy(&res, &dv, 8); }
            else { float fv = (float)v; uint32_t uu; memcpy(&uu, &fv, 4); res = uu; }
            break;
        }
        case 0x1F: case 0x3F: {                                                                  /* FCVTZS, FCVTZU (fixed) */
            if (esize != 32 && esize != 64) return A64_UD(t);
            double f = 1.0;
            for (unsigned k = 0; k < rshift; ++k) f *= 2.0;
            double v = esize == 64 ? a.fd[e] : (double)a.f[e];
            res = f2int(v * f, 3u, !u, esize);
            break;
        }
        default:
            return A64_UD(t);
        }
        set_el(&r, esize, e, res & m);
    }
    if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); set_el(&z, esize, 0, el(&r, esize, 0)); c->v[rd] = z; return 0; }
    finish(c, rd, &r, q);
    return 0;
}

/* ---- three different (long/wide) ---------------------------------------------------------------- */

static int simd_three_diff(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool q = BIT(w, 30), u = BIT(w, 29);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);
    const unsigned opcode = BITS(w, 15, 12);
    if (size == 3u) return A64_UD(t);
    const unsigned esize = 8u << size, n = 64u / esize, base = q ? n : 0u;
    const vreg_t a = c->v[rn], b = c->v[rm], d = c->v[rd];
    vreg_t r;
    memset(&r, 0, sizeof(r));
    const unsigned we = esize * 2u;
    const uint64_t wm = emask(we);

    if (opcode == 4u || opcode == 6u) {             /* ADDHN, SUBHN (+R) */
        vreg_t out = d;
        if (!q) memset(&out, 0, sizeof(out));
        for (unsigned e = 0; e < n; ++e) {
            uint64_t x = el(&a, we, e), y = el(&b, we, e);
            uint64_t s = opcode == 4u ? x + y : x - y;
            if (u) s += 1ULL << (esize - 1u);
            set_el(&out, esize, base + e, (s >> esize) & emask(esize));
        }
        c->v[rd] = out;
        return 0;
    }
    for (unsigned e = 0; e < n; ++e) {
        const bool wide = opcode == 1u || opcode == 3u;   /* SADDW/UADDW, SSUBW/USUBW */
        int64_t sx_ = wide ? sel(&a, we, e) : sel(&a, esize, base + e);
        uint64_t ux = wide ? el(&a, we, e) : el(&a, esize, base + e);
        int64_t sy = sel(&b, esize, base + e);
        uint64_t uy = el(&b, esize, base + e);
        uint64_t acc = el(&d, we, e), res;
        switch (opcode) {
        case 0: case 1: res = u ? ux + uy : (uint64_t)(sx_ + sy); break;                       /* ADDL, ADDW */
        case 2: case 3: res = u ? ux - uy : (uint64_t)(sx_ - sy); break;                       /* SUBL, SUBW */
        case 5: res = acc + (u ? (ux > uy ? ux - uy : uy - ux) : (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_)); break; /* ABAL */
        case 7: res = u ? (ux > uy ? ux - uy : uy - ux) : (uint64_t)(sx_ > sy ? sx_ - sy : sy - sx_); break;          /* ABDL */
        case 8: res = acc + (u ? ux * uy : (uint64_t)(sx_ * sy)); break;                       /* MLAL */
        case 10: res = acc - (u ? ux * uy : (uint64_t)(sx_ * sy)); break;                      /* MLSL */
        case 12: res = u ? ux * uy : (uint64_t)(sx_ * sy); break;                              /* MULL */
        case 14: {                                                                             /* PMULL (8-bit only) */
            if (u || size != 0u) return A64_UD(t);
            uint64_t p = 0;
            for (unsigned k = 0; k < 8u; ++k) if ((uy >> k) & 1u) p ^= ux << k;
            res = p;
            break;
        }
        default:
            return A64_UD(t);
        }
        set_el(&r, we, e, res & wm);
    }
    c->v[rd] = r;
    return 0;
}

/* ---- by element (MUL, MLA, FMUL, FMLA ...) ---------------------------------------------------- */

static int simd_by_element(gthread_t *t, a64cpu_t *c, uint32_t w, bool scalar)
{
    const bool q = BIT(w, 30) || scalar, u = BIT(w, 29);
    const unsigned size = BITS(w, 23, 22), rd = w & 31u, rn = BITS(w, 9, 5);
    const unsigned opcode = BITS(w, 15, 12);
    const unsigned L = BIT(w, 21), M = BIT(w, 20), H = BIT(w, 11);
    unsigned rm = BITS(w, 19, 16), idx;
    const vreg_t a = c->v[rn];
    vreg_t r;
    memset(&r, 0, sizeof(r));

    if (opcode == 1u || opcode == 5u || opcode == 9u) {   /* FMLA, FMLS, FMUL(X) by element */
        if (size == 2u) { idx = (H << 1) | L; rm |= M << 4; }
        else if (size == 3u) { if (L) return A64_UD(t); idx = H; rm |= M << 4; }
        else return A64_UD(t);
        const bool dbl = size == 3u;
        const unsigned lanes = scalar ? 1u : (dbl ? 2u : (q ? 4u : 2u));
        const double ev = dbl ? c->v[rm].fd[idx] : c->v[rm].f[idx];
        for (unsigned e = 0; e < lanes; ++e) {
            double x = dbl ? a.fd[e] : a.f[e], acc = dbl ? c->v[rd].fd[e] : c->v[rd].f[e], res;
            if (opcode == 9u) res = dbl ? a64_fop2_d(c, 0, x, ev) : a64_fop2_f(c, 0, (float)x, (float)ev);
            else if (opcode == 1u) res = dbl ? fma_d(x, ev, acc) : fma_f((float)x, (float)ev, (float)acc);
            else res = dbl ? fma_d(-x, ev, acc) : fma_f(-(float)x, (float)ev, (float)acc);
            if (dbl) r.fd[e] = res; else r.f[e] = (float)res;
        }
        if (scalar) { vreg_t z; memset(&z, 0, sizeof(z)); if (dbl) z.d[0] = r.d[0]; else z.s[0] = r.s[0]; c->v[rd] = z; return 0; }
        finish(c, rd, &r, q);
        return 0;
    }
    if (size == 1u) idx = (H << 2) | (L << 1) | M;
    else if (size == 2u) { idx = (H << 1) | L; rm |= M << 4; }
    else return A64_UD(t);
    const unsigned esize = 8u << size;
    const uint64_t ux = el(&c->v[rm], esize, idx);
    const int64_t sxv = sel(&c->v[rm], esize, idx);
    if ((!u && opcode == 8u) || (u && (opcode == 0u || opcode == 4u))) {  /* MUL, MLA, MLS */
        const unsigned lanes = (q ? 128u : 64u) / esize;
        for (unsigned e = 0; e < lanes; ++e) {
            uint64_t p = el(&a, esize, e) * ux, d = el(&c->v[rd], esize, e);
            uint64_t res = opcode == 8u ? p : opcode == 0u ? d + p : d - p;
            set_el(&r, esize, e, res & emask(esize));
        }
        finish(c, rd, &r, q);
        return 0;
    }
    if (opcode == 0xAu || opcode == 2u || opcode == 6u) {  /* SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL */
        const unsigned n = 64u / esize, base = q ? n : 0u, we = esize * 2u;
        for (unsigned e = 0; e < n; ++e) {
            uint64_t p = u ? el(&a, esize, base + e) * ux : (uint64_t)(sel(&a, esize, base + e) * sxv);
            uint64_t d = el(&c->v[rd], we, e);
            uint64_t res = opcode == 0xAu ? p : opcode == 2u ? d + p : d - p;
            set_el(&r, we, e, res & emask(we));
        }
        c->v[rd] = r;
        return 0;
    }
    return A64_UD(t);
}

/* ---- dispatch ------------------------------------------------------------------------------------- */

int a64_simd_dp(gthread_t *t, uint32_t w)
{
    a64cpu_t *c = A64(t);

    if ((w & 0x5F000000u) == 0x1E000000u || (w & 0xFF000000u) == 0x1F000000u) {
        return fp_scalar(t, c, w);
    }
    if ((w & 0x9F200400u) == 0x0E200400u) return simd_three_same(t, c, w, false);
    if ((w & 0xDF200400u) == 0x5E200400u) return simd_three_same(t, c, w, true);
    if ((w & 0x9F3E0C00u) == 0x0E200800u) return simd_two_misc(t, c, w, false);
    if ((w & 0xDF3E0C00u) == 0x5E200800u) return simd_two_misc(t, c, w, true);
    if ((w & 0x9F3E0C00u) == 0x0E300800u) return simd_across(t, c, w);
    if ((w & 0xDF3E0C00u) == 0x5E300800u) {          /* scalar pairwise: ADDP, FADDP, F(MAX|MIN)(NM)P */
        const unsigned rd = w & 31u, rn = BITS(w, 9, 5), opcode = BITS(w, 16, 12), size = BITS(w, 23, 22);
        const bool u = BIT(w, 29);
        vreg_t r;
        memset(&r, 0, sizeof(r));
        if (!u && opcode == 0x1Bu && size == 3u) {
            r.d[0] = c->v[rn].d[0] + c->v[rn].d[1];
        } else if (u) {
            const bool dbl = (size & 1u) != 0u, hi = (size & 2u) != 0u;
            unsigned fop;
            switch (opcode) {
            case 0x0D: fop = 2u; break;                  /* FADDP */
            case 0x0C: fop = hi ? 7u : 6u; break;        /* FMINNMP / FMAXNMP */
            case 0x0F: fop = hi ? 5u : 4u; break;        /* FMINP / FMAXP */
            default: return A64_UD(t);
            }
            if (dbl) r.fd[0] = a64_fop2_d(c, fop, c->v[rn].fd[0], c->v[rn].fd[1]);
            else r.f[0] = a64_fop2_f(c, fop, c->v[rn].f[0], c->v[rn].f[1]);
        } else {
            return A64_UD(t);
        }
        c->v[rd] = r;
        return 0;
    }
    if ((w & 0x9FE08400u) == 0x0E000400u) return simd_copy(t, c, w);
    if ((w & 0xFFE0FC00u) == 0x5E000400u) {          /* DUP (element, scalar) = MOV Vd, Vn.T[i] */
        const unsigned imm5 = BITS(w, 20, 16), rd = w & 31u, rn = BITS(w, 9, 5);
        if ((imm5 & 15u) == 0u) return A64_UD(t);
        const unsigned sz = (unsigned)__builtin_ctz(imm5), esize = 8u << sz;
        vreg_t r;
        memset(&r, 0, sizeof(r));
        set_el(&r, esize, 0, el(&c->v[rn], esize, imm5 >> (sz + 1u)));
        c->v[rd] = r;
        return 0;
    }
    if ((w & 0xBF208C00u) == 0x0E000800u) return simd_permute(t, c, w);
    if ((w & 0xBFE08400u) == 0x2E000000u) return simd_ext(t, c, w);
    if ((w & 0xBFE08C00u) == 0x0E000000u) return simd_tbl(t, c, w);
    if ((w & 0x9FF80400u) == 0x0F000400u) return simd_modimm(t, c, w);
    if ((w & 0x9F800400u) == 0x0F000400u) return simd_shift_imm(t, c, w, false);
    if ((w & 0xDF800400u) == 0x5F000400u) return simd_shift_imm(t, c, w, true);
    if ((w & 0x9F200C00u) == 0x0E200000u) return simd_three_diff(t, c, w);
    if ((w & 0x9F000400u) == 0x0F000000u) return simd_by_element(t, c, w, false);
    if ((w & 0xDF000400u) == 0x5F000000u) return simd_by_element(t, c, w, true);
    return A64_UD(t);
}

/* ---- SIMD loads and stores ---------------------------------------------------------------------- */

int a64_simd_ldst(gthread_t *t, uint32_t w, uint64_t addr, int size_log2,
                  bool load, int rt)
{
    a64cpu_t *c = A64(t);
    (void)w;
    const size_t n = (size_t)1 << size_log2;
    if (load) {
        vreg_t r;
        memset(&r, 0, sizeof(r));
        memcpy(&r, (const void *)(uintptr_t)addr, n);
        c->v[rt] = r;
    } else {
        memcpy((void *)(uintptr_t)addr, &c->v[rt], n);
    }
    return 0;
}

int a64_simd_ldst_multi(gthread_t *t, uint32_t w)
{
    a64cpu_t *c = A64(t);
    const bool q = BIT(w, 30), load = BIT(w, 22), post = BIT(w, 23), single = BIT(w, 24);
    const unsigned rn = BITS(w, 9, 5), rm = BITS(w, 20, 16), rt = w & 31u;
    const unsigned size = BITS(w, 11, 10);
    const uint64_t base = rn == 31u ? c->x[A64_SP] : c->x[rn];
    uint64_t a = base;
    uint64_t total;

    if (!single) {
        const unsigned opcode = BITS(w, 15, 12);
        unsigned rpt, selem;
        switch (opcode) {
        case 0x0: rpt = 1; selem = 4; break;
        case 0x2: rpt = 4; selem = 1; break;
        case 0x4: rpt = 1; selem = 3; break;
        case 0x6: rpt = 3; selem = 1; break;
        case 0x7: rpt = 1; selem = 1; break;
        case 0x8: rpt = 1; selem = 2; break;
        case 0xA: rpt = 2; selem = 1; break;
        default: return A64_UD(t);
        }
        if (size == 3u && !q && selem != 1u) return A64_UD(t);
        const unsigned esize = 8u << size, ebytes = esize / 8u;
        const unsigned elements = (q ? 128u : 64u) / esize;
        vreg_t regs[4];
        for (unsigned r = 0; r < rpt * selem && r < 4u; ++r) {
            regs[r] = load ? (vreg_t){ .d = { 0, 0 } } : c->v[(rt + r) & 31u];
        }
        for (unsigned r = 0; r < rpt; ++r) {
            for (unsigned e = 0; e < elements; ++e) {
                for (unsigned s = 0; s < selem; ++s) {
                    unsigned reg = r + s;
                    if (load) set_el(&regs[reg], esize, e, ld_n(a, (int)ebytes));
                    else st_n(a, (int)ebytes, el(&regs[reg], esize, e));
                    a += ebytes;
                }
            }
        }
        if (load) {
            for (unsigned r = 0; r < rpt * selem; ++r) c->v[(rt + r) & 31u] = regs[r];
        }
        total = a - base;
    } else {
        const unsigned opcode = BITS(w, 15, 13), S = BIT(w, 12), R = BIT(w, 21);
        const unsigned selem = (((opcode & 1u) << 1) | R) + 1u;
        unsigned scale = opcode >> 1, index;
        bool replicate = false;
        switch (scale) {
        case 0: index = (q << 3) | (S << 2) | size; break;
        case 1: if (size & 1u) return A64_UD(t); index = (q << 2) | (S << 1) | (size >> 1); break;
        case 2:
            if ((size & 2u) != 0u) return A64_UD(t);
            if (size == 0u) index = (q << 1) | S;
            else { if (S) return A64_UD(t); index = q; scale = 3; }
            break;
        default:
            if (!load || S) return A64_UD(t);
            scale = size;
            replicate = true;
            index = 0;
            break;
        }
        const unsigned esize = 8u << scale, ebytes = esize / 8u;
        for (unsigned s = 0; s < selem; ++s) {
            vreg_t *v = &c->v[(rt + s) & 31u];
            if (replicate) {
                uint64_t x = ld_n(a, (int)ebytes);
                vreg_t r;
                memset(&r, 0, sizeof(r));
                for (unsigned e = 0; e < (q ? 128u : 64u) / esize; ++e) set_el(&r, esize, e, x);
                *v = r;
            } else if (load) {
                set_el(v, esize, index, ld_n(a, (int)ebytes));
            } else {
                st_n(a, (int)ebytes, el(v, esize, index));
            }
            a += ebytes;
        }
        total = a - base;
    }
    if (post) {
        uint64_t nb = rm == 31u ? base + total : base + c->x[rm];
        if (rn == 31u) c->x[A64_SP] = nb; else c->x[rn] = nb;
    }
    return 0;
}
