/*
 * PROJECT:     LiberNT Hyper-V VMBus Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Executable page the hypervisor overlays with its hypercall code
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <asm.inc>

#ifdef _M_IX86
/* i386: the hypervisor's code takes the control word in EDX:EAX, the input
 * in EBX:ECX and the output in EDI:ESI, and returns its status in EDX:EAX.
 * The page is reached only through HvHypercallX86, which loads those registers. */
.code

.align 4096
PUBLIC _HvHypercallCode
_HvHypercallCode:
    mov eax, HEX(FFFF)
    xor edx, edx
    ret
    .fill 4088, 1, HEX(CC)

/* ULONG64 __stdcall HvHypercallX86(ULONG64 Control, ULONG64 Input, ULONG64 Output) */
PUBLIC _HvHypercallX86@24
_HvHypercallX86@24:
    push ebx
    push esi
    push edi
    /* Stack: 3 saved registers (12) + return address (4) = 16 bytes to the first argument */
    mov eax, [esp + 16]
    mov edx, [esp + 20]
    mov ecx, [esp + 24]
    mov ebx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    call _HvHypercallCode
    pop edi
    pop esi
    pop ebx
    ret 24
#else
#ifdef _USE_ML
HVCALL SEGMENT ALIGN(4096) READ EXECUTE ALIAS(".text") 'CODE'
#else
.code64

.align 4096
#endif
PUBLIC HvHypercallPage
HvHypercallPage:
    mov rax, HEX(FFFF)
    ret
    .fill 4088, 1, HEX(CC)
#ifdef _USE_ML
HVCALL ENDS
#endif
#endif

END
