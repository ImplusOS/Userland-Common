/* JIT-specific checks: self-modifying code, code remapped at the same
 * address, async signals in a tight loop, register/flag state seen by a
 * SIGSEGV handler and restart after it fixes the address. */
typedef unsigned long size_t;
typedef unsigned long u64;
int printf(const char *, ...);
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int sigaction(int, const void *, void *);
int setitimer(int, const void *, void *);
void *memcpy(void *, const void *, size_t);
struct ksa { void (*h)(int, void *, void *); unsigned long mask[16]; int flags; void (*restorer)(void); };

/* mov eax, imm32 ; ret */
static void put_fn(unsigned char *p, unsigned v)
{
    p[0] = 0xB8; memcpy(p + 1, &v, 4); p[5] = 0xC3;
}

static volatile int alarmed;
static void on_alarm(int s, void *i, void *u) { (void)s; (void)i; (void)u; alarmed = 1; }

static u64 seen_rax, seen_rip, seen_fl, seen_rbx;
static u64 good_cell;
static void on_segv(int s, void *info, void *uc)
{
    (void)s; (void)info;
    u64 *gregs = (u64 *)((char *)uc + 40);      /* r8..r15 rdi rsi rbp rbx rdx rax rcx rsp rip eflags */
    seen_rax = gregs[13]; seen_rbx = gregs[11]; seen_rip = gregs[16]; seen_fl = gregs[17];
    gregs[14] = (u64)&good_cell;                /* rcx: a valid address; the store is retried */
}

int main(void)
{
    /* 1. self-modifying code in an RWX page */
    unsigned char *code = mmap(0, 4096, 7, 0x22, -1, 0);
    int (*fn)(void) = (int (*)(void))code;
    long sum = 0;
    for (int i = 0; i < 1000; ++i) {
        put_fn(code, (unsigned)i);
        sum += fn();
        sum += fn();
    }
    printf("smc rwx: %ld (want %ld)\n", sum, 999L * 1000L);

    /* 2. RW -> RX with mprotect, rewrite, again */
    unsigned char *c2 = mmap(0, 4096, 3, 0x22, -1, 0);
    int (*fn2)(void) = (int (*)(void))c2;
    sum = 0;
    for (int i = 0; i < 200; ++i) {
        mprotect(c2, 4096, 3);
        put_fn(c2, (unsigned)(i * 3));
        mprotect(c2, 4096, 5);
        sum += fn2() + fn2();
    }
    printf("smc mprotect: %ld (want %ld)\n", sum, 2L * 3L * 199L * 200L / 2L);

    /* 3. unmap and map new code at the same address */
    unsigned char *c3 = mmap(0, 4096, 3, 0x22, -1, 0);
    put_fn(c3, 11); mprotect(c3, 4096, 5);
    int a = ((int (*)(void))c3)();
    munmap(c3, 4096);
    unsigned char *c4 = mmap(c3, 4096, 3, 0x32 /* FIXED */, -1, 0);
    put_fn(c4, 22); mprotect(c4, 4096, 5);
    int b = ((int (*)(void))c4)();
    printf("remap: %d %d same=%d\n", a, b, c3 == c4);

    /* 4. SIGALRM ends a loop that never leaves its block */
    struct ksa sa = { 0 };
    sa.h = on_alarm; sa.flags = 4;
    sigaction(14, &sa, 0);
    long it[4] = { 0, 0, 0, 20000 };            /* it_interval 0, it_value 20 ms */
    setitimer(0, it, 0);
    u64 spins = 0;
    while (!alarmed) spins++;
    printf("alarm: stopped=%d spun=%s\n", alarmed, spins > 1000 ? "yes" : "no");

    /* 5. a fault: what the handler sees, and the restart */
    struct ksa sb = { 0 };
    sb.h = on_segv; sb.flags = 4;
    sigaction(11, &sb, 0);
    u64 rax_out, fl_out; unsigned char lt, z;
    __asm__ volatile(
        "movq $8, %%rcx\n\t"            /* bad address */
        "movq $40, %%rax\n\t"
        "movq $3, %%rbx\n\t"
        "addq %%rbx, %%rax\n\t"         /* 43 */
        "cmpq $50, %%rax\n\t"           /* flags: 43 - 50 */
        "movq %%rax, (%%rcx)\n\t"       /* faults; the handler fixes rcx */
        "setl %2\n\t"
        "setz %3\n\t"
        "pushfq\n\tpopq %1\n\t"
        : "=a"(rax_out), "=r"(fl_out), "=r"(lt), "=r"(z) : : "rbx", "rcx", "memory", "cc");
    printf("fault: handler rax=%lu rbx=%lu flags=%03lx rip_ok=%d | after: cell=%lu lt=%d z=%d flags=%03lx\n",
           seen_rax, seen_rbx, seen_fl & 0x8d5, seen_rip != 0, good_cell, lt, z, fl_out & 0x8d5);
    return 0;
}

#ifndef __APPLE__
/* glibc entry (no crt1.o in a freestanding build) */
__asm__(".globl _start\n_start:\n\txorl %ebp, %ebp\n\tmovq %rdx, %r9\n\tpopq %rsi\n\tmovq %rsp, %rdx\n\tandq $-16, %rsp\n\tpushq %rax\n\tpushq %rsp\n\txorl %r8d, %r8d\n\txorl %ecx, %ecx\n\tleaq main(%rip), %rdi\n\tcall *__libc_start_main@GOTPCREL(%rip)\n\thlt\n");
#endif
