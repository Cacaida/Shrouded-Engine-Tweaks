; Ordinary Windows x64 ABI only. RAX carries the context, not an argument.
; The thunk leaves RSP intact. This fixed frame supplies shadow space, an outgoing area for stack arguments 5 to 8 and
; unwind metadata.
; Saved matches hooks.cpp: outgoing stack arguments at 20h, xmm at 40h, context at A0h, RCX at D0h, the entry's rule
; order and after mask at D8h and E0h, return address at E8h (the caller's stack arguments start at 110h).
; hk_entry returns 0 to run the original, 1 when a rule blocked the call (the return value is then in the context slot
; for RAX and the saved XMM0), 2 when rules change the return value: the stub then calls the original instead of
; jumping to it, and hk_after changes what it returned.

EXTERN hk_entry:PROC
EXTERN hk_after:PROC

TRAMPOLINE EQU 0  ; offset of the trampoline pointer in the context (hooks.cpp checks it)

.code

hk_common PROC FRAME
    sub rsp, 0E8h
    .allocstack 0E8h
    .endprolog
    mov [rsp + 0A0h], rax
    mov [rsp + 0A8h], r11
    mov [rsp + 0B0h], r10
    mov [rsp + 0B8h], r9
    mov [rsp + 0C0h], r8
    mov [rsp + 0C8h], rdx
    mov [rsp + 0D0h], rcx
    movdqu xmmword ptr [rsp + 40h], xmm0
    movdqu xmmword ptr [rsp + 50h], xmm1
    movdqu xmmword ptr [rsp + 60h], xmm2
    movdqu xmmword ptr [rsp + 70h], xmm3
    movdqu xmmword ptr [rsp + 80h], xmm4
    movdqu xmmword ptr [rsp + 90h], xmm5
    mov rcx, rax
    mov rdx, rsp
    call hk_entry  ; may change the saved argument registers and stack arguments restored below
    test eax, eax
    jnz special
    movdqu xmm0, xmmword ptr [rsp + 40h]  ; inline: this is every hooked call's path
    movdqu xmm1, xmmword ptr [rsp + 50h]
    movdqu xmm2, xmmword ptr [rsp + 60h]
    movdqu xmm3, xmmword ptr [rsp + 70h]
    movdqu xmm4, xmmword ptr [rsp + 80h]
    movdqu xmm5, xmmword ptr [rsp + 90h]
    mov rax, [rsp + 0A0h]
    mov r11, [rsp + 0A8h]
    mov r10, [rsp + 0B0h]
    mov r9, [rsp + 0B8h]
    mov r8, [rsp + 0C0h]
    mov rdx, [rsp + 0C8h]
    mov rcx, [rsp + 0D0h]
    add rsp, 0E8h
    jmp qword ptr [rax + TRAMPOLINE]
special:
    cmp eax, 1
    jne after
    ; A rule blocked the call: return its value, as the original would. Only volatile registers and the frame
    ; allocated above were touched, and an x64 callee pops no arguments, so the caller sees an ordinary return.
    movdqu xmm0, xmmword ptr [rsp + 40h]
    mov rax, [rsp + 0A0h]
    add rsp, 0E8h
    ret
after:
    ; Call the original from this frame, with stack arguments 5 to 8 copied below it (reading slots past the real
    ; arity only reads the caller's own frame), then let hk_after change the result.
    mov r10, [rsp + 110h]
    mov [rsp + 20h], r10
    mov r10, [rsp + 118h]
    mov [rsp + 28h], r10
    mov r10, [rsp + 120h]
    mov [rsp + 30h], r10
    mov r10, [rsp + 128h]
    mov [rsp + 38h], r10
    call restore
    call qword ptr [rax + TRAMPOLINE]
    mov [rsp + 0A8h], rax
    movdqu xmmword ptr [rsp + 40h], xmm0
    mov rcx, [rsp + 0A0h]
    mov rdx, rsp
    call hk_after  ; reads and rewrites the result in the R11 slot and the saved XMM0
    movdqu xmm0, xmmword ptr [rsp + 40h]
    mov rax, [rsp + 0A8h]
    add rsp, 0E8h
    ret
hk_common ENDP

; The return-change path: the saved registers back from the caller's frame (hk_common's, one return address above this
; one's RSP), RAX = the context. A leaf without a frame: it needs no unwind metadata.
restore PROC
    movdqu xmm0, xmmword ptr [rsp + 48h]
    movdqu xmm1, xmmword ptr [rsp + 58h]
    movdqu xmm2, xmmword ptr [rsp + 68h]
    movdqu xmm3, xmmword ptr [rsp + 78h]
    movdqu xmm4, xmmword ptr [rsp + 88h]
    movdqu xmm5, xmmword ptr [rsp + 98h]
    mov rax, [rsp + 0A8h]
    mov r11, [rsp + 0B0h]
    mov r10, [rsp + 0B8h]
    mov r9, [rsp + 0C0h]
    mov r8, [rsp + 0C8h]
    mov rdx, [rsp + 0D0h]
    mov rcx, [rsp + 0D8h]
    ret
restore ENDP

END
