/*
 * The AArch64 guest's block engine: decode once, run many times.
 *
 * The same design as the x86 guest's (guest/x86_64/x86_block.c, which has
 * the longer explanation): guest code is translated into traces of uops --
 * a handler plus predecoded operands -- that follow B/BL and one side of
 * each conditional branch, run direct-threaded (each handler tail-calls
 * the next), and live in a per-thread cache checked against guest memory
 * on entry unless the code is known read-only. Instructions without a
 * handler of their own run through the interpreter's a64_step().
 *
 * Register numbers are mapped at decode time so handlers never test for
 * register 31: a read of XZR is x[31] (always 0), SP is x[32], and a write
 * to XZR goes to x[33].
 *
 * XL_NOBLOCKS=1 selects the plain interpreter.
 */

#include "a64_ops.h"
#include "host.h"

#define AB_MAX_UOPS   64
#define AB_MAX_SEGS   8
#define AB_TABLE_BITS 15
#define AB_TABLE_SIZE (1u << AB_TABLE_BITS)
#define AB_ARENA_SIZE (8u * 1024u * 1024u)
#define AB_EXIT       (-1)

typedef struct au au_t;
typedef int (*afn)(a64cpu_t *c, const au_t *u);

struct au {
    afn fn;
    uint64_t pc;
    uint64_t imm;
    uint64_t imm2;
    uint8_t rd, rn, rm, ra;
    uint8_t sf;         /* 64-bit operation */
    uint8_t op;         /* operation / addressing mode / branch kind */
    uint8_t amt;        /* shift amount / extend option / bit number */
    uint8_t cond;       /* condition / shift type / extend shift */
    uint8_t s;          /* flag-setting / sign extension / exit sense */
    uint8_t sz;         /* access size, bytes */
};

typedef struct {
    uint64_t start;
    uint32_t len;
    uint32_t off;
} ab_seg_t;

typedef struct ab_block {
    uint64_t pc;
    uint32_t nuops;
    uint32_t nsegs;
    uint32_t gen;
    uint32_t nbytes;
    const uint8_t *code;
    uint64_t ro_epoch;
    uint64_t seen_epoch;
    uint64_t link_pc[2];
    struct ab_block *link[2];
    uint32_t link_next;
    ab_seg_t seg[AB_MAX_SEGS];
    au_t uops[];
} ab_block_t;

typedef struct {
    ab_block_t *way[2];
} ab_set_t;

typedef struct {
    ab_set_t table[AB_TABLE_SIZE];
    uint8_t *arena;
    size_t used;
    uint32_t gen;
} ab_cache_t;

static int g_ab_disabled = -1;

#define NEXT __attribute__((musttail)) return u[1].fn(c, u + 1)
#define X(n) c->x[(n)]
#define RES(v) (u->sf ? (uint64_t)(v) : (uint64_t)(uint32_t)(v))

static inline gthread_t *thread_of(a64cpu_t *c)
{
    return (gthread_t *)(void *)((uint8_t *)c - __builtin_offsetof(gthread_t, cpu));
}

/* ---- handlers ------------------------------------------------------------------ */

static int h_end(a64cpu_t *c, const au_t *u)
{
    c->pc = u->imm;
    return AB_EXIT;
}

static int h_nop(a64cpu_t *c, const au_t *u) { NEXT; }

static int h_movi(a64cpu_t *c, const au_t *u)          /* MOVZ/MOVN/ADR/ADRP */
{
    X(u->rd) = u->imm;
    NEXT;
}

static int h_movk(a64cpu_t *c, const au_t *u)
{
    X(u->rd) = RES((X(u->rd) & ~u->imm2) | u->imm);
    NEXT;
}

static int h_addi(a64cpu_t *c, const au_t *u)
{
    X(u->rd) = RES(X(u->rn) + u->imm);
    NEXT;
}

static int h_subi(a64cpu_t *c, const au_t *u)
{
    X(u->rd) = RES(X(u->rn) - u->imm);
    NEXT;
}

static int h_addsubi_s(a64cpu_t *c, const au_t *u)    /* ADDS/SUBS imm (CMP, CMN) */
{
    const uint64_t r = u->op ? add_carry(c, X(u->rn), ~u->imm, 1, u->sf, true)
                             : add_carry(c, X(u->rn), u->imm, 0, u->sf, true);
    X(u->rd) = r;
    NEXT;
}

static int h_andi(a64cpu_t *c, const au_t *u) { X(u->rd) = RES(X(u->rn) & u->imm); NEXT; }
static int h_orri(a64cpu_t *c, const au_t *u) { X(u->rd) = RES(X(u->rn) | u->imm); NEXT; }
static int h_eori(a64cpu_t *c, const au_t *u) { X(u->rd) = RES(X(u->rn) ^ u->imm); NEXT; }

static int h_andsi(a64cpu_t *c, const au_t *u)
{
    const uint64_t r = RES(X(u->rn) & u->imm);
    logic_flags(c, r, u->sf);
    X(u->rd) = r;
    NEXT;
}

static int h_addsub_sh(a64cpu_t *c, const au_t *u)    /* ADD/SUB(S) shifted register */
{
    const uint64_t b = shift_reg(X(u->rm), u->cond, u->amt, u->sf);
    uint64_t r;
    if (u->s) {
        r = u->op ? add_carry(c, X(u->rn), ~b, 1, u->sf, true) : add_carry(c, X(u->rn), b, 0, u->sf, true);
    } else {
        r = RES(u->op ? X(u->rn) - b : X(u->rn) + b);
    }
    X(u->rd) = r;
    NEXT;
}

static int h_addsub_ext(a64cpu_t *c, const au_t *u)   /* ADD/SUB(S) extended register */
{
    const uint64_t b = extend_reg(X(u->rm), u->amt, u->cond);
    uint64_t r;
    if (u->s) {
        r = u->op ? add_carry(c, X(u->rn), ~b, 1, u->sf, true) : add_carry(c, X(u->rn), b, 0, u->sf, true);
    } else {
        r = RES(u->op ? X(u->rn) - b : X(u->rn) + b);
    }
    X(u->rd) = r;
    NEXT;
}

static int h_logic_sh(a64cpu_t *c, const au_t *u)     /* AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS */
{
    uint64_t b = shift_reg(X(u->rm), u->cond, u->amt, u->sf);
    if (u->s & 2u) b = ~b;
    const uint64_t a = X(u->rn);
    uint64_t r;
    switch (u->op) {
    case 0: r = a & b; break;
    case 1: r = a | b; break;
    case 2: r = a ^ b; break;
    default: r = a & b; break;
    }
    r = RES(r);
    if (u->op == 3u) logic_flags(c, r, u->sf);
    X(u->rd) = r;
    NEXT;
}

static int h_movr(a64cpu_t *c, const au_t *u)         /* MOV (ORR Rd, ZR, Rm) */
{
    X(u->rd) = RES(X(u->rm));
    NEXT;
}

static int h_bfm(a64cpu_t *c, const au_t *u)          /* SBFM, BFM, UBFM */
{
    const unsigned immr = (unsigned)u->imm, imms = (unsigned)u->imm2;
    const unsigned bits = u->sf ? 64u : 32u;
    const uint64_t src = X(u->rn);
    uint64_t r;
    if (imms >= immr) {
        const unsigned width = imms - immr + 1u;
        const uint64_t field = (src >> immr) & ones(width);
        if (u->op == 0u) r = (uint64_t)sext(field, width);
        else if (u->op == 2u) r = field;
        else r = (X(u->rd) & ~ones(width)) | field;
    } else {
        const unsigned width = imms + 1u, sh = bits - immr;
        const uint64_t field = src & ones(width);
        if (u->op == 0u) r = (uint64_t)sext(field, width) << sh;
        else if (u->op == 2u) r = field << sh;
        else r = (X(u->rd) & ~(ones(width) << sh)) | (field << sh);
    }
    X(u->ra) = RES(r);      /* ra: the write mapping of rd */
    NEXT;
}

static int h_csel(a64cpu_t *c, const au_t *u)
{
    uint64_t r;
    if (a64_cond(c->nzcv, u->cond)) {
        r = X(u->rn);
    } else {
        r = X(u->rm);
        switch (u->op) {
        case 1: r += 1u; break;
        case 2: r = ~r; break;
        case 3: r = (uint64_t)0 - r; break;
        default: break;
        }
    }
    X(u->rd) = RES(r);
    NEXT;
}

static int h_ccmp(a64cpu_t *c, const au_t *u)
{
    if (a64_cond(c->nzcv, u->cond)) {
        const uint64_t b = u->s ? u->imm : X(u->rm);
        if (u->op) (void)add_carry(c, X(u->rn), ~b, 1, u->sf, true);
        else (void)add_carry(c, X(u->rn), b, 0, u->sf, true);
    } else {
        c->nzcv = (uint32_t)u->imm2;
    }
    NEXT;
}

static int h_madd(a64cpu_t *c, const au_t *u)
{
    const uint64_t p = X(u->rn) * X(u->rm);
    X(u->rd) = RES(u->op ? X(u->ra) - p : X(u->ra) + p);
    NEXT;
}

static int h_mull(a64cpu_t *c, const au_t *u)         /* SMADDL/UMADDL (+SUBL), SMULH/UMULH */
{
    const uint64_t a = X(u->rn), b = X(u->rm);
    uint64_t r;
    switch (u->op) {
    case 0: r = X(u->ra) + (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b); break;
    case 1: r = X(u->ra) - (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b); break;
    case 2: r = X(u->ra) + (uint64_t)(uint32_t)a * (uint32_t)b; break;
    case 3: r = X(u->ra) - (uint64_t)(uint32_t)a * (uint32_t)b; break;
    case 4: r = (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); break;
    default: r = (uint64_t)(((unsigned __int128)a * b) >> 64); break;
    }
    X(u->rd) = r;
    NEXT;
}

static int h_dp2(a64cpu_t *c, const au_t *u)          /* UDIV SDIV LSLV LSRV ASRV RORV */
{
    uint64_t a = X(u->rn), b = X(u->rm), r;
    const unsigned bits = u->sf ? 64u : 32u;
    if (!u->sf) { a = (uint32_t)a; b = (uint32_t)b; }
    switch (u->op) {
    case 2: r = b == 0u ? 0u : a / b; break;
    case 3:
        if (b == 0u) r = 0;
        else if (u->sf) r = ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? a : (uint64_t)((int64_t)a / (int64_t)b);
        else r = ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b);
        break;
    case 8: r = a << (b & (bits - 1u)); break;
    case 9: r = a >> (b & (bits - 1u)); break;
    case 10: r = shift_reg(a, 2, (unsigned)(b & (bits - 1u)), u->sf); break;
    default: r = shift_reg(a, 3, (unsigned)(b & (bits - 1u)), u->sf); break;
    }
    X(u->rd) = RES(r);
    NEXT;
}

static int h_dp1(a64cpu_t *c, const au_t *u)          /* RBIT REV16 REV32/REV REV CLZ */
{
    uint64_t a = X(u->rn), r;
    if (!u->sf) a = (uint32_t)a;
    switch (u->op) {
    case 0:
        r = 0;
        for (unsigned k = 0; k < (u->sf ? 64u : 32u); ++k) r |= ((a >> k) & 1u) << ((u->sf ? 63u : 31u) - k);
        break;
    case 1: r = ((a & 0x00FF00FF00FF00FFULL) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFULL); break;
    case 2:
        if (u->sf) r = ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32) | __builtin_bswap32((uint32_t)a);
        else r = __builtin_bswap32((uint32_t)a);
        break;
    case 3: r = __builtin_bswap64(a); break;
    default:
        r = a == 0u ? (u->sf ? 64u : 32u) : (uint64_t)(u->sf ? __builtin_clzll(a) : __builtin_clz((uint32_t)a));
        break;
    }
    X(u->rd) = RES(r);
    NEXT;
}

static int h_mrs_tpidr(a64cpu_t *c, const au_t *u) { X(u->rd) = c->tpidr; NEXT; }

static int h_extr(a64cpu_t *c, const au_t *u)         /* EXTR (ROR immediate) */
{
    const unsigned lsb = u->amt;
    const uint64_t hi = X(u->rn), lo = X(u->rm);
    uint64_t r;
    if (u->sf) {
        r = lsb == 0u ? lo : (lo >> lsb) | (hi << (64u - lsb));
    } else {
        const uint64_t cat = ((hi & 0xFFFFFFFFu) << 32) | (lo & 0xFFFFFFFFu);
        r = (uint32_t)(cat >> lsb);
    }
    X(u->rd) = r;
    NEXT;
}

/* FADD FSUB FMUL FDIV FMAX FMIN FMAXNM FMINNM FNMUL, scalar */
static int h_fp2(a64cpu_t *c, const au_t *u)
{
    vreg_t *d = &c->v[u->rd];
    if (u->sf) {
        const double r = a64_fop2_d(c, u->op, c->v[u->rn].fd[0], c->v[u->rm].fd[0]);
        d->d[1] = 0;
        d->fd[0] = r;
    } else {
        const float r = a64_fop2_f(c, u->op, c->v[u->rn].f[0], c->v[u->rm].f[0]);
        d->d[1] = 0;
        d->d[0] = 0;
        d->f[0] = r;
    }
    NEXT;
}

/* Any other FP/Advanced SIMD instruction, and the structure loads and
 * stores: the interpreter's implementation, called directly. */
static __attribute__((noinline)) int simd_exec(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    return u->s ? a64_simd_ldst_multi(thread_of(c), (uint32_t)u->imm)
                : a64_simd_dp(thread_of(c), (uint32_t)u->imm);
}

static int h_simd(a64cpu_t *c, const au_t *u)
{
    const int ev = simd_exec(c, u);
    if (ev != 0) {
        return ev;
    }
    NEXT;
}
static int h_dmb(a64cpu_t *c, const au_t *u) { __atomic_thread_fence(__ATOMIC_SEQ_CST); NEXT; }

/* ---- loads and stores ----
 * op: 0 base+imm, 1 pre-index, 2 post-index, 3 base + extended register
 * (amt = extend option, cond = shift); s: 0 zero-extend, 1 sign-extend to
 * 64, 2 sign-extend to 32. rd: data register (mapped), rn: base (SP
 * mapping). */

static inline uint64_t ls_addr(a64cpu_t *c, const au_t *u, uint64_t *wb)
{
    const uint64_t base = X(u->rn);
    switch (u->op) {
    case 0: return base + u->imm;
    case 1: *wb = base + u->imm; return *wb;
    case 2: *wb = base + u->imm; return base;
    default: return base + extend_reg(X(u->rm), u->amt, u->cond);
    }
}

static int h_ld(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    uint64_t v = ld_n(a, u->sz);
    if (u->s == 1u) v = (uint64_t)sext(v, 8u * u->sz);
    else if (u->s == 2u) v = (uint32_t)sext(v, 8u * u->sz);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    X(u->rd) = v;
    NEXT;
}

static int h_st(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    st_n(a, u->sz, X(u->rd));
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

static int h_ldp(a64cpu_t *c, const au_t *u)          /* rd, ra: the two registers */
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    uint64_t v0 = ld_n(a, u->sz), v1 = ld_n(a + u->sz, u->sz);
    if (u->s) { v0 = (uint64_t)(int64_t)(int32_t)v0; v1 = (uint64_t)(int64_t)(int32_t)v1; }
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    X(u->rd) = v0;
    X(u->ra) = v1;
    NEXT;
}

static int h_stp(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    const uint64_t v0 = X(u->rd), v1 = X(u->ra);
    st_n(a, u->sz, v0);
    st_n(a + u->sz, u->sz, v1);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

/* SIMD&FP registers: rd/ra are V register numbers. The loads go through
 * a helper so a fault leaves the register untouched. */
static __attribute__((noinline)) void vload(vreg_t *dst, uint64_t a, unsigned sz)
{
    vreg_t r;
    memset(&r, 0, sizeof(r));
    __builtin_memcpy(&r, (const void *)(uintptr_t)a, sz);
    *dst = r;
}

static __attribute__((noinline)) void vload2(vreg_t *d0, vreg_t *d1, uint64_t a, unsigned sz)
{
    vreg_t r0, r1;
    memset(&r0, 0, sizeof(r0));
    memset(&r1, 0, sizeof(r1));
    __builtin_memcpy(&r0, (const void *)(uintptr_t)a, sz);
    __builtin_memcpy(&r1, (const void *)(uintptr_t)(a + sz), sz);
    *d0 = r0;
    *d1 = r1;
}

static int h_vld(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    vload(&c->v[u->rd], a, u->sz);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

static int h_vst(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    __builtin_memcpy((void *)(uintptr_t)a, &c->v[u->rd], u->sz);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

static int h_vldp(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    vload2(&c->v[u->rd], &c->v[u->ra], a, u->sz);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

static int h_vstp(a64cpu_t *c, const au_t *u)
{
    c->pc = u->pc;
    uint64_t wb = 0;
    const uint64_t a = ls_addr(c, u, &wb);
    __builtin_memcpy((void *)(uintptr_t)a, &c->v[u->rd], u->sz);
    __builtin_memcpy((void *)(uintptr_t)(a + u->sz), &c->v[u->ra], u->sz);
    if (u->op == 1u || u->op == 2u) X(u->rn) = wb;
    NEXT;
}

/* ---- branches ----
 * h_side: a conditional branch inside a trace. op: 0 B.cond, 1 CBZ, 2 CBNZ,
 * 3 TBZ, 4 TBNZ; the block is left for imm when "taken" differs from s
 * (s = 0: leave when taken; s = 1: the taken side was followed, leave
 * when not taken). */

static inline bool taken(const a64cpu_t *c, const au_t *u)
{
    switch (u->op) {
    case 0: return a64_cond(c->nzcv, u->cond);
    case 1: return (u->sf ? c->x[u->rd] : (uint32_t)c->x[u->rd]) == 0u;
    case 2: return (u->sf ? c->x[u->rd] : (uint32_t)c->x[u->rd]) != 0u;
    case 3: return ((c->x[u->rd] >> u->amt) & 1u) == 0u;
    default: return ((c->x[u->rd] >> u->amt) & 1u) != 0u;
    }
}

static int h_side(a64cpu_t *c, const au_t *u)
{
    if (taken(c, u) != (u->s != 0u)) {
        c->pc = u->imm;
        return AB_EXIT;
    }
    NEXT;
}

static int h_bcond(a64cpu_t *c, const au_t *u)        /* a conditional branch ending a block */
{
    c->pc = taken(c, u) ? u->imm : u->pc + 4u;
    return AB_EXIT;
}

static int h_b(a64cpu_t *c, const au_t *u) { c->pc = u->imm; return AB_EXIT; }

static int h_bl(a64cpu_t *c, const au_t *u)
{
    c->x[30] = u->pc + 4u;
    c->pc = u->imm;
    return AB_EXIT;
}

static int h_bl_follow(a64cpu_t *c, const au_t *u)
{
    c->x[30] = u->pc + 4u;
    NEXT;
}

static int h_br(a64cpu_t *c, const au_t *u)           /* BR, RET; BLR with s */
{
    const uint64_t target = X(u->rn);
    if (u->s) c->x[30] = u->pc + 4u;
    c->pc = target;
    return AB_EXIT;
}

#ifdef XL_PROFILE
uint64_t g_ab_entries, g_ab_trans, g_ab_uops, g_ab_fallbacks;
#define AB_PROF(x) (x)
#else
#define AB_PROF(x) ((void)0)
#endif

#ifdef XL_PROFILE
static uint64_t g_ab_fb_hist[2048];
static uint32_t g_ab_fb_example[2048];

void guest_prof_dump(void);
void guest_prof_dump(void)
{
    xl_log("blocks: %lu entries, %lu translations, %lu uops (%lu per block), %lu fallbacks",
           g_ab_entries, g_ab_trans, g_ab_uops, g_ab_entries ? g_ab_uops / g_ab_entries : 0,
           g_ab_fallbacks);
    for (int n = 0; n < 24; ++n) {
        int best = -1;
        for (int k = 0; k < 2048; ++k) {
            if (g_ab_fb_hist[k] != 0u && (best < 0 || g_ab_fb_hist[k] > g_ab_fb_hist[best])) best = k;
        }
        if (best < 0) break;
        xl_log("  %08x...: %lu", g_ab_fb_example[best], g_ab_fb_hist[best]);
        g_ab_fb_hist[best] = 0;
    }
}
#endif

static int h_fallback(a64cpu_t *c, const au_t *u)
{
#ifdef XL_PROFILE
    {
        uint32_t w;
        __builtin_memcpy(&w, (const void *)(uintptr_t)u->pc, 4);
        ++g_ab_fb_hist[w >> 21];
        g_ab_fb_example[w >> 21] = w;
    }
#endif
    AB_PROF(++g_ab_fallbacks);
    c->pc = u->pc;
    const int ev = a64_step(thread_of(c), c);
    if (ev != 0) {
        return ev;
    }
    if (c->pc != u->pc + 4u) {
        return AB_EXIT;
    }
    NEXT;
}

/* ---- the decoder ------------------------------------------------------------------- */

static inline uint8_t rzr_w(unsigned n) { return (uint8_t)(n == 31u ? A64_DISCARD : n); }  /* write, ZR context */
static inline uint8_t rsp(unsigned n) { return (uint8_t)(n == 31u ? A64_SP : n); }          /* SP context */

/* Fill u for the instruction word w at pc. Returns false for "use the
 * interpreter". *br: the uop transfers control (h_b, h_bl, h_bcond,
 * h_side, h_br). */
static bool decode(uint32_t w, uint64_t pc, au_t *u)
{
    memset(u, 0, sizeof(*u));
    u->pc = pc;
    const bool sf = BIT(w, 31);
    u->sf = sf;
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);

    switch (BITS(w, 28, 25)) {
    case 0x8: case 0x9:                              /* data processing, immediate */
        if ((w & 0x1F000000u) == 0x10000000u) {     /* ADR, ADRP */
            int64_t imm = sext(((uint64_t)BITS(w, 23, 5) << 2) | BITS(w, 30, 29), 21);
            u->rd = rzr_w(rd);
            u->imm = BIT(w, 31) ? (pc & ~0xFFFULL) + ((uint64_t)imm << 12) : pc + (uint64_t)imm;
            u->fn = h_movi;
            return true;
        }
        if ((w & 0x1F800000u) == 0x11000000u) {     /* ADD/SUB immediate */
            uint64_t imm = BITS(w, 21, 10);
            if (BIT(w, 22)) imm <<= 12;
            u->imm = imm;
            u->rn = rsp(rn);
            if (BIT(w, 29)) {
                u->fn = h_addsubi_s; u->op = BIT(w, 30); u->rd = rzr_w(rd);
            } else {
                u->fn = BIT(w, 30) ? h_subi : h_addi; u->rd = rsp(rd);
            }
            return true;
        }
        if ((w & 0x1F800000u) == 0x12000000u) {     /* logical immediate */
            uint64_t imm;
            if (!decode_bitmask(BIT(w, 22), BITS(w, 15, 10), BITS(w, 21, 16), sf, &imm)) return false;
            u->imm = imm;
            u->rn = (uint8_t)rn;    /* ZR read */
            switch (BITS(w, 30, 29)) {
            case 0: u->fn = h_andi; u->rd = rsp(rd); break;
            case 1: u->fn = h_orri; u->rd = rsp(rd); break;
            case 2: u->fn = h_eori; u->rd = rsp(rd); break;
            default: u->fn = h_andsi; u->rd = rzr_w(rd); break;
            }
            return true;
        }
        if ((w & 0x1F800000u) == 0x12800000u) {     /* MOVN, MOVZ, MOVK */
            const unsigned hw = BITS(w, 22, 21);
            if (!sf && hw > 1u) return false;
            const uint64_t imm = (uint64_t)BITS(w, 20, 5) << (16u * hw);
            u->rd = rzr_w(rd);
            switch (BITS(w, 30, 29)) {
            case 0: u->fn = h_movi; u->imm = sf ? ~imm : (uint32_t)~imm; break;
            case 2: u->fn = h_movi; u->imm = imm; break;
            case 3: u->fn = h_movk; u->imm = imm; u->imm2 = 0xFFFFULL << (16u * hw); break;
            default: return false;
            }
            return true;
        }
        if ((w & 0x1F800000u) == 0x13000000u) {     /* SBFM, BFM, UBFM */
            const unsigned opc = BITS(w, 30, 29), immr = BITS(w, 21, 16), imms = BITS(w, 15, 10);
            const unsigned bits = sf ? 64u : 32u;
            if (opc == 3u || BIT(w, 22) != (unsigned)sf || immr >= bits || imms >= bits) return false;
            u->fn = h_bfm; u->op = (uint8_t)opc; u->imm = immr; u->imm2 = imms;
            u->rn = (uint8_t)rn; u->rd = (uint8_t)rd; u->ra = rzr_w(rd);
            return true;
        }
        if ((w & 0x1F800000u) == 0x13800000u) {     /* EXTR */
            const unsigned lsb = BITS(w, 15, 10);
            if (BIT(w, 22) != (unsigned)sf || BIT(w, 21) || (!sf && lsb >= 32u)) return false;
            u->fn = h_extr; u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            u->amt = (uint8_t)lsb;
            return true;
        }
        return false;

    case 0xA: case 0xB:                              /* branches, system */
        if ((w & 0x7C000000u) == 0x14000000u) {     /* B, BL */
            u->imm = pc + (uint64_t)(sext(BITS(w, 25, 0), 26) * 4);
            u->fn = BIT(w, 31) ? h_bl : h_b;
            return true;
        }
        if ((w & 0xFF000010u) == 0x54000000u) {     /* B.cond */
            u->imm = pc + (uint64_t)(sext(BITS(w, 23, 5), 19) * 4);
            u->op = 0; u->cond = (uint8_t)(w & 15u);
            if (u->cond >= 14u) { u->fn = h_b; return true; }   /* AL */
            u->fn = h_bcond;
            return true;
        }
        if ((w & 0x7E000000u) == 0x34000000u) {     /* CBZ, CBNZ */
            u->imm = pc + (uint64_t)(sext(BITS(w, 23, 5), 19) * 4);
            u->op = BIT(w, 24) ? 2 : 1; u->rd = (uint8_t)rd;
            u->fn = h_bcond;
            return true;
        }
        if ((w & 0x7E000000u) == 0x36000000u) {     /* TBZ, TBNZ */
            u->imm = pc + (uint64_t)(sext(BITS(w, 18, 5), 14) * 4);
            u->op = BIT(w, 24) ? 4 : 3; u->rd = (uint8_t)rd;
            u->amt = (uint8_t)((BIT(w, 31) << 5) | BITS(w, 23, 19));
            u->sf = 1;
            u->fn = h_bcond;
            return true;
        }
        if ((w & 0xFE1FFC1Fu) == 0xD61F0000u) {     /* BR, BLR, RET */
            const unsigned opc = BITS(w, 24, 21);
            if (opc > 2u) return false;
            u->rn = (uint8_t)rn; u->s = opc == 1u;
            u->fn = h_br;
            return true;
        }
        if ((w & 0xFFFFF01Fu) == 0xD503201Fu) {     /* hints */
            u->fn = h_nop;
            return true;
        }
        if ((w & 0xFFFFF0FFu) == 0xD50330BFu) {     /* DMB */
            u->fn = h_dmb;
            return true;
        }
        if ((w & 0xFFFFFFE0u) == 0xD53BD040u) {     /* MRS Xt, TPIDR_EL0 */
            u->fn = h_mrs_tpidr; u->rd = rzr_w(rd);
            return true;
        }
        return false;

    case 0x5: case 0xD:                              /* data processing, register */
        if ((w & 0x1F000000u) == 0x0A000000u) {     /* logical shifted register */
            const unsigned amount = BITS(w, 15, 10);
            if (!sf && amount >= 32u) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            u->cond = (uint8_t)BITS(w, 23, 22); u->amt = (uint8_t)amount;
            u->op = (uint8_t)BITS(w, 30, 29);
            u->s = BIT(w, 21) ? 2 : 0;
            if (u->op == 1u && rn == 31u && amount == 0u && !BIT(w, 21)) {
                u->fn = h_movr;
                return true;
            }
            u->fn = h_logic_sh;
            return true;
        }
        if ((w & 0x1F200000u) == 0x0B000000u) {     /* ADD/SUB shifted register */
            const unsigned amount = BITS(w, 15, 10), type = BITS(w, 23, 22);
            if (type == 3u || (!sf && amount >= 32u)) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            u->cond = (uint8_t)type; u->amt = (uint8_t)amount;
            u->op = BIT(w, 30); u->s = BIT(w, 29);
            u->fn = h_addsub_sh;
            return true;
        }
        if ((w & 0x1F200000u) == 0x0B200000u) {     /* ADD/SUB extended register */
            const unsigned sh = BITS(w, 12, 10);
            if (sh > 4u || BITS(w, 23, 22) != 0u) return false;
            u->rn = rsp(rn); u->rm = (uint8_t)rm;
            u->amt = (uint8_t)BITS(w, 15, 13); u->cond = (uint8_t)sh;
            u->op = BIT(w, 30); u->s = BIT(w, 29);
            u->rd = u->s ? rzr_w(rd) : rsp(rd);
            u->fn = h_addsub_ext;
            return true;
        }
        if ((w & 0x1FE00410u) == 0x1A400000u) {     /* CCMN, CCMP */
            if (!BIT(w, 29)) return false;
            u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            u->s = BIT(w, 11); u->imm = rm;
            u->cond = (uint8_t)BITS(w, 15, 12); u->op = BIT(w, 30);
            u->imm2 = (uint64_t)(w & 15u) << 28;
            u->fn = h_ccmp;
            return true;
        }
        if ((w & 0x1FE00000u) == 0x1A800000u) {     /* CSEL family */
            if (BIT(w, 29) || BIT(w, 11)) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            u->cond = (uint8_t)BITS(w, 15, 12);
            u->op = (uint8_t)((BIT(w, 30) << 1) | BIT(w, 10));
            u->fn = h_csel;
            return true;
        }
        if ((w & 0x5FE00000u) == 0x1AC00000u) {     /* 2 sources */
            const unsigned op = BITS(w, 15, 10);
            if (!(op == 2u || op == 3u || (op >= 8u && op <= 11u))) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm; u->op = (uint8_t)op;
            u->fn = h_dp2;
            return true;
        }
        if ((w & 0x5FFF0000u) == 0x5AC00000u) {     /* 1 source */
            const unsigned op = BITS(w, 15, 10);
            if (op > 4u || (op == 3u && !sf)) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->op = (uint8_t)op;
            u->fn = h_dp1;
            return true;
        }
        if ((w & 0x1F000000u) == 0x1B000000u) {     /* 3 sources */
            const unsigned op31 = BITS(w, 23, 21), ra = BITS(w, 14, 10);
            const bool o0 = BIT(w, 15);
            if (BITS(w, 30, 29) != 0u) return false;
            u->rd = rzr_w(rd); u->rn = (uint8_t)rn; u->rm = (uint8_t)rm; u->ra = (uint8_t)ra;
            if (op31 == 0u) { u->fn = h_madd; u->op = o0; return true; }
            if (!sf) return false;
            switch (op31) {
            case 1: u->op = o0 ? 1 : 0; break;
            case 5: u->op = o0 ? 3 : 2; break;
            case 2: if (o0) return false; u->op = 4; break;
            case 6: if (o0) return false; u->op = 5; break;
            default: return false;
            }
            u->fn = h_mull;
            return true;
        }
        return false;

    case 0x7: case 0xF:                              /* FP and Advanced SIMD */
        if ((w & 0xFF200C00u) == 0x1E200800u && BITS(w, 15, 12) <= 8u &&
            BITS(w, 23, 22) <= 1u) {                 /* FP 2 sources, S or D */
            u->fn = h_fp2; u->sf = BIT(w, 22);
            u->op = (uint8_t)BITS(w, 15, 12);
            u->rd = (uint8_t)rd; u->rn = (uint8_t)rn; u->rm = (uint8_t)rm;
            return true;
        }
        u->fn = h_simd; u->imm = w; u->s = 0;
        return true;

    case 0x4: case 0x6: case 0xC: case 0xE: {        /* loads and stores */
        const bool simd = BIT(w, 26);
        if ((w & 0xBF000000u) == 0x0C000000u || (w & 0xBF000000u) == 0x0D000000u) {
            u->fn = h_simd; u->imm = w; u->s = 1;
            return true;
        }
        if ((w & 0x3A000000u) == 0x28000000u) {     /* load/store pair */
            const unsigned opc = BITS(w, 31, 30), type = BITS(w, 24, 23);
            const bool load = BIT(w, 22);
            int scale;
            if (simd) {
                if (opc == 3u) return false;
                scale = 2 + (int)opc;
            } else {
                if (opc == 3u || (opc == 1u && !load)) return false;
                scale = 2 + (int)(opc >> 1);
                u->s = opc == 1u;
            }
            u->sz = (uint8_t)(1 << scale);
            u->imm = (uint64_t)(sext(BITS(w, 21, 15), 7) * (int64_t)u->sz);
            u->op = type == 1u ? 2 : type == 3u ? 1 : 0;
            u->rn = rsp(rn);
            if (simd) {
                u->rd = (uint8_t)rd; u->ra = (uint8_t)BITS(w, 14, 10);
                u->fn = load ? h_vldp : h_vstp;
            } else if (load) {
                u->rd = rzr_w(rd); u->ra = rzr_w(BITS(w, 14, 10));
                u->fn = h_ldp;
            } else {
                u->rd = (uint8_t)rd; u->ra = (uint8_t)BITS(w, 14, 10);
                u->fn = h_stp;
            }
            return true;
        }
        if ((w & 0x3B000000u) == 0x38000000u || (w & 0x3B000000u) == 0x39000000u) {
            const unsigned size = BITS(w, 31, 30), opc = BITS(w, 23, 22);
            int scale = (int)size;
            if (simd && (opc & 2u)) {
                if (size != 0u) return false;
                scale = 4;
            }
            u->rn = rsp(rn);
            if (BIT(w, 24)) {                        /* unsigned offset */
                u->op = 0;
                u->imm = (uint64_t)BITS(w, 21, 10) << scale;
            } else if (BIT(w, 21)) {                 /* register offset */
                if (BITS(w, 11, 10) != 2u) return false;
                const unsigned option = BITS(w, 15, 13);
                if ((option & 2u) == 0u) return false;
                u->op = 3; u->rm = (uint8_t)rm; u->amt = (uint8_t)option;
                u->cond = BIT(w, 12) ? (uint8_t)scale : 0;
            } else {
                const int64_t imm = sext(BITS(w, 20, 12), 9);
                u->imm = (uint64_t)imm;
                switch (BITS(w, 11, 10)) {
                case 0: case 2: u->op = 0; break;
                case 1: u->op = 2; break;
                default: u->op = 1; break;
                }
            }
            u->sz = (uint8_t)(1 << scale);
            if (simd) {
                u->rd = (uint8_t)rd;
                u->fn = (opc & 1u) ? h_vld : h_vst;
                return true;
            }
            switch (opc) {
            case 0: u->rd = (uint8_t)rd; u->fn = h_st; return true;
            case 1: u->rd = rzr_w(rd); u->fn = h_ld; return true;
            case 2:
                if (size == 3u) { u->fn = h_nop; return true; }      /* PRFM */
                u->rd = rzr_w(rd); u->s = 1; u->fn = h_ld; return true;
            default:
                if (size >= 2u) return false;
                u->rd = rzr_w(rd); u->s = 2; u->fn = h_ld; return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

/* ---- blocks ---------------------------------------------------------------------------- */

static ab_cache_t *cache_of(gthread_t *t)
{
    ab_cache_t *bc = t->engine;
    if (XL_UNLIKELY(bc == NULL)) {
        bc = xl_map(sizeof(ab_cache_t));
        if (bc == NULL) xl_fatal("out of memory for the translation cache");
        bc->arena = xl_map(AB_ARENA_SIZE);
        if (bc->arena == NULL) xl_fatal("out of memory for the translation cache");
        t->engine = bc;
    }
    return bc;
}

static inline uint32_t slot_of(uint64_t pc)
{
    return (uint32_t)(((pc >> 2) ^ (pc >> (AB_TABLE_BITS + 2)) ^ (pc >> 30)) & (AB_TABLE_SIZE - 1u));
}

static ab_block_t *translate(ab_cache_t *bc, uint64_t pc)
{
    const size_t worst = sizeof(ab_block_t) + (AB_MAX_UOPS + 1u) * sizeof(au_t) + AB_MAX_UOPS * 4u + 16u;
    if (bc->used + worst > AB_ARENA_SIZE) {
        memset(bc->table, 0, sizeof(bc->table));
        bc->used = 0;
        bc->gen++;
    }
    ab_block_t *b = (ab_block_t *)(void *)(bc->arena + bc->used);
    b->pc = pc;
    b->gen = bc->gen;
    b->nsegs = 0;
    uint64_t cur = pc, seg_start = pc;
    uint32_t n = 0;
#define CLOSE_SEG(endpc) do { if ((endpc) > seg_start) { b->seg[b->nsegs].start = seg_start; \
        b->seg[b->nsegs].len = (uint32_t)((endpc) - seg_start); ++b->nsegs; } } while (0)
    while (n < AB_MAX_UOPS) {
        au_t *u = &b->uops[n];
        uint32_t w;
        __builtin_memcpy(&w, (const void *)(uintptr_t)cur, 4);
        const bool can_follow = b->nsegs + 2u <= AB_MAX_SEGS && n + 2u < AB_MAX_UOPS;
        if (decode(w, cur, u)) {
            if (u->fn == h_b && can_follow) {
                CLOSE_SEG(cur + 4u);
                cur = seg_start = u->imm;
                continue;
            }
            if (u->fn == h_bl && can_follow) {
                u->fn = h_bl_follow;
                CLOSE_SEG(cur + 4u);
                cur = seg_start = u->imm;
                ++n;
                continue;
            }
            if (u->fn == h_bcond && can_follow) {
                u->fn = h_side;
                if (u->imm <= u->pc) {               /* backward: follow it */
                    const uint64_t target = u->imm;
                    u->s = 1;
                    u->imm = u->pc + 4u;
                    CLOSE_SEG(cur + 4u);
                    cur = seg_start = target;
                } else {
                    cur += 4u;
                }
                ++n;
                continue;
            }
            const bool ends = u->fn == h_b || u->fn == h_bl || u->fn == h_bcond || u->fn == h_br;
            cur += 4u;
            ++n;
            if (ends) break;
        } else {
            memset(u, 0, sizeof(*u));
            u->fn = h_fallback;
            u->pc = cur;
            cur += 4u;
            ++n;
            /* anything left in the branch/system space may transfer
             * control or trap (SVC, BRK, ...): end the block after it */
            const unsigned op0 = BITS(w, 28, 25);
            if (op0 == 0xA || op0 == 0xB || op0 < 0x4) break;
        }
    }
    CLOSE_SEG(cur);
#undef CLOSE_SEG
    b->nuops = n;
    memset(&b->uops[n], 0, sizeof(au_t));
    b->uops[n].fn = h_end;
    b->uops[n].imm = cur;
    b->uops[n].pc = cur;
    uint8_t *code = (uint8_t *)&b->uops[n + 1u];
    uint32_t off = 0;
    for (uint32_t k = 0; k < b->nsegs; ++k) {
        b->seg[k].off = off;
        memcpy(code + off, (const void *)(uintptr_t)b->seg[k].start, b->seg[k].len);
        off += b->seg[k].len;
    }
    b->nbytes = off;
    b->code = code;
    b->ro_epoch = 0;
    b->seen_epoch = 0;
    b->link_pc[0] = b->link_pc[1] = 1;
    b->link[0] = b->link[1] = NULL;
    b->link_next = 0;
    bc->used += (sizeof(ab_block_t) + (n + 1u) * sizeof(au_t) + off + 15u) & ~(size_t)15u;
    ab_set_t *set = &bc->table[slot_of(pc)];
    if (set->way[0] != NULL && set->way[0]->pc == pc) {
        set->way[0] = b;
    } else {
        set->way[1] = set->way[0];
        set->way[0] = b;
    }
    return b;
}

static inline bool code_unchanged(const ab_block_t *b)
{
    for (uint32_t k = 0; k < b->nsegs; ++k) {
        const uint8_t *g = (const uint8_t *)(uintptr_t)b->seg[k].start;
        const uint8_t *m = b->code + b->seg[k].off;
        const uint32_t n = b->seg[k].len;
        for (uint32_t i = 0; i < n; i += 4u) {
            uint32_t x, y;
            __builtin_memcpy(&x, g + i, 4);
            __builtin_memcpy(&y, m + i, 4);
            if (x != y) return false;
        }
    }
    return true;
}

static bool code_readonly(const ab_block_t *b)
{
    for (uint32_t k = 0; k < b->nsegs; ++k) {
        if (!xl_code_is_readonly(b->seg[k].start, b->seg[k].start + b->seg[k].len)) return false;
    }
    return true;
}

static inline bool block_valid(ab_block_t *b)
{
    const uint64_t e = g_xl_code_epoch;
    if (XL_LIKELY(b->ro_epoch == e)) return true;
    if (!code_unchanged(b)) return false;
    if (b->seen_epoch != e) {
        b->seen_epoch = e;
        if (code_readonly(b)) b->ro_epoch = e;
    }
    return true;
}

static inline void link_to(ab_block_t *prev, uint64_t pc, ab_block_t *b)
{
    const uint32_t k = prev->link_next;
    prev->link_pc[k] = pc;
    prev->link[k] = b;
    prev->link_next = k ^ 1u;
}

int guest_run(gthread_t *t)
{
    if (XL_UNLIKELY(g_ab_disabled < 0)) {
        g_ab_disabled = 0;
        for (char **p = g_xl_envp; p != NULL && *p != NULL; ++p) {
            if (strcmp(*p, "XL_NOBLOCKS=1") == 0) g_ab_disabled = 1;
        }
    }
    if (g_ab_disabled) {
        return a64_run_interp(t);
    }
    a64cpu_t *c = A64(t);
    ab_cache_t *bc = cache_of(t);
    ab_block_t *prev = NULL;
    for (;;) {
        if (XL_UNLIKELY(t->sig_pending_flag)) {
            return XL_EV_SIGNAL;
        }
        const uint64_t pc = c->pc;
        ab_block_t *b = NULL;
        const bool linkable = prev != NULL && prev->gen == bc->gen;
        if (linkable) {
            if (prev->link_pc[0] == pc) b = prev->link[0];
            else if (prev->link_pc[1] == pc) b = prev->link[1];
        }
        if (b == NULL) {
            ab_set_t *set = &bc->table[slot_of(pc)];
            b = set->way[0];
            if (b == NULL || b->pc != pc) {
                b = set->way[1];
                if (b != NULL && b->pc == pc) {
                    set->way[1] = set->way[0];
                    set->way[0] = b;
                } else {
                    b = NULL;
                }
            }
            if (b != NULL && linkable) link_to(prev, pc, b);
        }
        if (XL_UNLIKELY(b == NULL || !block_valid(b))) {
            b = translate(bc, pc);
            AB_PROF(++g_ab_trans);
            if (prev != NULL && prev->gen == bc->gen) link_to(prev, pc, b);
        }
        AB_PROF(++g_ab_entries);
        AB_PROF(g_ab_uops += b->nuops);
        const int r = b->uops[0].fn(c, b->uops);
        if (XL_UNLIKELY(r != AB_EXIT)) {
            return r;
        }
        prev = b;
    }
}

bool guest_jit_fault(gthread_t *t, void *uc)
{
    (void)t;
    (void)uc;
    return false;
}
