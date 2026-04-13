

#include <linux/linkage.h>
#include <asm/nospec-branch.h>
#include <asm/frame.h>


#define HSL_SIZE            0x90  //MUST BE 16 BYTE ALIGNED

#define HSL_GVMCB_PA  0x00
#define HSL_HVMCB_PA  0x08
#define HSL_SELF      0x10
#define HSL_SHAREDATA 0x18
#define HSL_PADDING   0x20
#define HSL_RESERVED  0x28
#define HSL_HRSP      0x30
#define HSL_PAD       0x38
#define HSL_GXCR0     0x40
#define HSL_HXCR0     0x48
#define HSL_GXSS      0x50
#define HSL_HXSS      0x58
#define HSL_GPKRU     0x60
#define HSL_HPKRU     0x68
#define HSL_FFLGS     0x70
#define HSL_PAD2      0x78
#define EXTRA1        0x80
#define EXTRA2        0x88


#define HSL_FEAT_XSAVE      0x1
#define HSL_FEAT_XSAVES     0x2
#define HSL_FEAT_PKU        0x4

#define GUEST_REGS_SIZE     0x80 //sizeof(struct guest_regs)

#define XMM_AREA_SIZE       0x80 // 0x20 alignment pad + 6 × 16 bytes = 0x80 

#define SNAP_HOST_VMCB_VA 0x18
#define SNAP_HOST_VMCB      0x20
#define SNAP_GUEST_VMCB     0x08
#define HOST_STACK_OFFSET   0x00    //offsetof(struct snap, host_stack) 
#define PAGE_SIZE_VAL       0x4000

#define VMCB_SAVE_RSP       0x5d8
#define VMCB_SAVE_RAX       0x5f8


.macro PUSHALL    //pushes all 16 registers, real rsp lives in hsl
    push    %rax
    push    %rcx
    push    %rdx
    push    %rbx
    push    $-1
    push    %rbp
    push    %rsi
    push    %rdi
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15
.endm


.macro POPALL
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx    //double pop to not restore -1 but the actual value
    pop     %rbx
    pop     %rdx
    pop     %rcx
    pop     %rax
.endm


    .text
    .globl  virt
    .type   virt, @function

virt:

    mov     %rsp, %r8
    mov     HOST_STACK_OFFSET(%rdi), %rsp       //rsp = snap->host_stack       
    add     $(PAGE_SIZE_VAL - HSL_SIZE), %rsp   //rsp = top of page - hsl 
    mov     %r8, HSL_HRSP(%rsp)


.Lrun:

    //handles pkru and xsave 

    mov     %rcx, HSL_PADDING(%rsp)  //this and the next one is just u64 padding so i'm using that rather than adding more shit and fucking up the offsets
    mov     %rdx, HSL_RESERVED(%rsp)

    testq   $HSL_FEAT_XSAVE, HSL_FFLGS(%rsp)
    jz      78f

    xor     %ecx, %ecx
    xgetbv
    shl     $32, %rdx
    or      %rdx, %rax
    mov     %rax, HSL_HXCR0(%rsp)

    testq   $HSL_FEAT_XSAVES, HSL_FFLGS(%rsp)
    jz      77f
    mov     $MSR_IA32_XSS, %ecx
    rdmsr
    shl     $32, %rdx
    or      %rdx, %rax
    mov     %rax, HSL_HXSS(%rsp)
    testq   $HSL_FEAT_PKU, HSL_FFLGS(%rsp)
    jz      78f
    xor     %ecx, %ecx
    .byte   0x0f, 0x01, 0xee
    mov     %eax, HSL_HPKRU(%rsp)

    testq   $HSL_FEAT_XSAVE, HSL_FFLGS(%rsp)
    jz      80f

    mov     HSL_GXCR0(%rsp), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    xor     %ecx, %ecx
    xsetbv

    testq   $HSL_FEAT_XSAVES, HSL_FFLGS(%rsp)
    jz      79f
    mov     HSL_GXSS(%rsp), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    mov     $MSR_IA32_XSS, %ecx
    wrmsr
    testq   $HSL_FEAT_PKU, HSL_FFLGS(%rsp)
    jz      80f
    mov     HSL_GPKRU(%rsp), %eax
    xor     %ecx, %ecx
    xor     %edx, %edx
    .byte   0x0f, 0x01, 0xef
    mov     HSL_RESERVED(%rsp), %rdx
    mov     HSL_PADDING(%rsp), %rcx

.Srun:
    mov     HSL_GVMCB_PA(%rsp), %rax
    mov     %r10, HSL_PAD(%rsp)
    mov     %r11, HSL_PAD2(%rsp)
    mov     HSL_SELF(%rsp), %r11
    mov     SNAP_HOST_VMCB_VA(%r11), %r10
    mov     %rax, VMCB_SAVE_RAX(%r10)
    mov     %rsp, VMCB_SAVE_RSP(%r10)
    mov     HSL_GVMCB_PA(%rsp), %rax  //pretty sure this is redundant but whatever
    mov     HSL_PAD(%rsp), %r10
    mov     HSL_PAD2(%rsp), %r11
    vmload  %rax
    vmrun   %rax

.Prun:
    mov     HSL_GVMCB_PA(%rsp), %rax
    vmsave  %rax

    mov     %r11, EXTRA1(%rsp)            // saves guests r11 before being clobbered
    mov     HSL_SELF(%rsp), %r11
    mov     SNAP_GUEST_VMCB(%r11), %r11
    mov     VMCB_SAVE_RAX(%r11), %rax
    mov     EXTRA1(%rsp), %r11            // and restores it for PUSHALL
    PUSHALL                              //rsp -= 0x80 

    mov     HSL_SELF + GUEST_REGS_SIZE(%rsp), %r11

    mov     SNAP_HOST_VMCB(%r11), %rax
    vmload %rax                         //loads host and hidden state, has to be quick.

    sub     $XMM_AREA_SIZE, %rsp    //vmload didn't overwrite those so they should still be guest's regs, gotta save and restore them prior to next entry. also rsp -= 0x80 
    movaps  %xmm0, 0x20(%rsp)
    movaps  %xmm1, 0x30(%rsp)
    movaps  %xmm2, 0x40(%rsp)
    movaps  %xmm3, 0x50(%rsp)
    movaps  %xmm4, 0x60(%rsp)
    movaps  %xmm5, 0x70(%rsp)

    lea     XMM_AREA_SIZE + GUEST_REGS_SIZE(%rsp), %r11
    testq   $HSL_FEAT_XSAVE, HSL_FFLGS(%r11)
    jz      90f

    push    %rax
    push    %rcx
    push    %rdx

    xor     %ecx, %ecx
    xgetbv
    shl     $32, %rdx
    or      %rdx, %rax
    mov     %rax, HSL_GXCR0(%r11)

    mov     HSL_HXCR0(%r11), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    xor     %ecx, %ecx
    xsetbv

    testq   $HSL_FEAT_XSAVES, HSL_FFLGS(%r11)
    jz      89f
    mov     $MSR_IA32_XSS, %ecx
    rdmsr
    shl     $32, %rdx
    or      %rdx, %rax
    mov     %rax, HSL_GXSS(%r11)

    mov     HSL_HXSS(%r11), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    mov     $MSR_IA32_XSS, %ecx
    wrmsr
    testq   $HSL_FEAT_PKU, HSL_FFLGS(%r11)
    jz      88f
    xor     %ecx, %ecx
    .byte   0x0f, 0x01, 0xee
    mov     %eax, HSL_GPKRU(%r11)

    mov     HSL_HPKRU(%r11), %eax
    xor     %ecx, %ecx
    xor     %edx, %edx
    .byte   0x0f, 0x01, 0xef

    pop     %rdx
    pop     %rcx
    pop     %rax

    lea     XMM_AREA_SIZE(%rsp), %rdi   //rdi = guest_regs * 
    mov     XMM_AREA_SIZE + GUEST_REGS_SIZE + HSL_SELF(%rsp), %rsi     //rsi = snap/self * 
    call    exithandler

    lea     XMM_AREA_SIZE + GUEST_REGS_SIZE(%rsp), %r11
    testq   $HSL_FEAT_XSAVE, HSL_FFLGS(%r11)
    jz      100f

    push    %rax
    push    %rcx
    push    %rdx

    mov     HSL_GXCR0(%r11), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    xor     %ecx, %ecx
    xsetbv

    testq   $HSL_FEAT_XSAVES, HSL_FFLGS(%r11)
    jz      99f
    mov     HSL_GXSS(%r11), %rax
    mov     %rax, %rdx
    shr     $32, %rdx
    mov     $MSR_IA32_XSS, %ecx
    wrmsr

    testq   $HSL_FEAT_PKU, HSL_FFLGS(%r11)
    jz      98f
    mov     HSL_GPKRU(%r11), %eax
    xor     %ecx, %ecx
    xor     %edx, %edx
    .byte   0x0f, 0x01, 0xef

    pop     %rdx
    pop     %rcx
    pop     %rax

    //as mentioned gotta restore them prior to the next entry 
    movaps  0x70(%rsp), %xmm5
    movaps  0x60(%rsp), %xmm4
    movaps  0x50(%rsp), %xmm3
    movaps  0x40(%rsp), %xmm2
    movaps  0x30(%rsp), %xmm1
    movaps  0x20(%rsp), %xmm0
    add     $XMM_AREA_SIZE, %rsp     //rsp += 0x80, should be guest_regs atp 

    test    %al, %al
    POPALL                   //restores guest's registers
    mov     %rcx, HSL_PADDING(%rsp)
    mov     %rdx, HSL_RESERVED(%rsp)

    jnz     .Ldone          //exvirt 
    jmp     .Srun


.Ldone:

    mov %rcx, %rsp
    push %rax
    popfq
    mov $0x6b, %ecx
    jmp *%rbx

    .size virt, . - virt
