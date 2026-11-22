/*
 * The x86 guest's block engine: decode once, run many times.
 *
 * Code is translated a basic block at a time into an array of "uops", each
 * a handler function plus the instruction's predecoded operands (registers,
 * effective-address parts, immediates, branch targets). The common
 * instruction forms -- moves, ALU ops, LEA, TEST, PUSH/POP, MOVZX/MOVSX,
 * CMOVcc/SETcc, shifts, IMUL, Jcc/JMP/CALL/RET -- get handlers of their own
 * that skip prefix, ModRM and immediate decoding entirely. Everything else
 * becomes a "fallback" uop that runs the instruction through the
 * instruction-at-a-time interpreter (x86_step), so every instruction the
 * interpreter knows can sit inside a block; a block ends at a control
 * transfer or after XB_MAX_UOPS.
 *
 * Both paths share the flag and ALU code in x86_ops.h, so they compute the
 * same results. Blocks live in a per-thread cache (gthread_t.engine): a
 * direct-mapped table keyed by guest address, over an arena that is simply
 * emptied when full. Each block keeps a copy of the bytes it was decoded
 * from and is checked against guest memory on every entry, so code the
 * guest rewrites (JITs, trampolines, dlopen over an unmapped library) is
 * retranslated rather than run stale.
 *
 * Precise state: a handler that touches memory stores its instruction's
 * address in cpu->rip before the access, so a fault is reported on the
 * right instruction; registers are only ever written after the access.
 * Signals are taken between blocks.
 *
 * guest_run() (x86_jit.c) picks this engine on hosts without a JIT back
 * end for the x86 guest, and under XL_NOJIT=1.
 */

#include "x86_ops.h"
#include "host.h"

#define XB_MAX_UOPS    64
#define XB_TABLE_BITS  15
#define XB_TABLE_SIZE  (1u << XB_TABLE_BITS)
#define XB_ARENA_SIZE  (8u * 1024u * 1024u)
#define XB_EXIT        (-1)    /* handler set cpu->rip: leave the block */
#define NOREG          16u     /* cpu->r[16]: always zero */

typedef struct xb_uop xb_uop_t;
typedef int (*xb_fn)(x86cpu_t *c, const xb_uop_t *u);

struct xb_uop {
    xb_fn fn;
    uint64_t pc;        /* the instruction */
    uint64_t next;      /* the one after it */
    uint64_t imm;       /* immediate, or branch target */
    int64_t disp;       /* EA displacement (RIP-relative: absolute) */
    uint8_t dst;        /* register operand written */
    uint8_t src;        /* register operand read */
    uint8_t base;       /* EA base register, NOREG */
    uint8_t index;      /* EA index register, NOREG */
    uint8_t scale;
    uint8_t seg;        /* 0, 4 = FS, 5 = GS */
    uint8_t sz;         /* operand size, bytes */
    uint8_t op;         /* ALU op / condition / shift kind / source size */
    uint8_t rex;        /* a REX prefix was present (byte registers) */
    uint8_t rexb;       /* the REX byte itself (SSE uops) */
    uint8_t pfx66;      /* SSE uops: 66 prefix */
    uint8_t rep;        /* SSE uops: F2/F3 prefix */
    uint8_t mod;        /* SSE uops: ModRM.mod */
};

#define XB_MAX_SEGS 8

/* A trace can follow jumps, calls and one side of conditional branches,
 * so its code is several byte ranges. */
typedef struct {
    uint64_t start;
    uint32_t len;
    uint32_t off;       /* of its copy in code[] */
} xb_seg_t;

typedef struct xb_block {
    uint64_t pc;
    uint64_t end_pc;    /* where running off the last uop continues */
    uint32_t nuops;
    uint32_t nsegs;
    uint32_t gen;       /* cache generation (arena resets) */
    uint32_t nbytes;    /* total bytes in code[] */
    const uint8_t *code;
    /* g_xl_code_epoch at which the code was known to be read-only (no
     * compare needed while it stays the same), and at which that was last
     * looked up */
    uint64_t ro_epoch;
    uint64_t seen_epoch;
    /* the blocks most recently run after this one */
    uint64_t link_pc[2];
    struct xb_block *link[2];
    uint32_t link_next;
    xb_seg_t seg[XB_MAX_SEGS];
    xb_uop_t uops[];
} xb_block_t;

typedef struct {
    xb_block_t *way[2];     /* way[0]: most recently used */
} xb_set_t;

typedef struct {
    xb_set_t table[XB_TABLE_SIZE];
    uint8_t *arena;
    size_t used;
    uint32_t gen;
} xb_cache_t;

/* ---- operands ------------------------------------------------------------- */

static inline uint64_t ea(const x86cpu_t *c, const xb_uop_t *u)
{
    uint64_t a = (uint64_t)u->disp + c->r[u->base] + (c->r[u->index] << u->scale);
    if (XL_UNLIKELY(u->seg != 0u)) a += u->seg == 4u ? c->fs_base : c->gs_base;
    return a;
}

#define RD(r) x86_rd(c, (r), u->sz, u->rex != 0u)
#define WR(r, v) x86_wr(c, (r), u->sz, u->rex != 0u, (v))

static inline uint64_t cin_for(x86cpu_t *c, unsigned op)
{
    return (op == 2u || op == 3u) ? get_cf(c) : 0u;
}

/* Handlers are direct-threaded: each ends by jumping straight to the next
 * uop's handler (a guaranteed tail call, so the host stack does not grow);
 * the last uop of every block is h_end, which returns to the run loop. */
#define NEXT __attribute__((musttail)) return u[1].fn(c, u + 1)

static int h_end(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->imm;
    return XB_EXIT;
}

/* ---- handlers ------------------------------------------------------------------ */

static int h_alu_rr(x86cpu_t *c, const xb_uop_t *u)
{
    uint64_t r = x86_alu(c, u->op, u->sz, RD(u->dst), RD(u->src), cin_for(c, u->op));
    if (u->op != 7u) WR(u->dst, r);
    NEXT;
}

static int h_alu_ri(x86cpu_t *c, const xb_uop_t *u)
{
    uint64_t r = x86_alu(c, u->op, u->sz, RD(u->dst), u->imm, cin_for(c, u->op));
    if (u->op != 7u) WR(u->dst, r);
    NEXT;
}

static int h_alu_rm(x86cpu_t *c, const xb_uop_t *u)    /* reg op= [mem] */
{
    c->rip = u->pc;
    uint64_t b = mrd(ea(c, u), u->sz);
    uint64_t r = x86_alu(c, u->op, u->sz, RD(u->dst), b, cin_for(c, u->op));
    if (u->op != 7u) WR(u->dst, r);
    NEXT;
}

static int h_alu_mr(x86cpu_t *c, const xb_uop_t *u)    /* [mem] op= reg */
{
    c->rip = u->pc;
    const uint64_t a = ea(c, u);
    uint64_t r = x86_alu(c, u->op, u->sz, mrd(a, u->sz), RD(u->src), cin_for(c, u->op));
    if (u->op != 7u) mwr(a, u->sz, r);
    NEXT;
}

static int h_alu_mi(x86cpu_t *c, const xb_uop_t *u)    /* [mem] op= imm */
{
    c->rip = u->pc;
    const uint64_t a = ea(c, u);
    uint64_t r = x86_alu(c, u->op, u->sz, mrd(a, u->sz), u->imm, cin_for(c, u->op));
    if (u->op != 7u) mwr(a, u->sz, r);
    NEXT;
}

static int h_mov_rr(x86cpu_t *c, const xb_uop_t *u) { WR(u->dst, RD(u->src)); NEXT; }
static int h_mov_ri(x86cpu_t *c, const xb_uop_t *u) { WR(u->dst, u->imm); NEXT; }

static int h_mov_load(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    WR(u->dst, mrd(ea(c, u), u->sz));
    NEXT;
}

static int h_mov_store(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    mwr(ea(c, u), u->sz, RD(u->src));
    NEXT;
}

static int h_mov_mi(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    mwr(ea(c, u), u->sz, u->imm);
    NEXT;
}

static int h_lea(x86cpu_t *c, const xb_uop_t *u)
{
    WR(u->dst, (uint64_t)u->disp + c->r[u->base] + (c->r[u->index] << u->scale));
    NEXT;
}

static int h_lea64(x86cpu_t *c, const xb_uop_t *u)
{
    c->r[u->dst] = (uint64_t)u->disp + c->r[u->base] + (c->r[u->index] << u->scale);
    NEXT;
}

static int h_lea32(x86cpu_t *c, const xb_uop_t *u)
{
    c->r[u->dst] = (uint32_t)((uint64_t)u->disp + c->r[u->base] + (c->r[u->index] << u->scale));
    NEXT;
}

/* ---- ALU and MOV, specialized by operation and size ----
 * op 0..7: ADD OR ADC SBB AND SUB XOR CMP; 8: TEST. The generic h_alu_*
 * handlers above are replaced by these when a block is built. */

#define AOP(OP) ((OP) == 8 ? 4 : (OP))
#define AWR(OP) ((OP) != 7 && (OP) != 8)

#define DEF_ALU(OP, SZ) \
static int h_arr_##OP##_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    const bool rx = u->rex != 0u; \
    uint64_t r = x86_alu(c, AOP(OP), SZ, x86_rd(c, u->dst, SZ, rx), x86_rd(c, u->src, SZ, rx), cin_for(c, AOP(OP))); \
    if (AWR(OP)) x86_wr(c, u->dst, SZ, rx, r); \
    NEXT; } \
static int h_ari_##OP##_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    const bool rx = u->rex != 0u; \
    uint64_t r = x86_alu(c, AOP(OP), SZ, x86_rd(c, u->dst, SZ, rx), u->imm, cin_for(c, AOP(OP))); \
    if (AWR(OP)) x86_wr(c, u->dst, SZ, rx, r); \
    NEXT; } \
static int h_arm_##OP##_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    const bool rx = u->rex != 0u; \
    c->rip = u->pc; \
    const uint64_t m = mrd(ea(c, u), SZ); \
    uint64_t r = x86_alu(c, AOP(OP), SZ, x86_rd(c, u->dst, SZ, rx), m, cin_for(c, AOP(OP))); \
    if (AWR(OP)) x86_wr(c, u->dst, SZ, rx, r); \
    NEXT; } \
static int h_amr_##OP##_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    c->rip = u->pc; \
    const uint64_t a = ea(c, u); \
    uint64_t r = x86_alu(c, AOP(OP), SZ, mrd(a, SZ), x86_rd(c, u->src, SZ, u->rex != 0u), cin_for(c, AOP(OP))); \
    if (AWR(OP)) mwr(a, SZ, r); \
    NEXT; } \
static int h_ami_##OP##_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    c->rip = u->pc; \
    const uint64_t a = ea(c, u); \
    uint64_t r = x86_alu(c, AOP(OP), SZ, mrd(a, SZ), u->imm, cin_for(c, AOP(OP))); \
    if (AWR(OP)) mwr(a, SZ, r); \
    NEXT; }

#define DEF_ALU4(OP) DEF_ALU(OP, 1) DEF_ALU(OP, 2) DEF_ALU(OP, 4) DEF_ALU(OP, 8)
DEF_ALU4(0) DEF_ALU4(1) DEF_ALU4(2) DEF_ALU4(3) DEF_ALU4(4) DEF_ALU4(5) DEF_ALU4(6) DEF_ALU4(7) DEF_ALU4(8)

#define ROW4(F, OP) { F##_##OP##_1, F##_##OP##_2, F##_##OP##_4, F##_##OP##_8 }
#define TAB9(F) { ROW4(F, 0), ROW4(F, 1), ROW4(F, 2), ROW4(F, 3), ROW4(F, 4), ROW4(F, 5), \
                  ROW4(F, 6), ROW4(F, 7), ROW4(F, 8) }
static const xb_fn g_arr[9][4] = TAB9(h_arr);
static const xb_fn g_ari[9][4] = TAB9(h_ari);
static const xb_fn g_arm[9][4] = TAB9(h_arm);
static const xb_fn g_amr[9][4] = TAB9(h_amr);
static const xb_fn g_ami[9][4] = TAB9(h_ami);

#define DEF_MOV(SZ) \
static int h_mrr_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    x86_wr(c, u->dst, SZ, u->rex != 0u, x86_rd(c, u->src, SZ, u->rex != 0u)); NEXT; } \
static int h_mri_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    x86_wr(c, u->dst, SZ, u->rex != 0u, u->imm); NEXT; } \
static int h_mld_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    c->rip = u->pc; x86_wr(c, u->dst, SZ, u->rex != 0u, mrd(ea(c, u), SZ)); NEXT; } \
static int h_mst_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    c->rip = u->pc; mwr(ea(c, u), SZ, x86_rd(c, u->src, SZ, u->rex != 0u)); NEXT; } \
static int h_mmi_##SZ(x86cpu_t *c, const xb_uop_t *u) { \
    c->rip = u->pc; mwr(ea(c, u), SZ, u->imm); NEXT; }
DEF_MOV(1) DEF_MOV(2) DEF_MOV(4) DEF_MOV(8)
static const xb_fn g_mrr[4] = { h_mrr_1, h_mrr_2, h_mrr_4, h_mrr_8 };
static const xb_fn g_mri[4] = { h_mri_1, h_mri_2, h_mri_4, h_mri_8 };
static const xb_fn g_mld[4] = { h_mld_1, h_mld_2, h_mld_4, h_mld_8 };
static const xb_fn g_mst[4] = { h_mst_1, h_mst_2, h_mst_4, h_mst_8 };
static const xb_fn g_mmi[4] = { h_mmi_1, h_mmi_2, h_mmi_4, h_mmi_8 };

static inline int szi(int sz)
{
    return sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3;
}

static int h_push_r(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, c->r[u->src]);
    c->r[RSP] = sp;
    NEXT;
}

static int h_push_i(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, u->imm);
    c->r[RSP] = sp;
    NEXT;
}

static int h_pop_r(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t v = mrd(c->r[RSP], 8);
    c->r[RSP] += 8u;
    c->r[u->dst] = v;
    NEXT;
}

/* MOVZX/MOVSX/MOVSXD: op = source size, imm = 1 for sign extension */
static int h_movx_r(x86cpu_t *c, const xb_uop_t *u)
{
    uint64_t v = x86_rd(c, u->src, u->op, u->rex != 0u);
    if (u->imm) v = (uint64_t)sx(v, u->op);
    WR(u->dst, v);
    NEXT;
}

static int h_movx_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    uint64_t v = mrd(ea(c, u), u->op);
    if (u->imm) v = (uint64_t)sx(v, u->op);
    WR(u->dst, v);
    NEXT;
}

static int h_cmov_r(x86cpu_t *c, const xb_uop_t *u)
{
    const uint64_t v = RD(u->src);
    if (x86_cond(c, u->op)) WR(u->dst, v);
    else if (u->sz == 4u) c->r[u->dst] = (uint32_t)c->r[u->dst];
    NEXT;
}

static int h_cmov_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t v = mrd(ea(c, u), u->sz);
    if (x86_cond(c, u->op)) WR(u->dst, v);
    else if (u->sz == 4u) c->r[u->dst] = (uint32_t)c->r[u->dst];
    NEXT;
}

static int h_setcc_r(x86cpu_t *c, const xb_uop_t *u)
{
    x86_wr(c, u->dst, 1, u->rex != 0u, x86_cond(c, u->op) ? 1u : 0u);
    NEXT;
}

static int h_incdec_r(x86cpu_t *c, const xb_uop_t *u)
{
    WR(u->dst, x86_inc_dec(c, u->sz, RD(u->dst), u->op != 0u));
    NEXT;
}

static int h_incdec_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t a = ea(c, u);
    mwr(a, u->sz, x86_inc_dec(c, u->sz, mrd(a, u->sz), u->op != 0u));
    NEXT;
}

static int h_shift_ri(x86cpu_t *c, const xb_uop_t *u)   /* count in imm */
{
    const unsigned cnt = (unsigned)u->imm & (u->sz == 8u ? 63u : 31u);
    if (cnt != 0u) WR(u->dst, x86_shift(c, u->op, u->sz, RD(u->dst), cnt));
    NEXT;
}

static int h_shift_rc(x86cpu_t *c, const xb_uop_t *u)   /* count in CL */
{
    const unsigned cnt = (unsigned)c->r[RCX] & (u->sz == 8u ? 63u : 31u);
    if (cnt != 0u) WR(u->dst, x86_shift(c, u->op, u->sz, RD(u->dst), cnt));
    NEXT;
}

static inline void imul_set(x86cpu_t *c, const xb_uop_t *u, uint64_t x, uint64_t y)
{
    const int sz = u->sz;
    const int64_t a = sx(x, sz), b = sx(y, sz);
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
    WR(u->dst, r);
    setcc(c, CC_MUL, sz, 0, ovf, r);
}

static int h_imul_rr(x86cpu_t *c, const xb_uop_t *u) { imul_set(c, u, RD(u->dst), RD(u->src)); NEXT; }
static int h_imul_rri(x86cpu_t *c, const xb_uop_t *u) { imul_set(c, u, RD(u->src), u->imm); NEXT; }

static int h_imul_rm(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    imul_set(c, u, RD(u->dst), mrd(ea(c, u), u->sz));
    NEXT;
}

static int h_imul_rmi(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    imul_set(c, u, mrd(ea(c, u), u->sz), u->imm);
    NEXT;
}

static int h_cdq(x86cpu_t *c, const xb_uop_t *u)          /* 98, 99 */
{
    if (u->op == 0u) {          /* CBW/CWDE/CDQE */
        if (u->sz == 8u) c->r[RAX] = (uint64_t)(int64_t)(int32_t)c->r[RAX];
        else if (u->sz == 4u) c->r[RAX] = (uint32_t)(int32_t)(int16_t)c->r[RAX];
        else x86_wr(c, RAX, 2, false, (uint64_t)(int64_t)(int8_t)c->r[RAX]);
    } else {                    /* CWD/CDQ/CQO */
        if (u->sz == 8u) c->r[RDX] = (int64_t)c->r[RAX] < 0 ? ~0ULL : 0u;
        else if (u->sz == 4u) c->r[RDX] = (int32_t)c->r[RAX] < 0 ? 0xFFFFFFFFu : 0u;
        else x86_wr(c, RDX, 2, false, (int16_t)c->r[RAX] < 0 ? 0xFFFFu : 0u);
    }
    NEXT;
}

static int h_nop(x86cpu_t *c, const xb_uop_t *u) { (void)c; (void)u; NEXT; }

static int h_jcc(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = x86_cond(c, u->op) ? u->imm : u->next;
    return XB_EXIT;
}

static int h_jmp(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->imm;
    return XB_EXIT;
}

static int h_call(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, u->next);
    c->r[RSP] = sp;
    c->rip = u->imm;
    return XB_EXIT;
}

static int h_call_r(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t target = c->r[u->src];
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, u->next);
    c->r[RSP] = sp;
    c->rip = target;
    return XB_EXIT;
}

static int h_call_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t target = mrd(ea(c, u), 8);
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, u->next);
    c->r[RSP] = sp;
    c->rip = target;
    return XB_EXIT;
}

static int h_jmp_r(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = c->r[u->src];
    return XB_EXIT;
}

static int h_jmp_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    c->rip = mrd(ea(c, u), 8);
    return XB_EXIT;
}

static int h_ret(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t target = mrd(c->r[RSP], 8);
    c->r[RSP] += 8u + u->imm;
    c->rip = target;
    return XB_EXIT;
}

/* In a trace: a conditional branch whose followed side continues in the
 * block -- leave only when the other side is taken. */
static int h_side(x86cpu_t *c, const xb_uop_t *u)
{
    if (x86_cond(c, u->op)) {
        c->rip = u->imm;
        return XB_EXIT;
    }
    NEXT;
}

/* In a trace: CALL whose callee is translated into the same block. */
static int h_call_follow(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t sp = c->r[RSP] - 8u;
    mwr(sp, 8, u->next);
    c->r[RSP] = sp;
    NEXT;
}

static inline gthread_t *thread_of(x86cpu_t *c)
{
    return (gthread_t *)(void *)((uint8_t *)c - __builtin_offsetof(gthread_t, cpu));
}

#ifdef XL_PROFILE
uint64_t g_xb_entries, g_xb_trans, g_xb_uops, g_xb_fallbacks, g_xb_fb_op[512];
#define XB_PROF(x) (x)
#else
#define XB_PROF(x) ((void)0)
#endif

static int h_fallback(x86cpu_t *c, const xb_uop_t *u)
{
#ifdef XL_PROFILE
    {
        const uint8_t *p = (const uint8_t *)(uintptr_t)u->pc;
        while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0xF0 || *p == 0x64 || *p == 0x65 || *p == 0x2E || *p == 0x3E || *p == 0x67) ++p;
        if ((*p & 0xF0) == 0x40) ++p;
        ++g_xb_fallbacks;
        ++g_xb_fb_op[*p == 0x0F ? 256 + p[1] : *p];
    }
#endif
    c->rip = u->pc;
    int ev = x86_step(thread_of(c), c);
    if (ev != 0) {
        return ev;
    }
    if (c->rip != u->next) {
        return XB_EXIT;
    }
    NEXT;
}

/* TEST: an AND whose result is not written */
static int h_test_rr(x86cpu_t *c, const xb_uop_t *u)
{
    (void)x86_alu(c, 4, u->sz, RD(u->dst), RD(u->src), 0);
    NEXT;
}

static int h_test_ri(x86cpu_t *c, const xb_uop_t *u)
{
    (void)x86_alu(c, 4, u->sz, RD(u->dst), u->imm, 0);
    NEXT;
}

static int h_test_mr(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    (void)x86_alu(c, 4, u->sz, mrd(ea(c, u), u->sz), RD(u->src), 0);
    NEXT;
}

/* ---- more integer forms ---- */

static int h_test_mi(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    (void)x86_alu(c, 4, u->sz, mrd(ea(c, u), u->sz), u->imm, 0);
    NEXT;
}

static int h_not_r(x86cpu_t *c, const xb_uop_t *u)
{
    WR(u->dst, ~RD(u->dst));
    NEXT;
}

static int h_neg_r(x86cpu_t *c, const xb_uop_t *u)
{
    WR(u->dst, x86_alu(c, 5, u->sz, 0, RD(u->dst), 0));
    NEXT;
}

static inline void bitscan(x86cpu_t *c, const xb_uop_t *u, uint64_t v)
{
    uint64_t f = x86_get_flags(c) & ~(uint64_t)F_ZF;
    if (v == 0u) {
        x86_set_flags(c, f | F_ZF);
        return;
    }
    WR(u->dst, u->op ? (uint64_t)(63 - __builtin_clzll(v)) : (uint64_t)__builtin_ctzll(v));
    x86_set_flags(c, f);
}

static int h_bsx_r(x86cpu_t *c, const xb_uop_t *u) { bitscan(c, u, RD(u->src)); NEXT; }

static int h_bsx_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    bitscan(c, u, mrd(ea(c, u), u->sz));
    NEXT;
}

static int h_xchg_rr(x86cpu_t *c, const xb_uop_t *u)
{
    const uint64_t a = RD(u->dst), b = RD(u->src);
    WR(u->dst, b);
    WR(u->src, a);
    NEXT;
}

static int h_leave(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    const uint64_t v = mrd(c->r[RBP], 8);
    c->r[RSP] = c->r[RBP] + 8u;
    c->r[RBP] = v;
    NEXT;
}

/* ---- SSE ---- */

typedef uint8_t v16u8 __attribute__((vector_size(16)));

/* Any SSE/SSE2/SSE3/MMX instruction, operands predecoded: the
 * interpreter's own implementation, minus its decoding. */
static __attribute__((noinline)) int sse_exec(x86cpu_t *c, const xb_uop_t *u)
{
    insn_t i;
    i.t = thread_of(c);
    i.c = c;
    i.start = u->pc;
    i.pc = u->next;
    i.rex = u->rexb;
    i.opsz = (u->rexb & 8u) ? 8 : (u->pfx66 ? 2 : 4);
    i.adsz32 = 0;
    i.rep = u->rep;
    i.p66 = u->pfx66;
    i.lock = 0;
    i.seg = u->seg;
    i.modrm = 0;
    i.mod = u->mod;
    i.reg = u->dst;
    i.rm = u->src;
    i.riprel = 0;
    i.ea_base = u->base == NOREG ? (int8_t)-1 : (int8_t)u->base;
    i.ea_index = u->index == NOREG ? (int8_t)-1 : (int8_t)u->index;
    i.ea_scale = u->scale;
    i.ea_disp = u->disp;
    c->rip = u->pc;
    return x86_sse_body(&i, u->op, (unsigned)u->imm);
}

static int h_sse(x86cpu_t *c, const xb_uop_t *u)
{
    const int ev = sse_exec(c, u);
    if (ev != 0) {
        return ev;
    }
    NEXT;
}

static int h_xmov_rr(x86cpu_t *c, const xb_uop_t *u)
{
    c->xmm[u->dst] = c->xmm[u->src];
    NEXT;
}

static int h_xld16(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    xmm_t v;
    mrd_n(ea(c, u), &v, 16);
    c->xmm[u->dst] = v;
    NEXT;
}

static int h_xst16(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    mwr_n(ea(c, u), &c->xmm[u->src], 16);
    NEXT;
}

static inline void xlogic(xmm_t *g, const xmm_t *s, unsigned op)
{
    for (int k = 0; k < 2; ++k) {
        const uint64_t a = g->q[k], b = s->q[k];
        g->q[k] = op == 0u ? a & b : op == 1u ? ~a & b : op == 2u ? a | b : a ^ b;
    }
}

static int h_xlogic_r(x86cpu_t *c, const xb_uop_t *u)
{
    xlogic(&c->xmm[u->dst], &c->xmm[u->src], u->op);
    NEXT;
}

static int h_xlogic_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    xmm_t v;
    mrd_n(ea(c, u), &v, 16);
    xlogic(&c->xmm[u->dst], &v, u->op);
    NEXT;
}

static inline void pcmpeqb(xmm_t *g, const xmm_t *s)
{
    v16u8 a, b;
    __builtin_memcpy(&a, g, 16);
    __builtin_memcpy(&b, s, 16);
    v16u8 r = (v16u8)(a == b);
    __builtin_memcpy(g, &r, 16);
}

static int h_pcmpeqb_r(x86cpu_t *c, const xb_uop_t *u)
{
    pcmpeqb(&c->xmm[u->dst], &c->xmm[u->src]);
    NEXT;
}

static int h_pcmpeqb_m(x86cpu_t *c, const xb_uop_t *u)
{
    c->rip = u->pc;
    xmm_t v;
    mrd_n(ea(c, u), &v, 16);
    pcmpeqb(&c->xmm[u->dst], &v);
    NEXT;
}

static int h_pmovmskb(x86cpu_t *c, const xb_uop_t *u)
{
    const xmm_t *x = &c->xmm[u->src];
    const uint64_t k = 0x0002040810204081ULL, m = 0x8080808080808080ULL;
    const uint64_t lo = ((x->q[0] & m) * k) >> 56, hi = ((x->q[1] & m) * k) >> 56;
    c->r[u->dst] = lo | (hi << 8);
    NEXT;
}

/* ---- instruction length ------------------------------------------------------------ */

/* Bits for the one-byte map: has ModRM, immediate kind. */
enum { I_NONE = 0, I_B = 1, I_Z = 2, I_W = 3, I_WB = 4, I_V = 5 /* z, or 8 with REX.W */ };

static int imm_size(int kind, bool p66, bool rexw)
{
    switch (kind) {
    case I_B: return 1;
    case I_Z: return p66 ? 2 : 4;
    case I_W: return 2;
    case I_WB: return 3;
    case I_V: return rexw ? 8 : (p66 ? 2 : 4);
    default: return 0;
    }
}

static int modrm_len(const uint8_t *p)
{
    const uint8_t m = p[0];
    const unsigned mod = m >> 6, rm = m & 7u;
    int n = 1;
    if (mod == 3u) return n;
    if (rm == 4u) {
        const uint8_t sib = p[1];
        n += 1;
        if (mod == 0u && (sib & 7u) == 5u) n += 4;
    } else if (mod == 0u && rm == 5u) {
        n += 4;
    }
    if (mod == 1u) n += 1;
    else if (mod == 2u) n += 4;
    return n;
}

/* Length of the instruction at p, 0 if not decodable. *ends: it can
 * transfer control (the block stops after it). */
int x86_insn_len(const uint8_t *p0, bool *ends)
{
    const uint8_t *p = p0;
    bool p66 = false, rexw = false;
    *ends = false;
    for (int k = 0; k < 14; ++k) {
        const uint8_t b = *p;
        if (b == 0x66) { p66 = true; ++p; continue; }
        if (b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF0 || b == 0x2E || b == 0x36 ||
            b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { ++p; continue; }
        break;
    }
    if ((*p & 0xF0u) == 0x40u) {
        rexw = (*p & 8u) != 0u;
        ++p;
    }
    const uint8_t op = *p++;
    bool modrm = false;
    int imm = I_NONE;
    if (op == 0x0F) {
        const uint8_t o2 = *p++;
        if (o2 == 0x38) {
            ++p;
            modrm = true;
        } else if (o2 == 0x3A) {
            ++p;
            modrm = true;
            imm = I_B;
        } else if (o2 >= 0x80 && o2 <= 0x8F) {
            *ends = true;
            return (int)(p - p0) + 4;
        } else {
            switch (o2) {
            case 0x05: case 0x07: case 0x0B: case 0x34: case 0x35:
                *ends = true;
                return (int)(p - p0);
            case 0x06: case 0x08: case 0x09: case 0x0E: case 0x30: case 0x31: case 0x32:
            case 0x33: case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9:
            case 0xAA:
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                return (int)(p - p0);
            case 0x70: case 0x71: case 0x72: case 0x73: case 0xA4: case 0xAC: case 0xBA:
            case 0xC2: case 0xC4: case 0xC5: case 0xC6:
                modrm = true;
                imm = I_B;
                break;
            case 0x0F:
                modrm = true;
                imm = I_B;
                break;
            default:
                modrm = true;
                break;
            }
        }
    } else {
        if (op < 0x40) {
            if ((op & 7u) < 4u) modrm = true;
            else if ((op & 7u) == 4u) imm = I_B;
            else if ((op & 7u) == 5u) imm = I_Z;
            /* 06/07/0E/16/17/1E/1F/27/2F/37/3F: invalid in 64-bit mode */
            else return 0;
        } else if (op >= 0x50 && op <= 0x5F) {
        } else if (op >= 0x70 && op <= 0x7F) {
            imm = I_B;
            *ends = true;
        } else if (op >= 0x84 && op <= 0x8F) {
            modrm = true;
        } else if (op >= 0x90 && op <= 0x99) {
        } else if (op >= 0xB0 && op <= 0xB7) {
            imm = I_B;
        } else if (op >= 0xB8 && op <= 0xBF) {
            imm = I_V;
        } else if (op >= 0xD8 && op <= 0xDF) {
            modrm = true;
        } else {
            switch (op) {
            case 0x63: modrm = true; break;
            case 0x68: imm = I_Z; break;
            case 0x69: modrm = true; imm = I_Z; break;
            case 0x6A: imm = I_B; break;
            case 0x6B: modrm = true; imm = I_B; break;
            case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6: modrm = true; imm = I_B; break;
            case 0x81: case 0xC7: modrm = true; imm = I_Z; break;
            case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: break;
            case 0xA0: case 0xA1: case 0xA2: case 0xA3: return 0;   /* moffs: interpreter */
            case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
            case 0xAC: case 0xAD: case 0xAE: case 0xAF: break;
            case 0xA8: imm = I_B; break;
            case 0xA9: imm = I_Z; break;
            case 0xC2: imm = I_W; *ends = true; break;
            case 0xC3: case 0xCC: case 0xF4: *ends = true; break;
            case 0xC8: imm = I_WB; break;
            case 0xC9: case 0xD7: case 0xF5: case 0xF8: case 0xF9: case 0xFC: case 0xFD: break;
            case 0xCD: imm = I_B; *ends = true; break;
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: modrm = true; break;
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xEB: imm = I_B; *ends = true; break;
            case 0xE8: case 0xE9: imm = I_Z; *ends = true; break;
            case 0xF6: case 0xF7: {
                modrm = true;
                const unsigned reg = (p[0] >> 3) & 7u;
                if (reg <= 1u) imm = op == 0xF6 ? I_B : I_Z;
                break;
            }
            case 0xFE: modrm = true; break;
            case 0xFF: {
                modrm = true;
                const unsigned reg = (p[0] >> 3) & 7u;
                if (reg >= 2u && reg <= 5u) *ends = true;
                break;
            }
            default:
                return 0;
            }
        }
    }
    int n = (int)(p - p0);
    if (modrm) n += modrm_len(p);
    return n + imm_size(imm, p66, rexw);
}

/* ---- the fast decoder ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    uint8_t rex, p66, seg, riprel;
    uint8_t mod, reg, rm;
} dec_t;

/* ModRM (+SIB, disp) into u's EA fields; reg/rm with REX extension */
static void dec_modrm(dec_t *d, xb_uop_t *u)
{
    const uint8_t m = *d->p++;
    d->mod = m >> 6;
    d->reg = (uint8_t)(((m >> 3) & 7u) | ((d->rex & 4u) ? 8u : 0u));
    const uint8_t rm = m & 7u;
    u->base = NOREG;
    u->index = NOREG;
    u->scale = 0;
    u->disp = 0;
    d->riprel = 0;
    if (d->mod == 3u) {
        d->rm = (uint8_t)(rm | ((d->rex & 1u) ? 8u : 0u));
        return;
    }
    if (rm == 4u) {
        const uint8_t sib = *d->p++;
        const uint8_t base = sib & 7u, idx = (uint8_t)(((sib >> 3) & 7u) | ((d->rex & 2u) ? 8u : 0u));
        u->scale = sib >> 6;
        if (idx != 4u) u->index = idx;
        if (base == 5u && d->mod == 0u) {
            int32_t v;
            __builtin_memcpy(&v, d->p, 4);
            d->p += 4;
            u->disp = v;
        } else {
            u->base = (uint8_t)(base | ((d->rex & 1u) ? 8u : 0u));
        }
    } else if (rm == 5u && d->mod == 0u) {
        int32_t v;
        __builtin_memcpy(&v, d->p, 4);
        d->p += 4;
        u->disp = v;
        d->riprel = 1;
        return;
    } else {
        u->base = (uint8_t)(rm | ((d->rex & 1u) ? 8u : 0u));
    }
    if (d->mod == 1u) {
        u->disp += (int8_t)*d->p++;
    } else if (d->mod == 2u) {
        int32_t v;
        __builtin_memcpy(&v, d->p, 4);
        d->p += 4;
        u->disp += v;
    }
}

static uint64_t dec_imm(dec_t *d, int n)
{
    uint64_t v = 0;
    switch (n) {
    case 1: v = (uint64_t)(int64_t)(int8_t)d->p[0]; break;
    case 2: { int16_t x; __builtin_memcpy(&x, d->p, 2); v = (uint64_t)(int64_t)x; break; }
    case 4: { int32_t x; __builtin_memcpy(&x, d->p, 4); v = (uint64_t)(int64_t)x; break; }
    default: __builtin_memcpy(&v, d->p, 8); break;
    }
    d->p += n;
    return v;
}

/* Build a fast uop for the instruction at p. Returns its length, or 0 when
 * it has no fast form (it then becomes a fallback uop). */
static int fast_decode(const uint8_t *p0, uint64_t pc, xb_uop_t *u)
{
    dec_t d;
    d.p = p0;
    d.rex = 0;
    d.p66 = 0;
    d.seg = 0;
    uint8_t rep = 0;
    for (int k = 0; k < 6; ++k) {
        const uint8_t b = *d.p;
        if (b == 0x66) { d.p66 = 1; ++d.p; continue; }
        if (b == 0x64) { d.seg = 4; ++d.p; continue; }
        if (b == 0x65) { d.seg = 5; ++d.p; continue; }
        if (b == 0x2E || b == 0x3E) { ++d.p; continue; }
        if (b == 0xF3 || b == 0xF2) { rep = b; ++d.p; continue; }
        if (b == 0xF0 || b == 0x67 || b == 0x36 || b == 0x26) return 0;
        break;
    }
    if ((*d.p & 0xF0u) == 0x40u) d.rex = *d.p++;
    const uint8_t op = *d.p++;
    const int sz = (d.rex & 8u) ? 8 : (d.p66 ? 2 : 4);
    memset(u, 0, sizeof(*u));
    u->pc = pc;
    u->sz = (uint8_t)sz;
    u->rex = d.rex != 0u;
    u->seg = d.seg;
    u->base = NOREG;
    u->index = NOREG;

    if (op == 0x0F) {
        const uint8_t o2 = d.p[0];
        const bool sse = (o2 >= 0x10 && o2 <= 0x17) || (o2 >= 0x28 && o2 <= 0x2F) ||
                         (o2 >= 0x50 && o2 <= 0x7F && o2 != 0x77 && (o2 < 0x78 || o2 > 0x7B)) ||
                         (o2 >= 0xC2 && o2 <= 0xC6) || (o2 >= 0xD0 && o2 <= 0xFE);
        if (sse) {
            ++d.p;
            dec_modrm(&d, u);
            u->rexb = d.rex;
            u->pfx66 = d.p66;
            u->rep = rep;
            u->mod = d.mod;
            u->dst = d.reg;
            u->src = d.rm;
            u->op = o2;
            u->imm = x86_sse_has_imm8(o2) ? *d.p++ : 0u;
            const bool none = !d.p66 && !rep, p66 = d.p66 && !rep, f3 = rep == 0xF3;
            const bool reg = d.mod == 3u;
            u->fn = h_sse;
            if ((o2 == 0x6F && (p66 || f3)) || ((o2 == 0x28 || o2 == 0x10) && (none || p66))) {
                u->fn = reg ? h_xmov_rr : h_xld16;
            } else if ((o2 == 0x7F && (p66 || f3)) || ((o2 == 0x29 || o2 == 0x11) && (none || p66))) {
                if (reg) { u->fn = h_xmov_rr; u->dst = d.rm; u->src = d.reg; }
                else { u->fn = h_xst16; u->src = d.reg; }
            } else if ((p66 && (o2 == 0xEF || o2 == 0xEB || o2 == 0xDB || o2 == 0xDF)) ||
                       ((none || p66) && o2 >= 0x54 && o2 <= 0x57)) {
                u->op = (o2 == 0xEF || o2 == 0x57) ? 3 : (o2 == 0xEB || o2 == 0x56) ? 2 :
                        (o2 == 0xDF || o2 == 0x55) ? 1 : 0;
                u->fn = reg ? h_xlogic_r : h_xlogic_m;
            } else if (p66 && o2 == 0x74) {
                u->fn = reg ? h_pcmpeqb_r : h_pcmpeqb_m;
            } else if (p66 && o2 == 0xD7 && reg) {
                u->fn = h_pmovmskb;
            }
            goto done;
        }
    }

    if (rep) {
        /* F3 only in its NOP-like uses: ENDBR64, PAUSE, REP RET */
        if (rep == 0xF3) {
            if (op == 0x0F && d.p[0] == 0x1E && d.p[1] == 0xFA) { d.p += 2; u->fn = h_nop; goto done; }
            if (op == 0x90) { u->fn = h_nop; goto done; }
            if (op == 0xC3) { u->fn = h_ret; u->imm = 0; goto done; }
        }
        return 0;
    }

    if (op < 0x40 && (op & 7u) < 6u) {
        const unsigned aop = op >> 3;
        const int s = (op & 1u) ? sz : 1;
        u->sz = (uint8_t)s;
        u->op = (uint8_t)aop;
        switch (op & 7u) {
        case 0: case 1:
            dec_modrm(&d, u);
            if (d.mod == 3u) { u->fn = h_alu_rr; u->dst = d.rm; u->src = d.reg; }
            else { u->fn = h_alu_mr; u->src = d.reg; }
            break;
        case 2: case 3:
            dec_modrm(&d, u);
            u->dst = d.reg;
            if (d.mod == 3u) { u->fn = h_alu_rr; u->src = d.rm; }
            else u->fn = h_alu_rm;
            break;
        case 4:
            u->fn = h_alu_ri; u->dst = RAX; u->imm = dec_imm(&d, 1);
            break;
        default:
            u->fn = h_alu_ri; u->dst = RAX; u->imm = dec_imm(&d, s == 2 ? 2 : 4);
            break;
        }
        goto done;
    }

    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (d.p66) return 0;
        u->fn = h_push_r; u->src = (uint8_t)((op & 7u) | ((d.rex & 1u) ? 8u : 0u));
        goto done;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (d.p66) return 0;
        u->fn = h_pop_r; u->dst = (uint8_t)((op & 7u) | ((d.rex & 1u) ? 8u : 0u));
        goto done;
    case 0x63:
        dec_modrm(&d, u);
        u->dst = d.reg; u->op = 4; u->imm = (d.rex & 8u) ? 1u : 0u;
        if (d.mod == 3u) { u->fn = h_movx_r; u->src = d.rm; } else u->fn = h_movx_m;
        goto done;
    case 0x68: case 0x6A:
        if (d.p66) return 0;
        u->fn = h_push_i; u->imm = dec_imm(&d, op == 0x6A ? 1 : 4);
        goto done;
    case 0x69: case 0x6B:
        dec_modrm(&d, u);
        u->dst = d.reg;
        u->imm = dec_imm(&d, op == 0x6B ? 1 : (sz == 2 ? 2 : 4));
        if (d.mod == 3u) { u->fn = h_imul_rri; u->src = d.rm; } else u->fn = h_imul_rmi;
        goto done;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int64_t rel = (int8_t)*d.p++;
        u->fn = h_jcc; u->op = op & 15u;
        u->next = pc + (uint64_t)(d.p - p0);
        u->imm = u->next + (uint64_t)rel;
        goto done;
    }
    case 0x80: case 0x81: case 0x83: {
        const int s = op == 0x80 ? 1 : sz;
        u->sz = (uint8_t)s;
        dec_modrm(&d, u);
        u->op = d.reg & 7u;
        u->imm = dec_imm(&d, op == 0x81 ? (s == 2 ? 2 : 4) : 1);
        if (d.mod == 3u) { u->fn = h_alu_ri; u->dst = d.rm; } else u->fn = h_alu_mi;
        goto done;
    }
    case 0x84: case 0x85: {
        u->sz = (uint8_t)(op == 0x84 ? 1 : sz);
        u->op = 4;
        dec_modrm(&d, u);
        if (d.mod == 3u) { u->fn = h_test_rr; u->dst = d.rm; u->src = d.reg; }
        else { u->fn = h_test_mr; u->src = d.reg; }
        goto done;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B:
        u->sz = (uint8_t)((op & 1u) ? sz : 1);
        dec_modrm(&d, u);
        if (op <= 0x89) {
            if (d.mod == 3u) { u->fn = h_mov_rr; u->dst = d.rm; u->src = d.reg; }
            else { u->fn = h_mov_store; u->src = d.reg; }
        } else {
            u->dst = d.reg;
            if (d.mod == 3u) { u->fn = h_mov_rr; u->src = d.rm; } else u->fn = h_mov_load;
        }
        goto done;
    case 0x8D:
        dec_modrm(&d, u);
        if (d.mod == 3u) return 0;
        u->fn = h_lea; u->dst = d.reg; u->seg = 0;
        goto done;
    case 0x90:
        if (d.rex & 1u) { u->fn = h_xchg_rr; u->dst = RAX; u->src = R8; goto done; }
        u->fn = h_nop;
        goto done;
    case 0x98: case 0x99:
        u->fn = h_cdq; u->op = op & 1u;
        goto done;
    case 0xA8: case 0xA9:
        u->sz = (uint8_t)(op == 0xA8 ? 1 : sz);
        u->fn = h_test_ri; u->dst = RAX;
        u->imm = dec_imm(&d, op == 0xA8 ? 1 : (sz == 2 ? 2 : 4));
        goto done;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        u->sz = 1;
        u->fn = h_mov_ri; u->dst = (uint8_t)((op & 7u) | ((d.rex & 1u) ? 8u : 0u));
        u->imm = (uint8_t)dec_imm(&d, 1);
        goto done;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        u->fn = h_mov_ri; u->dst = (uint8_t)((op & 7u) | ((d.rex & 1u) ? 8u : 0u));
        u->imm = dec_imm(&d, sz);
        if (sz == 4) u->imm = (uint32_t)u->imm;
        goto done;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const int s = (op & 1u) ? sz : 1;
        u->sz = (uint8_t)s;
        dec_modrm(&d, u);
        if (d.mod != 3u) return 0;
        u->dst = d.rm; u->op = d.reg & 7u;
        if (op >= 0xD2) u->fn = h_shift_rc;
        else { u->fn = h_shift_ri; u->imm = op <= 0xC1 ? (uint8_t)*d.p++ : 1u; }
        goto done;
    }
    case 0xC2:
        u->fn = h_ret; u->imm = (uint16_t)dec_imm(&d, 2);
        goto done;
    case 0xC3:
        u->fn = h_ret; u->imm = 0;
        goto done;
    case 0xC6: case 0xC7: {
        const int s = op == 0xC6 ? 1 : sz;
        u->sz = (uint8_t)s;
        dec_modrm(&d, u);
        if ((d.reg & 7u) != 0u) return 0;
        u->imm = dec_imm(&d, s == 8 ? 4 : s);
        if (d.mod == 3u) { u->fn = h_mov_ri; u->dst = d.rm; } else u->fn = h_mov_mi;
        goto done;
    }
    case 0xF6: case 0xF7: {
        const int s = op == 0xF6 ? 1 : sz;
        u->sz = (uint8_t)s;
        dec_modrm(&d, u);
        const unsigned sub = d.reg & 7u;
        if (sub <= 1u) {
            u->imm = dec_imm(&d, s == 1 ? 1 : (s == 2 ? 2 : 4));
            if (d.mod == 3u) { u->fn = h_test_ri; u->dst = d.rm; } else u->fn = h_test_mi;
            goto done;
        }
        if (d.mod == 3u && sub == 2u) { u->fn = h_not_r; u->dst = d.rm; goto done; }
        if (d.mod == 3u && sub == 3u) { u->fn = h_neg_r; u->dst = d.rm; goto done; }
        return 0;
    }
    case 0x86: case 0x87:
        u->sz = (uint8_t)(op == 0x86 ? 1 : sz);
        dec_modrm(&d, u);
        if (d.mod != 3u) return 0;      /* memory: implicitly locked */
        u->fn = h_xchg_rr; u->dst = d.rm; u->src = d.reg;
        goto done;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        u->fn = h_xchg_rr; u->dst = RAX; u->src = (uint8_t)((op & 7u) | ((d.rex & 1u) ? 8u : 0u));
        goto done;
    case 0xC9:
        if (d.p66) return 0;
        u->fn = h_leave;
        goto done;
    case 0xE8: case 0xE9: case 0xEB: {
        int64_t rel = op == 0xEB ? (int8_t)*d.p++ : (int64_t)dec_imm(&d, 4);
        u->fn = op == 0xE8 ? h_call : h_jmp;
        u->next = pc + (uint64_t)(d.p - p0);
        u->imm = u->next + (uint64_t)rel;
        goto done;
    }
    case 0xFE: case 0xFF: {
        const int s = op == 0xFE ? 1 : sz;
        u->sz = (uint8_t)s;
        dec_modrm(&d, u);
        const unsigned sub = d.reg & 7u;
        if (sub <= 1u) {
            u->op = (uint8_t)sub;
            if (d.mod == 3u) { u->fn = h_incdec_r; u->dst = d.rm; } else u->fn = h_incdec_m;
            goto done;
        }
        if (op == 0xFE || d.p66) return 0;
        if (sub == 2u) {
            if (d.mod == 3u) { u->fn = h_call_r; u->src = d.rm; } else u->fn = h_call_m;
            goto done;
        }
        if (sub == 4u) {
            if (d.mod == 3u) { u->fn = h_jmp_r; u->src = d.rm; } else u->fn = h_jmp_m;
            goto done;
        }
        return 0;
    }
    case 0x0F: {
        const uint8_t o2 = *d.p++;
        if (o2 >= 0x80 && o2 <= 0x8F) {
            int64_t rel = (int64_t)dec_imm(&d, 4);
            u->fn = h_jcc; u->op = o2 & 15u;
            u->next = pc + (uint64_t)(d.p - p0);
            u->imm = u->next + (uint64_t)rel;
            goto done;
        }
        if (o2 >= 0x40 && o2 <= 0x4F) {
            dec_modrm(&d, u);
            u->dst = d.reg; u->op = o2 & 15u;
            if (d.mod == 3u) { u->fn = h_cmov_r; u->src = d.rm; } else u->fn = h_cmov_m;
            goto done;
        }
        if (o2 >= 0x90 && o2 <= 0x9F) {
            dec_modrm(&d, u);
            if (d.mod != 3u) return 0;
            u->fn = h_setcc_r; u->dst = d.rm; u->op = o2 & 15u;
            goto done;
        }
        switch (o2) {
        case 0x1E: case 0x1F: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
            dec_modrm(&d, u);
            u->fn = h_nop;
            goto done;
        case 0xBC: case 0xBD:
            dec_modrm(&d, u);
            u->dst = d.reg; u->op = o2 == 0xBD;
            if (d.mod == 3u) { u->fn = h_bsx_r; u->src = d.rm; } else u->fn = h_bsx_m;
            goto done;
        case 0xAF:
            dec_modrm(&d, u);
            u->dst = d.reg;
            if (d.mod == 3u) { u->fn = h_imul_rr; u->src = d.rm; } else u->fn = h_imul_rm;
            goto done;
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:
            dec_modrm(&d, u);
            u->dst = d.reg;
            u->op = (o2 & 1u) ? 2 : 1;
            u->imm = o2 >= 0xBE ? 1u : 0u;
            if (d.mod == 3u) { u->fn = h_movx_r; u->src = d.rm; } else u->fn = h_movx_m;
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
        const int zi = szi(u->sz);
        if (u->fn == h_alu_rr) u->fn = g_arr[u->op][zi];
        else if (u->fn == h_alu_ri) u->fn = g_ari[u->op][zi];
        else if (u->fn == h_alu_rm) u->fn = g_arm[u->op][zi];
        else if (u->fn == h_alu_mr) u->fn = g_amr[u->op][zi];
        else if (u->fn == h_alu_mi) u->fn = g_ami[u->op][zi];
        else if (u->fn == h_test_rr) u->fn = g_arr[8][zi];
        else if (u->fn == h_test_ri) u->fn = g_ari[8][zi];
        else if (u->fn == h_test_mr) u->fn = g_amr[8][zi];
        else if (u->fn == h_test_mi) u->fn = g_ami[8][zi];
        else if (u->fn == h_mov_rr) u->fn = g_mrr[zi];
        else if (u->fn == h_mov_ri) u->fn = g_mri[zi];
        else if (u->fn == h_mov_load) u->fn = g_mld[zi];
        else if (u->fn == h_mov_store) u->fn = g_mst[zi];
        else if (u->fn == h_mov_mi) u->fn = g_mmi[zi];
        else if (u->fn == h_lea && u->sz == 8u) u->fn = h_lea64;
        else if (u->fn == h_lea && u->sz == 4u) u->fn = h_lea32;
    }
    {
        const int len = (int)(d.p - p0);
        u->next = pc + (uint64_t)len;
        if (d.riprel) {
            u->disp += (int64_t)u->next;
        }
        return len;
    }
}

/* ---- blocks ------------------------------------------------------------------------------ */

static xb_cache_t *cache_of(gthread_t *t)
{
    xb_cache_t *bc = t->engine;
    if (XL_UNLIKELY(bc == NULL)) {
        bc = xl_map(sizeof(xb_cache_t));
        if (bc == NULL) {
            xl_fatal("out of memory for the translation cache");
        }
        bc->arena = xl_map(XB_ARENA_SIZE);
        if (bc->arena == NULL) {
            xl_fatal("out of memory for the translation cache");
        }
        bc->used = 0;
        t->engine = bc;
    }
    return bc;
}

static inline uint32_t slot_of(uint64_t pc)
{
    return (uint32_t)((pc ^ (pc >> XB_TABLE_BITS) ^ (pc >> 28)) & (XB_TABLE_SIZE - 1u));
}

static xb_block_t *translate(xb_cache_t *bc, uint64_t pc)
{
    const size_t worst = sizeof(xb_block_t) + (XB_MAX_UOPS + 1u) * sizeof(xb_uop_t) +
                         XB_MAX_UOPS * 16u + 16u;
    if (bc->used + worst > XB_ARENA_SIZE) {
        memset(bc->table, 0, sizeof(bc->table));
        bc->used = 0;
        bc->gen++;
    }
    xb_block_t *b = (xb_block_t *)(void *)(bc->arena + bc->used);
    b->pc = pc;
    b->gen = bc->gen;
    b->nsegs = 0;
    uint64_t cur = pc, seg_start = pc;
    uint32_t n = 0;
#define CLOSE_SEG(endpc) do { if ((endpc) > seg_start) { b->seg[b->nsegs].start = seg_start; \
        b->seg[b->nsegs].len = (uint32_t)((endpc) - seg_start); ++b->nsegs; } } while (0)
    while (n < XB_MAX_UOPS) {
        xb_uop_t *u = &b->uops[n];
        const uint8_t *p = (const uint8_t *)(uintptr_t)cur;
        bool ends = false;
        int len = fast_decode(p, cur, u);
        if (len > 0) {
            /* room to start another code range? */
            const bool can_follow = b->nsegs + 2u <= XB_MAX_SEGS && n + 2u < XB_MAX_UOPS;
            if (u->fn == h_jmp && can_follow) {
                /* follow: no uop at all */
                CLOSE_SEG(u->next);
                cur = seg_start = u->imm;
                continue;
            }
            if (u->fn == h_call && can_follow) {
                u->fn = h_call_follow;
                CLOSE_SEG(u->next);
                cur = seg_start = u->imm;
                ++n;
                continue;
            }
            if (u->fn == h_jcc && can_follow) {
                u->fn = h_side;
                if (u->imm <= u->pc) {
                    /* backward (a loop): stay in the loop, leave when it
                     * is not taken */
                    const uint64_t target = u->imm;
                    u->op ^= 1u;
                    u->imm = u->next;
                    CLOSE_SEG(u->next);
                    cur = seg_start = target;
                } else {
                    /* forward: fall through, leave when taken */
                    cur = u->next;
                }
                ++n;
                continue;
            }
            ends = u->fn == h_jcc || u->fn == h_jmp || u->fn == h_call || u->fn == h_ret ||
                   u->fn == h_call_r || u->fn == h_call_m || u->fn == h_jmp_r || u->fn == h_jmp_m;
        } else {
            len = x86_insn_len(p, &ends);
            memset(u, 0, sizeof(*u));
            u->fn = h_fallback;
            u->pc = cur;
            if (len <= 0) {
                /* not decodable here: let the interpreter decide (it
                 * raises SIGILL for real undefined opcodes) */
                u->next = cur + 1u;
                cur = u->next;
                ++n;
                break;
            }
            u->next = cur + (uint64_t)len;
        }
        cur = u->next;
        ++n;
        if (ends) {
            break;
        }
    }
    CLOSE_SEG(cur);
#undef CLOSE_SEG
    b->nuops = n;
    b->end_pc = cur;
    memset(&b->uops[n], 0, sizeof(xb_uop_t));
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
    b->link_pc[0] = b->link_pc[1] = 1;     /* never a guest address */
    b->link[0] = b->link[1] = NULL;
    b->link_next = 0;
    bc->used += (sizeof(xb_block_t) + (n + 1u) * sizeof(xb_uop_t) + off + 15u) & ~(size_t)15u;
    xb_set_t *set = &bc->table[slot_of(pc)];
    if (set->way[0] != NULL && set->way[0]->pc == pc) {
        set->way[0] = b;                /* retranslated in place */
    } else {
        set->way[1] = set->way[0];
        set->way[0] = b;
    }
    return b;
}

static inline bool code_unchanged(const xb_block_t *b)
{
    for (uint32_t k = 0; k < b->nsegs; ++k) {
        const uint8_t *g = (const uint8_t *)(uintptr_t)b->seg[k].start;
        const uint8_t *m = b->code + b->seg[k].off;
        const uint32_t n = b->seg[k].len;
        uint32_t i = 0;
        for (; i + 8u <= n; i += 8u) {
            uint64_t x, y;
            __builtin_memcpy(&x, g + i, 8);
            __builtin_memcpy(&y, m + i, 8);
            if (x != y) return false;
        }
        for (; i < n; ++i) {
            if (g[i] != m[i]) return false;
        }
    }
    return true;
}

static bool code_readonly(const xb_block_t *b)
{
    for (uint32_t k = 0; k < b->nsegs; ++k) {
        if (!xl_code_is_readonly(b->seg[k].start, b->seg[k].start + b->seg[k].len)) {
            return false;
        }
    }
    return true;
}

/* May the block run? Code known read-only since the last change to the
 * address space needs no compare; anything else is compared with guest
 * memory (and its read-only status looked up once per change). */
static inline bool block_valid(xb_block_t *b)
{
    const uint64_t e = g_xl_code_epoch;
    if (XL_LIKELY(b->ro_epoch == e)) {
        return true;
    }
    if (!code_unchanged(b)) {
        return false;
    }
    if (b->seen_epoch != e) {
        b->seen_epoch = e;
        if (code_readonly(b)) {
            b->ro_epoch = e;
        }
    }
    return true;
}

int x86_run_blocks(gthread_t *t)
{
    x86cpu_t *c = X86(t);
    xb_cache_t *bc = cache_of(t);
    xb_block_t *prev = NULL;
    for (;;) {
        if (XL_UNLIKELY(t->sig_pending_flag)) {
            return XL_EV_SIGNAL;
        }
        const uint64_t pc = c->rip;
        xb_block_t *b = NULL;
        const bool linkable = prev != NULL && prev->gen == bc->gen;
        if (linkable) {
            if (prev->link_pc[0] == pc) b = prev->link[0];
            else if (prev->link_pc[1] == pc) b = prev->link[1];
        }
        if (b == NULL) {
            xb_set_t *set = &bc->table[slot_of(pc)];
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
            if (b != NULL && linkable) {
                const uint32_t k = prev->link_next;
                prev->link_pc[k] = pc;
                prev->link[k] = b;
                prev->link_next = k ^ 1u;
            }
        }
        if (XL_UNLIKELY(b == NULL || !block_valid(b))) {
            b = translate(bc, pc);
            XB_PROF(++g_xb_trans);
            if (prev != NULL && prev->gen == bc->gen) {
                const uint32_t k = prev->link_next;
                prev->link_pc[k] = pc;
                prev->link[k] = b;
                prev->link_next = k ^ 1u;
            }
        }
        XB_PROF(++g_xb_entries);
        XB_PROF(g_xb_uops += b->nuops);
        const int r = b->uops[0].fn(c, b->uops);
        if (XL_UNLIKELY(r != XB_EXIT)) {
            return r;
        }
        prev = b;
    }
}
