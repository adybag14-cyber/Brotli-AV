; pure_brotli.asm — Pure hand-written Brotli-family (Win64 NASM)
; LZ77 bit-packed; min-match 3, medium chain, different hash (brotli-like).
; Format: 'pBR1' | u32le size | bits
BITS 64
DEFAULT REL

global pure_brotli_encode_asm
global pure_brotli_decode_asm

HASH_SIZE equ 65536
MIN_MATCH equ 3
MAX_MATCH equ 258
WIN       equ 65536

section .bss
align 16
b_out: resq 1
b_pos: resq 1
b_cap: resq 1
b_acc: resd 1
b_n:   resd 1
b_ok:  resd 1
bi_in: resq 1
bi_pos:resq 1
bi_len:resq 1
bi_acc:resd 1
bi_n:  resd 1

section .text

bputbit:
    mov eax, [rel b_acc]
    shl eax, 1
    mov edx, ecx
    and edx, 1
    or eax, edx
    mov [rel b_acc], eax
    inc dword [rel b_n]
    cmp dword [rel b_n], 8
    jb .ok
    mov rax, [rel b_pos]
    cmp rax, [rel b_cap]
    jae .fail
    mov rdx, [rel b_out]
    mov ecx, [rel b_acc]
    mov [rdx+rax], cl
    inc rax
    mov [rel b_pos], rax
    mov dword [rel b_acc], 0
    mov dword [rel b_n], 0
.ok: ret
.fail:
    mov dword [rel b_ok], 0
    ret

bputn:
    push rbx
    push r9
    mov ebx, ecx
    mov r9d, r8d
.lp:
    test r9d, r9d
    jz .dn
    mov ecx, ebx
    and ecx, 1
    call bputbit
    cmp dword [rel b_ok], 0
    je .dn
    shr ebx, 1
    dec r9d
    jmp .lp
.dn:
    pop r9
    pop rbx
    ret

bflush:
    cmp dword [rel b_n], 0
    je .dn
    mov rax, [rel b_pos]
    cmp rax, [rel b_cap]
    jae .fail
    mov rdx, [rel b_out]
    mov r8d, [rel b_acc]
    mov ecx, 8
    sub ecx, [rel b_n]
    shl r8d, cl
    mov [rdx+rax], r8b
    inc rax
    mov [rel b_pos], rax
.dn: ret
.fail:
    mov dword [rel b_ok], 0
    ret

bgetbit:
    cmp dword [rel bi_n], 0
    jne .hv
    mov rax, [rel bi_pos]
    cmp rax, [rel bi_len]
    jae .fail
    mov rdx, [rel bi_in]
    movzx eax, byte [rdx+rax]
    mov [rel bi_acc], eax
    inc qword [rel bi_pos]
    mov dword [rel bi_n], 8
.hv:
    mov eax, [rel bi_acc]
    shr eax, 7
    and eax, 1
    shl dword [rel bi_acc], 1
    and dword [rel bi_acc], 0xFF
    dec dword [rel bi_n]
    clc
    ret
.fail: stc
    ret

bgetn:
    push rbx
    push r9
    xor ebx, ebx
    xor r9d, r9d
.lp:
    cmp r9d, r8d
    jae .dn
    call bgetbit
    jc .bad
    mov ecx, r9d
    shl eax, cl
    or ebx, eax
    inc r9d
    jmp .lp
.dn:
    mov eax, ebx
    clc
    pop r9
    pop rbx
    ret
.bad:
    stc
    pop r9
    pop rbx
    ret

pure_brotli_encode_asm:
    push rbp
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    mov r14, [rsp+0x28+64]
    mov r15, [rsp+0x30+64]
    mov rsi, rcx
    mov r12, rdx
    mov rdi, r8
    mov r13, r9
    cmp r13, 16
    jb .fail
    test r14, r14
    jz .fail
    test r15, r15
    jz .fail
    mov dword [rdi], 0x31524270
    mov [rdi+4], r12d
    mov [rel b_out], rdi
    mov qword [rel b_pos], 8
    mov [rel b_cap], r13
    mov dword [rel b_acc], 0
    mov dword [rel b_n], 0
    mov dword [rel b_ok], 1
    xor eax, eax
    mov rcx, r14
    mov edx, HASH_SIZE
.clr:
    mov [rcx], eax
    add rcx, 4
    dec edx
    jnz .clr
    xor ebp, ebp
    test r12, r12
    jz .fin
.main:
    cmp rbp, r12
    jae .fin
    xor r8d, r8d
    xor r9d, r9d
    lea rax, [rbp+MIN_MATCH]
    cmp rax, r12
    ja .dec
    movzx eax, byte [rsi+rbp]
    movzx ecx, byte [rsi+rbp+1]
    movzx edx, byte [rsi+rbp+2]
    shl ecx, 8
    or eax, ecx
    shl edx, 16
    or eax, edx
    imul eax, 0xC2B2AE3D
    shr eax, 16
    and eax, 0xFFFF
    mov ecx, [r14+rax*4]
    lea edx, [ebp+1]
    mov [r14+rax*4], edx
    mov [r15+rbp*4], ecx
    mov r10d, 56
.walk:
    test ecx, ecx
    jz .dec
    lea ebx, [ecx-1]
    mov eax, ebp
    sub eax, ebx
    jbe .wn
    cmp eax, WIN
    ja .dec
    xor r11d, r11d
.cmp:
    mov rdx, rbp
    add rdx, r11
    cmp rdx, r12
    jae .cd
    lea rdx, [rsi+rbp]
    mov cl, [rdx+r11]
    lea rdx, [rsi+rbx]
    cmp cl, [rdx+r11]
    jne .cd
    inc r11d
    cmp r11d, MAX_MATCH
    jb .cmp
.cd:
    cmp r11d, MIN_MATCH
    jb .wn
    cmp r11d, r8d
    jbe .wn
    mov r8d, r11d
    mov r9d, eax
    cmp r11d, 36
    jae .dec
.wn:
    mov ecx, [r15+rbx*4]
    dec r10d
    jnz .walk
.dec:
    cmp r8d, MIN_MATCH
    jb .lit
    push r8
    push r9
    mov ecx, 1
    call bputbit
    pop r9
    pop r8
    cmp dword [rel b_ok], 0
    je .fail
    mov ecx, r8d
    sub ecx, MIN_MATCH
    cmp ecx, 255
    jbe .lok
    mov ecx, 255
.lok:
    push r8
    mov r8d, 8
    call bputn
    pop r8
    push r8
    mov ecx, r9d
    mov r8d, 16
    call bputn
    pop r8
    cmp dword [rel b_ok], 0
    je .fail
    add rbp, r8
    jmp .main
.lit:
    xor ecx, ecx
    call bputbit
    movzx ecx, byte [rsi+rbp]
    mov r8d, 8
    call bputn
    cmp dword [rel b_ok], 0
    je .fail
    inc rbp
    jmp .main
.fin:
    call bflush
    cmp dword [rel b_ok], 0
    je .fail
    mov rax, [rel b_pos]
    jmp .ret
.fail:
    mov rax, -1
.ret:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret

pure_brotli_decode_asm:
    push rbp
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    mov rsi, rcx
    mov r12, rdx
    mov rdi, r8
    mov r13, r9
    cmp r12, 8
    jb .fail
    cmp dword [rsi], 0x31524270
    jne .fail
    mov r14d, [rsi+4]
    cmp r14, r13
    ja .fail
    mov [rel bi_in], rsi
    mov qword [rel bi_pos], 8
    mov [rel bi_len], r12
    mov dword [rel bi_acc], 0
    mov dword [rel bi_n], 0
    xor ebp, ebp
    test r14d, r14d
    jz .ok
.main:
    cmp ebp, r14d
    jae .ok
    call bgetbit
    jc .fail
    test al, al
    jnz .match
    mov r8d, 8
    call bgetn
    jc .fail
    mov [rdi+rbp], al
    inc ebp
    jmp .main
.match:
    mov r8d, 8
    call bgetn
    jc .fail
    add eax, MIN_MATCH
    mov r15d, eax
    mov r8d, 16
    call bgetn
    jc .fail
    test eax, eax
    jz .fail
    cmp eax, ebp
    ja .fail
    mov ebx, eax
    mov eax, ebp
    sub eax, ebx
    mov ecx, r15d
.cpy:
    cmp ebp, r14d
    jae .ok
    mov dl, [rdi+rax]
    mov [rdi+rbp], dl
    inc eax
    inc ebp
    dec ecx
    jnz .cpy
    jmp .main
.ok:
    mov eax, r14d
    jmp .ret
.fail:
    mov rax, -1
.ret:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret
