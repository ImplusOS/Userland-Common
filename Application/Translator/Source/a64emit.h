#pragma once

/*
 * AArch64 instruction encoders for the JIT back end that runs on an arm64
 * host (the x86_64 guest's translator). Each function appends one
 * instruction (or a short fixed idiom) to an emission buffer; nothing here
 * knows about guests.
 *
 * Register numbers are 0..30; 31 is XZR/WZR or SP depending on the
 * instruction, as in the architecture. `sf` selects the 64-bit form.
 */

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t *p;        /* next instruction */
    uint32_t *start;
    uint32_t *end;      /* capacity limit */
    bool overflow;
} a64_t;

#define A64_ZR 31u
#define A64_SP 31u

/* condition codes */
enum {
    A64_EQ = 0, A64_NE, A64_CS, A64_CC, A64_MI, A64_PL, A64_VS, A64_VC,
    A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL, A64_NV,
};
#define A64_HS A64_CS
#define A64_LO A64_CC

static inline void a64_put(a64_t *a, uint32_t ins)
{
    if (a->p < a->end) {
        *a->p++ = ins;
    } else {
        a->overflow = true;
    }
}

static inline uint32_t *a64_here(const a64_t *a) { return a->p; }

/* ---- immediates ---------------------------------------------------------- */

/* The (N, immr, imms) encoding of a logical immediate, or -1. */
static inline int a64_logical_imm(uint64_t v, bool sf, uint32_t *enc)
{
    if (!sf) {
        v &= 0xFFFFFFFFu;
        v |= v << 32;
    }
    if (v == 0u || v == ~0ULL) {
        return -1;
    }
    /* smallest repeating element */
    unsigned size = 64;
    while (size > 2u) {
        const unsigned h = size / 2u;
        const uint64_t mask = (1ULL << h) - 1u;
        if ((v & mask) != ((v >> h) & mask)) {
            break;
        }
        size = h;
    }
    const uint64_t emask = size == 64u ? ~0ULL : (1ULL << size) - 1u;
    const uint64_t e = v & emask;
    /* e must be a rotated run of ones */
    const unsigned ones = (unsigned)__builtin_popcountll(e);
    if (ones == 0u || ones == size) {
        return -1;
    }
    unsigned rot = 0;
    for (; rot < size; ++rot) {
        /* rotate right by rot: is it 0..01..1 ? */
        const uint64_t r = size == 64u ? ((e >> rot) | (e << ((64u - rot) & 63u)))
                                       : (((e >> rot) | (e << (size - rot))) & emask);
        if (r == (1ULL << ones) - 1u) {
            break;
        }
    }
    if (rot == size) {
        return -1;
    }
    const uint32_t immr = (size - rot) % size;
    uint32_t imms = (uint32_t)(ones - 1u);
    uint32_t n = 0;
    if (size == 64u) {
        n = 1;
    } else {
        imms |= (uint32_t)(~(size * 2u - 1u) & 0x3Fu);
    }
    if (!sf && n) {
        return -1;
    }
    *enc = (n << 22) | (immr << 16) | ((imms & 0x3Fu) << 10);
    return 0;
}

/* ---- data processing: immediate -------------------------------------------- */

static inline uint32_t a64_sf(bool sf) { return sf ? 0x80000000u : 0u; }

/* ADD/SUB(S) Rd, Rn, #imm12 [, LSL #12] */
static inline void a64_addsub_imm(a64_t *a, bool sf, bool sub, bool setf, unsigned rd,
                                  unsigned rn, uint32_t imm12, bool lsl12)
{
    a64_put(a, a64_sf(sf) | (sub ? 0x40000000u : 0u) | (setf ? 0x20000000u : 0u) |
               0x11000000u | (lsl12 ? 0x00400000u : 0u) | ((imm12 & 0xFFFu) << 10) |
               (rn << 5) | rd);
}

static inline bool a64_imm12_ok(uint64_t v) { return v <= 0xFFFu || ((v & 0xFFFu) == 0u && v <= 0xFFF000u); }

static inline void a64_mov_sp(a64_t *a, unsigned rd, unsigned rn)   /* MOV to/from SP */
{
    a64_addsub_imm(a, true, false, false, rd, rn, 0, false);
}

/* logical immediate; the caller has checked a64_logical_imm() */
enum { A64_AND = 0, A64_ORR = 1, A64_EOR = 2, A64_ANDS = 3 };

static inline void a64_logic_imm_enc(a64_t *a, bool sf, unsigned opc, unsigned rd,
                                     unsigned rn, uint32_t enc)
{
    a64_put(a, a64_sf(sf) | (opc << 29) | 0x12000000u | enc | (rn << 5) | rd);
}

/* MOVZ/MOVN/MOVK */
static inline void a64_movz(a64_t *a, bool sf, unsigned rd, uint32_t imm16, unsigned hw)
{
    a64_put(a, a64_sf(sf) | 0x52800000u | (hw << 21) | ((imm16 & 0xFFFFu) << 5) | rd);
}

static inline void a64_movn(a64_t *a, bool sf, unsigned rd, uint32_t imm16, unsigned hw)
{
    a64_put(a, a64_sf(sf) | 0x12800000u | (hw << 21) | ((imm16 & 0xFFFFu) << 5) | rd);
}

static inline void a64_movk(a64_t *a, bool sf, unsigned rd, uint32_t imm16, unsigned hw)
{
    a64_put(a, a64_sf(sf) | 0x72800000u | (hw << 21) | ((imm16 & 0xFFFFu) << 5) | rd);
}

/* Load any constant: the shortest of MOVZ/MOVN(+MOVK) and ORR #bitmask. */
static inline void a64_mov_imm(a64_t *a, bool sf, unsigned rd, uint64_t v)
{
    if (!sf) {
        v &= 0xFFFFFFFFu;
    }
    const unsigned nh = sf ? 4u : 2u;
    unsigned zeros = 0, ones = 0;
    for (unsigned k = 0; k < nh; ++k) {
        const uint32_t h = (uint32_t)(v >> (16u * k)) & 0xFFFFu;
        zeros += h == 0u;
        ones += h == 0xFFFFu;
    }
    if (zeros >= nh - 1u) {
        unsigned k = 0;
        while (k + 1u < nh && ((v >> (16u * k)) & 0xFFFFu) == 0u) ++k;
        a64_movz(a, sf, rd, (uint32_t)(v >> (16u * k)), k);
        return;
    }
    if (ones >= nh - 1u) {
        unsigned k = 0;
        while (k + 1u < nh && ((v >> (16u * k)) & 0xFFFFu) == 0xFFFFu) ++k;
        a64_movn(a, sf, rd, (uint32_t)(~(v >> (16u * k))), k);
        return;
    }
    uint32_t enc;
    if (a64_logical_imm(v, sf, &enc) == 0) {
        a64_logic_imm_enc(a, sf, A64_ORR, rd, A64_ZR, enc);
        return;
    }
    if (ones > zeros) {
        bool first = true;
        for (unsigned k = 0; k < nh; ++k) {
            const uint32_t h = (uint32_t)(v >> (16u * k)) & 0xFFFFu;
            if (h == 0xFFFFu) continue;
            if (first) { a64_movn(a, sf, rd, ~h, k); first = false; }
            else a64_movk(a, sf, rd, h, k);
        }
        return;
    }
    bool first = true;
    for (unsigned k = 0; k < nh; ++k) {
        const uint32_t h = (uint32_t)(v >> (16u * k)) & 0xFFFFu;
        if (h == 0u) continue;
        if (first) { a64_movz(a, sf, rd, h, k); first = false; }
        else a64_movk(a, sf, rd, h, k);
    }
}

/* Always four instructions (patchable 64-bit constant). */
static inline void a64_mov_imm64_fixed(a64_t *a, unsigned rd, uint64_t v)
{
    a64_movz(a, true, rd, (uint32_t)v, 0);
    a64_movk(a, true, rd, (uint32_t)(v >> 16), 1);
    a64_movk(a, true, rd, (uint32_t)(v >> 32), 2);
    a64_movk(a, true, rd, (uint32_t)(v >> 48), 3);
}

/* bitfield moves */
static inline void a64_bfm(a64_t *a, unsigned opc, bool sf, unsigned rd, unsigned rn,
                           unsigned immr, unsigned imms)
{
    /* opc: 0 SBFM, 1 BFM, 2 UBFM */
    a64_put(a, a64_sf(sf) | (opc << 29) | 0x13000000u | (sf ? 0x00400000u : 0u) |
               ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10) | (rn << 5) | rd);
}

static inline unsigned a64_bits(bool sf) { return sf ? 64u : 32u; }

static inline void a64_lsl_imm(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned sh)
{
    const unsigned w = a64_bits(sf);
    a64_bfm(a, 2, sf, rd, rn, (w - sh) % w, w - 1u - sh);
}

static inline void a64_lsr_imm(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned sh)
{
    a64_bfm(a, 2, sf, rd, rn, sh, a64_bits(sf) - 1u);
}

static inline void a64_asr_imm(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned sh)
{
    a64_bfm(a, 0, sf, rd, rn, sh, a64_bits(sf) - 1u);
}

static inline void a64_ubfx(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    a64_bfm(a, 2, sf, rd, rn, lsb, lsb + width - 1u);
}

static inline void a64_sbfx(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    a64_bfm(a, 0, sf, rd, rn, lsb, lsb + width - 1u);
}

static inline void a64_ubfiz(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    const unsigned w = a64_bits(sf);
    a64_bfm(a, 2, sf, rd, rn, (w - lsb) % w, width - 1u);
}

/* BFI Rd, Rn, #lsb, #width: insert the low `width` bits of Rn at lsb */
static inline void a64_bfi(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    const unsigned w = a64_bits(sf);
    a64_bfm(a, 1, sf, rd, rn, (w - lsb) % w, width - 1u);
}

/* BFXIL Rd, Rn, #lsb, #width: Rn<lsb+width-1:lsb> to Rd<width-1:0> */
static inline void a64_bfxil(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
    a64_bfm(a, 1, sf, rd, rn, lsb, lsb + width - 1u);
}

static inline void a64_sxt(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned from_bits)
{
    a64_bfm(a, 0, sf, rd, rn, 0, from_bits - 1u);
}

static inline void a64_uxt(a64_t *a, unsigned rd, unsigned rn, unsigned from_bits)
{
    a64_bfm(a, 2, false, rd, rn, 0, from_bits - 1u);
}

/* EXTR Rd, Rn, Rm, #lsb (ROR imm when Rn == Rm) */
static inline void a64_extr(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm, unsigned lsb)
{
    a64_put(a, (sf ? 0x93C00000u : 0x13800000u) | (rm << 16) | ((lsb & 0x3Fu) << 10) |
               (rn << 5) | rd);
}

static inline void a64_ror_imm(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned sh)
{
    a64_extr(a, sf, rd, rn, rn, sh);
}

/* ADR Rd, pc + off */
static inline void a64_adr(a64_t *a, unsigned rd, int64_t off)
{
    const uint32_t imm = (uint32_t)off & 0x1FFFFFu;
    a64_put(a, 0x10000000u | ((imm & 3u) << 29) | (((imm >> 2) & 0x7FFFFu) << 5) | rd);
}

/* ---- data processing: register ------------------------------------------------ */

enum { A64_LSL = 0, A64_LSR = 1, A64_ASR = 2, A64_ROR = 3 };

/* AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS (shifted register) */
enum { A64_L_AND = 0, A64_L_BIC, A64_L_ORR, A64_L_ORN, A64_L_EOR, A64_L_EON, A64_L_ANDS, A64_L_BICS };

static inline void a64_logic(a64_t *a, bool sf, unsigned op, unsigned rd, unsigned rn,
                             unsigned rm, unsigned shift, unsigned amount)
{
    a64_put(a, a64_sf(sf) | ((op >> 1) << 29) | 0x0A000000u | ((op & 1u) << 21) |
               (shift << 22) | (rm << 16) | ((amount & 0x3Fu) << 10) | (rn << 5) | rd);
}

static inline void a64_mov(a64_t *a, bool sf, unsigned rd, unsigned rm)
{
    a64_logic(a, sf, A64_L_ORR, rd, A64_ZR, rm, 0, 0);
}

/* ADD/ADDS/SUB/SUBS (shifted register) */
static inline void a64_addsub(a64_t *a, bool sf, bool sub, bool setf, unsigned rd,
                              unsigned rn, unsigned rm, unsigned shift, unsigned amount)
{
    a64_put(a, a64_sf(sf) | (sub ? 0x40000000u : 0u) | (setf ? 0x20000000u : 0u) |
               0x0B000000u | (shift << 22) | (rm << 16) | ((amount & 0x3Fu) << 10) |
               (rn << 5) | rd);
}

static inline void a64_cmp(a64_t *a, bool sf, unsigned rn, unsigned rm)
{
    a64_addsub(a, sf, true, true, A64_ZR, rn, rm, 0, 0);
}

static inline void a64_cmn(a64_t *a, bool sf, unsigned rn, unsigned rm)
{
    a64_addsub(a, sf, false, true, A64_ZR, rn, rm, 0, 0);
}

static inline void a64_tst(a64_t *a, bool sf, unsigned rn, unsigned rm)
{
    a64_logic(a, sf, A64_L_ANDS, A64_ZR, rn, rm, 0, 0);
}

/* ADD Rd, Rn, Wm, UXTW / SXTW (extended register); also allows SP as Rn */
static inline void a64_add_ext(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm,
                               unsigned option, unsigned amount)
{
    a64_put(a, a64_sf(sf) | 0x0B200000u | (rm << 16) | (option << 13) | ((amount & 7u) << 10) |
               (rn << 5) | rd);
}

/* ADC/ADCS/SBC/SBCS */
static inline void a64_adc(a64_t *a, bool sf, bool sub, bool setf, unsigned rd, unsigned rn,
                           unsigned rm)
{
    a64_put(a, a64_sf(sf) | (sub ? 0x40000000u : 0u) | (setf ? 0x20000000u : 0u) |
               0x1A000000u | (rm << 16) | (rn << 5) | rd);
}

/* CSEL/CSINC/CSINV/CSNEG */
static inline void a64_csel_op(a64_t *a, bool sf, unsigned op, unsigned rd, unsigned rn,
                               unsigned rm, unsigned cond)
{
    /* op: 0 CSEL, 1 CSINC, 2 CSINV, 3 CSNEG */
    a64_put(a, a64_sf(sf) | ((op >> 1) << 30) | 0x1A800000u | ((op & 1u) << 10) |
               (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

static inline void a64_csel(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_csel_op(a, sf, 0, rd, rn, rm, cond);
}

static inline void a64_cset(a64_t *a, bool sf, unsigned rd, unsigned cond)
{
    a64_csel_op(a, sf, 1, rd, A64_ZR, A64_ZR, cond ^ 1u);
}

static inline void a64_csetm(a64_t *a, bool sf, unsigned rd, unsigned cond)
{
    a64_csel_op(a, sf, 2, rd, A64_ZR, A64_ZR, cond ^ 1u);
}

/* CCMP Rn, Rm / #imm5, #nzcv, cond */
static inline void a64_ccmp(a64_t *a, bool sf, unsigned rn, unsigned rm, unsigned nzcv, unsigned cond)
{
    a64_put(a, a64_sf(sf) | 0x7A400000u | (rm << 16) | (cond << 12) | (rn << 5) | nzcv);
}

static inline void a64_ccmp_imm(a64_t *a, bool sf, unsigned rn, unsigned imm5, unsigned nzcv,
                                unsigned cond)
{
    a64_put(a, a64_sf(sf) | 0x7A400800u | ((imm5 & 31u) << 16) | (cond << 12) | (rn << 5) | nzcv);
}

/* two-source: UDIV SDIV LSLV LSRV ASRV RORV */
enum { A64_UDIV = 2, A64_SDIV = 3, A64_LSLV = 8, A64_LSRV = 9, A64_ASRV = 10, A64_RORV = 11 };

static inline void a64_dp2(a64_t *a, bool sf, unsigned opcode, unsigned rd, unsigned rn, unsigned rm)
{
    a64_put(a, a64_sf(sf) | 0x1AC00000u | (rm << 16) | (opcode << 10) | (rn << 5) | rd);
}

/* one-source: RBIT REV16 REV32/REV(w) REV(x) CLZ */
enum { A64_RBIT = 0, A64_REV16 = 1, A64_REV32 = 2, A64_REV64 = 3, A64_CLZ = 4 };

static inline void a64_dp1(a64_t *a, bool sf, unsigned opcode, unsigned rd, unsigned rn)
{
    a64_put(a, a64_sf(sf) | 0x5AC00000u | (opcode << 10) | (rn << 5) | rd);
}

/* MADD/MSUB/SMADDL/UMADDL/SMULH/UMULH */
static inline void a64_madd(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    a64_put(a, a64_sf(sf) | 0x1B000000u | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

static inline void a64_mul(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm)
{
    a64_madd(a, sf, rd, rn, rm, A64_ZR);
}

static inline void a64_msub(a64_t *a, bool sf, unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    a64_put(a, a64_sf(sf) | 0x1B008000u | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

static inline void a64_smull(a64_t *a, unsigned rd, unsigned rn, unsigned rm)   /* Xd = Wn * Wm */
{
    a64_put(a, 0x9B200000u | (rm << 16) | (31u << 10) | (rn << 5) | rd);
}

static inline void a64_umull(a64_t *a, unsigned rd, unsigned rn, unsigned rm)
{
    a64_put(a, 0x9BA00000u | (rm << 16) | (31u << 10) | (rn << 5) | rd);
}

static inline void a64_smulh(a64_t *a, unsigned rd, unsigned rn, unsigned rm)
{
    a64_put(a, 0x9B407C00u | (rm << 16) | (rn << 5) | rd);
}

static inline void a64_umulh(a64_t *a, unsigned rd, unsigned rn, unsigned rm)
{
    a64_put(a, 0x9BC07C00u | (rm << 16) | (rn << 5) | rd);
}

/* ---- branches --------------------------------------------------------------------- */

static inline void a64_b(a64_t *a, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)a->p) / 4;
    a64_put(a, 0x14000000u | ((uint32_t)off & 0x3FFFFFFu));
}

static inline void a64_bl(a64_t *a, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)a->p) / 4;
    a64_put(a, 0x94000000u | ((uint32_t)off & 0x3FFFFFFu));
}

static inline bool a64_b_in_range(const void *from, const void *to)
{
    const int64_t off = ((const uint8_t *)to - (const uint8_t *)from);
    return off >= -(1LL << 27) && off < (1LL << 27);
}

static inline uint32_t a64_enc_b(const void *at, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)at) / 4;
    return 0x14000000u | ((uint32_t)off & 0x3FFFFFFu);
}

static inline void a64_bcond(a64_t *a, unsigned cond, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)a->p) / 4;
    a64_put(a, 0x54000000u | (((uint32_t)off & 0x7FFFFu) << 5) | cond);
}

static inline void a64_cbz(a64_t *a, bool sf, bool nz, unsigned rt, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)a->p) / 4;
    a64_put(a, a64_sf(sf) | (nz ? 0x35000000u : 0x34000000u) | (((uint32_t)off & 0x7FFFFu) << 5) | rt);
}

static inline void a64_tbz(a64_t *a, bool nz, unsigned rt, unsigned bit, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)a->p) / 4;
    a64_put(a, ((bit >> 5) << 31) | (nz ? 0x37000000u : 0x36000000u) | ((bit & 31u) << 19) |
               (((uint32_t)off & 0x3FFFu) << 5) | rt);
}

/* Patch a forward branch emitted with a placeholder target. */
static inline void a64_patch_b(uint32_t *at, const void *target)
{
    const int64_t off = ((const uint8_t *)target - (const uint8_t *)at) / 4;
    const uint32_t ins = *at;
    if ((ins & 0x7C000000u) == 0x14000000u) {                   /* B, BL */
        *at = (ins & 0xFC000000u) | ((uint32_t)off & 0x3FFFFFFu);
    } else if ((ins & 0xFF000010u) == 0x54000000u ||             /* B.cond */
               (ins & 0x7E000000u) == 0x34000000u) {             /* CBZ/CBNZ */
        *at = (ins & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFFu) << 5);
    } else if ((ins & 0x7E000000u) == 0x36000000u) {             /* TBZ/TBNZ */
        *at = (ins & 0xFFF8001Fu) | (((uint32_t)off & 0x3FFFu) << 5);
    }
}

static inline void a64_br(a64_t *a, unsigned rn)  { a64_put(a, 0xD61F0000u | (rn << 5)); }
static inline void a64_blr(a64_t *a, unsigned rn) { a64_put(a, 0xD63F0000u | (rn << 5)); }
static inline void a64_ret(a64_t *a)              { a64_put(a, 0xD65F03C0u); }
static inline void a64_nop(a64_t *a)              { a64_put(a, 0xD503201Fu); }
static inline void a64_brk(a64_t *a, unsigned imm){ a64_put(a, 0xD4200000u | ((imm & 0xFFFFu) << 5)); }
static inline void a64_dmb_ish(a64_t *a)          { a64_put(a, 0xD5033BBFu); }
static inline void a64_dmb_ishld(a64_t *a)        { a64_put(a, 0xD50339BFu); }
static inline void a64_dmb_ishst(a64_t *a)        { a64_put(a, 0xD5033ABFu); }

/* ---- loads and stores ----------------------------------------------------------------- */

/*
 * Integer accesses by size (1, 2, 4, 8 bytes): kind
 *   A64_ST    store
 *   A64_LDZ   load, zero-extended (to 64 bits for every size)
 *   A64_LDSX  load, sign-extended to 64 bits
 *   A64_LDSW  load, sign-extended to 32 bits (1, 2 only)
 */
enum { A64_ST = 0, A64_LDZ = 1, A64_LDSX = 2, A64_LDSW = 3 };

static inline uint32_t a64_ls_opc(unsigned size_log2, unsigned kind)
{
    /* bits: size<31:30> opc<23:22> */
    unsigned opc;
    switch (kind) {
    case A64_ST: opc = 0; break;
    case A64_LDZ: opc = 1; break;
    case A64_LDSX: opc = 2; break;
    default: opc = 3; break;
    }
    return (size_log2 << 30) | (opc << 22);
}

static inline unsigned a64_log2sz(unsigned sz)
{
    return sz == 1u ? 0u : sz == 2u ? 1u : sz == 4u ? 2u : 3u;
}

/* [Rn, #off]: unsigned scaled imm12 or unscaled simm9; false if neither fits */
static inline bool a64_ls_imm(a64_t *a, unsigned sz, unsigned kind, unsigned rt, unsigned rn,
                              int64_t off)
{
    const unsigned l = a64_log2sz(sz);
    const uint32_t base = a64_ls_opc(l, kind);
    if (off >= 0 && (off & ((1 << l) - 1)) == 0 && (off >> l) <= 0xFFF) {
        a64_put(a, 0x39000000u | base | ((uint32_t)(off >> l) << 10) | (rn << 5) | rt);
        return true;
    }
    if (off >= -256 && off <= 255) {
        a64_put(a, 0x38000000u | base | (((uint32_t)off & 0x1FFu) << 12) | (rn << 5) | rt);
        return true;
    }
    return false;
}

static inline bool a64_ls_imm_ok(unsigned sz, int64_t off)
{
    const unsigned l = a64_log2sz(sz);
    return (off >= 0 && (off & ((1 << l) - 1)) == 0 && (off >> l) <= 0xFFF) ||
           (off >= -256 && off <= 255);
}

/* [Rn, Rm{, LSL #log2(sz)}] */
static inline void a64_ls_reg(a64_t *a, unsigned sz, unsigned kind, unsigned rt, unsigned rn,
                              unsigned rm, bool scaled)
{
    const unsigned l = a64_log2sz(sz);
    a64_put(a, 0x38200800u | a64_ls_opc(l, kind) | (rm << 16) | (3u << 13) |
               (scaled ? 0x1000u : 0u) | (rn << 5) | rt);
}

/* pre/post-indexed (writeback) simm9 */
static inline void a64_ls_wb(a64_t *a, unsigned sz, unsigned kind, unsigned rt, unsigned rn,
                             int64_t off, bool pre)
{
    const unsigned l = a64_log2sz(sz);
    a64_put(a, 0x38000000u | a64_ls_opc(l, kind) | (((uint32_t)off & 0x1FFu) << 12) |
               (pre ? 0xC00u : 0x400u) | (rn << 5) | rt);
}

/* LDP/STP (64-bit), signed offset */
static inline void a64_ldp(a64_t *a, unsigned rt1, unsigned rt2, unsigned rn, int64_t off)
{
    a64_put(a, 0xA9400000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void a64_stp(a64_t *a, unsigned rt1, unsigned rt2, unsigned rn, int64_t off)
{
    a64_put(a, 0xA9000000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void a64_stp_pre(a64_t *a, unsigned rt1, unsigned rt2, unsigned rn, int64_t off)
{
    a64_put(a, 0xA9800000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void a64_ldp_post(a64_t *a, unsigned rt1, unsigned rt2, unsigned rn, int64_t off)
{
    a64_put(a, 0xA8C00000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

/* LDR (literal), 64-bit */
static inline void a64_ldr_lit(a64_t *a, bool sf, unsigned rt, const void *lit)
{
    const int64_t off = ((const uint8_t *)lit - (const uint8_t *)a->p) / 4;
    a64_put(a, (sf ? 0x58000000u : 0x18000000u) | (((uint32_t)off & 0x7FFFFu) << 5) | rt);
}

/* load-acquire / store-release / exclusives */
static inline void a64_ldar(a64_t *a, unsigned sz, unsigned rt, unsigned rn)
{
    a64_put(a, (a64_log2sz(sz) << 30) | 0x08DFFC00u | (rn << 5) | rt);
}

static inline void a64_stlr(a64_t *a, unsigned sz, unsigned rt, unsigned rn)
{
    a64_put(a, (a64_log2sz(sz) << 30) | 0x089FFC00u | (rn << 5) | rt);
}

static inline void a64_ldaxr(a64_t *a, unsigned sz, unsigned rt, unsigned rn)
{
    a64_put(a, (a64_log2sz(sz) << 30) | 0x085FFC00u | (rn << 5) | rt);
}

static inline void a64_stlxr(a64_t *a, unsigned sz, unsigned rs, unsigned rt, unsigned rn)
{
    a64_put(a, (a64_log2sz(sz) << 30) | 0x0800FC00u | (rs << 16) | (rn << 5) | rt);
}

/* ---- FP / SIMD ---------------------------------------------------------------------- */

/* LDR/STR Qt/Dt/St, [Rn, #off] (scaled unsigned imm12, else unscaled simm9) */
static inline bool a64_fls_imm(a64_t *a, unsigned sz, bool load, unsigned rt, unsigned rn, int64_t off)
{
    uint32_t sbits, opc_st, l;
    switch (sz) {
    case 16: sbits = 0; opc_st = 2; l = 4; break;
    case 8: sbits = 3; opc_st = 0; l = 3; break;
    case 4: sbits = 2; opc_st = 0; l = 2; break;
    default: sbits = 1; opc_st = 0; l = 1; break;   /* H */
    }
    const uint32_t opc = opc_st | (load ? 1u : 0u);
    const uint32_t base = (sbits << 30) | 0x04000000u | (opc << 22);
    if (off >= 0 && (off & ((1 << l) - 1)) == 0 && (off >> l) <= 0xFFF) {
        a64_put(a, 0x39000000u | base | ((uint32_t)(off >> l) << 10) | (rn << 5) | rt);
        return true;
    }
    if (off >= -256 && off <= 255) {
        a64_put(a, 0x38000000u | base | (((uint32_t)off & 0x1FFu) << 12) | (rn << 5) | rt);
        return true;
    }
    return false;
}

static inline void a64_fls_reg(a64_t *a, unsigned sz, bool load, unsigned rt, unsigned rn, unsigned rm)
{
    uint32_t sbits, opc_st;
    switch (sz) {
    case 16: sbits = 0; opc_st = 2; break;
    case 8: sbits = 3; opc_st = 0; break;
    case 4: sbits = 2; opc_st = 0; break;
    default: sbits = 1; opc_st = 0; break;
    }
    const uint32_t opc = opc_st | (load ? 1u : 0u);
    a64_put(a, 0x38200800u | (sbits << 30) | 0x04000000u | (opc << 22) | (rm << 16) |
               (3u << 13) | (rn << 5) | rt);
}

/* FMOV between general and FP registers */
static inline void a64_fmov_to_d(a64_t *a, unsigned vd, unsigned xn)   { a64_put(a, 0x9E670000u | (xn << 5) | vd); }
static inline void a64_fmov_from_d(a64_t *a, unsigned xd, unsigned vn) { a64_put(a, 0x9E660000u | (vn << 5) | xd); }
static inline void a64_fmov_to_s(a64_t *a, unsigned vd, unsigned wn)   { a64_put(a, 0x1E270000u | (wn << 5) | vd); }
static inline void a64_fmov_from_s(a64_t *a, unsigned wd, unsigned vn) { a64_put(a, 0x1E260000u | (vn << 5) | wd); }
static inline void a64_fmov_to_d1(a64_t *a, unsigned vd, unsigned xn)  { a64_put(a, 0x9EAF0000u | (xn << 5) | vd); }
static inline void a64_fmov_from_d1(a64_t *a, unsigned xd, unsigned vn){ a64_put(a, 0x9EAE0000u | (vn << 5) | xd); }

/* scalar FP two-source: FMUL FDIV FADD FSUB FMAX FMIN (dbl: double) */
enum { A64_FMUL = 0, A64_FDIV = 1, A64_FADD = 2, A64_FSUB = 3, A64_FMAX = 4, A64_FMIN = 5 };

static inline void a64_fp2(a64_t *a, bool dbl, unsigned op, unsigned vd, unsigned vn, unsigned vm)
{
    a64_put(a, (dbl ? 0x1E600800u : 0x1E200800u) | (vm << 16) | (op << 12) | (vn << 5) | vd);
}

static inline void a64_fsqrt(a64_t *a, bool dbl, unsigned vd, unsigned vn)
{
    a64_put(a, (dbl ? 0x1E61C000u : 0x1E21C000u) | (vn << 5) | vd);
}

static inline void a64_fcmp(a64_t *a, bool dbl, unsigned vn, unsigned vm)
{
    a64_put(a, (dbl ? 0x1E602000u : 0x1E202000u) | (vm << 16) | (vn << 5));
}

static inline void a64_fcsel(a64_t *a, bool dbl, unsigned vd, unsigned vn, unsigned vm, unsigned cond)
{
    a64_put(a, (dbl ? 0x1E600C00u : 0x1E200C00u) | (vm << 16) | (cond << 12) | (vn << 5) | vd);
}

static inline void a64_fcvt_d_s(a64_t *a, unsigned vd, unsigned vn) { a64_put(a, 0x1E22C000u | (vn << 5) | vd); }
static inline void a64_fcvt_s_d(a64_t *a, unsigned vd, unsigned vn) { a64_put(a, 0x1E624000u | (vn << 5) | vd); }
static inline void a64_frintx(a64_t *a, bool dbl, unsigned vd, unsigned vn)
{
    a64_put(a, (dbl ? 0x1E674000u : 0x1E274000u) | (vn << 5) | vd);
}

/* SCVTF Vd, Rn (sf: 64-bit integer source) */
static inline void a64_scvtf(a64_t *a, bool dbl, bool sf, unsigned vd, unsigned rn)
{
    a64_put(a, a64_sf(sf) | (dbl ? 0x1E620000u : 0x1E220000u) | (rn << 5) | vd);
}

/* FCVTZS Rd, Vn */
static inline void a64_fcvtzs(a64_t *a, bool dbl, bool sf, unsigned rd, unsigned vn)
{
    a64_put(a, a64_sf(sf) | (dbl ? 0x1E780000u : 0x1E380000u) | (vn << 5) | rd);
}

/* ---- Advanced SIMD (128-bit, Q = 1) ---- */

enum { A64_V_AND = 0x4E201C00u, A64_V_BIC = 0x4E601C00u, A64_V_ORR = 0x4EA01C00u,
       A64_V_ORN = 0x4EE01C00u, A64_V_EOR = 0x6E201C00u, A64_V_BSL = 0x6E601C00u,
       A64_V_BIT = 0x6EA01C00u, A64_V_BIF = 0x6EE01C00u };

static inline void a64_v3(a64_t *a, uint32_t opc, unsigned vd, unsigned vn, unsigned vm)
{
    a64_put(a, opc | (vm << 16) | (vn << 5) | vd);
}

/* element-size variants: size 0 = 8-bit, 1 = 16, 2 = 32, 3 = 64 */
static inline void a64_v3s(a64_t *a, uint32_t opc, unsigned size, unsigned vd, unsigned vn, unsigned vm)
{
    a64_put(a, opc | (size << 22) | (vm << 16) | (vn << 5) | vd);
}

enum {
    A64_V_ADD = 0x4E208400u, A64_V_SUB = 0x6E208400u,
    A64_V_CMEQ = 0x6E208C00u, A64_V_CMGT = 0x4E203400u, A64_V_CMHI = 0x6E203400u,
    A64_V_UMIN = 0x6E206C00u, A64_V_UMAX = 0x6E206400u,
    A64_V_SMIN = 0x4E206C00u, A64_V_SMAX = 0x4E206400u,
    A64_V_ZIP1 = 0x4E003800u, A64_V_ZIP2 = 0x4E007800u,
    A64_V_UZP1 = 0x4E001800u, A64_V_UZP2 = 0x4E005800u,
    A64_V_SQADD = 0x4E200C00u, A64_V_UQADD = 0x6E200C00u,
    A64_V_SQSUB = 0x4E202C00u, A64_V_UQSUB = 0x6E202C00u,
    A64_V_URHADD = 0x6E201400u, A64_V_MUL = 0x4E209C00u,
};

/* FP vector: sz 0 = 4S, 1 = 2D */
enum {
    A64_V_FADD = 0x4E20D400u, A64_V_FSUB = 0x4EA0D400u, A64_V_FMUL = 0x6E20DC00u,
    A64_V_FDIV = 0x6E20FC00u, A64_V_FCMEQ = 0x4E20E400u, A64_V_FCMGT = 0x6EA0E400u,
    A64_V_FCMGE = 0x6E20E400u,
};

static inline void a64_vf3(a64_t *a, uint32_t opc, bool dbl, unsigned vd, unsigned vn, unsigned vm)
{
    a64_put(a, opc | (dbl ? 0x00400000u : 0u) | (vm << 16) | (vn << 5) | vd);
}

static inline void a64_vmov(a64_t *a, unsigned vd, unsigned vn)
{
    a64_v3(a, A64_V_ORR, vd, vn, vn);
}

/* EXT Vd.16B, Vn.16B, Vm.16B, #idx */
static inline void a64_ext(a64_t *a, unsigned vd, unsigned vn, unsigned vm, unsigned idx)
{
    a64_put(a, 0x6E000000u | (vm << 16) | ((idx & 15u) << 11) | (vn << 5) | vd);
}

/* shifts by immediate: SHL, USHR, SSHR (esize 8/16/32/64) */
static inline void a64_vshl_imm(a64_t *a, unsigned esize, unsigned vd, unsigned vn, unsigned sh)
{
    a64_put(a, 0x4F005400u | ((esize + sh) << 16) | (vn << 5) | vd);
}

static inline void a64_vushr(a64_t *a, unsigned esize, unsigned vd, unsigned vn, unsigned sh)
{
    a64_put(a, 0x6F000400u | ((2u * esize - sh) << 16) | (vn << 5) | vd);
}

static inline void a64_vsshr(a64_t *a, unsigned esize, unsigned vd, unsigned vn, unsigned sh)
{
    a64_put(a, 0x4F000400u | ((2u * esize - sh) << 16) | (vn << 5) | vd);
}

/* UMINV/UMAXV/ADDV across lanes (size as above) */
static inline void a64_uminv(a64_t *a, unsigned size, unsigned vd, unsigned vn)
{
    a64_put(a, 0x6E31A800u | (size << 22) | (vn << 5) | vd);
}

/* CMLT #0 */
static inline void a64_cmlt0(a64_t *a, unsigned size, unsigned vd, unsigned vn)
{
    a64_put(a, 0x4E20A800u | (size << 22) | (vn << 5) | vd);
}

/* UMOV Wd/Xd, Vn.<T>[idx] */
static inline void a64_umov(a64_t *a, unsigned esize, unsigned rd, unsigned vn, unsigned idx)
{
    uint32_t imm5;
    bool q = false;
    switch (esize) {
    case 8: imm5 = (idx << 1) | 1u; break;
    case 16: imm5 = (idx << 2) | 2u; break;
    case 32: imm5 = (idx << 3) | 4u; break;
    default: imm5 = (idx << 4) | 8u; q = true; break;
    }
    a64_put(a, (q ? 0x4E003C00u : 0x0E003C00u) | (imm5 << 16) | (vn << 5) | rd);
}

/* INS Vd.<T>[idx], Rn */
static inline void a64_ins_gen(a64_t *a, unsigned esize, unsigned vd, unsigned idx, unsigned rn)
{
    uint32_t imm5;
    switch (esize) {
    case 8: imm5 = (idx << 1) | 1u; break;
    case 16: imm5 = (idx << 2) | 2u; break;
    case 32: imm5 = (idx << 3) | 4u; break;
    default: imm5 = (idx << 4) | 8u; break;
    }
    a64_put(a, 0x4E001C00u | (imm5 << 16) | (rn << 5) | vd);
}

/* INS Vd.<T>[di], Vn.<T>[si] */
static inline void a64_ins_elt(a64_t *a, unsigned esize, unsigned vd, unsigned di, unsigned vn, unsigned si)
{
    uint32_t imm5, imm4;
    switch (esize) {
    case 8: imm5 = (di << 1) | 1u; imm4 = si; break;
    case 16: imm5 = (di << 2) | 2u; imm4 = si << 1; break;
    case 32: imm5 = (di << 3) | 4u; imm4 = si << 2; break;
    default: imm5 = (di << 4) | 8u; imm4 = si << 3; break;
    }
    a64_put(a, 0x6E000400u | (imm5 << 16) | (imm4 << 11) | (vn << 5) | vd);
}

/* DUP Vd.<T>, Vn.<T>[idx] */
static inline void a64_dup_elt(a64_t *a, unsigned esize, unsigned vd, unsigned vn, unsigned idx)
{
    uint32_t imm5;
    switch (esize) {
    case 8: imm5 = (idx << 1) | 1u; break;
    case 16: imm5 = (idx << 2) | 2u; break;
    case 32: imm5 = (idx << 3) | 4u; break;
    default: imm5 = (idx << 4) | 8u; break;
    }
    a64_put(a, 0x4E000400u | (imm5 << 16) | (vn << 5) | vd);
}

/* TBL Vd.16B, {Vn.16B}, Vm.16B */
static inline void a64_tbl1(a64_t *a, unsigned vd, unsigned vn, unsigned vm)
{
    a64_put(a, 0x4E000000u | (vm << 16) | (vn << 5) | vd);
}

/* MOVI Vd.2D, #0 */
static inline void a64_vzero(a64_t *a, unsigned vd)
{
    a64_put(a, 0x6F00E400u | vd);
}

/* ---- cache maintenance ------------------------------------------------------------------ */

#if defined(__aarch64__)
/* Make freshly written code visible to instruction fetch. */
static inline void a64_sync_icache(void *begin, void *end)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t dline = 4u << ((ctr >> 16) & 0xFu);
    const uintptr_t iline = 4u << (ctr & 0xFu);
    const uintptr_t s = (uintptr_t)begin, e = (uintptr_t)end;
    if ((ctr & (1u << 28)) == 0u) {         /* IDC clear: clean D-cache to PoU */
        for (uintptr_t p = s & ~(dline - 1u); p < e; p += dline) {
            __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
        }
    }
    __asm__ volatile("dsb ish" ::: "memory");
    if ((ctr & (1u << 29)) == 0u) {         /* DIC clear: invalidate I-cache to PoU */
        for (uintptr_t p = s & ~(iline - 1u); p < e; p += iline) {
            __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
        }
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
#endif
