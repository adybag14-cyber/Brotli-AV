; BAV full-research hot kernels — NASM Win64 (rcx, rdx, r8, r9)
BITS 64
DEFAULT REL

section .text

global bav_crc32_asm
global bav_mtf_encode_asm
global bav_mtf_decode_asm
global bav_rle0_encode_asm
global bav_rle0_decode_asm
global bav_transpose_asm
global bav_untranspose_asm
global bav_sub_delta_asm
global bav_sub_delta_inv_asm
global bav_xor_delta_asm
global bav_xor_delta_inv_asm

; uint32_t bav_crc32_asm(const uint8_t *data, size_t len)
; rcx=data, rdx=len  → eax = zlib-compatible CRC32
bav_crc32_asm:
    push rbx
    mov eax, 0xFFFFFFFF
    test rdx, rdx
    jz .crc_done
    xor rbx, rbx
.crc_loop:
    movzx r8d, byte [rcx + rbx]
    xor al, r8b
    mov r9d, 8
.crc_bit:
    shr eax, 1
    jnc .crc_nobit
    xor eax, 0xEDB88320
.crc_nobit:
    dec r9d
    jnz .crc_bit
    inc rbx
    cmp rbx, rdx
    jb .crc_loop
.crc_done:
    not eax
    pop rbx
    ret

; void bav_mtf_encode_asm(const uint8_t *in, uint8_t *out, size_t n)
; rcx=in rdx=out r8=n
bav_mtf_encode_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    sub rsp, 512                 ; table[256] + pos[256]
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8
    ; table[i]=i, pos[i]=i
    xor eax, eax
.mtf_init:
    mov byte [rsp + rax], al
    mov byte [rsp + 256 + rax], al
    inc eax
    cmp eax, 256
    jb .mtf_init
    test rbp, rbp
    jz .mtf_enc_done
    xor rbx, rbx
.mtf_enc_loop:
    movzx ecx, byte [rsi + rbx]  ; b
    movzx edx, byte [rsp + 256 + rcx] ; r = pos[b]
    mov byte [rdi + rbx], dl
    test edx, edx
    jz .mtf_enc_next
    ; shift table: for j=r; j>0; j-- table[j]=table[j-1], pos[table[j]]=j
.mtf_shift:
    movzx eax, byte [rsp + rdx - 1]
    mov byte [rsp + rdx], al
    mov byte [rsp + 256 + rax], dl
    dec edx
    jnz .mtf_shift
    mov byte [rsp], cl           ; table[0]=b
    mov byte [rsp + 256 + rcx], 0
.mtf_enc_next:
    inc rbx
    cmp rbx, rbp
    jb .mtf_enc_loop
.mtf_enc_done:
    add rsp, 512
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; void bav_mtf_decode_asm(const uint8_t *in, uint8_t *out, size_t n)
bav_mtf_decode_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    sub rsp, 256
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8
    xor eax, eax
.mtf_dinit:
    mov byte [rsp + rax], al
    inc eax
    cmp eax, 256
    jb .mtf_dinit
    test rbp, rbp
    jz .mtf_dec_done
    xor rbx, rbx
.mtf_dec_loop:
    movzx edx, byte [rsi + rbx]  ; r
    movzx ecx, byte [rsp + rdx]  ; b = table[r]
    mov byte [rdi + rbx], cl
    test edx, edx
    jz .mtf_dec_next
.mtf_dshift:
    movzx eax, byte [rsp + rdx - 1]
    mov byte [rsp + rdx], al
    dec edx
    jnz .mtf_dshift
    mov byte [rsp], cl
.mtf_dec_next:
    inc rbx
    cmp rbx, rbp
    jb .mtf_dec_loop
.mtf_dec_done:
    add rsp, 256
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; size_t bav_rle0_encode_asm(const uint8_t *in, uint8_t *out, size_t n)
; returns encoded length in rax
bav_rle0_encode_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8                  ; n
    xor rbx, rbx                 ; i
    xor rax, rax                 ; out_len
    test rbp, rbp
    jz .rle_enc_done
.rle_enc_loop:
    cmp rbx, rbp
    jae .rle_enc_done
    movzx ecx, byte [rsi + rbx]
    test cl, cl
    jnz .rle_nonzero
    ; zeros
    mov r9, rbx
.rle_z:
    cmp r9, rbp
    jae .rle_z_end
    cmp byte [rsi + r9], 0
    jne .rle_z_end
    mov r10, r9
    sub r10, rbx
    cmp r10, 255
    jae .rle_z_end
    inc r9
    jmp .rle_z
.rle_z_end:
    mov r10, r9
    sub r10, rbx                 ; count
    mov byte [rdi + rax], 0
    inc rax
    mov byte [rdi + rax], r10b
    inc rax
    mov rbx, r9
    jmp .rle_enc_loop
.rle_nonzero:
    mov byte [rdi + rax], cl
    inc rax
    inc rbx
    jmp .rle_enc_loop
.rle_enc_done:
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; size_t bav_rle0_decode_asm(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
; returns decoded length or -1 on error
bav_rle0_decode_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    mov rsi, rcx                 ; in
    mov rbp, rdx                 ; in_len
    mov rdi, r8                  ; out
    mov r11, r9                  ; out_cap
    xor rbx, rbx                 ; i
    xor rax, rax                 ; out_len
.rle_dec_loop:
    cmp rbx, rbp
    jae .rle_dec_ok
    movzx ecx, byte [rsi + rbx]
    inc rbx
    test cl, cl
    jnz .rle_dec_nz
    cmp rbx, rbp
    jae .rle_dec_err
    movzx edx, byte [rsi + rbx]  ; count
    inc rbx
    ; write count zeros
.rle_w0:
    test edx, edx
    jz .rle_dec_loop
    cmp rax, r11
    jae .rle_dec_err
    mov byte [rdi + rax], 0
    inc rax
    dec edx
    jmp .rle_w0
.rle_dec_nz:
    cmp rax, r11
    jae .rle_dec_err
    mov byte [rdi + rax], cl
    inc rax
    jmp .rle_dec_loop
.rle_dec_ok:
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret
.rle_dec_err:
    mov rax, -1
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; void bav_transpose_asm(const uint8_t *in, uint8_t *out, size_t n, size_t width)
; rcx=in rdx=out r8=n r9=width
bav_transpose_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    push r12
    push r13
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8                  ; n
    mov r12, r9                  ; width
    ; if width<=1 or n < width*2: memcpy
    cmp r12, 1
    jbe .tr_copy
    mov rax, r12
    add rax, r12
    cmp rbp, rax
    jb .tr_copy
    ; nbody = n - n%width
    xor rdx, rdx
    mov rax, rbp
    div r12                      ; rax=rows-ish, rdx=rem — wait div r12: rax/r12
    ; restore: nbody = n - (n % width)
    mov rax, rbp
    xor rdx, rdx
    div r12                      ; rax = n/width, rdx = n%width
    mov r13, rbp
    sub r13, rdx                 ; nbody
    test r13, r13
    jz .tr_copy
    ; rows = nbody / width
    mov rax, r13
    xor rdx, rdx
    div r12
    mov r10, rax                 ; rows
    ; for col in 0..width-1
    xor r8, r8                   ; col
.tr_col:
    cmp r8, r12
    jae .tr_tail
    mov r9, r8
    imul r9, r10                 ; base = col * rows
    xor rbx, rbx                 ; row
.tr_row:
    cmp rbx, r10
    jae .tr_next_col
    ; out[base+row] = in[row*width+col]
    mov rax, rbx
    imul rax, r12
    add rax, r8
    movzx ecx, byte [rsi + rax]
    mov rax, r9
    add rax, rbx
    mov byte [rdi + rax], cl
    inc rbx
    jmp .tr_row
.tr_next_col:
    inc r8
    jmp .tr_col
.tr_tail:
    ; copy remainder
    mov rbx, r13
.tr_tloop:
    cmp rbx, rbp
    jae .tr_done
    movzx ecx, byte [rsi + rbx]
    mov byte [rdi + rbx], cl
    inc rbx
    jmp .tr_tloop
.tr_copy:
    xor rbx, rbx
.tr_cploop:
    cmp rbx, rbp
    jae .tr_done
    movzx ecx, byte [rsi + rbx]
    mov byte [rdi + rbx], cl
    inc rbx
    jmp .tr_cploop
.tr_done:
    pop r13
    pop r12
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; void bav_untranspose_asm(const uint8_t *in, uint8_t *out, size_t n, size_t width)
; same args — inverse of transpose
bav_untranspose_asm:
    push rbx
    push rsi
    push rdi
    push rbp
    push r12
    push r13
    mov rsi, rcx
    mov rdi, rdx
    mov rbp, r8
    mov r12, r9
    cmp r12, 1
    jbe .ut_copy
    mov rax, r12
    add rax, r12
    cmp rbp, rax
    jb .ut_copy
    mov rax, rbp
    xor rdx, rdx
    div r12
    mov r13, rbp
    sub r13, rdx                 ; nbody
    test r13, r13
    jz .ut_copy
    mov rax, r13
    xor rdx, rdx
    div r12
    mov r10, rax                 ; rows
    xor r8, r8                   ; col
.ut_col:
    cmp r8, r12
    jae .ut_tail
    mov r9, r8
    imul r9, r10                 ; base
    xor rbx, rbx
.ut_row:
    cmp rbx, r10
    jae .ut_nc
    ; out[row*width+col] = in[base+row]
    mov rax, r9
    add rax, rbx
    movzx ecx, byte [rsi + rax]
    mov rax, rbx
    imul rax, r12
    add rax, r8
    mov byte [rdi + rax], cl
    inc rbx
    jmp .ut_row
.ut_nc:
    inc r8
    jmp .ut_col
.ut_tail:
    mov rbx, r13
.ut_tloop:
    cmp rbx, rbp
    jae .ut_done
    movzx ecx, byte [rsi + rbx]
    mov byte [rdi + rbx], cl
    inc rbx
    jmp .ut_tloop
.ut_copy:
    xor rbx, rbx
.ut_cploop:
    cmp rbx, rbp
    jae .ut_done
    movzx ecx, byte [rsi + rbx]
    mov byte [rdi + rbx], cl
    inc rbx
    jmp .ut_cploop
.ut_done:
    pop r13
    pop r12
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret

; void bav_sub_delta_asm(const uint8_t *in, uint8_t *out, size_t n, size_t dist)
; rcx=in rdx=out r8=n r9=dist  — out[i]=in[i]-in[i-dist] for i from n-1 down to dist
bav_sub_delta_asm:
    push rbx
    ; copy first
    xor rbx, rbx
.sd_copy:
    cmp rbx, r8
    jae .sd_do
    movzx eax, byte [rcx + rbx]
    mov byte [rdx + rbx], al
    inc rbx
    jmp .sd_copy
.sd_do:
    test r9, r9
    jz .sd_done
    mov rbx, r8
.sd_loop:
    cmp rbx, r9
    jbe .sd_done
    dec rbx
    movzx eax, byte [rcx + rbx]
    mov r10, rbx
    sub r10, r9
    movzx r11d, byte [rcx + r10]
    sub al, r11b
    mov byte [rdx + rbx], al
    jmp .sd_loop
.sd_done:
    pop rbx
    ret

; void bav_sub_delta_inv_asm(uint8_t *buf, size_t n, size_t dist)  in-place
; rcx=buf rdx=n r8=dist
bav_sub_delta_inv_asm:
    push rbx
    test r8, r8
    jz .sdi_done
    mov rbx, r8
.sdi_loop:
    cmp rbx, rdx
    jae .sdi_done
    mov r9, rbx
    sub r9, r8
    movzx eax, byte [rcx + rbx]
    add al, byte [rcx + r9]
    mov byte [rcx + rbx], al
    inc rbx
    jmp .sdi_loop
.sdi_done:
    pop rbx
    ret

; void bav_xor_delta_asm(const uint8_t *in, uint8_t *out, size_t n, size_t dist)
bav_xor_delta_asm:
    push rbx
    xor rbx, rbx
.xd_copy:
    cmp rbx, r8
    jae .xd_do
    movzx eax, byte [rcx + rbx]
    mov byte [rdx + rbx], al
    inc rbx
    jmp .xd_copy
.xd_do:
    test r9, r9
    jz .xd_done
    mov rbx, r8
.xd_loop:
    cmp rbx, r9
    jbe .xd_done
    dec rbx
    movzx eax, byte [rcx + rbx]
    mov r10, rbx
    sub r10, r9
    xor al, byte [rcx + r10]
    mov byte [rdx + rbx], al
    jmp .xd_loop
.xd_done:
    pop rbx
    ret

; void bav_xor_delta_inv_asm(uint8_t *buf, size_t n, size_t dist)
bav_xor_delta_inv_asm:
    push rbx
    test r8, r8
    jz .xdi_done
    mov rbx, r8
.xdi_loop:
    cmp rbx, rdx
    jae .xdi_done
    mov r9, rbx
    sub r9, r8
    mov al, byte [rcx + rbx]
    xor al, byte [rcx + r9]
    mov byte [rcx + rbx], al
    inc rbx
    jmp .xdi_loop
.xdi_done:
    pop rbx
    ret
