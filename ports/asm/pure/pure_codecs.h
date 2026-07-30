/* Pure hand-written compressors — no zlib/zstd/brotli/xz libraries.
 * Algorithms: LZMA-family range coding, Zstd-family FSE+sequences,
 * Brotli-family context prefix codes. All encode/decode in NASM. */
#ifndef BAV_PURE_CODECS_H
#define BAV_PURE_CODECS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Workspace sizes for encoders (allocated by glue). */
#define PURE_HASH_BITS 16
#define PURE_HASH_SIZE (1u << PURE_HASH_BITS)
#define PURE_WIN       (1u << 16)   /* 64 KiB sliding window */
#define PURE_MIN_MATCH 3
#define PURE_MAX_MATCH 258

/* LZMA-family (range-coded LZ77). Returns compressed size or (size_t)-1. */
size_t pure_lzma_encode_asm(const uint8_t *in, size_t n,
                            uint8_t *out, size_t out_cap,
                            uint32_t *hash, uint32_t *chain);
size_t pure_lzma_decode_asm(const uint8_t *in, size_t n,
                            uint8_t *out, size_t out_cap);

/* Zstd-family (sequences + FSE-ish ANS). */
size_t pure_zstd_encode_asm(const uint8_t *in, size_t n,
                            uint8_t *out, size_t out_cap,
                            uint32_t *hash, uint32_t *chain);
size_t pure_zstd_decode_asm(const uint8_t *in, size_t n,
                            uint8_t *out, size_t out_cap);

/* Brotli-family (context-modeled prefix / adaptive bits). */
size_t pure_brotli_encode_asm(const uint8_t *in, size_t n,
                              uint8_t *out, size_t out_cap,
                              uint32_t *hash, uint32_t *chain);
size_t pure_brotli_decode_asm(const uint8_t *in, size_t n,
                              uint8_t *out, size_t out_cap);

/* C glue used by BAV backends (malloc workspace, call ASM). */
int pure_backend_lzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int pure_backend_unlzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);
int pure_backend_zstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int pure_backend_unzstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);
int pure_backend_brotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len);
int pure_backend_unbrotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected);

#ifdef __cplusplus
}
#endif
#endif
