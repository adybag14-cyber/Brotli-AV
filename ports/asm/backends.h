#ifndef BAV_ASM_BACKENDS_H
#define BAV_ASM_BACKENDS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bav_backend_store(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int bav_backend_deflate(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int bav_backend_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);
int bav_backend_lzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int bav_backend_unlzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);
int bav_backend_zstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int bav_backend_unzstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);
int bav_backend_brotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int bav_backend_unbrotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);

#ifdef __cplusplus
}
#endif
#endif
