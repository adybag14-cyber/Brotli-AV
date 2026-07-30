/* Round-trip smoke test for pure NASM codecs */
#include "pure_codecs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rt(const char *name,
              int (*enc)(const uint8_t *, size_t, uint8_t **, size_t *),
              int (*dec)(const uint8_t *, size_t, uint8_t **, size_t *, size_t),
              const uint8_t *data, size_t n)
{
    uint8_t *c = NULL, *d = NULL;
    size_t cl = 0, dl = 0;
    if (enc(data, n, &c, &cl) != 0) {
        printf("FAIL %s encode\n", name);
        return 1;
    }
    if (dec(c, cl, &d, &dl, n) != 0) {
        printf("FAIL %s decode (c=%zu)\n", name, cl);
        free(c);
        return 1;
    }
    int bad = (dl != n) || (n && memcmp(data, d, n) != 0);
    printf("%s: %zu -> %zu -> %zu %s\n", name, n, cl, dl, bad ? "MISMATCH" : "OK");
    free(c);
    free(d);
    return bad ? 1 : 0;
}

int main(void) {
    const char *samples[] = {
        "",
        "a",
        "hello hello hello hello",
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog.",
        NULL
    };
    int fails = 0;
    for (int i = 0; samples[i]; i++) {
        const uint8_t *p = (const uint8_t *)samples[i];
        size_t n = strlen(samples[i]);
        printf("--- sample %d (n=%zu) ---\n", i, n);
        fails += rt("lzma", pure_backend_lzma, pure_backend_unlzma, p, n);
        fails += rt("zstd", pure_backend_zstd, pure_backend_unzstd, p, n);
        fails += rt("brotli", pure_backend_brotli, pure_backend_unbrotli, p, n);
    }
    /* larger synthetic */
    size_t N = 4096;
    uint8_t *buf = (uint8_t *)malloc(N);
    for (size_t i = 0; i < N; i++) buf[i] = (uint8_t)("ABCDEFGHIJKLMNOP"[i % 16] ^ (i / 64));
    printf("--- synthetic %zu ---\n", N);
    fails += rt("lzma", pure_backend_lzma, pure_backend_unlzma, buf, N);
    fails += rt("zstd", pure_backend_zstd, pure_backend_unzstd, buf, N);
    fails += rt("brotli", pure_backend_brotli, pure_backend_unbrotli, buf, N);
    free(buf);

    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
