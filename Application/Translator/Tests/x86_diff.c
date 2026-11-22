/* Differential test for the x86_64 guest (see ../README.md, "Testing"):
 * run under Rosetta (macOS x86_64 build) for the reference and under the
 * translator (static Linux build), and diff the output. Only
 * architecturally defined results and flags are printed. Build with
 * -mno-red-zone (the asm pushes and pops). */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#ifdef __APPLE__
long write(int, const void *, unsigned long);
static void out(const char *s, int n) { write(1, s, (unsigned long)n); }
#else
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static void out(const char *s, int n) { sys3(1, 1, (long)s, n); }
#endif

static char buf[256];
static int bl;
static void ps(const char *s) { while (*s) buf[bl++] = *s++; }
static void ph(u64 v) { buf[bl++] = ' '; for (int i = 60; i >= 0; i -= 4) buf[bl++] = "0123456789abcdef"[(v >> i) & 15]; }
static void nl(void) { buf[bl++] = '\n'; out(buf, bl); bl = 0; }

static const u64 vals[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
    0xffffffff, 0x7fffffffffffffffUL, 0x8000000000000000UL, 0xffffffffffffffffUL,
    0x123456789abcdef0UL, 0xfedcba9876543210UL, 0x00ff00ff00ff00ffUL, 3, 31, 33, 63, 64,
};
#define NV (sizeof(vals) / sizeof(vals[0]))

#define ARITH 0x8d5UL
#define LOGIC 0x8c5UL      /* AF undefined */
#define CF 1UL
#define SHMASK 0x0c5UL     /* CF ZF SF PF; OF only for count 1 */

static u64 szmask(int s) { return s == 8 ? ~0UL : (1UL << (s * 8)) - 1; }

#define BIN(mn, mod, IF) ({ u64 _r = a, _f; __asm__ volatile( \
    "pushq %3\n\tpopfq\n\t" mn " %" mod "2, %" mod "0\n\tpushfq\n\tpopq %1" \
    : "+r"(_r), "=&r"(_f) : "r"(b), "r"(IF) : "cc", "memory"); r = _r; f = _f; })
#define UN(mn, mod, IF) ({ u64 _r = a, _f; __asm__ volatile( \
    "pushq %2\n\tpopfq\n\t" mn " %" mod "0\n\tpushfq\n\tpopq %1" \
    : "+r"(_r), "=&r"(_f) : "r"(IF) : "cc", "memory"); r = _r; f = _f; })
#define SHC(mn, mod, IF) ({ u64 _r = a, _f; __asm__ volatile( \
    "pushq %3\n\tpopfq\n\t" mn " %%cl, %" mod "0\n\tpushfq\n\tpopq %1" \
    : "+r"(_r), "=&r"(_f) : "c"(b), "r"(IF) : "cc", "memory"); r = _r; f = _f; })

static void rep(const char *n, int sz, u64 a, u64 b, u64 r, u64 f, u64 mask)
{
    ps(n); ph((u64)sz); ph(a & szmask(sz)); ph(b); ph(r & szmask(sz)); ph(f & mask); nl();
}

#define BIN4(name, mask) do { \
    for (unsigned i = 0; i < NV; ++i) for (unsigned j = 0; j < NV; ++j) for (u64 cin = 0; cin < 2; ++cin) { \
        u64 a = vals[i], b = vals[j], r, f, IF = 0x202 | cin; \
        BIN(name "b", "b", IF); rep(name, 1, a, b, r, f, mask); \
        BIN(name "w", "w", IF); rep(name, 2, a, b, r, f, mask); \
        BIN(name "l", "k", IF); rep(name, 4, a, b, r, f, mask); \
        BIN(name "q", "q", IF); rep(name, 8, a, b, r, f, mask); } } while (0)

#define UN4(name, mask) do { \
    for (unsigned i = 0; i < NV; ++i) for (u64 cin = 0; cin < 2; ++cin) { \
        u64 a = vals[i], b = cin, r, f, IF = 0x202 | cin; \
        UN(name "b", "b", IF); rep(name, 1, a, b, r, f, mask); \
        UN(name "w", "w", IF); rep(name, 2, a, b, r, f, mask); \
        UN(name "l", "k", IF); rep(name, 4, a, b, r, f, mask); \
        UN(name "q", "q", IF); rep(name, 8, a, b, r, f, mask); } } while (0)

#define SH4(name, mask) do { \
    for (unsigned i = 0; i < NV; ++i) for (u64 cnt = 0; cnt < 66; cnt += (cnt < 9 ? 1 : 7)) for (u64 cin = 0; cin < 2; ++cin) { \
        u64 a = vals[i], b = cnt, r, f, IF = 0x202 | cin; \
        u64 m1 = mask | ((b & 31) == 1 ? 0x800UL : 0); \
        SHC(name "b", "b", IF); if ((b & 31) <= 8 || name[0] == 's') rep(name, 1, a, b, r, f, (b & 31) <= 8 ? m1 : 0); \
        SHC(name "w", "w", IF); if ((b & 31) <= 16 || name[0] == 's') rep(name, 2, a, b, r, f, (b & 31) <= 16 ? m1 : 0); \
        SHC(name "l", "k", IF); rep(name, 4, a, b, r, f, m1); \
        u64 m2 = mask | ((b & 63) == 1 ? 0x800UL : 0); \
        SHC(name "q", "q", IF); rep(name, 8, a, b, r, f, m2); } } while (0)

static void t_int(void)
{
    BIN4("add", ARITH); BIN4("sub", ARITH); BIN4("adc", ARITH); BIN4("sbb", ARITH);
    BIN4("and", LOGIC); BIN4("or", LOGIC); BIN4("xor", LOGIC); BIN4("cmp", ARITH);
    BIN4("test", LOGIC);
    UN4("neg", ARITH); UN4("not", 0xfff); UN4("inc", ARITH); UN4("dec", ARITH);
    SH4("shl", SHMASK); SH4("shr", SHMASK); SH4("sar", SHMASK);
    SH4("rol", CF); SH4("ror", CF); SH4("rcl", CF); SH4("rcr", CF);
    for (unsigned i = 0; i < NV; ++i) for (unsigned j = 0; j < NV; ++j) {
        u64 a = vals[i], b = vals[j], lo, hi, f;
        __asm__ volatile("mulq %3\n\tpushfq\n\tpopq %2" : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
        rep("mulq", 8, a, b, lo ^ (hi * 3), f, 0x801);
        __asm__ volatile("imulq %3\n\tpushfq\n\tpopq %2" : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
        rep("imulq", 8, a, b, lo ^ (hi * 3), f, 0x801);
        u64 r = a;
        __asm__ volatile("imull %k2, %k0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(b) : "cc");
        rep("imul2", 4, a, b, r, f, 0x801);
        __asm__ volatile("mull %k3\n\tpushfq\n\tpopq %2" : "=a"(lo), "=d"(hi), "=r"(f) : "r"(b), "0"(a) : "cc");
        rep("mull", 4, a, b, (lo & 0xffffffff) | (hi << 32), f, 0x801);
        if (b != 0) {
            u64 q, rm;
            __asm__ volatile("divq %4" : "=a"(q), "=d"(rm) : "0"(a), "1"(0UL), "r"(b));
            rep("divq", 8, a, b, q ^ (rm << 1), 0, 0);
            if ((long)b != -1 || a != 0x8000000000000000UL) {
                q = a; __asm__ volatile("cqo\n\tidivq %2" : "+a"(q), "=&d"(rm) : "r"(b));
                rep("idivq", 8, a, b, q ^ (rm << 1), 0, 0);
            }
            u32 b32 = (u32)b;
            if (b32 != 0) {
                u64 q2, r2;
                __asm__ volatile("divl %k4" : "=a"(q2), "=d"(r2) : "0"(a & 0xffffffff), "1"(0UL), "r"((u64)b32));
                rep("divl", 4, a, b, q2 | (r2 << 32), 0, 0);
            }
        }
        r = a;
        __asm__ volatile("btq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(b & 63) : "cc");
        rep("bt", 8, a, b, r, f, 1);
        r = a;
        __asm__ volatile("btsq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(b & 63) : "cc");
        rep("bts", 8, a, b, r, f, 1);
        r = a;
        __asm__ volatile("shldq %%cl, %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(b), "c"(b & 63) : "cc");
        rep("shld", 8, a, b & 63, r, f, (b & 63) ? 0xc5 | ((b & 63) == 1 ? 0x800 : 0) : 0);
        r = a;
        __asm__ volatile("shrdq %%cl, %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(b), "c"(b & 63) : "cc");
        rep("shrd", 8, a, b & 63, r, f, (b & 63) ? 0xc5 | ((b & 63) == 1 ? 0x800 : 0) : 0);
        /* cmov/setcc over the flags of cmp a,b */
        u64 sets = 0, cm = 0;
#define CC(n, i) do { u8 s; u64 x = 0; __asm__ volatile("cmpq %2, %3\n\tset" n " %0\n\tcmov" n " %4, %1" \
                    : "=r"(s), "+r"(x) : "r"(b), "r"(a), "r"((u64)i + 1) : "cc"); sets |= (u64)s << i; cm += x; } while (0)
        CC("o", 0); CC("no", 1); CC("b", 2); CC("ae", 3); CC("e", 4); CC("ne", 5); CC("be", 6); CC("a", 7);
        CC("s", 8); CC("ns", 9); CC("p", 10); CC("np", 11); CC("l", 12); CC("ge", 13); CC("le", 14); CC("g", 15);
        rep("cc", 8, a, b, sets, cm, 0);
    }
    for (unsigned i = 0; i < NV; ++i) {
        u64 a = vals[i], r = 0, f;
        if (a) {
            __asm__ volatile("bsfq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(a) : "cc");
            rep("bsf", 8, a, 0, r, f, 0x40);
            __asm__ volatile("bsrq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(r), "=r"(f) : "r"(a) : "cc");
            rep("bsr", 8, a, 0, r, f, 0x40);
        }
        r = a; __asm__ volatile("bswapq %0" : "+r"(r)); rep("bswapq", 8, a, 0, r, 0, 0);
        r = a; __asm__ volatile("bswapl %k0" : "+r"(r)); rep("bswapl", 4, a, 0, r, 0, 0);
        r = 0; __asm__ volatile("movsbq %b1, %0" : "=r"(r) : "r"(a)); rep("movsbq", 8, a, 0, r, 0, 0);
        r = 0; __asm__ volatile("movswl %w1, %k0" : "=r"(r) : "r"(a)); rep("movswl", 8, a, 0, r, 0, 0);
        r = 0; __asm__ volatile("movzbl %b1, %k0" : "=r"(r) : "r"(a)); rep("movzbl", 8, a, 0, r, 0, 0);
        r = 0; __asm__ volatile("movslq %k1, %0" : "=r"(r) : "r"(a)); rep("movslq", 8, a, 0, r, 0, 0);
        u64 cx = a, acc = 5, mem = a;
        __asm__ volatile("lock cmpxchgq %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f), "+m"(mem), "+r"(cx), "+a"(acc) : : "cc");
        rep("cmpxchg", 8, a, acc, mem, f, ARITH);
        mem = a; cx = 7;
        __asm__ volatile("lock xaddq %1, %0" : "+m"(mem), "+r"(cx) : : "cc");
        rep("xadd", 8, a, cx, mem, 0, 0);
        r = a;
        __asm__ volatile("leaq 0x12(%1,%1,4), %0" : "=r"(r) : "r"(a));
        rep("lea", 8, a, 0, r, 0, 0);
    }
}

/* ---- SSE / SSE2 / SSE3 ---- */
typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef long long v2q __attribute__((vector_size(16)));

static const u64 dv[] = {
    0, 0x8000000000000000UL, 0x3ff0000000000000UL, 0xbff8000000000000UL, 0x400a000000000000UL,
    0x7e37e43c8800759cUL, 0x7ff0000000000000UL, 0xfff0000000000000UL, 0x7ff8000000000000UL,
    0xfff8000000000001UL, 0x7ff4000000000000UL, 0x0000000000000001UL, 0x43e0000000000000UL,
    0xc3e0000000000000UL, 0x41e0000000000000UL, 0x3fe0000000000000UL, 0x4004000000000000UL,
    0xc004000000000000UL, 0x3fd5555555555555UL,
};
#define ND (sizeof(dv) / sizeof(dv[0]))
static const u32 fv[] = {
    0, 0x80000000u, 0x3f800000u, 0xbfc00000u, 0x40500000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0xffc00001u, 0x7fa00000u, 0x00000001u, 0x5f000000u, 0xcf000000u, 0x4f000000u,
    0x3f000000u, 0x40200000u, 0xc0200000u, 0x3eaaaaabu,
};
#define NF (sizeof(fv) / sizeof(fv[0]))

static void px(const char *n, u64 a, u64 b, v2q r) { ps(n); ph(a); ph(b); ph((u64)r[0]); ph((u64)r[1]); nl(); }

#define XOP(insn) do { v2q x = A, y = B; __asm__ volatile(insn " %1, %0" : "+x"(x) : "x"(y)); px(insn, a, b, x); } while (0)
#define XOPI(insn, imm) do { v2q x = A, y = B; __asm__ volatile(insn " $" #imm ", %1, %0" : "+x"(x) : "x"(y)); px(insn #imm, a, b, x); } while (0)

static void t_sse(void)
{
    for (unsigned i = 0; i < ND; ++i) for (unsigned j = 0; j < ND; ++j) {
        u64 a = dv[i], b = dv[j];
        v2q A = { (long long)a, (long long)dv[(i + 3) % ND] }, B = { (long long)b, (long long)dv[(j + 5) % ND] };
        XOP("addsd"); XOP("subsd"); XOP("mulsd"); XOP("divsd"); XOP("minsd"); XOP("maxsd"); XOP("sqrtsd");
        XOP("addpd"); XOP("mulpd"); XOP("minpd"); XOP("maxpd"); XOP("andpd"); XOP("xorpd");
        XOP("haddpd"); XOP("hsubpd"); XOP("addsubpd"); XOP("movddup"); XOP("unpcklpd"); XOP("unpckhpd");
        XOP("cvtsd2ss"); XOP("cvtpd2ps"); XOP("cvttpd2dq"); XOP("cvtpd2dq");
        XOPI("cmpsd", 0); XOPI("cmpsd", 1); XOPI("cmpsd", 3); XOPI("cmpsd", 6); XOPI("shufpd", 1);
        u64 f, r1, r2;
        v2q x = A, y = B;
        __asm__ volatile("ucomisd %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(x), "x"(y) : "cc");
        __asm__ volatile("cvttsd2si %1, %0" : "=r"(r1) : "x"(x));
        __asm__ volatile("cvtsd2si %1, %0" : "=r"(r2) : "x"(x));
        ps("ucomisd/cvt"); ph(a); ph(b); ph(f & 0x8d5); ph(r1); ph(r2); nl();
        __asm__ volatile("cvttsd2si %1, %k0" : "=r"(r1) : "x"(x));
        __asm__ volatile("cvtsd2si %1, %k0" : "=r"(r2) : "x"(x));
        ps("cvt32"); ph(a); ph(r1 & 0xffffffff); ph(r2 & 0xffffffff); nl();
    }
    for (unsigned i = 0; i < NF; ++i) for (unsigned j = 0; j < NF; ++j) {
        u64 a = fv[i] | ((u64)fv[(i + 2) % NF] << 32), b = fv[j] | ((u64)fv[(j + 7) % NF] << 32);
        v2q A = { (long long)a, (long long)(fv[(i + 4) % NF] | ((u64)fv[(i + 1) % NF] << 32)) };
        v2q B = { (long long)b, (long long)(fv[(j + 3) % NF] | ((u64)fv[(j + 9) % NF] << 32)) };
        XOP("addps"); XOP("subps"); XOP("mulps"); XOP("divps"); XOP("minps"); XOP("maxps"); XOP("sqrtps");
        XOP("addss"); XOP("minss"); XOP("maxss");
        XOP("haddps"); XOP("hsubps"); XOP("addsubps"); XOP("movshdup"); XOP("movsldup");
        XOP("cvtps2pd"); XOP("cvtss2sd"); XOP("cvttps2dq"); XOP("cvtps2dq"); XOP("cvtdq2ps");
        XOP("unpcklps"); XOP("unpckhps"); XOP("movhlps"); XOP("movlhps");
        XOPI("cmpps", 1); XOPI("cmpps", 4); XOPI("cmpps", 7); XOPI("shufps", 0x1b); XOPI("shufps", 0xe4);
        u64 f;
        v2q x = A, y = B;
        __asm__ volatile("comiss %2, %1\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(x), "x"(y) : "cc");
        ps("comiss"); ph(a); ph(b); ph(f & 0x8d5); nl();
    }
    /* integer SIMD on mixed lanes */
    for (unsigned i = 0; i < NV; ++i) for (unsigned j = 0; j < NV; j += 2) {
        u64 a = vals[i] * 0x9e3779b97f4a7c15UL, b = vals[j] ^ 0x5555aaaa33cc0ff0UL;
        v2q A = { (long long)a, (long long)(a >> 7 | a << 57) }, B = { (long long)b, (long long)(b * 3) };
        XOP("paddb"); XOP("paddw"); XOP("paddd"); XOP("paddq"); XOP("psubb"); XOP("psubusb"); XOP("psubsw");
        XOP("paddusw"); XOP("paddsb"); XOP("pmullw"); XOP("pmulhw"); XOP("pmulhuw"); XOP("pmuludq");
        XOP("pmaddwd"); XOP("psadbw"); XOP("pavgb"); XOP("pavgw"); XOP("pminub"); XOP("pmaxsw");
        XOP("pcmpeqb"); XOP("pcmpeqd"); XOP("pcmpgtw"); XOP("pcmpgtd"); XOP("packsswb"); XOP("packuswb");
        XOP("packssdw"); XOP("punpcklbw"); XOP("punpckhwd"); XOP("punpckldq"); XOP("punpckhqdq");
        XOP("pand"); XOP("pandn"); XOP("por"); XOP("pxor"); XOP("psllw"); XOP("psrad"); XOP("psrlq");
        XOPI("pshufd", 0x1b); XOPI("pshufhw", 0x39); XOPI("pshuflw", 0xc6);
        v2q x = A;
        u64 m;
        __asm__ volatile("pmovmskb %1, %k0" : "=r"(m) : "x"(x));
        ps("pmovmskb"); ph(a); ph(m); nl();
        __asm__ volatile("psrldq $5, %0" : "+x"(x)); px("psrldq5", a, 0, x);
        x = A; __asm__ volatile("pslldq $3, %0" : "+x"(x)); px("pslldq3", a, 0, x);
        x = A; __asm__ volatile("psraw $3, %0" : "+x"(x)); px("psraw3", a, 0, x);
        v2q y;
        __asm__ volatile("lddqu %1, %0" : "=x"(y) : "m"(B)); px("lddqu", b, 0, y);
    }
}

/* ---- x87 ---- */
static void t_x87(void)
{
    static const u64 xv[] = { 0x3ff0000000000000UL, 0x4008000000000000UL, 0xbfe0000000000000UL,
                              0x400921fb54442d18UL, 0x3fd5555555555555UL, 0x4197d78400000000UL, 0 };
    for (unsigned i = 0; i < 7; ++i) for (unsigned j = 0; j < 7; ++j) {
        double a, b;
        __builtin_memcpy(&a, &xv[i], 8);
        __builtin_memcpy(&b, &xv[j], 8);
        u8 ext[10];
        u64 r1, r2, f;
        long long ist;
        __asm__ volatile("fldl %3\n\tfldl %4\n\tfaddp\n\tfldl %3\n\tfmulp\n\tfldl %4\n\tfdivrp\n\t"
                         "fld %%st(0)\n\tfstpt %0\n\tfistpll %1\n\tfldl %3\n\tfsqrt\n\tfstpl %2"
                         : "=m"(ext), "=m"(ist), "=m"(r2) : "m"(a), "m"(b));
        __builtin_memcpy(&r1, ext, 8);
        u16 hi;
        __builtin_memcpy(&hi, ext + 8, 2);
        __asm__ volatile("fldl %2\n\tfldl %1\n\tfcomip %%st(1), %%st\n\tfstp %%st(0)\n\tpushfq\n\tpopq %0"
                         : "=r"(f) : "m"(a), "m"(b) : "cc");
        ps("x87"); ph(xv[i]); ph(xv[j]); ph(r1); ph(hi); ph((u64)ist); ph(r2); ph(f & 0x45); nl();
    }
}

/* ---- SSE forms a translator may handle natively: every immediate, every
 * shift count, memory operands, moves between registers and memory ---- */

#define I4(M, b) M(b) M((b) + 1) M((b) + 2) M((b) + 3)
#define I16(M, b) I4(M, b) I4(M, (b) + 4) I4(M, (b) + 8) I4(M, (b) + 12)
#define I64(M, b) I16(M, b) I16(M, (b) + 16) I16(M, (b) + 32) I16(M, (b) + 48)
#define I256(M) I64(M, 0) I64(M, 64) I64(M, 128) I64(M, 192)
#define I8(M) I4(M, 0) I4(M, 4)

static void pxi(const char *n, unsigned imm, v2q a, v2q b, v2q r)
{
    ps(n); ph(imm); ph((u64)a[0]); ph((u64)a[1]); ph((u64)b[0]); ph((u64)b[1]); ph((u64)r[0]); ph((u64)r[1]); nl();
}

#define XI(insn, imm) do { v2q x = A, y = B; __asm__ volatile(insn " $" #imm ", %1, %0" : "+x"(x) : "x"(y)); \
    pxi(insn, (imm), A, B, x); } while (0);
#define XIM(insn, imm) do { v2q x = A, y = B; __asm__ volatile(insn " $" #imm ", %1, %0" : "+x"(x) : "m"(y)); \
    pxi(insn "-m", (imm), A, B, x); } while (0);
#define XS(insn, imm) do { v2q x = A; __asm__ volatile(insn " $" #imm ", %0" : "+x"(x)); \
    pxi(insn, (imm), A, A, x); } while (0);
#define XM(insn) do { v2q x = A, y = B; __asm__ volatile(insn " %1, %0" : "+x"(x) : "m"(y)); \
    pxi(insn "-m", 0, A, B, x); } while (0)

#define M_PSHUFD(i) XI("pshufd", i)
#define M_PSHUFLW(i) XI("pshuflw", i)
#define M_PSHUFHW(i) XI("pshufhw", i)
#define M_SHUFPS(i) XI("shufps", i)
#define M_PSHUFDM(i) XIM("pshufd", i)
#define M_PSLLW(i) XS("psllw", i)
#define M_PSRLW(i) XS("psrlw", i)
#define M_PSRAW(i) XS("psraw", i)
#define M_PSLLD(i) XS("pslld", i)
#define M_PSRLD(i) XS("psrld", i)
#define M_PSRAD(i) XS("psrad", i)
#define M_PSLLQ(i) XS("psllq", i)
#define M_PSRLQ(i) XS("psrlq", i)
#define M_PSLLDQ(i) XS("pslldq", i)
#define M_PSRLDQ(i) XS("psrldq", i)
#define M_SHUFPD(i) XI("shufpd", i)
#define M_CMPPS(i) XI("cmpps", i)
#define M_CMPSS(i) XI("cmpss", i)
#define M_CMPPD(i) XI("cmppd", i)
#define M_CMPSD(i) XI("cmpsd", i)
#define M_PEXTRW(i) do { u64 g = 0x5a5a5a5a5a5a5a5aUL; __asm__ volatile("pextrw $" #i ", %1, %k0" : "+r"(g) : "x"(A)); \
    v2q r = { (long long)g, 0 }; pxi("pextrw", (i), A, A, r); } while (0);
#define M_PINSRW(i) do { v2q x = A; u64 g = (u64)B[0]; __asm__ volatile("pinsrw $" #i ", %k1, %0" : "+x"(x) : "r"(g)); \
    pxi("pinsrw", (i), A, B, x); } while (0);

static void t_sse2(void)
{
    static const u64 lanes[][2] = {
        { 0x0123456789abcdefUL, 0xfedcba9876543210UL }, { 0x8000ffff7fff0001UL, 0x00ff80017ffe8000UL },
        { 0x3ff0000000000000UL, 0xc000000000000000UL }, { 0x7fc00000bf800000UL, 0x3f80000000000000UL },
        { 0xffffffffffffffffUL, 0x0000000000000000UL },
    };
    const unsigned NL = sizeof(lanes) / sizeof(lanes[0]);
    for (unsigned i = 0; i < NL; ++i) {
        const unsigned j = (i + 1) % NL;
        const v2q A = { (long long)lanes[i][0], (long long)lanes[i][1] };
        const v2q B = { (long long)lanes[j][0], (long long)lanes[j][1] };
        I256(M_PSHUFD) I256(M_PSHUFLW) I256(M_PSHUFHW) I256(M_SHUFPS) I4(M_SHUFPD, 0)
        I16(M_PSHUFDM, 0x5a)
        I64(M_PSLLW, 0) I64(M_PSRLW, 0) I64(M_PSRAW, 0) I64(M_PSLLD, 0) I64(M_PSRLD, 0)
        I64(M_PSRAD, 0) I64(M_PSLLQ, 0) I64(M_PSRLQ, 0) I4(M_PSLLDQ, 252)
        I16(M_PSLLDQ, 0) I16(M_PSRLDQ, 0) M_PSRLDQ(255)
        I8(M_PEXTRW) I8(M_PINSRW) I8(M_CMPPS) I8(M_CMPSS) I8(M_CMPPD) I8(M_CMPSD)
        XM("addpd"); XM("mulsd"); XM("addss"); XM("minpd"); XM("maxss"); XM("divps"); XM("sqrtsd");
        XM("pxor"); XM("paddb"); XM("psubw"); XM("pcmpeqb"); XM("pcmpgtd"); XM("punpcklbw");
        XM("punpckhdq"); XM("packuswb"); XM("packssdw"); XM("pmaddwd"); XM("psadbw"); XM("pmulhw");
        XM("pmuludq"); XM("pminub"); XM("pavgw"); XM("unpcklps"); XM("unpckhpd"); XM("andnps");
        XM("cvtss2sd"); XM("cvtsd2ss"); XM("cvtdq2ps"); XM("movsd"); XM("movss"); XM("movddup");
        XM("movlps"); XM("movhps"); XM("movlpd"); XM("movhpd"); XM("movdqa"); XM("movdqu");
        XM("movups"); XM("movq"); XM("lddqu");
        {   /* stores and moves to/from general registers */
            v2q m = B, x = A;
            __asm__ volatile("movlps %1, %0" : "+m"(m) : "x"(x)); pxi("movlps-st", 0, A, B, m);
            m = B; __asm__ volatile("movhps %1, %0" : "+m"(m) : "x"(x)); pxi("movhps-st", 0, A, B, m);
            m = B; __asm__ volatile("movss %1, %0" : "+m"(m) : "x"(x)); pxi("movss-st", 0, A, B, m);
            m = B; __asm__ volatile("movsd %1, %0" : "+m"(m) : "x"(x)); pxi("movsd-st", 0, A, B, m);
            m = B; __asm__ volatile("movq %1, %0" : "+m"(m) : "x"(x)); pxi("movq-st", 0, A, B, m);
            m = B; __asm__ volatile("movd %1, %0" : "+m"(m) : "x"(x)); pxi("movd-st", 0, A, B, m);
            v2q y = B; __asm__ volatile("movss %1, %0" : "+x"(y) : "x"(x)); pxi("movss-rr", 0, A, B, y);
            y = B; __asm__ volatile("movsd %1, %0" : "+x"(y) : "x"(x)); pxi("movsd-rr", 0, A, B, y);
            y = B; __asm__ volatile("movq %1, %0" : "+x"(y) : "x"(x)); pxi("movq-rr", 0, A, B, y);
            y = B; __asm__ volatile("movhlps %1, %0" : "+x"(y) : "x"(x)); pxi("movhlps", 0, A, B, y);
            y = B; __asm__ volatile("movlhps %1, %0" : "+x"(y) : "x"(x)); pxi("movlhps", 0, A, B, y);
            u64 g = (u64)B[1], r1 = 0, r2 = 0;
            __asm__ volatile("movq %1, %0" : "=x"(y) : "r"(g)); pxi("movq-gx", 0, A, B, y);
            __asm__ volatile("movd %k1, %0" : "=x"(y) : "r"(g)); pxi("movd-gx", 0, A, B, y);
            __asm__ volatile("movq %1, %0" : "=r"(r1) : "x"(x));
            __asm__ volatile("movd %1, %k0" : "=r"(r2) : "x"(x));
            __asm__ volatile("pmovmskb %1, %k0" : "=r"(g) : "x"(x));
            u64 mp = 0, md = 0;
            __asm__ volatile("movmskps %1, %k0" : "=r"(mp) : "x"(x));
            __asm__ volatile("movmskpd %1, %k0" : "=r"(md) : "x"(x));
            v2q r = { (long long)r1, (long long)r2 }; pxi("movq/movd-xg", 0, A, B, r);
            v2q r3 = { (long long)g, (long long)(mp | md << 8) }; pxi("movmsk", 0, A, B, r3);
            long long cv = (long long)A[0] >> 3;
            __asm__ volatile("cvtsi2sdq %1, %0" : "+x"(y) : "m"(cv)); pxi("cvtsi2sd-m", 0, A, B, y);
            __asm__ volatile("cvtsi2ssl %1, %0" : "+x"(y) : "m"(cv)); pxi("cvtsi2ss-m", 0, A, B, y);
            __asm__ volatile("cvttsd2si %1, %0" : "=r"(r1) : "m"(B)); 
            __asm__ volatile("cvtss2si %1, %0" : "=r"(r2) : "m"(B));
            v2q r4 = { (long long)r1, (long long)r2 }; pxi("cvt-m", 0, A, B, r4);
        }
    }
}

/* ---- conditions read straight after the instruction that set them -------
 * (a translator may evaluate them from the instruction's operands instead
 * of computing EFLAGS): all sixteen with SETcc, then with Jcc, which
 * leaves the translated block and reads them again on the other side. */

#define SET16 \
    "seto %%al\n\tmovb %%al, 0(%2)\n\tsetno %%al\n\tmovb %%al, 1(%2)\n\t" \
    "setb %%al\n\tmovb %%al, 2(%2)\n\tsetae %%al\n\tmovb %%al, 3(%2)\n\t" \
    "sete %%al\n\tmovb %%al, 4(%2)\n\tsetne %%al\n\tmovb %%al, 5(%2)\n\t" \
    "setbe %%al\n\tmovb %%al, 6(%2)\n\tseta %%al\n\tmovb %%al, 7(%2)\n\t" \
    "sets %%al\n\tmovb %%al, 8(%2)\n\tsetns %%al\n\tmovb %%al, 9(%2)\n\t" \
    "setp %%al\n\tmovb %%al, 10(%2)\n\tsetnp %%al\n\tmovb %%al, 11(%2)\n\t" \
    "setl %%al\n\tmovb %%al, 12(%2)\n\tsetge %%al\n\tmovb %%al, 13(%2)\n\t" \
    "setle %%al\n\tmovb %%al, 14(%2)\n\tsetg %%al\n\tmovb %%al, 15(%2)\n\t"

#define JC(n, k) "j" n " 1f\n\tmovb $0, " #k "(%2)\n\tjmp 2f\n1:\n\tmovb $1, " #k "(%2)\n2:\n\t"
#define J16 JC("o", 0) JC("no", 1) JC("b", 2) JC("ae", 3) JC("e", 4) JC("ne", 5) JC("be", 6) \
    JC("a", 7) JC("s", 8) JC("ns", 9) JC("p", 10) JC("np", 11) JC("l", 12) JC("ge", 13) \
    JC("le", 14) JC("g", 15)

static u64 ccmask(const u8 *cc)
{
    u64 m = 0;
    for (int k = 0; k < 16; ++k) m |= (u64)(cc[k] & 1u) << k;
    return m;
}

/* defined conditions: all; OF undefined (shift counts > 1); only CF/OF (IMUL) */
#define M_ALL 0xffffUL
#define M_NOOF 0x0ffcUL
#define M_CO 0x000fUL

#define CS(tag, msk, code) do { u64 _r = a; u8 _cc[16]; \
    __asm__ volatile(code "\n\t" SET16 : "+r"(_r) : "r"(b), "r"(_cc) : "rax", "rbx", "rcx", "memory", "cc"); \
    ps(tag); ph(a); ph(b); ph(_r); ph(ccmask(_cc) & (msk)); nl(); } while (0)
#define CJ(tag, msk, code) do { u64 _r = a; u8 _cc[16]; \
    __asm__ volatile(code "\n\t" J16 : "+r"(_r) : "r"(b), "r"(_cc) : "rax", "rbx", "rcx", "memory", "cc"); \
    ps(tag); ph(a); ph(b); ph(_r); ph(ccmask(_cc) & (msk)); nl(); } while (0)
#define C4(tag, msk, mn) do { CS(tag "b", msk, mn "b %b1, %b0"); CS(tag "w", msk, mn "w %w1, %w0"); \
    CS(tag "l", msk, mn "l %k1, %k0"); CS(tag "q", msk, mn "q %1, %0"); } while (0)
#define C4U(tag, msk, mn) do { CS(tag "b", msk, mn "b %b0"); CS(tag "w", msk, mn "w %w0"); \
    CS(tag "l", msk, mn "l %k0"); CS(tag "q", msk, mn "q %0"); } while (0)

/* LOCKed read-modify-writes on memory (aligned and not), and the
 * instructions that change only some flags */
#define CM(tag, msk, code) do { u64 _r = a; u8 _cc[16]; \
    __asm__ volatile(code "\n\t" SET16 : "+r"(_r) : "r"(b), "r"(_cc), "r"(p) : "rax", "rbx", "rcx", "memory", "cc"); \
    ps(tag); ph(a); ph(b); ph(_r); ph(*(volatile u64 *)p); ph(ccmask(_cc) & (msk)); nl(); } while (0)
#define M_Z 0x0030UL

static void t_cond_atomic(u64 a)
{
    static u64 cell[4];
    for (int mis = 0; mis < 2; ++mis) {
        u64 *p = (u64 *)((char *)&cell[1] + (mis ? 3 : 0));
        for (unsigned j = 0; j < NV; j += 3) {
            const u64 b = vals[j];
            /* plain read-modify-write of memory */
            *(volatile u64 *)p = b; CM("m.addq", M_ALL, "addq %0, (%3)");
            *(volatile u64 *)p = b; CM("m.subl", M_ALL, "subl %k0, (%3)");
            *(volatile u64 *)p = b; CM("m.andw", M_ALL, "andw %w0, (%3)");
            *(volatile u64 *)p = b; CM("m.orb", M_ALL, "orb %b0, (%3)");
            *(volatile u64 *)p = b; CM("m.xorq", M_ALL, "xorq %0, (%3)");
            *(volatile u64 *)p = b; CM("m.adcq", M_ALL, "stc\n\tadcq %0, (%3)");
            *(volatile u64 *)p = b; CM("m.sbbl", M_ALL, "stc\n\tsbbl %k0, (%3)");
            *(volatile u64 *)p = b; CM("m.addqi", M_ALL, "addq $-3, (%3)");
            *(volatile u64 *)p = b; CM("m.subbi", M_ALL, "subb $7, (%3)");
            *(volatile u64 *)p = b; CM("m.andli", M_ALL, "andl $0x7f00ff, (%3)");
            *(volatile u64 *)p = b; CM("m.cmpq", M_ALL, "cmpq %0, (%3)");
            *(volatile u64 *)p = b; CM("m.testw", M_ALL, "testw %w0, (%3)");
            *(volatile u64 *)p = b; CM("m.incq", M_ALL, "incq (%3)");
            *(volatile u64 *)p = b; CM("m.decq", M_ALL, "decq (%3)");
            *(volatile u64 *)p = b; CM("m.incl", M_ALL, "incl (%3)");
            *(volatile u64 *)p = b; CM("m.decw", M_ALL, "decw (%3)");
            *(volatile u64 *)p = b; CM("m.incb", M_ALL, "incb (%3)");
            *(volatile u64 *)p = b; CM("m.notq", 0, "notq (%3)");
            *(volatile u64 *)p = b; CM("m.notb", 0, "notb (%3)");
            *(volatile u64 *)p = b; CM("m.negq", M_ALL, "negq (%3)");
            *(volatile u64 *)p = b; CM("m.negl", M_ALL, "negl (%3)");
            *(volatile u64 *)p = b; CM("m.negw", M_ALL, "negw (%3)");
            *(volatile u64 *)p = b; CM("m.movq", 0, "movq %0, (%3)");
            *(volatile u64 *)p = b; CM("m.movbi", 0, "movb $0x5a, (%3)");
            *(volatile u64 *)p = b; CM("m.movqi", 0, "movq $-2, (%3)");
            *(volatile u64 *)p = b; CM("m.sete", 0, "cmpq %0, (%3)\n\tsete 1(%3)");
            *(volatile u64 *)p = b;
            CM("a.addq", M_ALL, "lock addq %0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.subl", M_ALL, "lock subl %k0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.andw", M_ALL, "lock andw %w0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.orb", M_ALL, "lock orb %b0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.xorq", M_ALL, "lock xorq %0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.addimm", M_ALL, "lock addl $-7, (%3)");
            *(volatile u64 *)p = b;
            CM("a.adc", M_ALL, "stc\n\tlock adcq %0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.incq", M_ALL, "lock incq (%3)");
            *(volatile u64 *)p = b;
            CM("a.decb", M_ALL, "stc\n\tlock decb (%3)");
            *(volatile u64 *)p = b;
            CM("a.xaddq", M_ALL, "lock xaddq %0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.xaddw", M_ALL, "lock xaddw %w0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.xchgl", 0, "xchgl %k0, (%3)");
            *(volatile u64 *)p = b;
            CM("a.cx.ok", M_Z, "movq (%3), %%rax\n\tlock cmpxchgq %0, (%3)\n\tmovq %%rax, %%rbx");
            *(volatile u64 *)p = b;
            /* (EAX only: whether a successful 32-bit CMPXCHG clears the upper
             * half of RAX differs between implementations) */
            CM("a.cx.no", M_Z, "movq %0, %%rax\n\tnotq %%rax\n\tlock cmpxchgl %k0, (%3)\n\tmovl %%eax, %k0");
            *(volatile u64 *)p = b;
            CM("a.cx.b", M_Z, "movb (%3), %%al\n\tlock cmpxchgb %b0, (%3)\n\tmovq %%rax, %0");
        }
    }
    const u64 b = a ^ 0x5a5a;
    u64 *p = &cell[0];
    *p = 0;
    CM("f.bsf", M_Z, "bsfq %0, %%rcx");
    CM("f.bsr", M_Z, "bsrl %k0, %%ecx");
    CM("f.rol1", 0x000f, "rolq $1, %0");
    CM("f.ror1", 0x000f, "rorl $1, %k0");
    CM("f.rol9", 0x000c, "rolq $9, %0");
    CM("f.ror5", 0x000c, "rorq $5, %0");
    CM("f.cmprol", 0x003c, "cmpq %1, %0\n\trolq $3, %0");
    CM("f.bt", 0x000c, "btq $5, %0");
    CM("f.btr", 0x000c, "btq %1, %0");
    CM("f.cmpbt", 0x003c, "cmpq %1, %0\n\tbtl $31, %k0");
    CM("f.bts", 0x000c, "btsq $7, %0");
    CM("f.btc", 0x000c, "btcl %k1, %k0");
}

/* (U)COMISS/SD read straight back by SETcc / Jcc / CMOVcc / ADC, including
 * with other SSE and integer instructions in between */
#define FC(tag, code) do { u64 _r = a, b = 0x55; u8 _cc[16]; \
    __asm__ volatile(code "\n\t" SET16 : "+r"(_r) : "r"(b), "r"(_cc), "x"(xa), "x"(xb), "m"(mb) \
                     : "rax", "rbx", "rcx", "xmm7", "memory", "cc"); \
    ps(tag); ph(a); ph(bb); ph(_r); ph(ccmask(_cc) & M_ALL); nl(); } while (0)
#define FJ(tag, code) do { u64 _r = a, b = 0x55; u8 _cc[16]; \
    __asm__ volatile(code "\n\t" J16 : "+r"(_r) : "r"(b), "r"(_cc), "x"(xa), "x"(xb), "m"(mb) \
                     : "rax", "rbx", "rcx", "xmm7", "memory", "cc"); \
    ps(tag); ph(a); ph(bb); ph(_r); ph(ccmask(_cc) & M_ALL); nl(); } while (0)

static void t_fcond(void)
{
    static const u64 cv[] = {
        0, 0x8000000000000000UL, 0x3ff0000000000000UL, 0xbff0000000000000UL, 0x7ff0000000000000UL,
        0xfff0000000000000UL, 0x7ff8000000000000UL, 0x7ff4000000000000UL, 0x0000000000000001UL,
        0x3f8000003f800000UL, 0xbf800000ff800000UL, 0x7fc000007f800000UL, 0x400000003fffffffUL,
    };
    const unsigned NC = sizeof(cv) / sizeof(cv[0]);
    for (unsigned i = 0; i < NC; ++i) for (unsigned j = 0; j < NC; ++j) {
        const u64 a = cv[i], bb = cv[j];
        const v2q xa = { (long long)a, 0 }, xb = { (long long)bb, 0 };
        const v2q mb = xb;
        FC("f.ucomisd", "ucomisd %4, %3");
        FC("f.comisd", "comisd %4, %3");
        FC("f.ucomiss", "ucomiss %4, %3");
        FC("f.comiss", "comiss %4, %3");
        FC("f.ucomisd-m", "ucomisd %5, %3");
        FC("f.comiss-m", "comiss %5, %3");
        FJ("fj.ucomisd", "ucomisd %4, %3");
        FJ("fj.comiss", "comiss %4, %3");
        FJ("fj.ucomiss-m", "ucomiss %5, %3");
        FC("f.ucomisd-mov", "ucomisd %4, %3\n\tmovapd %4, %%xmm7\n\tmovq %%xmm7, %%rbx\n\tmovl $3, %%ecx");
        FC("f.ucomisd-add", "ucomisd %4, %3\n\tmovapd %3, %%xmm7\n\taddsd %4, %%xmm7\n\tmulsd %4, %%xmm7");
        FC("f.ucomiss-addps", "ucomiss %4, %3\n\tmovaps %3, %%xmm7\n\taddps %4, %%xmm7\n\tsqrtps %%xmm7, %%xmm7");
        FC("f.ucomisd-cvt", "ucomisd %4, %3\n\tcvttsd2si %4, %%rbx\n\tcvtsi2sd %%rbx, %%xmm7");
        FC("f.ucomisd-ld", "ucomisd %4, %3\n\tmovsd %5, %%xmm7\n\tmovq %5, %%rbx");
        FC("f.ucomisd-cmov", "ucomisd %4, %3\n\tmovq $7, %%rbx\n\tcmovaq %%rbx, %0\n\tcmovpq %1, %0\n\tcmovbq %%rbx, %0");
        FC("f.ucomisd-adc", "ucomisd %4, %3\n\tadcq %1, %0");
        FC("f.ucomisd-sbb", "ucomisd %4, %3\n\tsbbl %%eax, %%eax\n\tmovl %%eax, %k0");
        FC("f.ucomisd-setcmb", "ucomisd %4, %3\n\tsetnp %%bl\n\tsete %%cl\n\tandb %%bl, %%cl\n\tmovzbl %%cl, %k0\n\tucomisd %4, %3");
        FC("f.ucomisd-lea", "ucomisd %4, %3\n\tleaq 8(%0,%1,2), %0\n\tmovq %1, %%rbx");
        FC("f.ucomisd-pxor", "ucomisd %4, %3\n\tpxor %%xmm7, %%xmm7\n\tcvtsi2sdq %1, %%xmm7\n\tucomisd %%xmm7, %3");
        FC("f.cmp-ucomisd", "cmpq %1, %0\n\tucomisd %4, %3");
        FC("f.ucomisd-x2", "ucomisd %4, %3\n\tucomisd %3, %4");
    }
}

/* A carry read after a producer that leaves it constant: a logic op or
 * TEST clears CF, then ADC/SBB/INC+ADC read it with other instructions in
 * between. STC and a jump first leave a stale CF = 1 in the flags as last
 * written out, so a translation that loses the producer reads that one
 * instead. ("xor %r14,%r14 ... sbb (%rcx,%r14,8),%rax" is the start of the
 * final subtraction in BoringSSL's bn_mul_mont.) */
#define CY(tag, code) do { u64 _r = a; u8 _cc[16]; \
    __asm__ volatile("stc\n\tjmp 3f\n3:\n\t" code "\n\t" SET16 \
                     : "+r"(_r) : "r"(b), "r"(_cc), "r"(p) : "rbx", "rcx", "memory", "cc"); \
    ps(tag); ph(a); ph(b); ph(_r); ph(ccmask(_cc) & M_ALL); nl(); } while (0)

static void t_carry(void)
{
    static const u64 cv[] = { 0, 1, 2, 0x7fffffffffffffffUL, 0x8000000000000000UL, 0xffffffffffffffffUL,
                              0xfffffffffffffffeUL, 0x00000000ffffffffUL, 0x123456789abcdef0UL };
    static u64 cell[2];
    const unsigned NC = sizeof(cv) / sizeof(cv[0]);
    u64 *p = &cell[0];
    for (unsigned i = 0; i < NC; ++i) for (unsigned j = 0; j < NC; ++j) {
        const u64 a = cv[i], b = cv[j];
        cell[0] = b ^ 0x0f0f;
        CY("c.xor-sbb", "xorl %%ecx, %%ecx\n\tmovq %1, %%rbx\n\tsbbq %%rbx, %0");
        CY("c.xor-adc", "xorq %%rcx, %%rcx\n\tadcq %1, %0");
        CY("c.xor-sbb-m", "xorl %%ecx, %%ecx\n\tmovq %1, %%rbx\n\tsbbq (%3,%%rcx,8), %0\n\tmovq %0, 8(%3)");
        CY("c.xor-sbbi", "xorl %%ecx, %%ecx\n\tsbbq $0, %0");
        CY("c.xor-adcl", "xorl %%ecx, %%ecx\n\tadcl %k1, %k0");
        CY("c.and-adc", "movq %1, %%rcx\n\tandq %0, %%rcx\n\tadcq $0, %0");
        CY("c.or-sbb", "movq %1, %%rcx\n\torl $5, %%ecx\n\tsbbq %%rcx, %0");
        CY("c.test-adc", "testq %1, %0\n\tmovq %1, %%rbx\n\tadcq %%rbx, %0");
        CY("c.testi-sbb", "testl $0x80, %k0\n\tsbbq %1, %0");
        CY("c.xor-dec-adc", "movq %1, %%rbx\n\txorl %%ecx, %%ecx\n\tdecq %%rbx\n\tadcq %%rbx, %0");
        CY("c.xor-inc-sbb", "movq %1, %%rbx\n\txorl %%ecx, %%ecx\n\tincl %%ebx\n\tsbbq %%rbx, %0");
        CY("c.xor-loop", "xorq %%rcx, %%rcx\n\tmovl $3, %%ebx\n4:\n\tsbbq %1, %0\n\tleaq 1(%%rcx), %%rcx\n\tdecl %%ebx\n\tjnz 4b");
        CY("c.xor-sbb-sbb", "xorl %%ecx, %%ecx\n\tsbbq %1, %0\n\tsbbq %%rcx, %%rcx\n\taddq %%rcx, %0");
    }
}

/* IRETQ and LRETQ from user mode back to the same code segment (Wine's
 * x86_64 syscall dispatcher returns to Windows code with IRETQ): the flags
 * and RSP come from the frame, execution goes on at its RIP. */
static void t_iret(void)
{
    static const u64 fl[] = { 0x202, 0x203, 0x2c3, 0x8d7, 0x602, 0xa83 };
    for (unsigned k = 0; k < sizeof(fl) / sizeof(fl[0]); ++k) {
        u64 r = fl[k], sp0 = 0, sp1 = 0;
        __asm__ volatile(
            "movq %%rsp, %%rdx\n\t"
            "movq %%rsp, %%rax\n\t"
            "xorl %%ecx, %%ecx\n\t"
            "movw %%ss, %%cx\n\t"
            "pushq %%rcx\n\t"
            "pushq %%rax\n\t"
            "pushq %0\n\t"
            "movw %%cs, %%cx\n\t"
            "pushq %%rcx\n\t"
            "leaq 1f(%%rip), %%rcx\n\t"
            "pushq %%rcx\n\t"
            "iretq\n"
            "1:\n\t"
            "movq %%rsp, %%rsi\n\t"
            "pushfq\n\t"
            "popq %0\n\t"
            "cld"
            : "+r"(r), "=&d"(sp0), "=&S"(sp1)
            :
            : "rax", "rcx", "memory", "cc");
        ps("iretq"); ph(fl[k]); ph(r & 0xcd5); ph(sp0 == sp1); nl();
    }
    {
        u64 r = 0;
        __asm__ volatile(
            "xorl %%ecx, %%ecx\n\t"
            "movw %%cs, %%cx\n\t"
            "pushq %%rcx\n\t"
            "leaq 1f(%%rip), %%rcx\n\t"
            "pushq %%rcx\n\t"
            "lretq\n"
            "1:\n\t"
            "movq $0x1234, %0"
            : "=r"(r) : : "rcx", "memory");
        ps("lretq"); ph(r); nl();
    }
}

static void t_cond(void)
{
    for (unsigned i = 0; i < NV; ++i) {
        for (unsigned j = 0; j < NV; ++j) {
            const u64 a = vals[i], b = vals[j];
            C4("c.add", M_ALL, "add");
            C4("c.sub", M_ALL, "sub");
            C4("c.cmp", M_ALL, "cmp");
            C4("c.and", M_ALL, "and");
            C4("c.or", M_ALL, "or");
            C4("c.xor", M_ALL, "xor");
            C4("c.test", M_ALL, "test");
            CS("c.testself", M_ALL, "testq %0, %0");
            CS("c.adc0", M_ALL, "clc\n\tadcq %1, %0");
            CS("c.adc1", M_ALL, "stc\n\tadcq %1, %0");
            CS("c.sbb0", M_ALL, "clc\n\tsbbl %k1, %k0");
            CS("c.sbb1", M_ALL, "stc\n\tsbbw %w1, %w0");
            CS("c.addadc", M_ALL, "addq %1, %0\n\tadcq %1, %0");
            CS("c.addadcb", M_ALL, "addb %b1, %b0\n\tadcb %b1, %b0");
            CS("c.subsbb", M_ALL, "subq %1, %0\n\tsbbq %1, %0");
            CS("c.cmpincadc", M_ALL, "cmpl %k1, %k0\n\tincl %k0\n\tadcl %k1, %k0");
            CS("c.cmpdecsbb", M_ALL, "cmpq %1, %0\n\tdecq %0\n\tsbbq %1, %0");
            CS("c.stcinc", M_ALL, "stc\n\tincq %0");
            CS("c.negadc", M_ALL, "negl %k0\n\tadcl %k1, %k0");
            CS("c.shladc", M_ALL, "shlq $1, %0\n\tadcq %1, %0");
            CS("c.addimm", M_ALL, "addl $-1, %k0");
            CS("c.cmpimm", M_ALL, "cmpq $0x7f, %0");
            CS("c.cmpimm2", M_ALL, "cmpl $0x12345, %k0");
            CS("c.testimm", M_ALL, "testb $0x81, %b0");
            CS("c.andimm", M_ALL, "andw $0x8001, %w0");
            CS("c.hi8", M_ALL, "movq %0, %%rbx\n\tmovq %1, %%rcx\n\tsubb %%ch, %%bh\n\tmovq %%rbx, %0");
            CS("c.hi8b", M_ALL, "movq %0, %%rbx\n\tmovq %1, %%rcx\n\taddb %%bh, %%cl\n\tmovq %%rcx, %0");
            CS("c.hi8t", M_ALL, "movq %0, %%rcx\n\ttestb %%ch, %%ch");
            CJ("j.add", M_ALL, "addq %1, %0");
            CJ("j.subl", M_ALL, "subl %k1, %k0");
            CJ("j.cmpb", M_ALL, "cmpb %b1, %b0");
            CJ("j.andw", M_ALL, "andw %w1, %w0");
            CJ("j.decl", M_ALL, "decl %k0");
            CJ("j.shl3", M_NOOF, "shlq $3, %0");
            CJ("j.adc", M_ALL, "stc\n\tadcl %k1, %k0");
        }
        const u64 a = vals[i], b = 0;
        t_cond_atomic(a);
        C4U("c.inc", M_ALL, "inc");
        C4U("c.dec", M_ALL, "dec");
        C4U("c.neg", M_ALL, "neg");
        CS("c.shl1", M_ALL, "shlq $1, %0");
        CS("c.shl3", M_NOOF, "shlq $3, %0");
        CS("c.shl1l", M_ALL, "shll $1, %k0");
        CS("c.shl5b", M_NOOF, "shlb $5, %b0");
        CS("c.shl8b", M_NOOF, "shlb $8, %b0");
        CS("c.shl9b", M_NOOF, "shlb $9, %b0");
        CS("c.shr1", M_ALL, "shrq $1, %0");
        CS("c.shr7w", M_NOOF, "shrw $7, %w0");
        CS("c.shr16w", M_NOOF, "shrw $16, %w0");
        CS("c.shr31", M_NOOF, "shrl $31, %k0");
        CS("c.sar1", M_ALL, "sarq $1, %0");
        CS("c.sar9b", M_NOOF, "sarb $9, %b0");
        CS("c.sar40", M_NOOF, "sarq $40, %0");
        CS("c.imulq", M_CO, "imulq %0, %0");
        CS("c.imull", M_CO, "imull $0x10001, %k0, %k0");
        CS("c.imulw", M_CO, "imulw %w0, %w0");
        CS("c.mulq", M_CO, "movq %0, %%rax\n\tmulq %%rax\n\tmovq %%rax, %0");
    }
}

int main(void)
{
    t_int();
    t_sse();
    t_x87();
    t_sse2();
    t_fcond();
    t_iret();
    t_carry();
    t_cond();
    ps("done"); nl();
    return 0;
}

#ifndef __APPLE__
__attribute__((noreturn)) void _start(void);
__asm__(".globl _start\n_start:\n\tandq $-16, %rsp\n\tcall main\n\tmovl %eax, %edi\n\tmovl $231, %eax\n\tsyscall\n\thlt\n");
#endif
