/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        dll/win32/kernel32/client/amd64/fiber.S
 * PURPOSE:     Fiber context switch code for the amd64 architecture
 * PROGRAMMER:
 */

#include <asm.inc>
#include <ksamd64.inc>

EXTERN BaseThreadStartup:PROC

/* Not emitted by the asm-offset generator (nothing else in amd64 asm
 * referenced it before now). FIBER's layout here (see ndk/ketypes.h,
 * the pre-Longhorn #else branch, confirmed active by FbGuaranteedStackBytes
 * == 0x500 and FbFlsData == 0x508 with no FbWx86Tib/FbTebFlags emitted)
 * is: ..., GuaranteedStackBytes, FlsData, ActivationContextStackPointer --
 * i.e. right after the 8-byte FlsData pointer, needing no padding. */
FbActivationContextStackPointer equ FbFlsData + 8

.code64

/*
 * BaseFiberStartup is jumped to (never called) the first time a freshly
 * created fiber is switched to. BaseInitializeContext() stashes the
 * fiber's start routine/parameter in the saved Rcx/Rdx of its initial
 * FiberContext and points Rip here. Rcx/Rdx are volatile registers, so
 * SwitchToFiber() below does not restore them into real registers on an
 * ordinary switch -- fetch them back out of the fiber's own saved
 * context via the TEB, the same trick the x86 implementation plays with
 * its FIBER_CONTEXT_EAX/EBX fields.
 */
PUBLIC BaseFiberStartup
FUNC BaseFiberStartup
    .ENDPROLOG

    mov rax, gs:[TeSelf]
    mov rax, [rax + TeFiberData]
    mov rcx, [rax + FbFiberContext + CxRcx]
    mov rdx, [rax + FbFiberContext + CxRdx]

    /* BaseInitializeContext() gives a fresh fiber a 16-byte-*aligned* Rsp
     * (see its ASSERT), but every compiled function's prologue -- this
     * one included -- assumes the ABI's "as if reached by call" state:
     * Rsp = 8 (mod 16), leaving room for the implicit return address.
     * A bare jmp here would land 8 bytes short of that and misalign every
     * shadow-space spill and nested call inside BaseThreadStartup. Use
     * call instead (BaseThreadStartup is DECLSPEC_NORETURN and never
     * actually returns, so the pushed return address is never used). */
    call BaseThreadStartup

ENDFUNC


/*
 * VOID
 * SwitchToFiber(
 *     _In_ PVOID lpFiber <rcx>);
 *
 * Suspends the calling fiber and resumes lpFiber. Only the non-volatile
 * registers need to be preserved across the switch (the volatile ones
 * are the caller's problem, same as any other call); RSP/RIP of the
 * outgoing fiber are recovered from the return address already sitting
 * on the stack, so resuming it later continues exactly as if this call
 * had returned normally.
 */
PUBLIC SwitchToFiber
FUNC SwitchToFiber
    .ENDPROLOG

    mov r10, gs:[TeSelf]
    mov rax, [r10 + TeFiberData]           /* rax = outgoing PFIBER */

    /* Save non-volatile GP registers of the fiber we're leaving */
    mov [rax + FbFiberContext + CxRbx], rbx
    mov [rax + FbFiberContext + CxRbp], rbp
    mov [rax + FbFiberContext + CxRsi], rsi
    mov [rax + FbFiberContext + CxRdi], rdi
    mov [rax + FbFiberContext + CxR12], r12
    mov [rax + FbFiberContext + CxR13], r13
    mov [rax + FbFiberContext + CxR14], r14
    mov [rax + FbFiberContext + CxR15], r15

    /* Save the resume point: the return address on top of the stack,
     * and RSP as it will be once that address is popped -- i.e. exactly
     * as if this function had returned normally via 'ret' */
    mov r11, [rsp]
    mov [rax + FbFiberContext + CxRip], r11
    lea r11, [rsp + 8]
    mov [rax + FbFiberContext + CxRsp], r11

    /* Save FPU/SSE state unconditionally (MXCSR + non-volatile XMM6-15) */
    stmxcsr dword ptr [rax + FbFiberContext + CxMxCsr]
    movups [rax + FbFiberContext + CxXmm6],  xmm6
    movups [rax + FbFiberContext + CxXmm7],  xmm7
    movups [rax + FbFiberContext + CxXmm8],  xmm8
    movups [rax + FbFiberContext + CxXmm9],  xmm9
    movups [rax + FbFiberContext + CxXmm10], xmm10
    movups [rax + FbFiberContext + CxXmm11], xmm11
    movups [rax + FbFiberContext + CxXmm12], xmm12
    movups [rax + FbFiberContext + CxXmm13], xmm13
    movups [rax + FbFiberContext + CxXmm14], xmm14
    movups [rax + FbFiberContext + CxXmm15], xmm15

    /* Transfer the TEB-resident fiber state into the outgoing fiber */
    mov r11, gs:[0]                        /* NtTib.ExceptionList */
    mov [rax + FbExceptionList], r11
    mov r11, [r10 + TeStackLimit]
    mov [rax + FbStackLimit], r11
    mov r11, [r10 + TeGuaranteedStackBytes]
    mov [rax + FbGuaranteedStackBytes], r11
    mov r11, [r10 + TeFlsData]
    mov [rax + FbFlsData], r11
    mov r11, [r10 + TeActivationContextStackPointer]
    mov [rax + FbActivationContextStackPointer], r11

    /* Switch the TEB over to the new fiber (rcx = incoming PFIBER) */
    mov [r10 + TeFiberData], rcx

    mov r11, [rcx + FbExceptionList]
    mov gs:[0], r11
    mov r11, [rcx + FbStackBase]
    mov [r10 + TeStackBase], r11
    mov r11, [rcx + FbStackLimit]
    mov [r10 + TeStackLimit], r11
    mov r11, [rcx + FbDeallocationStack]
    mov [r10 + TeDeallocationStack], r11
    mov r11, [rcx + FbGuaranteedStackBytes]
    mov [r10 + TeGuaranteedStackBytes], r11
    mov r11, [rcx + FbActivationContextStackPointer]
    mov [r10 + TeActivationContextStackPointer], r11
    mov r11, [rcx + FbFlsData]
    mov [r10 + TeFlsData], r11

    /* Restore FPU/SSE state */
    ldmxcsr dword ptr [rcx + FbFiberContext + CxMxCsr]
    movups xmm6,  [rcx + FbFiberContext + CxXmm6]
    movups xmm7,  [rcx + FbFiberContext + CxXmm7]
    movups xmm8,  [rcx + FbFiberContext + CxXmm8]
    movups xmm9,  [rcx + FbFiberContext + CxXmm9]
    movups xmm10, [rcx + FbFiberContext + CxXmm10]
    movups xmm11, [rcx + FbFiberContext + CxXmm11]
    movups xmm12, [rcx + FbFiberContext + CxXmm12]
    movups xmm13, [rcx + FbFiberContext + CxXmm13]
    movups xmm14, [rcx + FbFiberContext + CxXmm14]
    movups xmm15, [rcx + FbFiberContext + CxXmm15]

    /* Restore non-volatile GP registers */
    mov rbx, [rcx + FbFiberContext + CxRbx]
    mov rbp, [rcx + FbFiberContext + CxRbp]
    mov rsi, [rcx + FbFiberContext + CxRsi]
    mov rdi, [rcx + FbFiberContext + CxRdi]
    mov r12, [rcx + FbFiberContext + CxR12]
    mov r13, [rcx + FbFiberContext + CxR13]
    mov r14, [rcx + FbFiberContext + CxR14]
    mov r15, [rcx + FbFiberContext + CxR15]

    /* Load the resume point and jump into it. For a never-before-run
     * fiber this is BaseFiberStartup with Rsp = its fresh stack, exactly
     * as set up by BaseInitializeContext(); for a previously suspended
     * fiber it is wherever its own SwitchToFiber() call left off. */
    mov r11, [rcx + FbFiberContext + CxRip]
    mov rsp, [rcx + FbFiberContext + CxRsp]

    jmp r11

ENDFUNC

END
