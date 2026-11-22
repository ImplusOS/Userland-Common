#pragma once

/*
 * The AArch64 guest: CPU state and what the interpreter's files share.
 *
 *   a64_cpu.c   decoder, integer/branch/system instructions, loads and
 *               stores (including exclusives), run loop
 *   a64_fp.c    scalar floating point and Advanced SIMD
 *   a64_sys.c   the guest descriptor, system call front end (asm-generic
 *               numbering -> canonical), signal frames
 *   a64_block.c the block engine (translation cache) that runs by default
 *   a64_ops.h   register/flag/operand helpers both engines share
 *
 * The interpreter models ARMv8.0-A with FP and Advanced SIMD -- what
 * AT_HWCAP reports (FP | ASIMD) -- so code that selects variants at run
 * time (glibc's ifuncs, -moutline-atomics) never takes an LSE, SVE, CRC or
 * crypto path. Pointer authentication and BTI instructions live in the
 * hint space and execute as NOPs, as on a CPU without those features.
 */

#include "xl.h"

typedef union {
    uint8_t  b[16];
    int8_t   sb[16];
    uint16_t h[8];
    int16_t  sh[8];
    uint32_t s[4];
    int32_t  ss[4];
    uint64_t d[2];
    int64_t  sd[2];
    float    f[4];
    double   fd[2];
} vreg_t;

/* x[31] always reads as zero (XZR), x[32] is SP, x[33] takes writes to
 * XZR in the block engine -- so a register number from an instruction can
 * index x[] directly once 31 is mapped for its context. */
#define A64_ZR   31u
#define A64_SP   32u
#define A64_DISCARD 33u

typedef struct {
    uint64_t x[34];
    uint64_t pc;
    uint32_t nzcv;          /* N Z C V in bits 31..28 */
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t _pad;
    uint64_t tpidr;
    _Alignas(16) vreg_t v[32];
    /* exclusive monitor (LDXR/STXR) */
    uint64_t excl_addr;
    uint64_t excl_val[2];
    uint32_t excl_size;     /* bytes; 0 = no reservation */
    uint32_t _pad2;
} a64cpu_t;

#define A64(t) ((a64cpu_t *)(void *)(t)->cpu)

#define NZCV_N 0x80000000u
#define NZCV_Z 0x40000000u
#define NZCV_C 0x20000000u
#define NZCV_V 0x10000000u

/* instruction fields */
#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, n) (((w) >> (n)) & 1u)

static inline int64_t sext(uint64_t v, unsigned bits)
{
    const uint64_t m = 1ULL << (bits - 1);
    v &= bits == 64 ? ~0ULL : ((1ULL << bits) - 1ULL);
    return (int64_t)((v ^ m) - m);
}

/* guest memory: the host's */
static inline uint64_t ld_n(uint64_t a, int n)
{
    switch (n) {
    case 1: return *(const uint8_t *)(uintptr_t)a;
    case 2: { uint16_t v; __builtin_memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
    case 4: { uint32_t v; __builtin_memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
    default: { uint64_t v; __builtin_memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
    }
}

static inline void st_n(uint64_t a, int n, uint64_t v)
{
    switch (n) {
    case 1: *(uint8_t *)(uintptr_t)a = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; __builtin_memcpy((void *)(uintptr_t)a, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; __builtin_memcpy((void *)(uintptr_t)a, &x, 4); break; }
    default: __builtin_memcpy((void *)(uintptr_t)a, &v, 8); break;
    }
}

/* Raise a synchronous guest signal at the current instruction. */
int a64_fault(gthread_t *t, int sig, int code, uint64_t addr);
#define A64_UD(t) a64_fault((t), XL_SIGILL, XL_ILL_ILLOPC, A64(t)->pc)


/* a64_fp.c: the FP/SIMD encoding groups. Return 0 or an XL_EV_* event. */
int a64_simd_dp(gthread_t *t, uint32_t w);              /* op0 = x111 */
int a64_simd_ldst(gthread_t *t, uint32_t w, uint64_t addr, int size_log2,
                  bool load, int rt);                    /* one register */
int a64_simd_ldst_multi(gthread_t *t, uint32_t w);      /* LD1..LD4/ST1..ST4 */
/* FP data-processing (2 source) op on doubles/floats, ARM NaN rules */
double a64_fop2_d(const a64cpu_t *c, unsigned op, double a, double b);
float a64_fop2_f(const a64cpu_t *c, unsigned op, float a, float b);
/* Host FP unit <- guest FPCR (rounding mode, flush-to-zero). */
void a64_fpcr_apply(a64cpu_t *c);
/* Guest FPSR <- host sticky exception flags. */
void a64_fpsr_collect(a64cpu_t *c);
