#ifndef BAV_ASM_KERNELS_H
#define BAV_ASM_KERNELS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t bav_crc32_asm(const uint8_t *data, size_t len);
void bav_mtf_encode_asm(const uint8_t *in, uint8_t *out, size_t n);
void bav_mtf_decode_asm(const uint8_t *in, uint8_t *out, size_t n);
size_t bav_rle0_encode_asm(const uint8_t *in, uint8_t *out, size_t n);
size_t bav_rle0_decode_asm(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);
void bav_transpose_asm(const uint8_t *in, uint8_t *out, size_t n, size_t width);
void bav_untranspose_asm(const uint8_t *in, uint8_t *out, size_t n, size_t width);
void bav_sub_delta_asm(const uint8_t *in, uint8_t *out, size_t n, size_t dist);
void bav_sub_delta_inv_asm(uint8_t *buf, size_t n, size_t dist);
void bav_xor_delta_asm(const uint8_t *in, uint8_t *out, size_t n, size_t dist);
void bav_xor_delta_inv_asm(uint8_t *buf, size_t n, size_t dist);

#ifdef __cplusplus
}
#endif
#endif
