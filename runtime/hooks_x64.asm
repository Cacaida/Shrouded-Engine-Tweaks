; Ordinary Windows x64 ABI only. RAX carries the context, not an argument.
; The thunk leaves RSP intact. This fixed frame supplies shadow space and unwind metadata.
; Saved matches hooks.cpp: xmm at 20h, context at 80h, RCX at B0h, return address at B8h (stack arguments follow it).

EXTERN hk_capture:PROC

TRAMPOLINE EQU 0  ; offset of the trampoline pointer in the context (hooks.cpp checks it)

.code

hk_common PROC FRAME
    sub rsp, 0B8h
    .allocstack 0B8h
    .endprolog
    mov [rsp + 80h], rax
    mov [rsp + 88h], r11
    mov [rsp + 90h], r10
    mov [rsp + 98h], r9
    mov [rsp + 0A0h], r8
    mov [rsp + 0A8h], rdx
    mov [rsp + 0B0h], rcx
    movdqu xmmword ptr [rsp + 20h], xmm0
    movdqu xmmword ptr [rsp + 30h], xmm1
    movdqu xmmword ptr [rsp + 40h], xmm2
    movdqu xmmword ptr [rsp + 50h], xmm3
    movdqu xmmword ptr [rsp + 60h], xmm4
    movdqu xmmword ptr [rsp + 70h], xmm5
    mov rcx, rax
    mov rdx, rsp
    call hk_capture
    movdqu xmm0, xmmword ptr [rsp + 20h]
    movdqu xmm1, xmmword ptr [rsp + 30h]
    movdqu xmm2, xmmword ptr [rsp + 40h]
    movdqu xmm3, xmmword ptr [rsp + 50h]
    movdqu xmm4, xmmword ptr [rsp + 60h]
    movdqu xmm5, xmmword ptr [rsp + 70h]
    mov rax, [rsp + 80h]
    mov r11, [rsp + 88h]
    mov r10, [rsp + 90h]
    mov r9, [rsp + 98h]
    mov r8, [rsp + 0A0h]
    mov rdx, [rsp + 0A8h]
    mov rcx, [rsp + 0B0h]
    add rsp, 0B8h
    jmp qword ptr [rax + TRAMPOLINE]
hk_common ENDP

END
