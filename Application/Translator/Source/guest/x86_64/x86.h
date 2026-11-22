#pragma once

/*
 * The x86_64 guest: CPU state and what the interpreter's files share.
 *
 *   x86_cpu.c   decoder, integer instructions, run loop
 *   x86_sse.c   SSE/SSE2 and MMX
 *   x87.c       x87 floating point, FXSAVE/FXRSTOR
 *   x86_sys.c   CPUID, system calls, signal frames, the guest descriptor
 *   x86_block.c the block engine (translation cache) that runs by default
 *   x86_ops.h   flags/ALU/register helpers both engines share
 *
 * The interpreter models the x86-64 baseline (what Debian's amd64 port
 * targets) plus SSE3: integer instructions, x87, MMX, SSE, SSE2 and SSE3
 * (which Google Chrome requires). CPUID reports exactly that, so glibc and
 * other ifunc-selecting code never pick an SSSE3+/AVX path.
 */

#include "xl.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
       R8, R9, R10, R11, R12, R13, R14, R15 };

/* EFLAGS */
#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u
#define F_AC 0x40000u
#define F_ID 0x200000u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

/* How the arithmetic flags were last set (they are computed on demand). */
enum {
    CC_EFLAGS = 0,  /* cpu->eflags holds them */
    CC_ADD, CC_ADC, CC_SUB, CC_SBB, CC_LOGIC, CC_INC, CC_DEC,
    CC_SHL, CC_SHR, CC_SAR, CC_MUL,
};

typedef union {
    uint8_t  b[16];
    int8_t   sb[16];
    uint16_t w[8];
    int16_t  sw[8];
    uint32_t d[4];
    int32_t  sd[4];
    uint64_t q[2];
    int64_t  sq[2];
    float    f[4];
    double   fd[2];
} xmm_t;

/* x87 registers are kept as the widest float the host compiler has: IEEE
 * binary128 on AArch64 (exact for every 80-bit value), double elsewhere. */
typedef long double x87_t;

typedef struct {
    uint64_t r[17];         /* r[16] is always 0 (the block engine's "no register") */
    uint64_t rip;
    uint64_t fs_base;
    uint64_t gs_base;
    uint32_t cc_op;
    uint32_t cc_size;       /* bytes */
    uint64_t cc_a, cc_b, cc_r, cc_cin;
    uint64_t eflags;        /* non-arithmetic bits always; arithmetic ones
                               when cc_op == CC_EFLAGS */
    _Alignas(16) xmm_t xmm[16];
    uint32_t mxcsr;
    uint32_t mxcsr_host;    /* the MXCSR the host FP unit was last set for */
    _Alignas(16) x87_t st[8];   /* physical registers */
    uint16_t fcw;
    uint16_t fsw;           /* without TOP */
    uint8_t ftop;
    uint8_t fempty;         /* bit i: physical register i is empty */
    uint16_t fop;
    uint64_t fip, fdp;
    uint64_t mm[8];         /* MMX (kept apart from the x87 stack) */
} x86cpu_t;

#define X86(t) ((x86cpu_t *)(void *)(t)->cpu)

/* ---- guest memory (shared with the host address space) ----------------- */

#if defined(__aarch64__)
#define X86_LOAD_FENCE()  __asm__ volatile("dmb ishld" ::: "memory")
#define X86_STORE_FENCE() __asm__ volatile("dmb ishst" ::: "memory")
#else
#define X86_LOAD_FENCE()  __asm__ volatile("" ::: "memory")
#define X86_STORE_FENCE() __asm__ volatile("" ::: "memory")
#endif

static inline uint64_t mrd(uint64_t a, int sz)
{
    uint64_t v;
    switch (sz) {
    case 1: v = *(const uint8_t *)(uintptr_t)a; break;
    case 2: { uint16_t x; __builtin_memcpy(&x, (const void *)(uintptr_t)a, 2); v = x; break; }
    case 4: { uint32_t x; __builtin_memcpy(&x, (const void *)(uintptr_t)a, 4); v = x; break; }
    default: __builtin_memcpy(&v, (const void *)(uintptr_t)a, 8); break;
    }
    if (XL_UNLIKELY(g_xl_mt)) {
        X86_LOAD_FENCE();
    }
    return v;
}

static inline void mwr(uint64_t a, int sz, uint64_t v)
{
    if (XL_UNLIKELY(g_xl_mt)) {
        X86_STORE_FENCE();
    }
    switch (sz) {
    case 1: *(uint8_t *)(uintptr_t)a = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; __builtin_memcpy((void *)(uintptr_t)a, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; __builtin_memcpy((void *)(uintptr_t)a, &x, 4); break; }
    default: __builtin_memcpy((void *)(uintptr_t)a, &v, 8); break;
    }
}

static inline void mrd_n(uint64_t a, void *dst, size_t n)
{
    __builtin_memcpy(dst, (const void *)(uintptr_t)a, n);
    if (XL_UNLIKELY(g_xl_mt)) {
        X86_LOAD_FENCE();
    }
}

static inline void mwr_n(uint64_t a, const void *src, size_t n)
{
    if (XL_UNLIKELY(g_xl_mt)) {
        X86_STORE_FENCE();
    }
    __builtin_memcpy((void *)(uintptr_t)a, src, n);
}

/* ---- the instruction being executed ------------------------------------ */

typedef struct {
    gthread_t *t;
    x86cpu_t *c;
    uint64_t start;     /* address of the first prefix */
    uint64_t pc;        /* decode cursor */
    uint8_t rex;
    uint8_t opsz;       /* 2, 4, 8 */
    uint8_t adsz32;     /* 0x67 */
    uint8_t rep;        /* 0, 0xF2, 0xF3 */
    uint8_t p66;
    uint8_t lock;
    uint8_t seg;        /* 0, 4 = FS, 5 = GS */
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg;        /* with REX.R */
    uint8_t rm;         /* with REX.B (register form) */
    uint8_t riprel;
    int8_t ea_base;     /* -1: none */
    int8_t ea_index;    /* -1: none */
    uint8_t ea_scale;
    int64_t ea_disp;
} insn_t;

#define REX_W(i) (((i)->rex & 8u) != 0u)
#define REX_R(i) (((i)->rex & 4u) != 0u)
#define REX_X(i) (((i)->rex & 2u) != 0u)
#define REX_B(i) (((i)->rex & 1u) != 0u)

static inline uint8_t fetch8(insn_t *i)
{
    uint8_t v = *(const uint8_t *)(uintptr_t)i->pc;
    i->pc += 1;
    return v;
}

static inline uint16_t fetch16(insn_t *i)
{
    uint16_t v;
    __builtin_memcpy(&v, (const void *)(uintptr_t)i->pc, 2);
    i->pc += 2;
    return v;
}

static inline uint32_t fetch32(insn_t *i)
{
    uint32_t v;
    __builtin_memcpy(&v, (const void *)(uintptr_t)i->pc, 4);
    i->pc += 4;
    return v;
}

static inline uint64_t fetch64(insn_t *i)
{
    uint64_t v;
    __builtin_memcpy(&v, (const void *)(uintptr_t)i->pc, 8);
    i->pc += 8;
    return v;
}

/* Decode ModRM (+ SIB + displacement) at the cursor. */
void x86_modrm(insn_t *i);
/* The memory operand's address; call after every immediate has been
 * fetched (a RIP-relative address counts from the end of the instruction). */
uint64_t x86_ea(insn_t *i);

/* Raise a synchronous guest signal for the current instruction. */
int x86_fault(insn_t *i, int sig, int code, uint64_t addr);
#define X86_UD(i) x86_fault((i), XL_SIGILL, XL_ILL_ILLOPN, (i)->start)

/* EFLAGS */
uint64_t x86_get_flags(x86cpu_t *c);
void x86_set_flags(x86cpu_t *c, uint64_t f);   /* arithmetic bits */

/* Instruction groups in other files. Return 0, or an XL_EV_* event. */
int x86_sse(insn_t *i, uint8_t op);             /* 0F xx (SSE, SSE2, MMX) */
int x86_sse_body(insn_t *i, uint8_t op, unsigned imm);   /* ModRM decoded */
bool x86_sse_has_imm8(uint8_t op);
int x86_x87(insn_t *i, uint8_t op);             /* D8..DF */
int x86_fxsave(insn_t *i, uint64_t addr);
int x86_fxrstor(insn_t *i, uint64_t addr);
void x86_fpu_reset(x86cpu_t *c);
void x86_cpuid(x86cpu_t *c);
/* Point the host FP unit's rounding mode etc. at the guest MXCSR. */
void x86_mxcsr_apply(x86cpu_t *c);
/* Fold the host's sticky FP exception flags into the guest MXCSR. */
void x86_mxcsr_collect(x86cpu_t *c);
uint16_t x86_fpu_status(const x86cpu_t *c);
uint16_t x86_fpu_tagword_abridged(const x86cpu_t *c);
/* x87 <-> 80-bit memory format */
x87_t x87_from_80(const uint8_t *p);
void x87_to_80(x87_t v, uint8_t *p);
