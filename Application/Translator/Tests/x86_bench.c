typedef unsigned long u64;
static long sys3(long n, long a, long b, long c)
{ long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static void ph(const char *s, u64 v) { char b[64]; int n = 0; while (*s) b[n++] = *s++; for (int i = 60; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(v >> i) & 15]; b[n++] = '\n'; sys3(1, 1, (long)b, n); }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static unsigned char buf1[65536], buf2[65536];
static __attribute__((noinline)) u64 my_strlen(const char *s) { const char *p = s; while (*p) ++p; return (u64)(p - s); }
int main(void)
{
    u64 acc = (u64)fib(27);
    ph("fib ", acc);
    for (int r = 0; r < 300; ++r) {
        for (int i = 0; i < 65536; ++i) buf1[i] = (unsigned char)(i * 7 + r);
        for (int i = 0; i < 65535; ++i) buf2[i] = buf1[i] ^ buf1[i + 1];
        buf2[65535] = 0;
        for (int i = 0; i < 65536; ++i) if (buf2[i] == 0) buf2[i] = 1;
        buf2[65535] = 0;
        acc += my_strlen((const char *)buf2);
    }
    ph("bytes ", acc);
    double x = 0;
    for (int i = 1; i < 3000000; ++i) x += 1.0 / ((double)i * i);
    u64 bits; __builtin_memcpy(&bits, &x, 8);
    ph("fp ", bits);
    u64 h = 1469598103934665603UL;
    for (int i = 0; i < 4000000; ++i) { h ^= (u64)i; h *= 1099511628211UL; h = (h << 13) | (h >> 51); }
    ph("hash ", h);
    return 0;
}
__attribute__((noreturn)) void _start(void);
__asm__(".globl _start\n_start:\n\tandq $-16, %rsp\n\tcall main\n\tmovl %eax, %edi\n\tmovl $231, %eax\n\tsyscall\n\thlt\n");
