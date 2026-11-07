; Test targets for hooks_test.cpp, in assembly so their register use and layout are exact.

.code

; Fails unless reg still holds all ones.
CHECK_XMM MACRO reg
    pcmpeqd xmm0, xmm0
    pcmpeqd xmm0, reg
    pmovmskb eax, xmm0
    cmp eax, 0FFFFh
    jne failed
ENDM

; hk_test_abi(target): calls target (hooked integers()) with 1..8 as its arguments, after filling every nonvolatile
; register with a known value. Returns 1 if the result is 204 (all eight arguments arrived) and every nonvolatile
; register survived the detour, else 0.
hk_test_abi PROC FRAME
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 118h
    .allocstack 118h
    movdqu [rsp + 40h], xmm6
    .savexmm128 xmm6, 40h
    movdqu [rsp + 50h], xmm7
    .savexmm128 xmm7, 50h
    movdqu [rsp + 60h], xmm8
    .savexmm128 xmm8, 60h
    movdqu [rsp + 70h], xmm9
    .savexmm128 xmm9, 70h
    movdqu [rsp + 80h], xmm10
    .savexmm128 xmm10, 80h
    movdqu [rsp + 90h], xmm11
    .savexmm128 xmm11, 90h
    movdqu [rsp + 0A0h], xmm12
    .savexmm128 xmm12, 0A0h
    movdqu [rsp + 0B0h], xmm13
    .savexmm128 xmm13, 0B0h
    movdqu [rsp + 0C0h], xmm14
    .savexmm128 xmm14, 0C0h
    movdqu [rsp + 0D0h], xmm15
    .savexmm128 xmm15, 0D0h
    .endprolog
    mov [rsp + 0E0h], rcx
    mov rbx, 101h
    mov rbp, 202h
    mov rsi, 303h
    mov rdi, 404h
    mov r12, 505h
    mov r13, 606h
    mov r14, 707h
    mov r15, 808h
    pcmpeqd xmm6, xmm6
    pcmpeqd xmm7, xmm7
    pcmpeqd xmm8, xmm8
    pcmpeqd xmm9, xmm9
    pcmpeqd xmm10, xmm10
    pcmpeqd xmm11, xmm11
    pcmpeqd xmm12, xmm12
    pcmpeqd xmm13, xmm13
    pcmpeqd xmm14, xmm14
    pcmpeqd xmm15, xmm15
    mov rcx, 1
    mov rdx, 2
    mov r8, 3
    mov r9, 4
    mov qword ptr [rsp + 20h], 5
    mov qword ptr [rsp + 28h], 6
    mov qword ptr [rsp + 30h], 7
    mov qword ptr [rsp + 38h], 8
    call qword ptr [rsp + 0E0h]
    cmp rax, 204
    jne failed
    cmp rbx, 101h
    jne failed
    cmp rbp, 202h
    jne failed
    cmp rsi, 303h
    jne failed
    cmp rdi, 404h
    jne failed
    cmp r12, 505h
    jne failed
    cmp r13, 606h
    jne failed
    cmp r14, 707h
    jne failed
    cmp r15, 808h
    jne failed
    CHECK_XMM xmm6
    CHECK_XMM xmm7
    CHECK_XMM xmm8
    CHECK_XMM xmm9
    CHECK_XMM xmm10
    CHECK_XMM xmm11
    CHECK_XMM xmm12
    CHECK_XMM xmm13
    CHECK_XMM xmm14
    CHECK_XMM xmm15
    mov eax, 1
    jmp done
failed:
    xor eax, eax
done:
    movdqu xmm6, [rsp + 40h]
    movdqu xmm7, [rsp + 50h]
    movdqu xmm8, [rsp + 60h]
    movdqu xmm9, [rsp + 70h]
    movdqu xmm10, [rsp + 80h]
    movdqu xmm11, [rsp + 90h]
    movdqu xmm12, [rsp + 0A0h]
    movdqu xmm13, [rsp + 0B0h]
    movdqu xmm14, [rsp + 0C0h]
    movdqu xmm15, [rsp + 0D0h]
    add rsp, 118h
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
hk_test_abi ENDP
; Two functions exactly 8 bytes apart: hooking both checks that one detour's patch doesn't hide or break the other.
ALIGN 16
hk_adjacent_a PROC
    lea rax, [rcx + 11h]
    ret
    db 90h, 90h, 90h
hk_adjacent_a ENDP
hk_adjacent_b PROC
    lea rax, [rcx + 22h]
    ret
    db 90h, 90h, 90h
hk_adjacent_b ENDP

; Another mod's hook on a function SET wants: the test writes a jump from the target to the detour, then SET must
; still find the target by its original bytes and chain in front of that jump. The odd constant keeps them unique.
hk_foreign_target PROC
    lea rax, [rcx + 12345678h]
    ret
    db 90h, 90h, 90h, 90h, 90h, 90h, 90h, 90h
hk_foreign_target ENDP
hk_foreign_detour PROC
    lea rax, [rcx + 2]
    ret
hk_foreign_detour ENDP
END
