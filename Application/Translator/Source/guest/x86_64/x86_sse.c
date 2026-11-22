/*
 * SSE, SSE2 and MMX (the 0F xx opcodes that are not integer instructions).
 *
 * Floating point runs on the host's FP unit, in the guest's rounding mode
 * (x86_mxcsr_apply loads MXCSR.RC/FZ into the host's control register), and
 * the host's sticky exception flags are folded back into MXCSR when the
 * guest reads it. Where IEEE leaves room and x86 and the host differ, the
 * x86 behaviour is reproduced explicitly:
 *   - a NaN result propagates the first NaN operand (quietened), and an
 *     invalid operation without NaN operands gives the negative "default"
 *     QNaN;
 *   - MIN/MAX return the second operand when either is NaN or both are zero;
 *   - float -> integer conversion that overflows (or of NaN) gives the
 *     "integer indefinite" 0x80..0.
 * MMX operates on mm[] -- separate from the x87 registers, which no code
 * that uses both relies on aliasing after EMMS.
 */

#include "x86.h"
#include "host.h"

enum { P_NONE, P_66, P_F3, P_F2 };

static inline uint32_t f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; __builtin_memcpy(&d, &u, 8); return d; }

static inline float nanfix_f(float r, float a, float b)
{
    if (r == r) return r;
    if (a != a) return u2f(f2u(a) | 0x00400000u);
    if (b != b) return u2f(f2u(b) | 0x00400000u);
    return u2f(0xFFC00000u);
}

static inline double nanfix_d(double r, double a, double b)
{
    if (r == r) return r;
    if (a != a) return u2d(d2u(a) | 0x0008000000000000ULL);
    if (b != b) return u2d(d2u(b) | 0x0008000000000000ULL);
    return u2d(0xFFF8000000000000ULL);
}

static float fop_f(uint8_t op, float a, float b)
{
    float r;
    switch (op) {
    case 0x58: r = a + b; break;
    case 0x59: r = a * b; break;
    case 0x5C: r = a - b; break;
    case 0x5E: r = a / b; break;
    case 0x5D: return a < b ? a : b;
    case 0x5F: return a > b ? a : b;
    case 0x51: r = __builtin_sqrtf(b); return nanfix_f(r, b, b);
    case 0x52: r = 1.0f / __builtin_sqrtf(b); return nanfix_f(r, b, b);
    case 0x53: r = 1.0f / b; return nanfix_f(r, b, b);
    default: r = 0; break;
    }
    return nanfix_f(r, a, b);
}

static double fop_d(uint8_t op, double a, double b)
{
    double r;
    switch (op) {
    case 0x58: r = a + b; break;
    case 0x59: r = a * b; break;
    case 0x5C: r = a - b; break;
    case 0x5E: r = a / b; break;
    case 0x5D: return a < b ? a : b;
    case 0x5F: return a > b ? a : b;
    case 0x51: r = __builtin_sqrt(b); return nanfix_d(r, b, b);
    default: r = 0; break;
    }
    return nanfix_d(r, a, b);
}

static bool fcmp_pred(unsigned p, double a, double b)
{
    const bool un = a != a || b != b;
    switch (p & 7u) {
    case 0: return a == b;          /* EQ */
    case 1: return a < b;           /* LT */
    case 2: return a <= b;          /* LE */
    case 3: return un;              /* UNORD */
    case 4: return !(a == b);       /* NEQ */
    case 5: return !(a < b);        /* NLT */
    case 6: return !(a <= b);       /* NLE */
    default: return !un;            /* ORD */
    }
}

/* float -> int32/int64; trunc: toward zero, else in the current mode */
static int64_t cvt_to_int(double v, bool trunc, bool is64)
{
    double r = trunc ? __builtin_trunc(v) : __builtin_rint(v);
    if (is64) {
        if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
            return INT64_MIN;
        }
        return (int64_t)r;
    }
    if (!(r >= -2147483648.0 && r < 2147483648.0)) {
        return INT32_MIN;
    }
    return (int32_t)r;
}

static inline int8_t   sat_s8(int32_t v)  { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
static inline uint8_t  sat_u8(int32_t v)  { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
static inline int16_t  sat_s16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline uint16_t sat_u16(int32_t v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }

/* ---- operands -------------------------------------------------------------- */

/* the E operand as an XMM register or `bytes` of memory (rest zero) */
static void load_xe(insn_t *i, xmm_t *v, int bytes)
{
    if (i->mod == 3) {
        *v = i->c->xmm[i->rm];
    } else {
        memset(v, 0, sizeof(*v));
        mrd_n(x86_ea(i), v, (size_t)bytes);
    }
}

/* the E operand as an MMX register or 8 bytes of memory */
static void load_me(insn_t *i, xmm_t *v)
{
    memset(v, 0, sizeof(*v));
    if (i->mod == 3) {
        v->q[0] = i->c->mm[i->rm & 7u];
    } else {
        mrd_n(x86_ea(i), v, 8);
    }
}

/* ---- packed integer ops (MMX: n = 8, SSE2: n = 16) -------------------------- */

static bool int_op(uint8_t op, xmm_t *a, const xmm_t *b, int n)
{
    xmm_t r;
    memset(&r, 0, sizeof(r));
    const int nb = n, nw = n / 2, nd = n / 4, nq = n / 8;
    switch (op) {
    case 0x60: for (int k = 0; k < nb / 2; ++k) { r.b[2*k] = a->b[k]; r.b[2*k+1] = b->b[k]; } break;
    case 0x61: for (int k = 0; k < nw / 2; ++k) { r.w[2*k] = a->w[k]; r.w[2*k+1] = b->w[k]; } break;
    case 0x62: for (int k = 0; k < nd / 2; ++k) { r.d[2*k] = a->d[k]; r.d[2*k+1] = b->d[k]; } break;
    case 0x68: for (int k = 0; k < nb / 2; ++k) { r.b[2*k] = a->b[k + nb/2]; r.b[2*k+1] = b->b[k + nb/2]; } break;
    case 0x69: for (int k = 0; k < nw / 2; ++k) { r.w[2*k] = a->w[k + nw/2]; r.w[2*k+1] = b->w[k + nw/2]; } break;
    case 0x6A: for (int k = 0; k < nd / 2; ++k) { r.d[2*k] = a->d[k + nd/2]; r.d[2*k+1] = b->d[k + nd/2]; } break;
    case 0x6C: if (n != 16) return false; r.q[0] = a->q[0]; r.q[1] = b->q[0]; break;
    case 0x6D: if (n != 16) return false; r.q[0] = a->q[1]; r.q[1] = b->q[1]; break;
    case 0x63: for (int k = 0; k < nw; ++k) { r.sb[k] = sat_s8(a->sw[k]); r.sb[k + nw] = sat_s8(b->sw[k]); } break;
    case 0x67: for (int k = 0; k < nw; ++k) { r.b[k] = sat_u8(a->sw[k]); r.b[k + nw] = sat_u8(b->sw[k]); } break;
    case 0x6B: for (int k = 0; k < nd; ++k) { r.sw[k] = sat_s16(a->sd[k]); r.sw[k + nd] = sat_s16(b->sd[k]); } break;
    case 0x64: for (int k = 0; k < nb; ++k) r.b[k] = a->sb[k] > b->sb[k] ? 0xFF : 0; break;
    case 0x65: for (int k = 0; k < nw; ++k) r.w[k] = a->sw[k] > b->sw[k] ? 0xFFFF : 0; break;
    case 0x66: for (int k = 0; k < nd; ++k) r.d[k] = a->sd[k] > b->sd[k] ? 0xFFFFFFFFu : 0; break;
    case 0x74: for (int k = 0; k < nb; ++k) r.b[k] = a->b[k] == b->b[k] ? 0xFF : 0; break;
    case 0x75: for (int k = 0; k < nw; ++k) r.w[k] = a->w[k] == b->w[k] ? 0xFFFF : 0; break;
    case 0x76: for (int k = 0; k < nd; ++k) r.d[k] = a->d[k] == b->d[k] ? 0xFFFFFFFFu : 0; break;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2:
    case 0xF1: case 0xF2: case 0xF3: {
        uint64_t cnt = b->q[0];
        const int lane = (op & 3u) == 1u ? 16 : (op & 3u) == 2u ? 32 : 64;
        const bool left = op >= 0xF1, arith = op == 0xE1 || op == 0xE2;
        if (cnt >= (uint64_t)lane && !arith) break;
        unsigned s = cnt >= (uint64_t)lane ? (unsigned)lane - 1u : (unsigned)cnt;
        if (lane == 16) for (int k = 0; k < nw; ++k)
            r.w[k] = left ? (uint16_t)(a->w[k] << s) : arith ? (uint16_t)(a->sw[k] >> s) : (uint16_t)(a->w[k] >> s);
        else if (lane == 32) for (int k = 0; k < nd; ++k)
            r.d[k] = left ? a->d[k] << s : arith ? (uint32_t)(a->sd[k] >> s) : a->d[k] >> s;
        else for (int k = 0; k < nq; ++k)
            r.q[k] = left ? a->q[k] << s : a->q[k] >> s;
        break;
    }
    case 0xD4: for (int k = 0; k < nq; ++k) r.q[k] = a->q[k] + b->q[k]; break;
    case 0xFB: for (int k = 0; k < nq; ++k) r.q[k] = a->q[k] - b->q[k]; break;
    case 0xD5: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)((int32_t)a->sw[k] * b->sw[k]); break;
    case 0xE5: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)(((int32_t)a->sw[k] * b->sw[k]) >> 16); break;
    case 0xE4: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)(((uint32_t)a->w[k] * b->w[k]) >> 16); break;
    case 0xD8: for (int k = 0; k < nb; ++k) r.b[k] = sat_u8((int32_t)a->b[k] - b->b[k]); break;
    case 0xD9: for (int k = 0; k < nw; ++k) r.w[k] = sat_u16((int32_t)a->w[k] - b->w[k]); break;
    case 0xDC: for (int k = 0; k < nb; ++k) r.b[k] = sat_u8((int32_t)a->b[k] + b->b[k]); break;
    case 0xDD: for (int k = 0; k < nw; ++k) r.w[k] = sat_u16((int32_t)a->w[k] + b->w[k]); break;
    case 0xE8: for (int k = 0; k < nb; ++k) r.sb[k] = sat_s8((int32_t)a->sb[k] - b->sb[k]); break;
    case 0xE9: for (int k = 0; k < nw; ++k) r.sw[k] = sat_s16((int32_t)a->sw[k] - b->sw[k]); break;
    case 0xEC: for (int k = 0; k < nb; ++k) r.sb[k] = sat_s8((int32_t)a->sb[k] + b->sb[k]); break;
    case 0xED: for (int k = 0; k < nw; ++k) r.sw[k] = sat_s16((int32_t)a->sw[k] + b->sw[k]); break;
    case 0xDA: for (int k = 0; k < nb; ++k) r.b[k] = a->b[k] < b->b[k] ? a->b[k] : b->b[k]; break;
    case 0xDE: for (int k = 0; k < nb; ++k) r.b[k] = a->b[k] > b->b[k] ? a->b[k] : b->b[k]; break;
    case 0xEA: for (int k = 0; k < nw; ++k) r.sw[k] = a->sw[k] < b->sw[k] ? a->sw[k] : b->sw[k]; break;
    case 0xEE: for (int k = 0; k < nw; ++k) r.sw[k] = a->sw[k] > b->sw[k] ? a->sw[k] : b->sw[k]; break;
    case 0xDB: for (int k = 0; k < nq; ++k) r.q[k] = a->q[k] & b->q[k]; break;
    case 0xDF: for (int k = 0; k < nq; ++k) r.q[k] = ~a->q[k] & b->q[k]; break;
    case 0xEB: for (int k = 0; k < nq; ++k) r.q[k] = a->q[k] | b->q[k]; break;
    case 0xEF: for (int k = 0; k < nq; ++k) r.q[k] = a->q[k] ^ b->q[k]; break;
    case 0xE0: for (int k = 0; k < nb; ++k) r.b[k] = (uint8_t)((a->b[k] + b->b[k] + 1u) >> 1); break;
    case 0xE3: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)((a->w[k] + b->w[k] + 1u) >> 1); break;
    case 0xF4: for (int k = 0; k < nq; ++k) r.q[k] = (uint64_t)a->d[2*k] * b->d[2*k]; break;
    case 0xF5: for (int k = 0; k < nd; ++k)
                   r.sd[k] = (int32_t)a->sw[2*k] * b->sw[2*k] + (int32_t)a->sw[2*k+1] * b->sw[2*k+1];
               break;
    case 0xF6: for (int k = 0; k < nq; ++k) {
                   uint32_t s = 0;
                   for (int j = 0; j < 8; ++j) {
                       int dlt = (int)a->b[8*k+j] - (int)b->b[8*k+j];
                       s += (uint32_t)(dlt < 0 ? -dlt : dlt);
                   }
                   r.q[k] = s;
               }
               break;
    case 0xF8: for (int k = 0; k < nb; ++k) r.b[k] = (uint8_t)(a->b[k] - b->b[k]); break;
    case 0xF9: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)(a->w[k] - b->w[k]); break;
    case 0xFA: for (int k = 0; k < nd; ++k) r.d[k] = a->d[k] - b->d[k]; break;
    case 0xFC: for (int k = 0; k < nb; ++k) r.b[k] = (uint8_t)(a->b[k] + b->b[k]); break;
    case 0xFD: for (int k = 0; k < nw; ++k) r.w[k] = (uint16_t)(a->w[k] + b->w[k]); break;
    case 0xFE: for (int k = 0; k < nd; ++k) r.d[k] = a->d[k] + b->d[k]; break;
    default:
        return false;
    }
    if (n == 8) {
        a->q[0] = r.q[0];
    } else {
        *a = r;
    }
    return true;
}

/* 0F 71/72/73: shift a register by an immediate */
static int shift_imm(insn_t *i, uint8_t op, int pfx, unsigned imm)
{
    if (i->mod != 3) {
        return X86_UD(i);
    }
    const unsigned sub = i->reg & 7u;
    const bool mmx = pfx == P_NONE;
    if (!mmx && pfx != P_66) {
        return X86_UD(i);
    }
    xmm_t v, cnt;
    memset(&v, 0, sizeof(v));
    memset(&cnt, 0, sizeof(cnt));
    if (mmx) v.q[0] = i->c->mm[i->rm & 7u]; else v = i->c->xmm[i->rm];
    if (op == 0x73 && (sub == 3u || sub == 7u)) {   /* PSRLDQ / PSLLDQ */
        if (mmx) return X86_UD(i);
        xmm_t r;
        memset(&r, 0, sizeof(r));
        unsigned s = imm > 16u ? 16u : imm;
        for (unsigned k = 0; k < 16u; ++k) {
            if (sub == 3u) { if (k + s < 16u) r.b[k] = v.b[k + s]; }
            else if (k >= s) r.b[k] = v.b[k - s];
        }
        i->c->xmm[i->rm] = r;
        return 0;
    }
    uint8_t eq;
    if (op == 0x71) eq = sub == 2u ? 0xD1 : sub == 4u ? 0xE1 : sub == 6u ? 0xF1 : 0;
    else if (op == 0x72) eq = sub == 2u ? 0xD2 : sub == 4u ? 0xE2 : sub == 6u ? 0xF2 : 0;
    else eq = sub == 2u ? 0xD3 : sub == 6u ? 0xF3 : 0;
    if (eq == 0) {
        return X86_UD(i);
    }
    cnt.q[0] = imm;
    (void)int_op(eq, &v, &cnt, mmx ? 8 : 16);
    if (mmx) i->c->mm[i->rm & 7u] = v.q[0]; else i->c->xmm[i->rm] = v;
    return 0;
}

/* ---- MXCSR <-> the host FP unit ------------------------------------------------ */

#if defined(__aarch64__)
static inline uint64_t rd_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void wr_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static inline uint64_t rd_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void wr_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" :: "r"(v)); }

void x86_mxcsr_apply(x86cpu_t *c)
{
    static const uint64_t rmode[4] = { 0u, 2u, 1u, 3u };   /* RN, RM, RP, RZ */
    uint64_t fpcr = rd_fpcr() & ~((3ULL << 22) | (1ULL << 24));
    fpcr |= rmode[(c->mxcsr >> 13) & 3u] << 22;
    if ((c->mxcsr & 0x8040u) != 0u) {
        fpcr |= 1ULL << 24;     /* FTZ or DAZ -> flush-to-zero */
    }
    wr_fpcr(fpcr);
    uint64_t fpsr = rd_fpsr() & ~0x9FULL;
    const uint32_t m = c->mxcsr;
    if (m & 0x01u) fpsr |= 0x01u;   /* IE -> IOC */
    if (m & 0x04u) fpsr |= 0x02u;   /* ZE -> DZC */
    if (m & 0x08u) fpsr |= 0x04u;   /* OE -> OFC */
    if (m & 0x10u) fpsr |= 0x08u;   /* UE -> UFC */
    if (m & 0x20u) fpsr |= 0x10u;   /* PE -> IXC */
    if (m & 0x02u) fpsr |= 0x80u;   /* DE -> IDC */
    wr_fpsr(fpsr);
    c->mxcsr_host = c->mxcsr;
}

void x86_mxcsr_collect(x86cpu_t *c)
{
    uint64_t fpsr = rd_fpsr();
    uint32_t f = 0;
    if (fpsr & 0x01u) f |= 0x01u;
    if (fpsr & 0x02u) f |= 0x04u;
    if (fpsr & 0x04u) f |= 0x08u;
    if (fpsr & 0x08u) f |= 0x10u;
    if (fpsr & 0x10u) f |= 0x20u;
    if (fpsr & 0x80u) f |= 0x02u;
    c->mxcsr = (c->mxcsr & ~0x3Fu) | f;
}
#else
void x86_mxcsr_apply(x86cpu_t *c) { c->mxcsr_host = c->mxcsr; }
void x86_mxcsr_collect(x86cpu_t *c) { (void)c; }
#endif

/* ---- the dispatcher ------------------------------------------------------------ */

bool x86_sse_has_imm8(uint8_t op);
bool x86_sse_has_imm8(uint8_t op)
{
    return op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 ||
           op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
}

int x86_sse(insn_t *i, uint8_t op)
{
    x86cpu_t *c = i->c;

    if (op == 0x77) {           /* EMMS */
        return 0;
    }
    if (op == 0x38 || op == 0x3A || op < 0x10 || (op >= 0x20 && op < 0x28) ||
        (op >= 0x30 && op < 0x40) || (op >= 0x78 && op <= 0x7B) || op == 0xFF) {
        return X86_UD(i);       /* SSSE3/SSE4, VMX, ... are not reported */
    }
    x86_modrm(i);
    unsigned imm = x86_sse_has_imm8(op) ? fetch8(i) : 0u;
    (void)c;
    return x86_sse_body(i, op, imm);
}

/* The instruction proper, with ModRM and the immediate already decoded
 * into `i` -- also what the block engine (x86_block.c) calls with
 * operands it decoded once. */
int x86_sse_body(insn_t *i, uint8_t op, unsigned imm)
{
    x86cpu_t *c = i->c;
    const int pfx = i->rep == 0xF3 ? P_F3 : i->rep == 0xF2 ? P_F2 : i->p66 ? P_66 : P_NONE;
    xmm_t *G = &c->xmm[i->reg];
    xmm_t s;

    switch (op) {
    case 0x10: /* MOVUPS/MOVUPD/MOVSS/MOVSD load */
        if (pfx == P_F3 || pfx == P_F2) {
            const int n = pfx == P_F3 ? 4 : 8;
            if (i->mod == 3) {
                if (n == 4) G->d[0] = c->xmm[i->rm].d[0]; else G->q[0] = c->xmm[i->rm].q[0];
            } else {
                load_xe(i, &s, n);
                *G = s;
            }
        } else {
            load_xe(i, &s, 16);
            *G = s;
        }
        return 0;
    case 0x11: case 0x29: case 0x2B: case 0xE7: /* stores */
        if (op == 0xE7 && pfx == P_NONE) {          /* MOVNTQ */
            if (i->mod == 3) return X86_UD(i);
            mwr(x86_ea(i), 8, c->mm[i->reg & 7u]);
            return 0;
        }
        if (op == 0x11 && (pfx == P_F3 || pfx == P_F2)) {
            const int n = pfx == P_F3 ? 4 : 8;
            if (i->mod == 3) {
                if (n == 4) c->xmm[i->rm].d[0] = G->d[0]; else c->xmm[i->rm].q[0] = G->q[0];
            } else {
                mwr_n(x86_ea(i), G, (size_t)n);
            }
            return 0;
        }
        if (i->mod == 3) {
            if (op == 0x2B || op == 0xE7) return X86_UD(i);
            c->xmm[i->rm] = *G;
        } else {
            mwr_n(x86_ea(i), G, 16);
        }
        return 0;
    case 0x28: case 0x6F: /* MOVAPS/MOVAPD, MOVQ mm / MOVDQA / MOVDQU */
        if (op == 0x6F && pfx == P_NONE) {
            load_me(i, &s);
            c->mm[i->reg & 7u] = s.q[0];
            return 0;
        }
        if (op == 0x6F && pfx == P_F2) return X86_UD(i);
        load_xe(i, &s, 16);
        *G = s;
        return 0;
    case 0x7F:
        if (pfx == P_NONE) {
            if (i->mod == 3) c->mm[i->rm & 7u] = c->mm[i->reg & 7u];
            else mwr(x86_ea(i), 8, c->mm[i->reg & 7u]);
            return 0;
        }
        if (pfx == P_F2) return X86_UD(i);
        if (i->mod == 3) c->xmm[i->rm] = *G; else mwr_n(x86_ea(i), G, 16);
        return 0;
    case 0xF0:  /* LDDQU */
        if (pfx != P_F2 || i->mod == 3) return X86_UD(i);
        load_xe(i, &s, 16);
        *G = s;
        return 0;
    case 0x12: case 0x13: case 0x16: case 0x17: {
        const bool high = op >= 0x16;
        if (op == 0x13 || op == 0x17) {     /* store a half */
            if (i->mod == 3 || pfx >= P_F3) return X86_UD(i);
            mwr(x86_ea(i), 8, G->q[high ? 1 : 0]);
            return 0;
        }
        if (pfx == P_F2) {                  /* MOVDDUP */
            if (high) return X86_UD(i);
            load_xe(i, &s, 8);
            G->q[0] = s.q[0];
            G->q[1] = s.q[0];
            return 0;
        }
        if (pfx == P_F3) {                  /* MOVSLDUP / MOVSHDUP */
            load_xe(i, &s, 16);
            int o = high ? 1 : 0;
            xmm_t r;
            r.d[0] = r.d[1] = s.d[o];
            r.d[2] = r.d[3] = s.d[2 + o];
            *G = r;
            return 0;
        }
        if (i->mod == 3) {
            if (pfx == P_66) return X86_UD(i);
            if (high) G->q[1] = c->xmm[i->rm].q[0];      /* MOVLHPS */
            else G->q[0] = c->xmm[i->rm].q[1];           /* MOVHLPS */
            return 0;
        }
        G->q[high ? 1 : 0] = mrd(x86_ea(i), 8);
        return 0;
    }
    case 0x14: case 0x15: { /* UNPCKL/HPS, UNPCKL/HPD */
        load_xe(i, &s, 16);
        xmm_t a = *G, r;
        if (pfx == P_66) {
            r.q[0] = op == 0x14 ? a.q[0] : a.q[1];
            r.q[1] = op == 0x14 ? s.q[0] : s.q[1];
        } else if (pfx == P_NONE) {
            int o = op == 0x14 ? 0 : 2;
            r.d[0] = a.d[o]; r.d[1] = s.d[o]; r.d[2] = a.d[o + 1]; r.d[3] = s.d[o + 1];
        } else {
            return X86_UD(i);
        }
        *G = r;
        return 0;
    }
    case 0x2A: { /* CVTPI2PS/PD, CVTSI2SS/SD */
        if (pfx == P_F3 || pfx == P_F2) {
            int64_t v;
            if (i->mod == 3) {
                v = REX_W(i) ? (int64_t)c->r[i->rm] : (int32_t)c->r[i->rm];
            } else {
                uint64_t a = x86_ea(i);
                v = REX_W(i) ? (int64_t)mrd(a, 8) : (int32_t)mrd(a, 4);
            }
            if (pfx == P_F3) G->f[0] = (float)v; else G->fd[0] = (double)v;
            return 0;
        }
        load_me(i, &s);
        if (pfx == P_NONE) { G->f[0] = (float)s.sd[0]; G->f[1] = (float)s.sd[1]; }
        else { G->fd[0] = (double)s.sd[0]; G->fd[1] = (double)s.sd[1]; }
        return 0;
    }
    case 0x2C: case 0x2D: { /* CVT(T)SS2SI/SD2SI, CVT(T)PS2PI/PD2PI */
        const bool trunc = op == 0x2C;
        if (pfx == P_F3 || pfx == P_F2) {
            load_xe(i, &s, pfx == P_F3 ? 4 : 8);
            double v = pfx == P_F3 ? (double)s.f[0] : s.fd[0];
            int64_t r = cvt_to_int(v, trunc, REX_W(i));
            if (REX_W(i)) c->r[i->reg] = (uint64_t)r;
            else c->r[i->reg] = (uint32_t)r;
            return 0;
        }
        load_xe(i, &s, pfx == P_NONE ? 8 : 16);
        xmm_t r;
        memset(&r, 0, sizeof(r));
        for (int k = 0; k < 2; ++k) {
            double v = pfx == P_NONE ? (double)s.f[k] : s.fd[k];
            r.sd[k] = (int32_t)cvt_to_int(v, trunc, false);
        }
        c->mm[i->reg & 7u] = r.q[0];
        return 0;
    }
    case 0x2E: case 0x2F: { /* (U)COMISS/SD */
        if (pfx >= P_F3) return X86_UD(i);
        load_xe(i, &s, pfx == P_NONE ? 4 : 8);
        double a = pfx == P_NONE ? (double)G->f[0] : G->fd[0];
        double b = pfx == P_NONE ? (double)s.f[0] : s.fd[0];
        uint64_t f;
        if (a != a || b != b) f = F_ZF | F_PF | F_CF;
        else if (a < b) f = F_CF;
        else if (a == b) f = F_ZF;
        else f = 0;
        x86_set_flags(c, f);
        return 0;
    }
    case 0x50: { /* MOVMSKPS/PD */
        if (i->mod != 3 || pfx >= P_F3) return X86_UD(i);
        xmm_t *v = &c->xmm[i->rm];
        uint64_t m = 0;
        if (pfx == P_NONE) for (int k = 0; k < 4; ++k) m |= (uint64_t)(v->d[k] >> 31) << k;
        else for (int k = 0; k < 2; ++k) m |= (v->q[k] >> 63) << k;
        c->r[i->reg] = m;
        return 0;
    }
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        if (pfx == P_F3) {
            load_xe(i, &s, 4);
            G->f[0] = fop_f(op, G->f[0], s.f[0]);
        } else if (pfx == P_F2) {
            if (op == 0x52 || op == 0x53) return X86_UD(i);
            load_xe(i, &s, 8);
            G->fd[0] = fop_d(op, G->fd[0], s.fd[0]);
        } else if (pfx == P_NONE) {
            load_xe(i, &s, 16);
            for (int k = 0; k < 4; ++k) G->f[k] = fop_f(op, G->f[k], s.f[k]);
        } else {
            if (op == 0x52 || op == 0x53) return X86_UD(i);
            load_xe(i, &s, 16);
            for (int k = 0; k < 2; ++k) G->fd[k] = fop_d(op, G->fd[k], s.fd[k]);
        }
        return 0;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: { /* AND, ANDN, OR, XOR */
        if (pfx >= P_F3) return X86_UD(i);
        load_xe(i, &s, 16);
        for (int k = 0; k < 2; ++k) {
            uint64_t a = G->q[k], b = s.q[k];
            G->q[k] = op == 0x54 ? a & b : op == 0x55 ? ~a & b : op == 0x56 ? a | b : a ^ b;
        }
        return 0;
    }
    case 0x7C: case 0x7D: case 0xD0: { /* SSE3: HADDPx, HSUBPx, ADDSUBPx */
        if (pfx != P_66 && pfx != P_F2) return X86_UD(i);
        load_xe(i, &s, 16);
        const xmm_t a = *G;
        xmm_t r;
        if (pfx == P_66) {          /* double */
            if (op == 0xD0) {
                r.fd[0] = nanfix_d(a.fd[0] - s.fd[0], a.fd[0], s.fd[0]);
                r.fd[1] = nanfix_d(a.fd[1] + s.fd[1], a.fd[1], s.fd[1]);
            } else {
                const bool sub = op == 0x7D;
                r.fd[0] = nanfix_d(sub ? a.fd[0] - a.fd[1] : a.fd[0] + a.fd[1], a.fd[0], a.fd[1]);
                r.fd[1] = nanfix_d(sub ? s.fd[0] - s.fd[1] : s.fd[0] + s.fd[1], s.fd[0], s.fd[1]);
            }
        } else {                    /* single */
            if (op == 0xD0) {
                for (int k = 0; k < 4; ++k) {
                    float x = (k & 1) ? a.f[k] + s.f[k] : a.f[k] - s.f[k];
                    r.f[k] = nanfix_f(x, a.f[k], s.f[k]);
                }
            } else {
                const bool sub = op == 0x7D;
                for (int k = 0; k < 2; ++k) {
                    float x0 = a.f[2 * k], x1 = a.f[2 * k + 1];
                    float y0 = s.f[2 * k], y1 = s.f[2 * k + 1];
                    r.f[k] = nanfix_f(sub ? x0 - x1 : x0 + x1, x0, x1);
                    r.f[2 + k] = nanfix_f(sub ? y0 - y1 : y0 + y1, y0, y1);
                }
            }
        }
        *G = r;
        return 0;
    }
    case 0x5A: { /* CVTPS2PD, CVTPD2PS, CVTSS2SD, CVTSD2SS */
        xmm_t r = *G;
        switch (pfx) {
        case P_NONE: load_xe(i, &s, 8); r.fd[0] = (double)s.f[0]; r.fd[1] = (double)s.f[1]; break;
        case P_66: load_xe(i, &s, 16); r.f[0] = (float)s.fd[0]; r.f[1] = (float)s.fd[1]; r.q[1] = 0; break;
        case P_F3: load_xe(i, &s, 4); r.fd[0] = (double)s.f[0]; break;
        default: load_xe(i, &s, 8); r.f[0] = (float)s.fd[0]; break;
        }
        *G = r;
        return 0;
    }
    case 0x5B: { /* CVTDQ2PS, CVTPS2DQ, CVTTPS2DQ */
        load_xe(i, &s, 16);
        xmm_t r;
        for (int k = 0; k < 4; ++k) {
            if (pfx == P_NONE) r.f[k] = (float)s.sd[k];
            else if (pfx == P_66) r.sd[k] = (int32_t)cvt_to_int(s.f[k], false, false);
            else if (pfx == P_F3) r.sd[k] = (int32_t)cvt_to_int(s.f[k], true, false);
            else return X86_UD(i);
        }
        *G = r;
        return 0;
    }
    case 0xE6: { /* CVTTPD2DQ, CVTDQ2PD, CVTPD2DQ */
        xmm_t r;
        memset(&r, 0, sizeof(r));
        if (pfx == P_F3) {
            load_xe(i, &s, 8);
            r.fd[0] = (double)s.sd[0];
            r.fd[1] = (double)s.sd[1];
        } else if (pfx == P_66 || pfx == P_F2) {
            load_xe(i, &s, 16);
            r.sd[0] = (int32_t)cvt_to_int(s.fd[0], pfx == P_66, false);
            r.sd[1] = (int32_t)cvt_to_int(s.fd[1], pfx == P_66, false);
        } else {
            return X86_UD(i);
        }
        *G = r;
        return 0;
    }
    case 0x6E: { /* MOVD/MOVQ mm/xmm, r/m */
        uint64_t v;
        if (i->mod == 3) v = REX_W(i) ? c->r[i->rm] : (uint32_t)c->r[i->rm];
        else v = mrd(x86_ea(i), REX_W(i) ? 8 : 4);
        if (pfx == P_NONE) {
            c->mm[i->reg & 7u] = v;
        } else if (pfx == P_66) {
            G->q[0] = v;
            G->q[1] = 0;
        } else {
            return X86_UD(i);
        }
        return 0;
    }
    case 0x7E: {
        if (pfx == P_F3) {     /* MOVQ xmm, xmm/m64 */
            load_xe(i, &s, 8);
            G->q[0] = s.q[0];
            G->q[1] = 0;
            return 0;
        }
        uint64_t v;
        if (pfx == P_NONE) v = c->mm[i->reg & 7u];
        else if (pfx == P_66) v = G->q[0];
        else return X86_UD(i);
        if (!REX_W(i)) v = (uint32_t)v;
        if (i->mod == 3) c->r[i->rm] = v;
        else mwr(x86_ea(i), REX_W(i) ? 8 : 4, v);
        return 0;
    }
    case 0xD6: { /* MOVQ xmm/m64, xmm */
        if (pfx != P_66) return X86_UD(i);
        if (i->mod == 3) {
            c->xmm[i->rm].q[0] = G->q[0];
            c->xmm[i->rm].q[1] = 0;
        } else {
            mwr(x86_ea(i), 8, G->q[0]);
        }
        return 0;
    }
    case 0x70: { /* PSHUFW, PSHUFD, PSHUFHW, PSHUFLW */
        xmm_t r;
        if (pfx == P_NONE) {
            load_me(i, &s);
            memset(&r, 0, sizeof(r));
            for (int k = 0; k < 4; ++k) r.w[k] = s.w[(imm >> (2 * k)) & 3u];
            c->mm[i->reg & 7u] = r.q[0];
            return 0;
        }
        load_xe(i, &s, 16);
        r = s;
        if (pfx == P_66) {
            for (int k = 0; k < 4; ++k) r.d[k] = s.d[(imm >> (2 * k)) & 3u];
        } else if (pfx == P_F3) {
            for (int k = 0; k < 4; ++k) r.w[4 + k] = s.w[4 + ((imm >> (2 * k)) & 3u)];
        } else {
            for (int k = 0; k < 4; ++k) r.w[k] = s.w[(imm >> (2 * k)) & 3u];
        }
        *G = r;
        return 0;
    }
    case 0x71: case 0x72: case 0x73:
        return shift_imm(i, op, pfx, imm);
    case 0xC2: { /* CMPPS/PD/SS/SD */
        if (pfx == P_F3) {
            load_xe(i, &s, 4);
            G->d[0] = fcmp_pred(imm, G->f[0], s.f[0]) ? 0xFFFFFFFFu : 0;
        } else if (pfx == P_F2) {
            load_xe(i, &s, 8);
            G->q[0] = fcmp_pred(imm, G->fd[0], s.fd[0]) ? ~0ULL : 0;
        } else if (pfx == P_NONE) {
            load_xe(i, &s, 16);
            for (int k = 0; k < 4; ++k) G->d[k] = fcmp_pred(imm, G->f[k], s.f[k]) ? 0xFFFFFFFFu : 0;
        } else {
            load_xe(i, &s, 16);
            for (int k = 0; k < 2; ++k) G->q[k] = fcmp_pred(imm, G->fd[k], s.fd[k]) ? ~0ULL : 0;
        }
        return 0;
    }
    case 0xC3: { /* MOVNTI */
        if (i->mod == 3 || pfx != P_NONE) return X86_UD(i);
        int n = REX_W(i) ? 8 : 4;
        mwr(x86_ea(i), n, n == 8 ? c->r[i->reg] : (uint32_t)c->r[i->reg]);
        return 0;
    }
    case 0xC4: { /* PINSRW */
        uint64_t v = i->mod == 3 ? c->r[i->rm] : mrd(x86_ea(i), 2);
        if (pfx == P_NONE) {
            xmm_t m;
            m.q[0] = c->mm[i->reg & 7u];
            m.w[imm & 3u] = (uint16_t)v;
            c->mm[i->reg & 7u] = m.q[0];
        } else if (pfx == P_66) {
            G->w[imm & 7u] = (uint16_t)v;
        } else {
            return X86_UD(i);
        }
        return 0;
    }
    case 0xC5: { /* PEXTRW */
        if (i->mod != 3) return X86_UD(i);
        if (pfx == P_NONE) {
            xmm_t m;
            m.q[0] = c->mm[i->rm & 7u];
            c->r[i->reg] = m.w[imm & 3u];
        } else if (pfx == P_66) {
            c->r[i->reg] = c->xmm[i->rm].w[imm & 7u];
        } else {
            return X86_UD(i);
        }
        return 0;
    }
    case 0xC6: { /* SHUFPS/SHUFPD */
        load_xe(i, &s, 16);
        xmm_t a = *G, r;
        if (pfx == P_NONE) {
            r.d[0] = a.d[imm & 3u];
            r.d[1] = a.d[(imm >> 2) & 3u];
            r.d[2] = s.d[(imm >> 4) & 3u];
            r.d[3] = s.d[(imm >> 6) & 3u];
        } else if (pfx == P_66) {
            r.q[0] = a.q[imm & 1u];
            r.q[1] = s.q[(imm >> 1) & 1u];
        } else {
            return X86_UD(i);
        }
        *G = r;
        return 0;
    }
    case 0xD7: { /* PMOVMSKB */
        if (i->mod != 3) return X86_UD(i);
        uint64_t m = 0;
        if (pfx == P_NONE) {
            xmm_t v;
            v.q[0] = c->mm[i->rm & 7u];
            for (int k = 0; k < 8; ++k) m |= (uint64_t)(v.b[k] >> 7) << k;
        } else if (pfx == P_66) {
            for (int k = 0; k < 16; ++k) m |= (uint64_t)(c->xmm[i->rm].b[k] >> 7) << k;
        } else {
            return X86_UD(i);
        }
        c->r[i->reg] = m;
        return 0;
    }
    case 0xF7: { /* MASKMOVQ / MASKMOVDQU */
        if (i->mod != 3) return X86_UD(i);
        xmm_t v, m;
        int n;
        if (pfx == P_NONE) {
            v.q[0] = c->mm[i->reg & 7u];
            m.q[0] = c->mm[i->rm & 7u];
            n = 8;
        } else if (pfx == P_66) {
            v = *G;
            m = c->xmm[i->rm];
            n = 16;
        } else {
            return X86_UD(i);
        }
        uint64_t a = i->adsz32 ? (uint32_t)c->r[RDI] : c->r[RDI];
        if (i->seg == 4) a += c->fs_base;
        if (i->seg == 5) a += c->gs_base;
        for (int k = 0; k < n; ++k) {
            if (m.b[k] & 0x80u) mwr(a + (uint64_t)k, 1, v.b[k]);
        }
        return 0;
    }
    default:
        break;
    }

    /* packed integer ops */
    if ((op >= 0x60 && op <= 0x6D) || (op >= 0x74 && op <= 0x76) ||
        (op >= 0xD1 && op <= 0xFE)) {
        xmm_t a;
        if (pfx == P_NONE) {
            load_me(i, &s);
            memset(&a, 0, sizeof(a));
            a.q[0] = c->mm[i->reg & 7u];
            if (!int_op(op, &a, &s, 8)) return X86_UD(i);
            c->mm[i->reg & 7u] = a.q[0];
            return 0;
        }
        if (pfx != P_66) {
            return X86_UD(i);
        }
        load_xe(i, &s, 16);
        a = *G;
        if (!int_op(op, &a, &s, 16)) return X86_UD(i);
        *G = a;
        return 0;
    }
    return X86_UD(i);
}
