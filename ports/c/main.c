#include "bav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s compress <in> [-o out] [-m auto|store|deflate|research]\n"
            "  %s decompress <in> [-o out]\n"
            "  %s version\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "version") == 0) {
        printf("bav-c 0.1.0%s\n",
#ifdef BAV_USE_NASM
               " (nasm kernels)"
#else
               ""
#endif
        );
        return 0;
    }
    if (strcmp(argv[1], "compress") == 0) {
        const char *in = NULL, *out = NULL, *mstr = "auto";
        bav_method_t method = BAV_METHOD_AUTO;
        size_t n = 0, flen = 0;
        uint8_t *data, *frame;
        char outbuf[4096];
        int i;
        for (i = 2; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
                out = argv[++i];
            else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--method") == 0) && i + 1 < argc)
                mstr = argv[++i];
            else if (!in)
                in = argv[i];
        }
        if (!in) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(mstr, "store") == 0)
            method = BAV_METHOD_STORE;
        else if (strcmp(mstr, "deflate") == 0)
            method = BAV_METHOD_DEFLATE;
        else if (strcmp(mstr, "research") == 0)
            method = BAV_METHOD_RESEARCH;
        else
            method = BAV_METHOD_AUTO;
        data = read_file(in, &n);
        if (!data && n) {
            fprintf(stderr, "read failed: %s\n", in);
            return 1;
        }
        if (!data) data = (uint8_t *)malloc(1);
        frame = bav_compress(data, n, method, &flen);
        free(data);
        if (!frame) {
            fprintf(stderr, "compress failed\n");
            return 1;
        }
        if (!out) {
            snprintf(outbuf, sizeof(outbuf), "%s.bav", in);
            out = outbuf;
        }
        if (write_file(out, frame, flen) != 0) {
            free(frame);
            fprintf(stderr, "write failed: %s\n", out);
            return 1;
        }
        printf("compressed %zu -> %zu bytes (%s)\n", n, flen, out);
        free(frame);
        return 0;
    }
    if (strcmp(argv[1], "decompress") == 0) {
        const char *in = NULL, *out = NULL;
        size_t n = 0, dlen = 0;
        uint8_t *frame, *data;
        char outbuf[4096];
        int i;
        for (i = 2; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
                out = argv[++i];
            else if (!in)
                in = argv[i];
        }
        if (!in) {
            usage(argv[0]);
            return 2;
        }
        frame = read_file(in, &n);
        if (!frame) {
            fprintf(stderr, "read failed: %s\n", in);
            return 1;
        }
        data = bav_decompress(frame, n, &dlen);
        free(frame);
        if (!data) {
            fprintf(stderr, "decompress failed\n");
            return 1;
        }
        if (!out) {
            size_t len = strlen(in);
            if (len > 4 && strcmp(in + len - 4, ".bav") == 0) {
                memcpy(outbuf, in, len - 4);
                outbuf[len - 4] = 0;
            } else {
                snprintf(outbuf, sizeof(outbuf), "%s.out", in);
            }
            out = outbuf;
        }
        if (write_file(out, data, dlen) != 0) {
            free(data);
            fprintf(stderr, "write failed: %s\n", out);
            return 1;
        }
        printf("decompressed %zu -> %zu bytes (%s)\n", n, dlen, out);
        free(data);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
