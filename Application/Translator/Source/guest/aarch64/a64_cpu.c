/*
 * AArch64 interpreter: the integer, branch, system and load/store
 * encodings, and the run loop. One instruction word at a time, straight
 * from guest memory (nothing to invalidate when the guest writes or maps
 * code). cpu->pc stays on the instruction until it completes, so a fault
 * inside it is reported there.
 */

#include "a64_ops.h"
#include "host.h"

#define PC_SET (-1)   /* handler set cpu->pc itself */

/* ---- registers ------------------------------------------------------------- */

int a64_fault(gthread_t *t, int sig, int code, uint64_t addr)
{
    xl_signal_raise_sync(t, sig, code, addr);
    return XL_EV_SIGNAL;
}

/* ---- data processing: immediate ----------------------------------------------- */

static int dp_imm(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool sf = BIT(w, 31);
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5);

    if ((w & 0x1F000000u) == 0x10000000u) {         /* ADR, ADRP */
        int64_t imm = sext(((uint64_t)BITS(w, 23, 5) << 2) | BITS(w, 30, 29), 21);
        if (BIT(w, 31)) {
            wx(c, rd, (c->pc & ~0xFFFULL) + ((uint64_t)imm << 12), true);
        } else {
            wx(c, rd, c->pc + (uint64_t)imm, true);
        }
        return 0;
    }
    if ((w & 0x1F800000u) == 0x11000000u) {         /* ADD/SUB immediate */
        uint64_t imm = BITS(w, 21, 10);
        if (BIT(w, 22)) imm <<= 12;
        const bool sub = BIT(w, 30), s = BIT(w, 29);
        uint64_t a = xsp(c, rn);
        uint64_t r = sub ? add_carry(c, a, ~imm, 1, sf, s) : add_carry(c, a, imm, 0, sf, s);
        if (s) wx(c, rd, r, sf); else wsp(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1F800000u) == 0x12000000u) {         /* logical immediate */
        uint64_t imm;
        if (!decode_bitmask(BIT(w, 22), BITS(w, 15, 10), BITS(w, 21, 16), sf, &imm)) {
            return A64_UD(t);
        }
        uint64_t a = xr(c, rn), r;
        switch (BITS(w, 30, 29)) {
        case 0: r = a & imm; wsp(c, rd, r, sf); break;
        case 1: r = a | imm; wsp(c, rd, r, sf); break;
        case 2: r = a ^ imm; wsp(c, rd, r, sf); break;
        default: r = a & imm; logic_flags(c, r, sf); wx(c, rd, r, sf); break;
        }
        return 0;
    }
    if ((w & 0x1F800000u) == 0x12800000u) {         /* MOVN, MOVZ, MOVK */
        unsigned hw = BITS(w, 22, 21);
        if (!sf && hw > 1u) return A64_UD(t);
        uint64_t imm = (uint64_t)BITS(w, 20, 5) << (16u * hw);
        switch (BITS(w, 30, 29)) {
        case 0: wx(c, rd, ~imm, sf); break;
        case 2: wx(c, rd, imm, sf); break;
        case 3: wx(c, rd, (xr(c, rd) & ~(0xFFFFULL << (16u * hw))) | imm, sf); break;
        default: return A64_UD(t);
        }
        return 0;
    }
    if ((w & 0x1F800000u) == 0x13000000u) {         /* SBFM, BFM, UBFM */
        const unsigned opc = BITS(w, 30, 29);
        const unsigned immr = BITS(w, 21, 16), imms = BITS(w, 15, 10);
        const unsigned bits = sf ? 64u : 32u;
        if (opc == 3u || BIT(w, 22) != (unsigned)sf || immr >= bits || imms >= bits) {
            return A64_UD(t);
        }
        uint64_t src = xr(c, rn), r;
        if (imms >= immr) {
            unsigned width = imms - immr + 1u;
            uint64_t field = (src >> immr) & ones(width);
            if (opc == 0u) r = (uint64_t)sext(field, width);
            else if (opc == 2u) r = field;
            else r = (xr(c, rd) & ~ones(width)) | field;
        } else {
            unsigned width = imms + 1u, sh = bits - immr;
            uint64_t field = src & ones(width);
            if (opc == 0u) r = (uint64_t)sext(field, width) << sh;
            else if (opc == 2u) r = field << sh;
            else r = (xr(c, rd) & ~(ones(width) << sh)) | (field << sh);
        }
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1F800000u) == 0x13800000u) {         /* EXTR */
        const unsigned lsb = BITS(w, 15, 10);
        uint64_t hi = xr(c, rn), lo = xr(c, BITS(w, 20, 16)), r;
        if (sf) {
            r = lsb == 0u ? lo : (lo >> lsb) | (hi << (64u - lsb));
        } else {
            if (lsb >= 32u) return A64_UD(t);
            uint64_t cat = ((hi & 0xFFFFFFFFu) << 32) | (lo & 0xFFFFFFFFu);
            r = (uint32_t)(cat >> lsb);
        }
        wx(c, rd, r, sf);
        return 0;
    }
    return A64_UD(t);
}

/* ---- branches, exceptions, system ---------------------------------------------- */

static uint64_t host_counter(void)
{
    struct { int64_t s, ns; } ts = { 0, 0 };
    (void)host_clock_gettime(1, &ts);
    /* a 1 GHz counter (CNTFRQ_EL0 reports 1000000000) */
    return (uint64_t)ts.s * 1000000000ULL + (uint64_t)ts.ns;
}

/* MRS/MSR: the EL0-accessible system registers */
static int sysreg(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool read = BIT(w, 21);
    const unsigned key = BITS(w, 20, 5);        /* o0:op1:CRn:CRm:op2 */
    const unsigned rt = w & 31u;
    /* encodings: (op0-2):op1:CRn:CRm:op2 = 1:3:CRn:CRm:op2 for the EL0 ones */
#define SR(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
    uint64_t v = 0;
    switch (key) {
    case SR(3, 3, 13, 0, 2):    /* TPIDR_EL0 */
        if (read) v = c->tpidr; else c->tpidr = xr(c, rt);
        break;
    case SR(3, 3, 13, 0, 3):    /* TPIDRRO_EL0 */
        if (!read) return A64_UD(t);
        break;
    case SR(3, 3, 4, 4, 0):     /* FPCR */
        if (read) v = c->fpcr;
        else { c->fpcr = (uint32_t)xr(c, rt); a64_fpcr_apply(c); }
        break;
    case SR(3, 3, 4, 4, 1):     /* FPSR */
        if (read) { a64_fpsr_collect(c); v = c->fpsr; }
        else { c->fpsr = (uint32_t)xr(c, rt); a64_fpcr_apply(c); }
        break;
    case SR(3, 3, 4, 2, 0):     /* NZCV */
        if (read) v = c->nzcv; else c->nzcv = (uint32_t)xr(c, rt) & 0xF0000000u;
        break;
    case SR(3, 3, 0, 0, 1):     /* CTR_EL0: 64-byte lines, VIPT */
        if (!read) return A64_UD(t);
        v = 0x8444C004u;
        break;
    case SR(3, 3, 0, 0, 7):     /* DCZID_EL0: DC ZVA prohibited */
        if (!read) return A64_UD(t);
        v = 0x10u;
        break;
    case SR(3, 3, 14, 0, 0):    /* CNTFRQ_EL0 */
        if (!read) return A64_UD(t);
        v = 1000000000u;
        break;
    case SR(3, 3, 14, 0, 1):    /* CNTPCT_EL0 */
    case SR(3, 3, 14, 0, 2):    /* CNTVCT_EL0 */
    case SR(3, 3, 14, 0, 5):    /* CNTPCTSS_EL0 */
    case SR(3, 3, 14, 0, 6):    /* CNTVCTSS_EL0 */
        if (!read) return A64_UD(t);
        v = host_counter();
        break;
    default:
        return A64_UD(t);
    }
#undef SR
    if (read) wx(c, rt, v, true);
    return 0;
}

static int branch_sys(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    if ((w & 0x7C000000u) == 0x14000000u) {         /* B, BL */
        int64_t off = sext(BITS(w, 25, 0), 26) * 4;
        if (BIT(w, 31)) c->x[30] = c->pc + 4u;
        c->pc += (uint64_t)off;
        return PC_SET;
    }
    if ((w & 0xFF000010u) == 0x54000000u) {         /* B.cond */
        if (a64_cond(c->nzcv, w & 15u)) {
            c->pc += (uint64_t)(sext(BITS(w, 23, 5), 19) * 4);
            return PC_SET;
        }
        return 0;
    }
    if ((w & 0x7E000000u) == 0x34000000u) {         /* CBZ, CBNZ */
        uint64_t v = xr(c, w & 31u);
        if (!BIT(w, 31)) v = (uint32_t)v;
        if ((v == 0u) != (bool)BIT(w, 24)) {
            c->pc += (uint64_t)(sext(BITS(w, 23, 5), 19) * 4);
            return PC_SET;
        }
        return 0;
    }
    if ((w & 0x7E000000u) == 0x36000000u) {         /* TBZ, TBNZ */
        unsigned bit = (BIT(w, 31) << 5) | BITS(w, 23, 19);
        bool set = ((xr(c, w & 31u) >> bit) & 1u) != 0u;
        if (set == (bool)BIT(w, 24)) {
            c->pc += (uint64_t)(sext(BITS(w, 18, 5), 14) * 4);
            return PC_SET;
        }
        return 0;
    }
    if ((w & 0xFE1FFC1Fu) == 0xD61F0000u) {         /* BR, BLR, RET */
        unsigned opc = BITS(w, 24, 21);
        uint64_t target = xr(c, BITS(w, 9, 5));
        if (opc == 1u) c->x[30] = c->pc + 4u;
        else if (opc != 0u && opc != 2u) return A64_UD(t);
        c->pc = target;
        return PC_SET;
    }
    if ((w & 0xFFE0001Fu) == 0xD4000001u) {         /* SVC */
        c->pc += 4u;
        return XL_EV_SYSCALL;
    }
    if ((w & 0xFFE0001Fu) == 0xD4200000u) {         /* BRK */
        return a64_fault(t, XL_SIGTRAP, XL_TRAP_BRKPT, c->pc);
    }
    if ((w & 0xFFFFF01Fu) == 0xD503201Fu) {         /* hints: NOP, YIELD, PAC, BTI */
        return 0;
    }
    if ((w & 0xFFFFF01Fu) == 0xD503301Fu) {         /* CLREX, DSB, DMB, ISB, SB */
        unsigned op2 = BITS(w, 7, 5);
        if (op2 == 2u) c->excl_size = 0;
        else if (op2 == 4u || op2 == 5u) __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return 0;
    }
    if ((w & 0xFFD00000u) == 0xD5100000u) {         /* MSR/MRS (register) */
        return sysreg(t, c, w);
    }
    if ((w & 0xFFF80000u) == 0xD5080000u) {         /* SYS: DC/IC maintenance */
        unsigned op1 = BITS(w, 18, 16), crn = BITS(w, 15, 12);
        unsigned crm = BITS(w, 11, 8), op2 = BITS(w, 7, 5);
        if (op1 == 3u && crn == 7u && crm == 4u && op2 == 1u) {   /* DC ZVA */
            uint64_t a = xr(c, w & 31u) & ~63ULL;
            memset((void *)(uintptr_t)a, 0, 64);
            return 0;
        }
        if (crn == 7u) {
            return 0;       /* cache maintenance: coherent here */
        }
        return A64_UD(t);
    }
    return A64_UD(t);
}

/* ---- data processing: register -------------------------------------------------- */

static int dp_reg(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const bool sf = BIT(w, 31);
    const unsigned rd = w & 31u, rn = BITS(w, 9, 5), rm = BITS(w, 20, 16);

    if ((w & 0x1F000000u) == 0x0A000000u) {         /* logical (shifted register) */
        unsigned amount = BITS(w, 15, 10);
        if (!sf && amount >= 32u) return A64_UD(t);
        uint64_t b = shift_reg(xr(c, rm), BITS(w, 23, 22), amount, sf);
        if (BIT(w, 21)) b = ~b;
        uint64_t a = xr(c, rn), r;
        switch (BITS(w, 30, 29)) {
        case 0: r = a & b; break;
        case 1: r = a | b; break;
        case 2: r = a ^ b; break;
        default: r = a & b; logic_flags(c, r, sf); break;
        }
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1F200000u) == 0x0B000000u) {         /* ADD/SUB (shifted register) */
        unsigned amount = BITS(w, 15, 10), type = BITS(w, 23, 22);
        if (type == 3u || (!sf && amount >= 32u)) return A64_UD(t);
        uint64_t b = shift_reg(xr(c, rm), type, amount, sf);
        const bool sub = BIT(w, 30), s = BIT(w, 29);
        uint64_t r = sub ? add_carry(c, xr(c, rn), ~b, 1, sf, s) : add_carry(c, xr(c, rn), b, 0, sf, s);
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1F200000u) == 0x0B200000u) {         /* ADD/SUB (extended register) */
        unsigned sh = BITS(w, 12, 10);
        if (sh > 4u || BITS(w, 23, 22) != 0u) return A64_UD(t);
        uint64_t b = extend_reg(xr(c, rm), BITS(w, 15, 13), sh);
        const bool sub = BIT(w, 30), s = BIT(w, 29);
        uint64_t a = xsp(c, rn);
        uint64_t r = sub ? add_carry(c, a, ~b, 1, sf, s) : add_carry(c, a, b, 0, sf, s);
        if (s) wx(c, rd, r, sf); else wsp(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1FE0FC00u) == 0x1A000000u) {         /* ADC, ADCS, SBC, SBCS */
        uint64_t b = xr(c, rm);
        if (BIT(w, 30)) b = ~b;
        uint64_t cy = (c->nzcv & NZCV_C) ? 1u : 0u;
        wx(c, rd, add_carry(c, xr(c, rn), b, cy, sf, BIT(w, 29)), sf);
        return 0;
    }
    if ((w & 0x1FE00410u) == 0x1A400000u) {         /* CCMN, CCMP */
        if (!BIT(w, 29)) return A64_UD(t);
        if (a64_cond(c->nzcv, BITS(w, 15, 12))) {
            uint64_t b = BIT(w, 11) ? rm : xr(c, rm);
            if (BIT(w, 30)) (void)add_carry(c, xr(c, rn), ~b, 1, sf, true);
            else (void)add_carry(c, xr(c, rn), b, 0, sf, true);
        } else {
            c->nzcv = (w & 15u) << 28;
        }
        return 0;
    }
    if ((w & 0x1FE00000u) == 0x1A800000u) {         /* CSEL, CSINC, CSINV, CSNEG */
        if (BIT(w, 29) || BIT(w, 11)) return A64_UD(t);
        uint64_t r;
        if (a64_cond(c->nzcv, BITS(w, 15, 12))) {
            r = xr(c, rn);
        } else {
            r = xr(c, rm);
            unsigned k = (BIT(w, 30) << 1) | BIT(w, 10);
            if (k == 1u) r += 1u;
            else if (k == 2u) r = ~r;
            else if (k == 3u) r = (uint64_t)0 - r;
        }
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x5FE00000u) == 0x1AC00000u) {         /* data processing, 2 sources */
        uint64_t a = xr(c, rn), b = xr(c, rm), r;
        const unsigned bits = sf ? 64u : 32u;
        if (!sf) { a = (uint32_t)a; b = (uint32_t)b; }
        switch (BITS(w, 15, 10)) {
        case 2: r = b == 0u ? 0u : a / b; break;
        case 3:
            if (b == 0u) r = 0;
            else if (sf) r = ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? a : (uint64_t)((int64_t)a / (int64_t)b);
            else r = ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b);
            break;
        case 8: r = a << (b & (bits - 1u)); break;
        case 9: r = a >> (b & (bits - 1u)); break;
        case 10: r = shift_reg(a, 2, (unsigned)(b & (bits - 1u)), sf); break;
        case 11: r = shift_reg(a, 3, (unsigned)(b & (bits - 1u)), sf); break;
        default: return A64_UD(t);
        }
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x5FFF0000u) == 0x5AC00000u) {         /* data processing, 1 source */
        uint64_t a = xr(c, rn), r;
        if (!sf) a = (uint32_t)a;
        switch (BITS(w, 15, 10)) {
        case 0: {   /* RBIT */
            r = 0;
            for (unsigned k = 0; k < (sf ? 64u : 32u); ++k) r |= ((a >> k) & 1u) << ((sf ? 63u : 31u) - k);
            break;
        }
        case 1: {   /* REV16 */
            r = ((a & 0x00FF00FF00FF00FFULL) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFULL);
            break;
        }
        case 2:     /* REV32 (64-bit) / REV (32-bit) */
            if (sf) r = ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32) | __builtin_bswap32((uint32_t)a);
            else r = __builtin_bswap32((uint32_t)a);
            break;
        case 3:
            if (!sf) return A64_UD(t);
            r = __builtin_bswap64(a);
            break;
        case 4:     /* CLZ */
            r = a == 0u ? (sf ? 64u : 32u) : (uint64_t)(sf ? __builtin_clzll(a) : __builtin_clz((uint32_t)a));
            break;
        case 5: {   /* CLS */
            uint64_t x = sf ? a ^ (uint64_t)((int64_t)a >> 1) : (uint32_t)(a ^ (uint64_t)((int32_t)(uint32_t)a >> 1));
            if (sf) r = (x << 1) == 0u ? 63u : (uint64_t)__builtin_clzll(x << 1);
            else r = ((uint32_t)x << 1) == 0u ? 31u : (uint64_t)__builtin_clz((uint32_t)x << 1);
            break;
        }
        default:
            return A64_UD(t);
        }
        wx(c, rd, r, sf);
        return 0;
    }
    if ((w & 0x1F000000u) == 0x1B000000u) {         /* data processing, 3 sources */
        const unsigned op31 = BITS(w, 23, 21), ra = BITS(w, 14, 10);
        const bool o0 = BIT(w, 15);
        uint64_t a = xr(c, rn), b = xr(c, rm), acc = xr(c, ra), r;
        if (BITS(w, 30, 29) != 0u) return A64_UD(t);
        switch (op31) {
        case 0:
            r = o0 ? acc - a * b : acc + a * b;
            break;
        case 1: {
            if (!sf) return A64_UD(t);
            int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
            r = o0 ? acc - (uint64_t)p : acc + (uint64_t)p;
            break;
        }
        case 5: {
            if (!sf) return A64_UD(t);
            uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b;
            r = o0 ? acc - p : acc + p;
            break;
        }
        case 2:
            if (!sf || o0) return A64_UD(t);
            r = (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64);
            break;
        case 6:
            if (!sf || o0) return A64_UD(t);
            r = (uint64_t)(((unsigned __int128)a * b) >> 64);
            break;
        default:
            return A64_UD(t);
        }
        wx(c, rd, r, sf);
        return 0;
    }
    return A64_UD(t);
}

/* ---- loads and stores ------------------------------------------------------------ */

static xl_lock_t g_pair_lock;

void guest_fork_locks(int acquire)
{
    if (acquire) xl_lock(&g_pair_lock);
    else xl_unlock(&g_pair_lock);
}

static bool cas_n(uint64_t a, int n, uint64_t *exp, uint64_t des)
{
    if ((a & (uint64_t)(n - 1)) != 0u) {
        return false;
    }
    switch (n) {
    case 1: { uint8_t e = (uint8_t)*exp; bool ok = __atomic_compare_exchange_n((uint8_t *)(uintptr_t)a, &e, (uint8_t)des, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    case 2: { uint16_t e = (uint16_t)*exp; bool ok = __atomic_compare_exchange_n((uint16_t *)(uintptr_t)a, &e, (uint16_t)des, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    case 4: { uint32_t e = (uint32_t)*exp; bool ok = __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, &e, (uint32_t)des, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *exp = e; return ok; }
    default: return __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, exp, des, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

/* LDXR/STXR/LDAXR/STLXR (+ pairs), LDAR/STLR */
static int ldst_exclusive(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const unsigned size = BITS(w, 31, 30), rt = w & 31u, rn = BITS(w, 9, 5);
    const unsigned rs = BITS(w, 20, 16), rt2 = BITS(w, 14, 10);
    const bool o2 = BIT(w, 23), load = BIT(w, 22), o1 = BIT(w, 21);
    const int n = 1 << size;
    const uint64_t a = xsp(c, rn);

    if (o2) {
        if (o1) return A64_UD(t);                   /* CAS: LSE, not reported */
        if (load) {                                  /* LDAR, LDLAR */
            uint64_t v = ld_n(a, n);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            wx(c, rt, v, true);
        } else {                                     /* STLR, STLLR */
            __atomic_thread_fence(__ATOMIC_RELEASE);
            st_n(a, n, xr(c, rt));
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
        return 0;
    }
    if ((a & (uint64_t)((o1 ? 2 * n : n) - 1)) != 0u) {
        return a64_fault(t, XL_SIGBUS, XL_BUS_ADRALN, a);
    }
    if (load) {
        if (o1) {                                    /* LDXP, LDAXP */
            if (size < 2u) return A64_UD(t);
            xl_lock(&g_pair_lock);
            uint64_t v0 = ld_n(a, n), v1 = ld_n(a + (uint64_t)n, n);
            xl_unlock(&g_pair_lock);
            c->excl_val[0] = v0;
            c->excl_val[1] = v1;
            c->excl_size = (uint32_t)(2 * n);
            c->excl_addr = a;
            wx(c, rt, v0, true);
            wx(c, rt2, v1, true);
        } else {                                     /* LDXR, LDAXR */
            uint64_t v = ld_n(a, n);
            c->excl_val[0] = v;
            c->excl_size = (uint32_t)n;
            c->excl_addr = a;
            wx(c, rt, v, true);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return 0;
    }
    /* STXR / STXP: succeed only if memory still holds what LDXR saw */
    uint64_t status = 1;
    if (o1) {
        if (size < 2u) return A64_UD(t);
        if (c->excl_size == (uint32_t)(2 * n) && c->excl_addr == a) {
            xl_lock(&g_pair_lock);
            if (ld_n(a, n) == c->excl_val[0] && ld_n(a + (uint64_t)n, n) == c->excl_val[1]) {
                st_n(a, n, xr(c, rt));
                st_n(a + (uint64_t)n, n, xr(c, rt2));
                status = 0;
            }
            xl_unlock(&g_pair_lock);
        }
    } else if (c->excl_size == (uint32_t)n && c->excl_addr == a) {
        uint64_t exp = c->excl_val[0];
        if (cas_n(a, n, &exp, xr(c, rt))) {
            status = 0;
        }
    }
    c->excl_size = 0;
    wx(c, rs, status, false);
    return 0;
}

/* one integer load/store; opc as in the "load/store register" encodings */
static int ldst_int(gthread_t *t, a64cpu_t *c, unsigned size, unsigned opc,
                    unsigned rt, uint64_t a)
{
    const int n = 1 << size;
    switch (opc) {
    case 0:
        st_n(a, n, xr(c, rt));
        return 0;
    case 1:
        wx(c, rt, ld_n(a, n), true);
        return 0;
    case 2:
        if (size == 3u) return 0;                    /* PRFM */
        wx(c, rt, (uint64_t)sext(ld_n(a, n), 8u * (unsigned)n), true);
        return 0;
    default:
        if (size >= 2u) return A64_UD(t);
        wx(c, rt, (uint32_t)sext(ld_n(a, n), 8u * (unsigned)n), false);
        return 0;
    }
}

static int ldst(gthread_t *t, a64cpu_t *c, uint32_t w)
{
    const unsigned rt = w & 31u, rn = BITS(w, 9, 5);
    const bool simd = BIT(w, 26);

    if ((w & 0x3F000000u) == 0x08000000u) {         /* exclusives, acquire/release */
        if (simd) return A64_UD(t);
        return ldst_exclusive(t, c, w);
    }
    if ((w & 0x3B000000u) == 0x18000000u) {         /* load literal */
        uint64_t a = c->pc + (uint64_t)(sext(BITS(w, 23, 5), 19) * 4);
        unsigned opc = BITS(w, 31, 30);
        if (simd) {
            if (opc == 3u) return A64_UD(t);
            return a64_simd_ldst(t, w, a, 2 + (int)opc, true, (int)rt);
        }
        switch (opc) {
        case 0: wx(c, rt, ld_n(a, 4), false); break;
        case 1: wx(c, rt, ld_n(a, 8), true); break;
        case 2: wx(c, rt, (uint64_t)(int64_t)(int32_t)ld_n(a, 4), true); break;
        default: break;                               /* PRFM */
        }
        return 0;
    }
    if ((w & 0x3A000000u) == 0x28000000u) {         /* load/store pair */
        const unsigned opc = BITS(w, 31, 30), type = BITS(w, 24, 23);
        const bool load = BIT(w, 22);
        const unsigned rt2 = BITS(w, 14, 10);
        int scale;
        if (simd) {
            if (opc == 3u) return A64_UD(t);
            scale = 2 + (int)opc;
        } else {
            if (opc == 3u || (opc == 1u && !load)) return A64_UD(t);
            scale = 2 + (int)(opc >> 1);
        }
        const int n = 1 << scale;
        int64_t off = sext(BITS(w, 21, 15), 7) * n;
        uint64_t base = xsp(c, rn);
        uint64_t a = type == 1u ? base : base + (uint64_t)off;
        if (simd) {
            int ev = a64_simd_ldst(t, w, a, scale, load, (int)rt);
            if (ev == 0) ev = a64_simd_ldst(t, w, a + (uint64_t)n, scale, load, (int)rt2);
            if (ev != 0) return ev;
        } else if (load) {
            uint64_t v0 = ld_n(a, n), v1 = ld_n(a + (uint64_t)n, n);
            if (opc == 1u) {                         /* LDPSW */
                v0 = (uint64_t)(int64_t)(int32_t)v0;
                v1 = (uint64_t)(int64_t)(int32_t)v1;
            }
            wx(c, rt, v0, true);
            wx(c, rt2, v1, true);
        } else {
            uint64_t v0 = xr(c, rt), v1 = xr(c, rt2);
            st_n(a, n, v0);
            st_n(a + (uint64_t)n, n, v1);
        }
        if (type == 1u || type == 3u) {
            wsp(c, rn, base + (uint64_t)off, true);
        }
        return 0;
    }
    if ((w & 0x3B000000u) == 0x38000000u || (w & 0x3B000000u) == 0x39000000u) {
        const unsigned size = BITS(w, 31, 30), opc = BITS(w, 23, 22);
        uint64_t a;
        bool writeback = false;
        uint64_t wb_value = 0;
        if (BIT(w, 24)) {                            /* unsigned offset */
            int scale = (int)size;
            if (simd && (opc & 2u)) scale = 4;
            a = xsp(c, rn) + ((uint64_t)BITS(w, 21, 10) << scale);
        } else if (BIT(w, 21)) {
            if (BITS(w, 11, 10) != 2u) return A64_UD(t);   /* atomics (LSE), PAC loads */
            unsigned option = BITS(w, 15, 13);
            if ((option & 2u) == 0u) return A64_UD(t);
            int scale = (int)size;
            if (simd && (opc & 2u)) scale = 4;
            uint64_t off = extend_reg(xr(c, BITS(w, 20, 16)), option, BIT(w, 12) ? (unsigned)scale : 0u);
            a = xsp(c, rn) + off;
        } else {
            int64_t imm = sext(BITS(w, 20, 12), 9);
            uint64_t base = xsp(c, rn);
            switch (BITS(w, 11, 10)) {
            case 0: case 2: a = base + (uint64_t)imm; break;          /* unscaled, unprivileged */
            case 1: a = base; writeback = true; wb_value = base + (uint64_t)imm; break;
            default: a = base + (uint64_t)imm; writeback = true; wb_value = a; break;
            }
        }
        int ev;
        if (simd) {
            int sz = (int)size;
            if (opc & 2u) {
                if (size != 0u) return A64_UD(t);
                sz = 4;
            }
            ev = a64_simd_ldst(t, w, a, sz, (opc & 1u) != 0u, (int)rt);
        } else {
            ev = ldst_int(t, c, size, opc, rt, a);
        }
        if (ev != 0) return ev;
        if (writeback) {
            wsp(c, rn, wb_value, true);
        }
        return 0;
    }
    if ((w & 0xBF000000u) == 0x0C000000u || (w & 0xBF000000u) == 0x0D000000u) {
        return a64_simd_ldst_multi(t, w);
    }
    return A64_UD(t);
}

/* ---- one instruction, and the plain run loop ------------------------------------ */

/* Execute the instruction at cpu->pc: 0 when it completed (pc advanced or
 * branched), else an XL_EV_* event. */
int a64_step(gthread_t *t, a64cpu_t *c)
{
    uint32_t w;
    __builtin_memcpy(&w, (const void *)(uintptr_t)c->pc, 4);
    int ev;
    switch (BITS(w, 28, 25)) {
    case 0x8: case 0x9:
        ev = dp_imm(t, c, w);
        break;
    case 0xA: case 0xB:
        ev = branch_sys(t, c, w);
        break;
    case 0x4: case 0x6: case 0xC: case 0xE:
        ev = ldst(t, c, w);
        break;
    case 0x5: case 0xD:
        ev = dp_reg(t, c, w);
        break;
    case 0x7: case 0xF:
        ev = a64_simd_dp(t, w);
        break;
    default:
        ev = A64_UD(t);
        break;
    }
    if (XL_LIKELY(ev == 0)) {
        c->pc += 4u;
        return 0;
    }
    return ev == PC_SET ? 0 : ev;
}

/* guest_run() (a64_block.c) runs this way when XL_NOBLOCKS=1. */
int a64_run_interp(gthread_t *t)
{
    a64cpu_t *c = A64(t);
    for (;;) {
        if (XL_UNLIKELY(t->sig_pending_flag)) {
            return XL_EV_SIGNAL;
        }
        int ev = a64_step(t, c);
        if (XL_UNLIKELY(ev != 0)) {
            return ev;
        }
    }
}
