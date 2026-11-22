/* x86 memory-model stress: message passing without fences (TSO orders the
 * stores, and the loads), LOCKed counters, a CMPXCHG spinlock. */
typedef unsigned long size_t;
int printf(const char *, ...);
int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
int pthread_join(unsigned long, void **);

#define N 4
#define ROUNDS 200000
static volatile long data[64], flag;
static volatile long ack;
static long bad;
static void *producer(void *a)
{
    (void)a;
    for (long i = 1; i <= ROUNDS; ++i) {
        data[i & 63] = i * 3;
        data[(i + 1) & 63] = i * 5;
        flag = i;                       /* plain store after the data */
        while (ack != i) { }            /* wait for the consumer */
    }
    return 0;
}
static void *consumer(void *a)
{
    (void)a;
    for (long i = 1; i <= ROUNDS; ++i) {
        while (flag != i) { }
        if (data[i & 63] != i * 3 || data[(i + 1) & 63] != i * 5) bad++;
        ack = i;
    }
    return 0;
}
static long counter, plain, lockword;
static void *adder(void *a)
{
    (void)a;
    for (int i = 0; i < 100000; ++i) {
        __asm__ volatile("lock addq $1, %0" : "+m"(counter) :: "cc");
        long v = 1;
        __asm__ volatile("lock xaddq %0, %1" : "+r"(v), "+m"(counter) :: "cc");
        /* spinlock: cmpxchg 0 -> 1 */
        for (;;) {
            long expect = 0, one = 1;
            unsigned char ok;
            __asm__ volatile("lock cmpxchgq %3, %1\n\tsete %0" : "=q"(ok), "+m"(lockword), "+a"(expect) : "r"(one) : "cc");
            if (ok) break;
            __asm__ volatile("pause");
        }
        plain++;
        __asm__ volatile("" ::: "memory");
        lockword = 0;                   /* release: a plain store on x86 */
    }
    return 0;
}
int main(void)
{
    unsigned long t[N];
    pthread_create(&t[0], 0, producer, 0);
    pthread_create(&t[1], 0, consumer, 0);
    pthread_join(t[0], 0); pthread_join(t[1], 0);
    printf("mp: rounds=%d bad=%ld\n", ROUNDS, bad);
    for (int i = 0; i < N; ++i) pthread_create(&t[i], 0, adder, 0);
    for (int i = 0; i < N; ++i) pthread_join(t[i], 0);
    printf("locked: counter=%ld plain=%ld (want %d %d)\n", counter, plain, N * 200000, N * 100000);
    return 0;
}

#ifndef __APPLE__
/* glibc entry (no crt1.o in a freestanding build) */
__asm__(".globl _start\n_start:\n\txorl %ebp, %ebp\n\tmovq %rdx, %r9\n\tpopq %rsi\n\tmovq %rsp, %rdx\n\tandq $-16, %rsp\n\tpushq %rax\n\tpushq %rsp\n\txorl %r8d, %r8d\n\txorl %ecx, %ecx\n\tleaq main(%rip), %rdi\n\tcall *__libc_start_main@GOTPCREL(%rip)\n\thlt\n");
#endif
