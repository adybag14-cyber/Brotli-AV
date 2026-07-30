/* Thin C glue: malloc workspace, call pure NASM codecs. No compressor libs. */
#include "pure_codecs.h"
#include <stdlib.h>
#include <string.h>

static int enc_common(
    size_t (*fn)(const uint8_t *, size_t, uint8_t *, size_t, uint32_t *, uint32_t *),
    const uint8_t *in, size_t n, uint8_t **out, size_t *out_len)
{
    size_t cap = n + n / 2 + 4096;
    if (cap < 64) cap = 64;
    uint8_t *buf = (uint8_t *)malloc(cap);
    uint32_t *hash = (uint32_t *)calloc(PURE_HASH_SIZE, sizeof(uint32_t));
    uint32_t *chain = (uint32_t *)calloc(n + 1, sizeof(uint32_t));
    if (!buf || !hash || !chain) {
        free(buf); free(hash); free(chain);
        return -1;
    }
    size_t got = fn(in, n, buf, cap, hash, chain);
    free(hash);
    free(chain);
    if (got == (size_t)-1) {
        /* grow once more for pathological expansion */
        free(buf);
        cap = n * 2 + 8192;
        buf = (uint8_t *)malloc(cap);
        hash = (uint32_t *)calloc(PURE_HASH_SIZE, sizeof(uint32_t));
        chain = (uint32_t *)calloc(n + 1, sizeof(uint32_t));
        if (!buf || !hash || !chain) {
            free(buf); free(hash); free(chain);
            return -1;
        }
        got = fn(in, n, buf, cap, hash, chain);
        free(hash);
        free(chain);
        if (got == (size_t)-1) {
            free(buf);
            return -1;
        }
    }
    *out = buf;
    *out_len = got;
    return 0;
}

static int dec_common(
    size_t (*fn)(const uint8_t *, size_t, uint8_t *, size_t),
    const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected)
{
    size_t cap = expected;
    if (cap == 0 && n >= 8) {
        cap = (size_t)in[4] | ((size_t)in[5] << 8) | ((size_t)in[6] << 16) | ((size_t)in[7] << 24);
    }
    if (cap == 0) cap = n * 4 + 64;
    if (cap > 256u * 1024u * 1024u) return -1;
    uint8_t *buf = (uint8_t *)malloc(cap ? cap : 1);
    if (!buf) return -1;
    size_t got = fn(in, n, buf, cap);
    if (got == (size_t)-1) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = got;
    return 0;
}

int pure_backend_lzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return enc_common(pure_lzma_encode_asm, in, n, out, out_len);
}
int pure_backend_unlzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return dec_common(pure_lzma_decode_asm, in, n, out, out_len, expected);
}
int pure_backend_zstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return enc_common(pure_zstd_encode_asm, in, n, out, out_len);
}
int pure_backend_unzstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return dec_common(pure_zstd_decode_asm, in, n, out, out_len, expected);
}
int pure_backend_brotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return enc_common(pure_brotli_encode_asm, in, n, out, out_len);
}
int pure_backend_unbrotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return dec_common(pure_brotli_decode_asm, in, n, out, out_len, expected);
}
