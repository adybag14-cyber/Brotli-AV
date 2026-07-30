/* BAV-asm backends: pure hand-written NASM lzma/zstd/brotli + zlib deflate. */
#include "backends.h"
#include "pure/pure_codecs.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint8_t *dup_bytes(const uint8_t *p, size_t n) {
    uint8_t *o = (uint8_t *)malloc(n ? n : 1);
    if (!o) return NULL;
    if (n) memcpy(o, p, n);
    return o;
}

int bav_backend_store(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    *out = dup_bytes(in, n);
    if (!*out) return -1;
    *out_len = n;
    return 0;
}

int bav_backend_deflate(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    uLongf bound = compressBound((uLong)n);
    uint8_t *buf = (uint8_t *)malloc(bound);
    if (!buf) return -1;
    uLongf dest = bound;
    if (compress2(buf, &dest, in, (uLong)n, Z_BEST_COMPRESSION) != Z_OK) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)dest;
    return 0;
}

int bav_backend_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    size_t cap = expected ? expected : (n * 4 + 64);
    if (cap < 64) cap = 64;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) return -1;
        uLongf dest = (uLongf)cap;
        int rc = uncompress(buf, &dest, in, (uLong)n);
        if (rc == Z_OK) {
            *out = buf;
            *out_len = (size_t)dest;
            return 0;
        }
        free(buf);
        if (rc != Z_BUF_ERROR) return -1;
        cap *= 2;
    }
    return -1;
}

int bav_backend_lzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return pure_backend_lzma(in, n, out, out_len);
}
int bav_backend_unlzma(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return pure_backend_unlzma(in, n, out, out_len, expected);
}

int bav_backend_zstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return pure_backend_zstd(in, n, out, out_len);
}
int bav_backend_unzstd(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return pure_backend_unzstd(in, n, out, out_len, expected);
}

int bav_backend_brotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    return pure_backend_brotli(in, n, out, out_len);
}
int bav_backend_unbrotli(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len, size_t expected) {
    return pure_backend_unbrotli(in, n, out, out_len, expected);
}
