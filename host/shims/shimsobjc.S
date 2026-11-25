#ifdef _WIN32
#define SYM(x) _##x
#define ALIGN_CALL(n)
#define POP_ARGS(n) addl $n, %esp
#else
#define SYM(x) x
#define ALIGN_CALL(n) subl $(12 - (n)) & 15, %esp
#define POP_ARGS(n) addl $(n) + ((12 - (n)) & 15), %esp
#endif
    .text
    .globl SYM(gmv_objc_msgSend)
SYM(gmv_objc_msgSend):
    movl    4(%esp), %eax
    movl    8(%esp), %edx
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_lookup)     /* IMP %eax */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      1f
    jmp     *%eax
1:
    xorl    %eax, %eax
    ret

    .globl SYM(gmv_objc_msgSend_fpret)
SYM(gmv_objc_msgSend_fpret):
    movl    4(%esp), %eax
    movl    8(%esp), %edx
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_lookup)     /* IMP %eax */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      3f
    jmp     *%eax
3:
    fldz
    ret

    .globl SYM(gmv_objc_msgSendSuper)
SYM(gmv_objc_msgSendSuper):
    movl    4(%esp), %eax
    movl    8(%esp), %edx
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_super_lookup)   /* IMP %eax */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      2f
    movl    4(%esp), %ecx
    movl    (%ecx), %ecx
    movl    %ecx, 4(%esp)
    jmp     *%eax
2:
    xorl    %eax, %eax
    ret

    .globl SYM(gmv_objc_msgSend_stret)
SYM(gmv_objc_msgSend_stret):
    movl    8(%esp), %eax
    movl    12(%esp), %edx
    ALIGN_CALL(8)
    pushl   %edx
    pushl   %eax
    call    SYM(gmv_objc_stret_lookup)   /* IMP %eax */
    POP_ARGS(8)
    testl   %eax, %eax
    jz      4f
    jmp     *%eax
4:
    ret

/* gmv_gl_forward __cdecl __stdcall %eax [esp] [esp+4..] */
#ifdef _WIN32
_gmv_gl_forward:
    pushl   %ebp
    movl    %esp, %ebp
    pushl   %esi
    pushl   %edi
    pushl   %ebx
    movl    %eax, %ebx           /* ebx GL */
    subl    $64, %esp
    leal    8(%ebp), %esi
    movl    %esp, %edi
    movl    $16, %ecx
    rep     movsl
    call    *%ebx                /* __stdcall GL */
    leal    -12(%ebp), %esp
    popl    %ebx
    popl    %edi
    popl    %esi
    popl    %ebp
    ret
#endif

/* gmv_gl_lazy %eax gmv_gl_forward */
    .globl SYM(gmv_gl_lazy)
SYM(gmv_gl_lazy):
    movl    (%eax), %edx
    testl   %edx, %edx
    jnz     5f
    ALIGN_CALL(4)
    pushl   %eax
    call    SYM(gmv_gl_lookup)   /* %eax */
#ifdef _WIN32
    popl    %ecx
#else
    POP_ARGS(4)
#endif
    movl    %eax, %edx
5:
    testl   %edx, %edx
    jz      6f
#ifdef _WIN32
    movl    %edx, %eax
    jmp     _gmv_gl_forward
#else
    jmp     *%edx                /* GL __cdecl */
#endif
6:
    xorl    %eax, %eax
    ret

    .globl SYM(gmv_oss_str)
SYM(gmv_oss_str):
    movl    8(%esp), %eax
    ALIGN_CALL(4)
    pushl   %eax
    call    SYM(oss_str_impl)    /* _M_p %eax */
    POP_ARGS(4)
    movl    4(%esp), %edx
    movl    %eax, (%edx)
    movl    %edx, %eax
    ret     $4

    .globl SYM(gmv_setjmp)
SYM(gmv_setjmp):
    movl    4(%esp), %eax
    movl    %ebx, 0(%eax)
    movl    %esi, 4(%eax)
    movl    %edi, 8(%eax)
    movl    %ebp, 12(%eax)
    leal    4(%esp), %ecx        /* esp */
    movl    %ecx, 16(%eax)
    movl    (%esp), %ecx
    movl    %ecx, 20(%eax)
    xorl    %eax, %eax
    ret

    .globl SYM(gmv_longjmp)
SYM(gmv_longjmp):
    movl    4(%esp), %edx
    movl    8(%esp), %eax
    testl   %eax, %eax
    jnz     7f
    incl    %eax
7:
    movl    0(%edx), %ebx
    movl    4(%edx), %esi
    movl    8(%edx), %edi
    movl    12(%edx), %ebp
    movl    16(%edx), %esp
    movl    20(%edx), %ecx
    jmp     *%ecx                /* eax */
#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
