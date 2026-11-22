/*
 * Host stubs for the arm64 translator (Linux AArch64 system call ABI:
 * svc #0, number in x8, arguments x0-x5, result x0). See host.h.
 */

    .text

    .global _start
    .type _start, %function
_start:
    mov     x29, #0
    mov     x30, #0
    mov     x0, sp
    bl      xl_main
    brk     #0

/* long host_syscall6(nr, a0, a1, a2, a3, a4, a5) */
    .global host_syscall6
    .type host_syscall6, %function
host_syscall6:
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    svc     #0
    ret

/* long host_safe_syscall(t, nr, a0, a1, a2, a3, a4, a5)
 * Between host_safe_syscall_start and host_safe_syscall_end (the svc
 * itself included) the call has not happened yet, or the kernel stepped
 * back over the svc to re-run it: a host signal handler that finds the PC
 * there moves it to host_safe_syscall_eintr. */
    .global host_safe_syscall
    .global host_safe_syscall_start
    .global host_safe_syscall_end
    .global host_safe_syscall_eintr
    .type host_safe_syscall, %function
host_safe_syscall:
    mov     x9, x0
    mov     x8, x1
    mov     x0, x2
    mov     x1, x3
    mov     x2, x4
    mov     x3, x5
    mov     x4, x6
    mov     x5, x7
host_safe_syscall_start:
    ldr     w10, [x9, #8]           /* gthread_t.sig_pending_flag */
    cbnz    w10, host_safe_syscall_eintr
    svc     #0
host_safe_syscall_end:
    ret
host_safe_syscall_eintr:
    mov     x0, #-4                 /* -EINTR */
    ret

/* long host_clone(flags, stack, ptid, ctid, tls, fn, arg) -- x86_64
 * argument order in; AArch64's clone is (flags, stack, ptid, tls, ctid). */
    .global host_clone
    .type host_clone, %function
host_clone:
    and     x1, x1, #~15
    stp     x5, x6, [x1, #-16]!
    mov     x9, x3
    mov     x3, x4
    mov     x4, x9
    mov     x8, #220
    svc     #0
    cbz     x0, 1f
    ret
1:
    ldp     x1, x0, [sp], #16
    mov     x29, #0
    mov     x30, #0
    blr     x1
    mov     x0, #0
    mov     x8, #93                 /* exit (this thread) */
    svc     #0
    brk     #0

/* int host_setjmp(buf) / void host_longjmp(buf, val) */
    .global host_setjmp
    .type host_setjmp, %function
host_setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     x0, #0
    ret

    .global host_longjmp
    .type host_longjmp, %function
host_longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    cmp     x1, #0
    csinc   x0, x1, xzr, ne
    ret

    .global host_sigreturn
    .type host_sigreturn, %function
host_sigreturn:
    mov     x8, #139
    svc     #0
    brk     #0

    .section .note.GNU-stack,"",%progbits
