#ifndef BAV_H
#define BAV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAV_MAGIC "BAV1"
#define BAV_FORMAT_VERSION 1
#define BAV_HEADER_SIZE 19

#define BAV_M_STORE 0
#define BAV_M_DEFLATE 1
#define BAV_M_TRANSPOSE 4
#define BAV_M_PREFILTER 6
#define BAV_M_BWT 7
#define BAV_M_BLOCKS 8

#define BAV_F_MTF 1
#define BAV_F_RLE0 2
#define BAV_F_MTF_RLE0 3

#define BAV_BWT_F_MTF 1
#define BAV_BWT_F_RLE0 2

typedef enum {
    BAV_METHOD_AUTO = 0,
    BAV_METHOD_STORE = 1,
    BAV_METHOD_DEFLATE = 2,
    BAV_METHOD_RESEARCH = 3
} bav_method_t;

/* Compress data into a newly malloc'd BAV1 frame. *out_len set. Caller frees. */
uint8_t *bav_compress(const uint8_t *data, size_t n, bav_method_t method, size_t *out_len);

/* Decompress frame into newly malloc'd buffer. *out_len set. Caller frees. */
uint8_t *bav_decompress(const uint8_t *frame, size_t frame_len, size_t *out_len);

/* Hot kernels (NASM when BAV_USE_NASM=1, else C). */
uint32_t bav_crc32(const uint8_t *data, size_t n);
void bav_mtf_encode(const uint8_t *in, uint8_t *out, size_t n);
void bav_mtf_decode(const uint8_t *in, uint8_t *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
