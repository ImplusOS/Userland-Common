/*
 * x87 floating point and FXSAVE/FXRSTOR.
 *
 * Registers hold x87_t (IEEE binary128 on an AArch64 host, which holds
 * every 80-bit value exactly). After each arithmetic operation the result
 * is rounded to the 64-bit significand of the extended format, so results
 * match the x87 in the default (64-bit) precision control; loads and stores
 * of the 80-bit format are bit-exact. Transcendental instructions (FSIN,
 * FPATAN, FYL2X, ...) are computed in double precision -- they are rare in
 * x86-64 code, whose libm uses SSE2.
 */

#include "x86.h"

/* ---- 80-bit format <-> x87_t ------------------------------------------------- */

#if __LDBL_MANT_DIG__ == 113
typedef struct { uint64_t lo, hi; } q128_t;

x87_t x87_from_80(const uint8_t *p)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    uint64_t exp = se & 0x7FFFu;
    uint64_t sign = (uint64_t)(se >> 15);
    uint64_t m63 = mant & 0x7FFFFFFFFFFFFFFFULL;
    q128_t q;
    if (exp == 0u) {
        m63 = mant;     /* denormal (or zero): no integer bit */
    } else if (exp != 0x7FFFu && (mant >> 63) == 0u) {
        /* unnormal: invalid on the 387 and later; read as NaN */
        exp = 0x7FFFu;
        m63 = 0x4000000000000000ULL;
    }
    q.hi = (sign << 63) | (exp << 48) | (m63 >> 15);
    q.lo = m63 << 49;
    x87_t v;
    memcpy(&v, &q, 16);
    return v;
}

void x87_to_80(x87_t v, uint8_t *p)
{
    q128_t q;
    memcpy(&q, &v, 16);
    uint64_t sign = q.hi >> 63;
    uint64_t exp = (q.hi >> 48) & 0x7FFFu;
    uint64_t fhi = q.hi & 0xFFFFFFFFFFFFULL;    /* 48 bits */
    uint64_t flo = q.lo;                         /* 64 bits */
    /* the top 63 bits of the 112-bit fraction, and the 49 below them */
    uint64_t m63 = (fhi << 15) | (flo >> 49);
    uint64_t rest = flo & ((1ULL << 49) - 1u);
    uint64_t mant;
    if (exp == 0x7FFFu) {
        mant = (1ULL << 63) | m63;
        if ((fhi | flo) != 0u && m63 == 0u) {
            mant |= 1ULL << 62;     /* keep a NaN a NaN */
        }
    } else {
        mant = (exp != 0u ? (1ULL << 63) : 0u) | m63;
        const uint64_t half = 1ULL << 48;
        if (rest > half || (rest == half && (mant & 1u))) {
            ++mant;
            if (exp != 0u && mant == 0u) {          /* carried out of the integer bit */
                mant = 1ULL << 63;
                ++exp;
            } else if (exp == 0u && (mant >> 63) != 0u) {
                exp = 1;                             /* denormal rounded up to normal */
            }
        }
        if (exp >= 0x7FFFu) {
            exp = 0x7FFFu;
            mant = 1ULL << 63;
        }
    }
    uint16_t se = (uint16_t)((sign << 15) | exp);
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}
#elif __LDBL_MANT_DIG__ == 64
x87_t x87_from_80(const uint8_t *p) { x87_t v = 0; memcpy(&v, p, 10); return v; }
void x87_to_80(x87_t v, uint8_t *p) { memcpy(p, &v, 10); }
#else   /* long double is double: precision is lost beyond 53 bits */
x87_t x87_from_80(const uint8_t *p)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int exp = se & 0x7FFF;
    double sign = (se & 0x8000u) ? -1.0 : 1.0;
    if (exp == 0x7FFF) {
        if ((mant << 1) == 0u) return (x87_t)(sign * __builtin_inf());
        return (x87_t)__builtin_nan("");
    }
    if (mant == 0u) return (x87_t)(sign * 0.0);
    return (x87_t)(sign * __builtin_ldexp((double)mant, exp - 16383 - 63));
}

void x87_to_80(x87_t v, uint8_t *p)
{
    double d = (double)v;
    uint64_t u;
    memcpy(&u, &d, 8);
    uint64_t sign = u >> 63;
    int exp = (int)((u >> 52) & 0x7FFu);
    uint64_t frac = u & 0xFFFFFFFFFFFFFULL;
    uint64_t mant;
    int e80;
    if (exp == 0x7FF) { e80 = 0x7FFF; mant = (1ULL << 63) | (frac << 11); }
    else if (exp == 0 && frac == 0u) { e80 = 0; mant = 0; }
    else if (exp == 0) {   /* subnormal double: frac * 2^-1074 */
        int sh = __builtin_clzll(frac);
        mant = frac << sh;
        e80 = 16383 + 63 - 1074 - sh;
    } else { e80 = exp - 1023 + 16383; mant = (1ULL << 63) | (frac << 11); }
    uint16_t se = (uint16_t)((sign << 15) | (uint64_t)e80);
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}
#endif

/* Round to the extended format's 64-bit significand. */
static inline x87_t ext(x87_t v)
{
#if __LDBL_MANT_DIG__ == 113
    uint8_t b[10];
    x87_to_80(v, b);
    return x87_from_80(b);
#else
    return v;
#endif
}

/* ---- the register stack -------------------------------------------------------- */

#define ST(c, n) ((c)->st[((c)->ftop + (n)) & 7u])
#define C0 0x0100u
#define C1 0x0200u
#define C2 0x0400u
#define C3 0x4000u

static inline void fpush(x86cpu_t *c, x87_t v)
{
    c->ftop = (uint8_t)((c->ftop - 1u) & 7u);
    c->st[c->ftop] = v;
    c->fempty &= (uint8_t)~(1u << c->ftop);
}

static inline void fpop(x86cpu_t *c)
{
    c->fempty |= (uint8_t)(1u << c->ftop);
    c->ftop = (uint8_t)((c->ftop + 1u) & 7u);
}

uint16_t x86_fpu_status(const x86cpu_t *c)
{
    return (uint16_t)((c->fsw & ~0x3800u) | ((unsigned)c->ftop << 11));
}

uint16_t x86_fpu_tagword_abridged(const x86cpu_t *c)
{
    return (uint16_t)(~c->fempty & 0xFFu);
}

static uint16_t full_tagword(const x86cpu_t *c)
{
    uint16_t tw = 0;
    for (unsigned r = 0; r < 8u; ++r) {
        unsigned tag;
        if (c->fempty & (1u << r)) {
            tag = 3;
        } else {
            x87_t v = c->st[r];
            if (v == 0) tag = 1;
            else if (v != v || v - v != 0) tag = 2;
            else tag = 0;
        }
        tw |= (uint16_t)(tag << (2 * r));
    }
    return tw;
}

void x86_fpu_reset(x86cpu_t *c)
{
    c->fcw = 0x037F;
    c->fsw = 0;
    c->ftop = 0;
    c->fempty = 0xFF;
    c->fop = 0;
    c->fip = 0;
    c->fdp = 0;
}

static void set_cc(x86cpu_t *c, uint16_t bits)
{
    c->fsw = (uint16_t)((c->fsw & ~(C0 | C1 | C2 | C3)) | bits);
}

static uint16_t compare_cc(x87_t a, x87_t b)
{
    if (a != a || b != b) return C0 | C2 | C3;
    if (a < b) return C0;
    if (a == b) return C3;
    return 0;
}

static void compare_eflags(x86cpu_t *c, x87_t a, x87_t b)
{
    uint64_t f;
    if (a != a || b != b) f = F_ZF | F_PF | F_CF;
    else if (a < b) f = F_CF;
    else if (a == b) f = F_ZF;
    else f = 0;
    x86_set_flags(c, f);
}

/* ---- rounding and integer conversion --------------------------------------------- */

static x87_t trunc_x(x87_t v)
{
    if (v != v || v - v != 0) return v;
    const x87_t big = (x87_t)9223372036854775808.0;
    if (v >= big || v <= -big) return v;   /* already integral */
    return (x87_t)(int64_t)v;
}

static x87_t round_x(x87_t v, unsigned rc)
{
    x87_t t = trunc_x(v);
    if (t == v || v != v) return v;
    switch (rc & 3u) {
    case 1: return v < 0 ? t - 1 : t;          /* down */
    case 2: return v > 0 ? t + 1 : t;          /* up */
    case 3: return t;                          /* toward zero */
    default: {
        x87_t d = v - t;
        if (d < 0) d = -d;
        x87_t one = v < 0 ? -1 : 1;
        if (d > (x87_t)0.5) return t + one;
        if (d < (x87_t)0.5) return t;
        int64_t ti = (int64_t)t;
        return (ti & 1) ? t + one : t;
    }
    }
}

static int64_t to_int(x87_t v, unsigned rc, int bytes)
{
    x87_t r = round_x(v, rc);
    const int64_t lim = bytes == 2 ? 32768 : bytes == 4 ? 2147483648LL : 0;
    if (bytes == 8) {
        if (!(r >= (x87_t)-9223372036854775808.0 && r < (x87_t)9223372036854775808.0)) {
            return INT64_MIN;
        }
        return (int64_t)r;
    }
    if (!(r >= (x87_t)-lim && r < (x87_t)lim)) {
        return -lim;
    }
    return (int64_t)r;
}

/* ---- double-precision transcendental helpers ---------------------------------------- */

static const double k_pi = 3.14159265358979323846;
static const double k_ln2 = 0.69314718055994530942;

static double d_scale(double x, int n)
{
    while (n > 1000) { x *= 0x1p1000; n -= 1000; }
    while (n < -1000) { x *= 0x1p-1000; n += 1000; }
    uint64_t u = (uint64_t)(n + 1023) << 52;
    double p;
    memcpy(&p, &u, 8);
    return x * p;
}

static double d_exp2(double x)
{
    if (x != x) return x;
    if (x > 1024) return __builtin_inf();
    if (x < -1080) return 0.0;
    double n = __builtin_floor(x + 0.5);
    double f = (x - n) * k_ln2;        /* |f| <= ln2/2 */
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 20; ++k) {
        term *= f / k;
        sum += term;
    }
    return d_scale(sum, (int)n);
}

static double d_expm1_2(double x)  /* 2^x - 1 for |x| <= 1 */
{
    if (x > -0.5 && x < 0.5) {
        double f = x * k_ln2, term = f, sum = f;
        for (int k = 2; k < 24; ++k) {
            term *= f / k;
            sum += term;
        }
        return sum;
    }
    return d_exp2(x) - 1.0;
}

static double d_ln(double x)
{
    if (x != x || x < 0) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    if (x - x != 0) return x;
    uint64_t u;
    memcpy(&u, &x, 8);
    int e = (int)((u >> 52) & 0x7FFu);
    if (e == 0) {           /* subnormal */
        x *= 0x1p64;
        memcpy(&u, &x, 8);
        e = (int)((u >> 52) & 0x7FFu) - 64;
    }
    e -= 1023;
    u = (u & 0x800FFFFFFFFFFFFFULL) | (1023ULL << 52);
    double m;
    memcpy(&m, &u, 8);
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        ++e;
    }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, t = s, sum = 0;
    for (int k = 1; k < 60; k += 2) {
        sum += t / k;
        t *= s2;
    }
    return 2.0 * sum + e * k_ln2;
}

static double d_ln1p(double x)
{
    if (x > -0.25 && x < 0.25) {
        /* atanh form: ln(1+x) = 2 atanh(x / (2 + x)) */
        double s = x / (2.0 + x), s2 = s * s, t = s, sum = 0;
        for (int k = 1; k < 60; k += 2) {
            sum += t / k;
            t *= s2;
        }
        return 2.0 * sum;
    }
    return d_ln(1.0 + x);
}

static double d_atan(double x)
{
    if (x != x) return x;
    bool neg = x < 0;
    if (neg) x = -x;
    bool inv = x > 1.0;
    if (inv) x = 1.0 / x;
    /* two argument halvings: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    double y = x / (1.0 + __builtin_sqrt(1.0 + x * x));
    y = y / (1.0 + __builtin_sqrt(1.0 + y * y));
    double y2 = y * y, t = y, sum = 0;
    for (int k = 1; k < 40; k += 2) {
        sum += ((k & 2) ? -t : t) / k;
        t *= y2;
    }
    double r = 4.0 * sum;
    if (inv) r = k_pi / 2 - r;
    return neg ? -r : r;
}

static double d_atan2(double y, double x)
{
    if (x != x || y != y) return x + y;
    if (x == 0) {
        if (y == 0) return __builtin_signbit(x) ? (__builtin_signbit(y) ? -k_pi : k_pi) : y;
        return y > 0 ? k_pi / 2 : -k_pi / 2;
    }
    double r = d_atan(y / x);
    if (x < 0) r += __builtin_signbit(y) ? -k_pi : k_pi;
    return r;
}

/* sin and cos of x; false if |x| is too large (the FPU's 2^63 limit) */
static bool d_sincos(double x, double *s, double *co)
{
    if (x != x || x - x != 0 || x > 9.2e18 || x < -9.2e18) return false;
    double n = __builtin_floor(x / (k_pi / 2) + 0.5);
    /* Cody-Waite reduction with pi/2 in three parts */
    double r = ((x - n * 1.57079632673412561417) - n * 6.07710050650619224932e-11)
               - n * 2.02226624879595063154e-21;
    double r2 = r * r, ts = r, tc = 1, ss = 0, cc = 0;
    for (int k = 1; k < 30; k += 2) {
        ss += ts;
        ts *= -r2 / ((k + 1) * (k + 2));
    }
    for (int k = 0; k < 30; k += 2) {
        cc += tc;
        tc *= -r2 / ((k + 1) * (k + 2));
    }
    int q = (int)((int64_t)n & 3);
    switch (q) {
    case 0: *s = ss; *co = cc; break;
    case 1: *s = cc; *co = -ss; break;
    case 2: *s = -ss; *co = -cc; break;
    default: *s = -cc; *co = ss; break;
    }
    return true;
}

static x87_t indefinite(void);

static x87_t sqrt_x(x87_t v)
{
    if (v != v || v == 0 || (v > 0 && v - v != 0)) return v;
    if (v < 0) return indefinite();
    x87_t s = (x87_t)__builtin_sqrt((double)v);
    if (s == 0 || s - s != 0) return s;
    /* Newton: 53 -> 106 -> 212 bits */
    s = (s + v / s) / 2;
    s = (s + v / s) / 2;
    return s;
}

/* Exact partial remainder: a - q*b with q = a/b truncated (FPREM) or rounded
 * to nearest even (FPREM1). Low quotient bits go to *qbits. */
static x87_t rem_x(x87_t a, x87_t b, bool nearest, uint64_t *qbits)
{
    *qbits = 0;
    if (a != a || b != b || b == 0 || a - a != 0) return (a - a) / (b - b + 0);
    if (b - b != 0 || a == 0) return a;
    x87_t ab = a < 0 ? -a : a, bb = b < 0 ? -b : b;
    /* long division by repeated doubling; exact in binary floating point */
    x87_t r = ab;
    uint64_t q = 0;
    if (r >= bb) {
        x87_t d = bb;
        int shifts = 0;
        while (d * 2 <= r && shifts < 20000) {
            d *= 2;
            ++shifts;
        }
        for (; shifts >= 0; --shifts) {
            q <<= 1;
            if (r >= d) {
                r -= d;
                q |= 1u;
            }
            d /= 2;
        }
    }
    if (nearest) {
        x87_t twice = r * 2;
        if (twice > bb || (twice == bb && (q & 1u))) {
            r -= bb;
            ++q;
        }
    }
    *qbits = q;
    return a < 0 ? -r : r;
}

/* ---- memory operands ---------------------------------------------------------------- */

static x87_t ld_f32(uint64_t a) { uint32_t u = (uint32_t)mrd(a, 4); float f; memcpy(&f, &u, 4); return (x87_t)f; }
static x87_t ld_f64(uint64_t a) { uint64_t u = mrd(a, 8); double d; memcpy(&d, &u, 8); return (x87_t)d; }
static void st_f32(uint64_t a, x87_t v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); mwr(a, 4, u); }
static void st_f64(uint64_t a, x87_t v) { double d = (double)v; uint64_t u; memcpy(&u, &d, 8); mwr(a, 8, u); }
static x87_t ld_f80(uint64_t a) { uint8_t b[10]; mrd_n(a, b, 10); return x87_from_80(b); }
static void st_f80(uint64_t a, x87_t v) { uint8_t b[10]; x87_to_80(v, b); mwr_n(a, b, 10); }

/* x87 NaN rules: an invalid operation without NaN operands gives the "real
 * indefinite" (negative QNaN, significand C000...); a NaN operand is
 * returned quietened, the larger significand of two. */
static x87_t indefinite(void)
{
    static const uint8_t b[10] = { 0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF };
    return x87_from_80(b);
}

static x87_t quiet(x87_t v, uint64_t *mant)
{
    uint8_t b[10];
    x87_to_80(v, b);
    b[7] |= 0x40u;
    memcpy(mant, b, 8);
    return x87_from_80(b);
}

static x87_t fixnan(x87_t r, x87_t a, x87_t b)
{
    if (r == r) return r;
    const bool na = a != a, nb = b != b;
    if (!na && !nb) return indefinite();
    uint64_t ma = 0, mb = 0;
    x87_t qa = na ? quiet(a, &ma) : a, qb = nb ? quiet(b, &mb) : b;
    if (na && nb) return ma >= mb ? qa : qb;
    return na ? qa : qb;
}

static x87_t arith(unsigned op, x87_t a, x87_t b)
{
    x87_t r;
    switch (op & 7u) {
    case 0: r = a + b; break;
    case 1: r = a * b; break;
    case 4: r = a - b; break;
    case 5: r = b - a; break;
    case 6: r = a / b; break;
    case 7: r = b / a; break;
    default: return a;
    }
    return ext(fixnan(r, a, b));
}

static void fstenv(x86cpu_t *c, uint64_t a)
{
    uint32_t env[7];
    env[0] = 0xFFFF0000u | c->fcw;
    env[1] = 0xFFFF0000u | x86_fpu_status(c);
    env[2] = 0xFFFF0000u | full_tagword(c);
    env[3] = (uint32_t)c->fip;
    env[4] = (uint32_t)c->fop << 16;
    env[5] = (uint32_t)c->fdp;
    env[6] = 0xFFFF0000u;
    mwr_n(a, env, sizeof(env));
}

static void fldenv(x86cpu_t *c, uint64_t a)
{
    uint32_t env[7];
    mrd_n(a, env, sizeof(env));
    c->fcw = (uint16_t)env[0];
    c->fsw = (uint16_t)(env[1] & ~0x3800u);
    c->ftop = (uint8_t)((env[1] >> 11) & 7u);
    c->fempty = 0;
    for (unsigned r = 0; r < 8u; ++r) {
        if (((env[2] >> (2 * r)) & 3u) == 3u) c->fempty |= (uint8_t)(1u << r);
    }
}

/* ---- the instructions ------------------------------------------------------------------- */

int x86_x87(insn_t *i, uint8_t op)
{
    x86cpu_t *c = i->c;
    x86_modrm(i);
    const unsigned sub = i->reg & 7u;
    const unsigned sti = i->modrm & 7u;
    const unsigned rc = (c->fcw >> 10) & 3u;

    if (i->mod != 3) {
        const uint64_t a = x86_ea(i);
        c->fdp = a;
        c->fip = i->start;
        switch (op) {
        case 0xD8: case 0xDA: case 0xDC: case 0xDE: {
            x87_t m;
            if (op == 0xD8) m = ld_f32(a);
            else if (op == 0xDC) m = ld_f64(a);
            else if (op == 0xDA) m = (x87_t)(int32_t)mrd(a, 4);
            else m = (x87_t)(int16_t)mrd(a, 2);
            if (sub == 2u || sub == 3u) {
                set_cc(c, compare_cc(ST(c, 0), m));
                if (sub == 3u) fpop(c);
            } else {
                ST(c, 0) = arith(sub, ST(c, 0), m);
            }
            return 0;
        }
        case 0xD9:
            switch (sub) {
            case 0: fpush(c, ld_f32(a)); return 0;
            case 2: st_f32(a, ST(c, 0)); return 0;
            case 3: st_f32(a, ST(c, 0)); fpop(c); return 0;
            case 4: fldenv(c, a); return 0;
            case 5: c->fcw = (uint16_t)mrd(a, 2); return 0;
            case 6: fstenv(c, a); return 0;
            case 7: mwr(a, 2, c->fcw); return 0;
            default: return X86_UD(i);
            }
        case 0xDB:
            switch (sub) {
            case 0: fpush(c, (x87_t)(int32_t)mrd(a, 4)); return 0;
            case 1: mwr(a, 4, (uint64_t)to_int(ST(c, 0), 3, 4)); fpop(c); return 0;
            case 2: mwr(a, 4, (uint64_t)to_int(ST(c, 0), rc, 4)); return 0;
            case 3: mwr(a, 4, (uint64_t)to_int(ST(c, 0), rc, 4)); fpop(c); return 0;
            case 5: fpush(c, ld_f80(a)); return 0;
            case 7: st_f80(a, ST(c, 0)); fpop(c); return 0;
            default: return X86_UD(i);
            }
        case 0xDD:
            switch (sub) {
            case 0: fpush(c, ld_f64(a)); return 0;
            case 1: mwr(a, 8, (uint64_t)to_int(ST(c, 0), 3, 8)); fpop(c); return 0;
            case 2: st_f64(a, ST(c, 0)); return 0;
            case 3: st_f64(a, ST(c, 0)); fpop(c); return 0;
            case 4: { /* FRSTOR */
                fldenv(c, a);
                for (unsigned k = 0; k < 8u; ++k) ST(c, k) = ld_f80(a + 28u + 10u * k);
                return 0;
            }
            case 6: { /* FNSAVE */
                fstenv(c, a);
                for (unsigned k = 0; k < 8u; ++k) st_f80(a + 28u + 10u * k, ST(c, k));
                x86_fpu_reset(c);
                return 0;
            }
            case 7: mwr(a, 2, x86_fpu_status(c)); return 0;
            default: return X86_UD(i);
            }
        default: /* 0xDF */
            switch (sub) {
            case 0: fpush(c, (x87_t)(int16_t)mrd(a, 2)); return 0;
            case 1: mwr(a, 2, (uint64_t)to_int(ST(c, 0), 3, 2)); fpop(c); return 0;
            case 2: mwr(a, 2, (uint64_t)to_int(ST(c, 0), rc, 2)); return 0;
            case 3: mwr(a, 2, (uint64_t)to_int(ST(c, 0), rc, 2)); fpop(c); return 0;
            case 5: fpush(c, (x87_t)(int64_t)mrd(a, 8)); return 0;
            case 7: mwr(a, 8, (uint64_t)to_int(ST(c, 0), rc, 8)); fpop(c); return 0;
            case 4: { /* FBLD */
                uint8_t b[10];
                mrd_n(a, b, 10);
                x87_t v = 0;
                for (int k = 8; k >= 0; --k) v = v * 100 + (b[k] >> 4) * 10 + (b[k] & 15);
                fpush(c, (b[9] & 0x80u) ? -v : v);
                return 0;
            }
            case 6: { /* FBSTP */
                x87_t v = round_x(ST(c, 0), rc);
                uint8_t b[10];
                memset(b, 0, sizeof(b));
                if (v < 0) { b[9] = 0x80; v = -v; }
                uint64_t n = (uint64_t)v;
                for (int k = 0; k < 9; ++k) {
                    uint8_t lo = (uint8_t)(n % 10u); n /= 10u;
                    uint8_t hi = (uint8_t)(n % 10u); n /= 10u;
                    b[k] = (uint8_t)(hi << 4 | lo);
                }
                mwr_n(a, b, 10);
                fpop(c);
                return 0;
            }
            default: return X86_UD(i);
            }
        }
    }

    /* register forms */
    switch (op) {
    case 0xD8:
        if (sub == 2u || sub == 3u) {
            set_cc(c, compare_cc(ST(c, 0), ST(c, sti)));
            if (sub == 3u) fpop(c);
        } else {
            ST(c, 0) = arith(sub, ST(c, 0), ST(c, sti));
        }
        return 0;
    case 0xDC: case 0xDE: {
        if (op == 0xDE && i->modrm == 0xD9) {           /* FCOMPP */
            set_cc(c, compare_cc(ST(c, 0), ST(c, 1)));
            fpop(c);
            fpop(c);
            return 0;
        }
        if (sub == 2u || sub == 3u) {
            set_cc(c, compare_cc(ST(c, 0), ST(c, sti)));
            if (sub == 3u) fpop(c);
            return 0;
        }
        /* ST(i) = ST(i) op ST(0): /4 FSUBR, /5 FSUB, /6 FDIVR, /7 FDIV
         * (the reverse forms of D8's /4../7) */
        x87_t a = ST(c, sti), b = ST(c, 0);
        const unsigned o = sub;
        x87_t r;
        switch (o) {
        case 4: r = ext(fixnan(b - a, a, b)); break;    /* FSUBR ST(i): ST(0) - ST(i) */
        case 5: r = ext(fixnan(a - b, a, b)); break;    /* FSUB ST(i): ST(i) - ST(0) */
        case 6: r = ext(fixnan(b / a, a, b)); break;    /* FDIVR ST(i): ST(0) / ST(i) */
        case 7: r = ext(fixnan(a / b, a, b)); break;    /* FDIV ST(i): ST(i) / ST(0) */
        default: r = arith(o, a, b); break;
        }
        ST(c, sti) = r;
        if (op == 0xDE) fpop(c);
        return 0;
    }
    case 0xD9:
        switch (i->modrm) {
        case 0xD0: return 0;                                        /* FNOP */
        case 0xE0: ST(c, 0) = -ST(c, 0); return 0;                  /* FCHS */
        case 0xE1: if (ST(c, 0) < 0 || (ST(c, 0) == 0 && __builtin_signbit((double)ST(c, 0)))) ST(c, 0) = -ST(c, 0); return 0;
        case 0xE4: set_cc(c, compare_cc(ST(c, 0), 0)); return 0;     /* FTST */
        case 0xE5: { /* FXAM */
            uint16_t cc = 0;
            x87_t v = ST(c, 0);
            if (__builtin_signbit((double)v)) cc |= C1;
            if (c->fempty & (1u << c->ftop)) cc |= C3 | C0;
            else if (v != v) cc |= C0;
            else if (v - v != 0) cc |= C2 | C0;
            else if (v == 0) cc |= C3;
            else cc |= C2;
            set_cc(c, cc);
            return 0;
        }
        case 0xE8: fpush(c, 1); return 0;
        case 0xE9: fpush(c, ext((x87_t)3.32192809488736234787031942948939L)); return 0;
        case 0xEA: fpush(c, ext((x87_t)1.44269504088896340735992468100189L)); return 0;
        case 0xEB: fpush(c, ext((x87_t)3.14159265358979323846264338327950L)); return 0;
        case 0xEC: fpush(c, ext((x87_t)0.30102999566398119521373889472449L)); return 0;
        case 0xED: fpush(c, ext((x87_t)0.69314718055994530941723212145818L)); return 0;
        case 0xEE: fpush(c, 0); return 0;
        case 0xF0: ST(c, 0) = (x87_t)d_expm1_2((double)ST(c, 0)); return 0;          /* F2XM1 */
        case 0xF1: ST(c, 1) = ext(ST(c, 1) * (x87_t)(d_ln((double)ST(c, 0)) / k_ln2)); fpop(c); return 0;
        case 0xF2: { /* FPTAN */
            double s, co;
            if (!d_sincos((double)ST(c, 0), &s, &co)) { set_cc(c, C2); return 0; }
            ST(c, 0) = (x87_t)(s / co);
            set_cc(c, 0);
            fpush(c, 1);
            return 0;
        }
        case 0xF3: ST(c, 1) = (x87_t)d_atan2((double)ST(c, 1), (double)ST(c, 0)); fpop(c); return 0;
        case 0xF4: { /* FXTRACT */
            x87_t v = ST(c, 0);
            if (v == 0 || v != v || v - v != 0) {
                fpush(c, v);
                return 0;
            }
            uint8_t b[10];
            x87_to_80(v, b);
            uint16_t se;
            memcpy(&se, b + 8, 2);
            int e = (int)(se & 0x7FFFu) - 16383;
            se = (uint16_t)((se & 0x8000u) | 16383u);
            memcpy(b + 8, &se, 2);
            ST(c, 0) = (x87_t)e;
            fpush(c, x87_from_80(b));
            return 0;
        }
        case 0xF5: case 0xF8: { /* FPREM1, FPREM */
            uint64_t q;
            ST(c, 0) = rem_x(ST(c, 0), ST(c, 1), i->modrm == 0xF5, &q);
            set_cc(c, (uint16_t)(((q & 1u) ? C1 : 0u) | ((q & 2u) ? C3 : 0u) | ((q & 4u) ? C0 : 0u)));
            return 0;
        }
        case 0xF6: c->ftop = (uint8_t)((c->ftop - 1u) & 7u); return 0;
        case 0xF7: c->ftop = (uint8_t)((c->ftop + 1u) & 7u); return 0;
        case 0xF9: ST(c, 1) = ext(ST(c, 1) * (x87_t)(d_ln1p((double)ST(c, 0)) / k_ln2)); fpop(c); return 0;
        case 0xFA: ST(c, 0) = ext(sqrt_x(ST(c, 0))); return 0;
        case 0xFB: { /* FSINCOS */
            double s, co;
            if (!d_sincos((double)ST(c, 0), &s, &co)) { set_cc(c, C2); return 0; }
            ST(c, 0) = (x87_t)s;
            set_cc(c, 0);
            fpush(c, (x87_t)co);
            return 0;
        }
        case 0xFC: ST(c, 0) = round_x(ST(c, 0), rc); return 0;
        case 0xFD: { /* FSCALE */
            x87_t s = trunc_x(ST(c, 1));
            x87_t v = ST(c, 0);
            if (v != 0 && v == v && v - v == 0 && s == s) {
                int64_t n = s > 40000 ? 40000 : s < -40000 ? -40000 : (int64_t)s;
                while (n > 0) { int k = n > 1000 ? 1000 : (int)n; v *= (x87_t)d_scale(1.0, k); n -= k; }
                while (n < 0) { int k = n < -1000 ? 1000 : (int)-n; v *= (x87_t)d_scale(1.0, -k); n += k; }
                ST(c, 0) = ext(v);
            }
            return 0;
        }
        case 0xFE: case 0xFF: {
            double s, co;
            if (!d_sincos((double)ST(c, 0), &s, &co)) { set_cc(c, C2); return 0; }
            ST(c, 0) = (x87_t)(i->modrm == 0xFE ? s : co);
            set_cc(c, 0);
            return 0;
        }
        default:
            break;
        }
        if (sub == 0u) { fpush(c, ST(c, sti)); return 0; }          /* FLD ST(i) */
        if (sub == 1u) {                                            /* FXCH */
            x87_t t = ST(c, 0);
            ST(c, 0) = ST(c, sti);
            ST(c, sti) = t;
            return 0;
        }
        return X86_UD(i);
    case 0xDA: case 0xDB:
        if (op == 0xDA && i->modrm == 0xE9) {       /* FUCOMPP */
            set_cc(c, compare_cc(ST(c, 0), ST(c, 1)));
            fpop(c);
            fpop(c);
            return 0;
        }
        if (op == 0xDB && i->modrm == 0xE2) {       /* FNCLEX */
            c->fsw &= 0x7F00u;
            return 0;
        }
        if (op == 0xDB && i->modrm == 0xE3) {       /* FNINIT */
            x86_fpu_reset(c);
            return 0;
        }
        if (op == 0xDB && (sub == 5u || sub == 6u)) { /* FUCOMI, FCOMI */
            compare_eflags(c, ST(c, 0), ST(c, sti));
            return 0;
        }
        if (sub <= 3u) {                            /* FCMOVcc */
            static const uint8_t cc[4] = { 2, 4, 6, 10 };   /* B, E, BE, U (x86 cond codes) */
            uint64_t f = x86_get_flags(c);
            bool t;
            switch (cc[sub]) {
            case 2: t = (f & F_CF) != 0u; break;
            case 4: t = (f & F_ZF) != 0u; break;
            case 6: t = (f & (F_CF | F_ZF)) != 0u; break;
            default: t = (f & F_PF) != 0u; break;
            }
            if (op == 0xDB) t = !t;
            if (t) ST(c, 0) = ST(c, sti);
            return 0;
        }
        return X86_UD(i);
    case 0xDD:
        switch (sub) {
        case 0: c->fempty |= (uint8_t)(1u << ((c->ftop + sti) & 7u)); return 0;  /* FFREE */
        case 2: ST(c, sti) = ST(c, 0); return 0;
        case 3: ST(c, sti) = ST(c, 0); fpop(c); return 0;
        case 4: set_cc(c, compare_cc(ST(c, 0), ST(c, sti))); return 0;
        case 5: set_cc(c, compare_cc(ST(c, 0), ST(c, sti))); fpop(c); return 0;
        default: return X86_UD(i);
        }
    default: /* 0xDF */
        if (i->modrm == 0xE0) {                     /* FNSTSW AX */
            c->r[RAX] = (c->r[RAX] & ~0xFFFFULL) | x86_fpu_status(c);
            return 0;
        }
        if (sub == 5u || sub == 6u) {               /* FUCOMIP, FCOMIP */
            compare_eflags(c, ST(c, 0), ST(c, sti));
            fpop(c);
            return 0;
        }
        if (sub == 0u) {                            /* FFREEP */
            c->fempty |= (uint8_t)(1u << ((c->ftop + sti) & 7u));
            fpop(c);
            return 0;
        }
        return X86_UD(i);
    }
}

/* ---- FXSAVE / FXRSTOR (64-bit layout) -------------------------------------------------- */

int x86_fxsave(insn_t *i, uint64_t a)
{
    x86cpu_t *c = i->c;
    uint8_t b[512];
    memset(b, 0, sizeof(b));
    x86_mxcsr_collect(c);
    uint16_t fsw = x86_fpu_status(c);
    uint8_t ftw = (uint8_t)x86_fpu_tagword_abridged(c);
    uint32_t mxcsr_mask = 0xFFFF;
    memcpy(b + 0, &c->fcw, 2);
    memcpy(b + 2, &fsw, 2);
    b[4] = ftw;
    memcpy(b + 6, &c->fop, 2);
    memcpy(b + 8, &c->fip, 8);
    memcpy(b + 16, &c->fdp, 8);
    memcpy(b + 24, &c->mxcsr, 4);
    memcpy(b + 28, &mxcsr_mask, 4);
    for (unsigned k = 0; k < 8u; ++k) {
        x87_to_80(ST(c, k), b + 32 + 16 * k);
    }
    memcpy(b + 160, c->xmm, 256);
    mwr_n(a, b, 512);
    return 0;
}

int x86_fxrstor(insn_t *i, uint64_t a)
{
    x86cpu_t *c = i->c;
    uint8_t b[512];
    mrd_n(a, b, 512);
    uint16_t fsw;
    uint32_t mxcsr;
    memcpy(&c->fcw, b + 0, 2);
    memcpy(&fsw, b + 2, 2);
    memcpy(&mxcsr, b + 24, 4);
    if ((mxcsr & 0xFFFF0000u) != 0u) {
        return x86_fault(i, XL_SIGSEGV, XL_SI_KERNEL, 0);
    }
    c->fsw = (uint16_t)(fsw & ~0x3800u);
    c->ftop = (uint8_t)((fsw >> 11) & 7u);
    /* abridged tag: bit set = physical register in use */
    c->fempty = (uint8_t)~b[4];
    memcpy(&c->fop, b + 6, 2);
    memcpy(&c->fip, b + 8, 8);
    memcpy(&c->fdp, b + 16, 8);
    for (unsigned k = 0; k < 8u; ++k) {
        ST(c, k) = x87_from_80(b + 32 + 16 * k);
    }
    memcpy(c->xmm, b + 160, 256);
    c->mxcsr = mxcsr;
    x86_mxcsr_apply(c);
    return 0;
}
