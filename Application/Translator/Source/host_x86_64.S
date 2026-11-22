/*
 * Host stubs for the x86_64 translator (Linux x86_64 system call ABI:
 * syscall, number in rax, arguments rdi, rsi, rdx, r10, r8, r9). See host.h.
 */

    .text

    .global _start
    .type _start, @function
_start:
    xor     %ebp, %ebp
    mov     %rsp, %rdi
    and     $-16, %rsp
    call    xl_main
    ud2

/* long host_syscall6(nr, a0, a1, a2, a3, a4, a5) */
    .global host_syscall6
    .type host_syscall6, @function
host_syscall6:
    mov     %rdi, %rax
    mov     %rsi, %rdi
    mov     %rdx, %rsi
    mov     %rcx, %rdx
    mov     %r8, %r10
    mov     %r9, %r8
    mov     8(%rsp), %r9
    syscall
    ret

/* long host_safe_syscall(t, nr, a0, a1, a2, a3, a4, a5) -- see host_arm64.S */
    .global host_safe_syscall
    .global host_safe_syscall_start
    .global host_safe_syscall_end
    .global host_safe_syscall_eintr
    .type host_safe_syscall, @function
host_safe_syscall:
    mov     %rdi, %r11
    mov     %rsi, %rax
    mov     %rdx, %rdi
    mov     %rcx, %rsi
    mov     %r8, %rdx
    mov     %r9, %r10
    mov     8(%rsp), %r8
    mov     16(%rsp), %r9
host_safe_syscall_start:
    cmpl    $0, 8(%r11)             /* gthread_t.sig_pending_flag */
    jne     host_safe_syscall_eintr
    syscall
host_safe_syscall_end:
    ret
host_safe_syscall_eintr:
    mov     $-4, %rax               /* -EINTR */
    ret

/* long host_clone(flags, stack, ptid, ctid, tls, fn, arg) */
    .global host_clone
    .type host_clone, @function
host_clone:
    and     $-16, %rsi
    mov     8(%rsp), %rax
    sub     $16, %rsi
    mov     %r9, 0(%rsi)
    mov     %rax, 8(%rsi)
    mov     %rcx, %r10
    mov     $56, %eax
    syscall
    test    %rax, %rax
    jz      1f
    ret
1:
    xor     %ebp, %ebp
    pop     %rax
    pop     %rdi
    call    *%rax
    xor     %edi, %edi
    mov     $60, %eax               /* exit (this thread) */
    syscall
    ud2

    .global host_setjmp
    .type host_setjmp, @function
host_setjmp:
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)
    lea     8(%rsp), %rdx
    mov     %rdx, 48(%rdi)
    mov     (%rsp), %rdx
    mov     %rdx, 56(%rdi)
    xor     %eax, %eax
    ret

    .global host_longjmp
    .type host_longjmp, @function
host_longjmp:
    mov     %esi, %eax
    test    %eax, %eax
    jnz     1f
    inc     %eax
1:
    mov     0(%rdi), %rbx
    mov     8(%rdi), %rbp
    mov     16(%rdi), %r12
    mov     24(%rdi), %r13
    mov     32(%rdi), %r14
    mov     40(%rdi), %r15
    mov     48(%rdi), %rsp
    jmp     *56(%rdi)

    .global host_sigreturn
    .type host_sigreturn, @function
host_sigreturn:
    mov     $15, %eax
    syscall
    ud2

    .section .note.GNU-stack,"",@progbits
