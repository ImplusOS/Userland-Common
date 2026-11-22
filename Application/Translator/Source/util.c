/*
 * Freestanding helpers: string functions, a small printf, logging, page
 * allocation and a spinlock. The translator links no C library.
 */

#include "xl.h"
#include "host.h"

int g_xl_debug;
int g_xl_strace;

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n >= 8u) {
        uint64_t v;
        __builtin_memcpy(&v, s, 8);
        __builtin_memcpy(d, &v, 8);
        d += 8; s += 8; n -= 8u;
    }
    while (n-- != 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    while (n-- != 0u) {
        d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint64_t v = (uint8_t)c;
    v |= v << 8; v |= v << 16; v |= v << 32;
    while (n >= 8u) {
        __builtin_memcpy(d, &v, 8);
        d += 8; n -= 8u;
    }
    while (n-- != 0u) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a; ++b;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        }
    }
    return 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; ++s) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            return (char *)last;
        }
    }
}

/* ---- formatting ---------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} fmt_out_t;

static void out_ch(fmt_out_t *o, char c)
{
    if (o->len + 1u < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_str(fmt_out_t *o, const char *s, int width, bool left)
{
    int n = (int)strlen(s);
    if (!left) {
        for (int i = n; i < width; ++i) out_ch(o, ' ');
    }
    while (*s != '\0') out_ch(o, *s++);
    if (left) {
        for (int i = n; i < width; ++i) out_ch(o, ' ');
    }
}

static void out_num(fmt_out_t *o, uint64_t v, unsigned base, bool neg,
                    int width, char pad, bool upper)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0u);
    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0') out_ch(o, '-');
    for (int i = total; i < width; ++i) out_ch(o, pad);
    if (neg && pad != '0') out_ch(o, '-');
    while (n > 0) out_ch(o, tmp[--n]);
}

int xl_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    fmt_out_t o = { buf, size, 0 };
    for (; *fmt != '\0'; ++fmt) {
        if (*fmt != '%') {
            out_ch(&o, *fmt);
            continue;
        }
        ++fmt;
        bool left = false;
        char pad = ' ';
        int width = 0;
        int lng = 0;
        if (*fmt == '-') { left = true; ++fmt; }
        if (*fmt == '0') { pad = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') { ++lng; ++fmt; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            out_num(&o, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10,
                    v < 0, width, pad, false);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            out_num(&o, v, 10, false, width, pad, false);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            out_num(&o, v, 16, false, width, pad, *fmt == 'X');
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            out_ch(&o, '0'); out_ch(&o, 'x');
            out_num(&o, v, 16, false, width, pad, false);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            out_str(&o, s != NULL ? s : "(null)", width, left);
            break;
        }
        case 'c':
            out_ch(&o, (char)va_arg(ap, int));
            break;
        case '%':
            out_ch(&o, '%');
            break;
        default:
            out_ch(&o, '%');
            out_ch(&o, *fmt);
            break;
        }
    }
    if (size != 0u) {
        buf[o.len < size ? o.len : size - 1u] = '\0';
    }
    return (int)o.len;
}

int xl_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = xl_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static void xl_vlog(const char *fmt, va_list ap)
{
    char line[512];
    size_t pre = (size_t)xl_snprintf(line, sizeof(line), "translator[%d]: ",
                                     (int)host_gettid());
    if (pre >= sizeof(line)) {
        pre = sizeof(line) - 1u;
    }
    int n = xl_vsnprintf(line + pre, sizeof(line) - pre - 1u, fmt, ap);
    size_t len = pre + (size_t)n;
    if (len > sizeof(line) - 2u) {
        len = sizeof(line) - 2u;
    }
    line[len++] = '\n';
    (void)host_write(2, line, len);
}

void xl_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xl_vlog(fmt, ap);
    va_end(ap);
}

void xl_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xl_vlog(fmt, ap);
    va_end(ap);
    host_exit_group(127);
}

/* ---- memory ---------------------------------------------------------------- */

void *xl_map(size_t size)
{
    long p = host_mmap(0, XL_PAGE_UP(size), HOST_PROT_READ | HOST_PROT_WRITE,
                       HOST_MAP_PRIVATE | HOST_MAP_ANONYMOUS, -1, 0);
    if (p < 0 && p > -4096) {
        return NULL;
    }
    return (void *)(uintptr_t)p;
}

void xl_unmap(void *p, size_t size)
{
    if (p != NULL) {
        (void)host_munmap((uint64_t)(uintptr_t)p, XL_PAGE_UP(size));
    }
}

void xl_lock(xl_lock_t *l)
{
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&l->v, __ATOMIC_RELAXED) != 0u) {
            (void)host_sched_yield();
        }
    }
}

void xl_unlock(xl_lock_t *l)
{
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
}
