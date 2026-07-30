; BAV hot-path kernels (NASM, Win64 / SysV compatible via C wrapper)
; Exports: bav_crc32_nasm, bav_mtf_encode_nasm, bav_mtf_decode_nasm
; Calling convention: Windows x64 (rcx, rdx, r8, r9)

BITS 64
DEFAULT REL

section .text

global bav_crc32_nasm
global bav_mtf_encode_nasm
global bav_mtf_decode_nasm

; uint32_t bav_crc32_nasm(const uint8_t *data, size_t len, uint32_t crc)
; Windows x64: rcx=data, rdx=len, r8d=crc
; Uses hardware CRC32 if available; otherwise table-free bit-by-bit poly 0xEDB88320
bav_crc32_nasm:
    push rbx
    mov eax, r8d          ; crc
    not eax
    test rdx, rdx
    jz .done
    xor rbx, rbx
.loop:
    movzx r9d, byte [rcx + rbx]
    xor al, r9b
    mov r10d, 8
.bit:
    shr eax, 1
    jnc .nobit
    xor eax, 0xEDB88320
.nobit:
    dec r10d
    jnz .bit
    inc rbx
    cmp rbx, rdx
    jb .loop
.done:
    not eax
    pop rbx
    ret

; void bav_mtf_encode_nasm(const uint8_t *in, uint8_t *out, size_t n)
; rcx=in, rdx=out, r8=n
; Uses stack-allocated 256-byte table
bav_mtf_encode_nasm:
    push rbx
    push rsi
    push rdi
    push rbp
    sub rsp, 288              ; table[256] + alignment
    mov rsi, rcx              ; in
    mov rdi, rdx              ; out
    mov rbp, r8               ; n
    ; init table[i] = i
    xor eax, eax
.init_t:
    mov byte [rsp + rax], al
    inc eax
    cmp eax, 256
    jb .init_t
    test rbp, rbp
    jz .mtf_enc_done
    xor rbx, rbx              ; i
.mtf_enc_loop:
    movzx ecx, byte [rsi + rbx]  ; symbol b
    ; find rank r of b in table
    xor edx, edx
.find:
    cmp byte [rsp + rdx], cl
    je .found
    inc edx
    cmp edx, 256
    jb .find
.found:
    mov byte [rdi + rbx], dl  ; out[i] = r
    ; move to front if r != 0
    test edx, edx
    jz .next
    ; shift table[0..r-1] right by one (from r-1 down to 0)
    mov r9d, edx
.shift:
    movzx eax, byte [rsp + r9 - 1]
    mov byte [rsp + r9], al
    dec r9d
    jnz .shift
    mov byte [rsp], cl
.next:
    inc rbx
    cmp rbx, rbp
    jb .mtf_enc_loop
.mtf_enc_done:
    add rsp, 288
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; void bav_mtf_decode_nasm(const uint8_t *in, uint8_t *out, size_t n)
; rcx=in, rdx=out, r8=n
bav_mtf_decode_nasm:
    push rbx
    push rsi
    push rdi
    push rbp
    sub rsp, 288
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8
    xor eax, eax
.init_d:
    mov byte [rsp + rax], al
    inc eax
    cmp eax, 256
    jb .init_d
    test rbp, rbp
    jz .mtf_dec_done
    xor rbx, rbx
.mtf_dec_loop:
    movzx edx, byte [rsi + rbx]  ; rank r
    movzx ecx, byte [rsp + rdx]  ; symbol b = table[r]
    mov byte [rdi + rbx], cl
    test edx, edx
    jz .next_d
    mov r9d, edx
.shift_d:
    movzx eax, byte [rsp + r9 - 1]
    mov byte [rsp + r9], al
    dec r9d
    jnz .shift_d
    mov byte [rsp], cl
.next_d:
    inc rbx
    cmp rbx, rbp
    jb .mtf_dec_loop
.mtf_dec_done:
    add rsp, 288
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret
