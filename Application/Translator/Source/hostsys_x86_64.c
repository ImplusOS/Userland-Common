/*
 * Canonical system calls on an x86_64 host: the canonical ABI is the
 * host's own, so a call goes through unchanged.
 */

#include "xl.h"
#include "host.h"

#if defined(__x86_64__)

long xl_host_syscall(gthread_t *t, long nr, uint64_t a[6])
{
    return host_safe_syscall(t, nr, (long)a[0], (long)a[1], (long)a[2],
                             (long)a[3], (long)a[4], (long)a[5]);
}

long xl_host_syscall_plain(long nr, uint64_t a[6])
{
    return host_syscall6(nr, (long)a[0], (long)a[1], (long)a[2], (long)a[3],
                         (long)a[4], (long)a[5]);
}

#endif /* __x86_64__ */
