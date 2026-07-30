#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

uint8_t *bav_asm_compress(const uint8_t *data, size_t n, size_t *out_len);
uint8_t *bav_asm_decompress(const uint8_t *frame, size_t frame_len, size_t *out_len);

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return buf;
}

static int write_all(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(p, 1, n, f) != n) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "BAV ASM-first full research compressor\n"
                "  bav-asm compress <in> [-o out]\n"
                "  bav-asm decompress <in> [-o out]\n"
                "  bav-asm version\n");
        return 2;
    }
    if (strcmp(argv[1], "version") == 0) {
        printf("bav-asm 0.3.0 (NASM transforms + pure NASM lzma/zstd/brotli)\n");
        return 0;
    }
    if (strcmp(argv[1], "compress") == 0) {
        if (argc < 3) return 2;
        const char *in = argv[2];
        char outbuf[1024];
        const char *out = NULL;
        for (int i = 3; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
                out = argv[++i];
        }
        if (!out) {
            snprintf(outbuf, sizeof(outbuf), "%s.bav", in);
            out = outbuf;
        }
        size_t n = 0;
        uint8_t *data = read_all(in, &n);
        if (!data) { fprintf(stderr, "read fail\n"); return 1; }
        size_t fl = 0;
        uint8_t *frame = bav_asm_compress(data, n, &fl);
        free(data);
        if (!frame) { fprintf(stderr, "compress fail\n"); return 1; }
        if (write_all(out, frame, fl) != 0) { free(frame); return 1; }
        printf("compressed %zu -> %zu bytes (%s)\n", n, fl, out);
        free(frame);
        return 0;
    }
    if (strcmp(argv[1], "decompress") == 0) {
        if (argc < 3) return 2;
        const char *in = argv[2];
        char outbuf[1024];
        const char *out = NULL;
        for (int i = 3; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
                out = argv[++i];
        }
        if (!out) {
            size_t L = strlen(in);
            if (L > 4 && strcmp(in + L - 4, ".bav") == 0) {
                memcpy(outbuf, in, L - 4);
                outbuf[L - 4] = 0;
            } else {
                snprintf(outbuf, sizeof(outbuf), "%s.out", in);
            }
            out = outbuf;
        }
        size_t n = 0;
        uint8_t *frame = read_all(in, &n);
        if (!frame) return 1;
        size_t dn = 0;
        uint8_t *data = bav_asm_decompress(frame, n, &dn);
        free(frame);
        if (!data) { fprintf(stderr, "decompress fail\n"); return 1; }
        if (write_all(out, data, dn) != 0) { free(data); return 1; }
        printf("decompressed %zu -> %zu bytes (%s)\n", n, dn, out);
        free(data);
        return 0;
    }
    return 2;
}
