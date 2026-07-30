; =============================================================================
; pure_lzma.asm — Pure hand-written LZMA-family codec (Win64 NASM)
; Hash-chain LZ77 + MSB-first bit packing (len/dist/literal).
; Format: 'pLZ1' | u32le size | packed bits
; =============================================================================
BITS 64
DEFAULT REL

global pure_lzma_encode_asm
global pure_lzma_decode_asm

HASH_SIZE equ 65536
MIN_MATCH equ 3
MAX_MATCH equ 258
WIN       equ 65536

section .bss
align 16
bw_out:  resq 1
bw_pos:  resq 1
bw_cap:  resq 1
bw_acc:  resd 1
bw_n:    resd 1
bw_ok:   resd 1
br_in:   resq 1
br_pos:  resq 1
br_len:  resq 1
br_acc:  resd 1
br_n:    resd 1

section .text

; ecx=bit
putbit:
    mov eax, [rel bw_acc]
    shl eax, 1
    mov edx, ecx
    and edx, 1
    or eax, edx
    mov [rel bw_acc], eax
    inc dword [rel bw_n]
    cmp dword [rel bw_n], 8
    jb .ok
    mov rax, [rel bw_pos]
    cmp rax, [rel bw_cap]
    jae .fail
    mov rdx, [rel bw_out]
    mov ecx, [rel bw_acc]
    mov [rdx+rax], cl
    inc rax
    mov [rel bw_pos], rax
    mov dword [rel bw_acc], 0
    mov dword [rel bw_n], 0
.ok:
    ret
.fail:
    mov dword [rel bw_ok], 0
    ret

; ecx=val, r8d=nbits (LSB-first emit)
putn:
    push rbx
    push r9
    mov ebx, ecx
    mov r9d, r8d
.lp:
    test r9d, r9d
    jz .dn
    mov ecx, ebx
    and ecx, 1
    call putbit
    cmp dword [rel bw_ok], 0
    je .dn
    shr ebx, 1
    dec r9d
    jmp .lp
.dn:
    pop r9
    pop rbx
    ret

flush_bits:
    cmp dword [rel bw_n], 0
    je .dn
    mov rax, [rel bw_pos]
    cmp rax, [rel bw_cap]
    jae .fail
    mov rdx, [rel bw_out]
    mov r8d, [rel bw_acc]
    mov ecx, 8
    sub ecx, [rel bw_n]
    shl r8d, cl
    mov [rdx+rax], r8b
    inc rax
    mov [rel bw_pos], rax
.dn:
    ret
.fail:
    mov dword [rel bw_ok], 0
    ret

; -> eax bit, CF=err
getbit:
    cmp dword [rel br_n], 0
    jne .hv
    mov rax, [rel br_pos]
    cmp rax, [rel br_len]
    jae .fail
    mov rdx, [rel br_in]
    movzx eax, byte [rdx+rax]
    mov [rel br_acc], eax
    inc qword [rel br_pos]
    mov dword [rel br_n], 8
.hv:
    mov eax, [rel br_acc]
    shr eax, 7
    and eax, 1
    shl dword [rel br_acc], 1
    and dword [rel br_acc], 0xFF
    dec dword [rel br_n]
    clc
    ret
.fail:
    stc
    ret

; r8d=nbits -> eax value LSB-first
getn:
    push rbx
    push r9
    xor ebx, ebx
    xor r9d, r9d
.lp:
    cmp r9d, r8d
    jae .dn
    call getbit
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

; =============================================================================
pure_lzma_encode_asm:
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

    mov dword [rdi], 0x315A4C70
    mov [rdi+4], r12d
    mov [rel bw_out], rdi
    mov qword [rel bw_pos], 8
    mov [rel bw_cap], r13
    mov dword [rel bw_acc], 0
    mov dword [rel bw_n], 0
    mov dword [rel bw_ok], 1

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

    ; match find -> r8d=len, r9d=dist
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
    imul eax, 0x9E3779B1
    shr eax, 16
    and eax, 0xFFFF

    mov ecx, [r14+rax*4]
    lea edx, [ebp+1]
    mov [r14+rax*4], edx
    mov [r15+rbp*4], ecx

    mov r10d, 48
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
    cmp r11d, 32
    jae .dec
.wn:
    mov ecx, [r15+rbx*4]
    dec r10d
    jnz .walk

.dec:
    cmp r8d, MIN_MATCH
    jb .lit

    ; save len in r12 temporarily? r12 is n. use stack
    push r8
    push r9
    mov ecx, 1
    call putbit
    pop r9
    pop r8
    cmp dword [rel bw_ok], 0
    je .fail

    mov ecx, r8d
    sub ecx, MIN_MATCH
    cmp ecx, 255
    jbe .lok
    mov ecx, 255
.lok:
    push r8
    mov r8d, 8
    call putn
    pop r8
    push r8
    mov ecx, r9d
    mov r8d, 16
    call putn
    pop r8
    cmp dword [rel bw_ok], 0
    je .fail
    add rbp, r8
    jmp .main

.lit:
    xor ecx, ecx
    call putbit
    movzx ecx, byte [rsi+rbp]
    mov r8d, 8
    call putn
    cmp dword [rel bw_ok], 0
    je .fail
    inc rbp
    jmp .main

.fin:
    call flush_bits
    cmp dword [rel bw_ok], 0
    je .fail
    mov rax, [rel bw_pos]
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

; =============================================================================
pure_lzma_decode_asm:
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
    cmp dword [rsi], 0x315A4C70
    jne .fail
    mov r14d, [rsi+4]
    cmp r14, r13
    ja .fail

    mov [rel br_in], rsi
    mov qword [rel br_pos], 8
    mov [rel br_len], r12
    mov dword [rel br_acc], 0
    mov dword [rel br_n], 0

    xor ebp, ebp
    test r14d, r14d
    jz .ok

.main:
    cmp ebp, r14d
    jae .ok
    call getbit
    jc .fail
    test al, al
    jnz .match

    mov r8d, 8
    call getn
    jc .fail
    mov [rdi+rbp], al
    inc ebp
    jmp .main

.match:
    mov r8d, 8
    call getn
    jc .fail
    add eax, MIN_MATCH
    mov r15d, eax
    mov r8d, 16
    call getn
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
