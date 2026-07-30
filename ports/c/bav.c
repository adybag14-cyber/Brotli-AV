/* BAV1 research codec — C port (zlib deflate + research transforms). */
#include "bav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef BAV_USE_NASM
uint32_t bav_crc32_nasm(const uint8_t *data, size_t len, uint32_t crc);
void bav_mtf_encode_nasm(const uint8_t *in, uint8_t *out, size_t n);
void bav_mtf_decode_nasm(const uint8_t *in, uint8_t *out, size_t n);
#endif

#define BWT_MAX_BYTES 120000

uint32_t bav_crc32(const uint8_t *data, size_t n) {
#ifdef BAV_USE_NASM
    return bav_crc32_nasm(data, n, 0);
#else
    return (uint32_t)crc32(0L, data, (uInt)n);
#endif
}

void bav_mtf_encode(const uint8_t *in, uint8_t *out, size_t n) {
#ifdef BAV_USE_NASM
    bav_mtf_encode_nasm(in, out, n);
#else
    uint8_t table[256];
    uint8_t pos[256];
    size_t i;
    int j;
    for (i = 0; i < 256; i++) {
        table[i] = (uint8_t)i;
        pos[i] = (uint8_t)i;
    }
    for (i = 0; i < n; i++) {
        uint8_t b = in[i];
        uint8_t r = pos[b];
        out[i] = r;
        if (r) {
            for (j = r; j > 0; j--) {
                uint8_t s = table[j - 1];
                table[j] = s;
                pos[s] = (uint8_t)j;
            }
            table[0] = b;
            pos[b] = 0;
        }
    }
#endif
}

void bav_mtf_decode(const uint8_t *in, uint8_t *out, size_t n) {
#ifdef BAV_USE_NASM
    bav_mtf_decode_nasm(in, out, n);
#else
    uint8_t table[256];
    size_t i;
    int j;
    for (i = 0; i < 256; i++) table[i] = (uint8_t)i;
    for (i = 0; i < n; i++) {
        uint8_t r = in[i];
        uint8_t b = table[r];
        out[i] = b;
        if (r) {
            for (j = r; j > 0; j--) table[j] = table[j - 1];
            table[0] = b;
        }
    }
#endif
}

static uint8_t *rle0_encode(const uint8_t *data, size_t n, size_t *out_n) {
    uint8_t *out = (uint8_t *)malloc(n * 2 + 8);
    size_t o = 0, i = 0;
    if (!out) return NULL;
    while (i < n) {
        if (data[i] == 0) {
            size_t j = i;
            while (j < n && data[j] == 0 && (j - i) < 255) j++;
            out[o++] = 0;
            out[o++] = (uint8_t)(j - i);
            i = j;
        } else {
            out[o++] = data[i++];
        }
    }
    *out_n = o;
    return out;
}

static uint8_t *rle0_decode(const uint8_t *data, size_t n, size_t *out_n) {
    size_t cap = n * 2 + 8, o = 0, i = 0;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    while (i < n) {
        if (data[i] == 0) {
            size_t count, k;
            if (i + 1 >= n) {
                free(out);
                return NULL;
            }
            count = data[i + 1];
            if (o + count > cap) {
                cap = (o + count) * 2;
                out = (uint8_t *)realloc(out, cap);
                if (!out) return NULL;
            }
            for (k = 0; k < count; k++) out[o++] = 0;
            i += 2;
        } else {
            if (o + 1 > cap) {
                cap *= 2;
                out = (uint8_t *)realloc(out, cap);
                if (!out) return NULL;
            }
            out[o++] = data[i++];
        }
    }
    *out_n = o;
    return out;
}

static uint8_t *transpose(const uint8_t *data, size_t n, size_t width, size_t *out_n) {
    size_t body_n, rows, col, row;
    uint8_t *out;
    if (width <= 1 || n < width * 2) {
        out = (uint8_t *)malloc(n ? n : 1);
        if (out && n) memcpy(out, data, n);
        *out_n = n;
        return out;
    }
    body_n = n - (n % width);
    if (body_n == 0) {
        out = (uint8_t *)malloc(n);
        memcpy(out, data, n);
        *out_n = n;
        return out;
    }
    rows = body_n / width;
    out = (uint8_t *)malloc(n);
    if (!out) return NULL;
    for (col = 0; col < width; col++) {
        size_t base = col * rows;
        for (row = 0; row < rows; row++) out[base + row] = data[row * width + col];
    }
    if (n > body_n) memcpy(out + body_n, data + body_n, n - body_n);
    *out_n = n;
    return out;
}

static uint8_t *untranspose(const uint8_t *data, size_t n, size_t width, size_t *out_n) {
    size_t body_n, rows, col, row;
    uint8_t *out;
    if (width <= 1 || n < width * 2) {
        out = (uint8_t *)malloc(n ? n : 1);
        if (out && n) memcpy(out, data, n);
        *out_n = n;
        return out;
    }
    body_n = n - (n % width);
    rows = body_n / width;
    out = (uint8_t *)malloc(n);
    if (!out) return NULL;
    for (col = 0; col < width; col++) {
        size_t base = col * rows;
        for (row = 0; row < rows; row++) out[row * width + col] = data[base + row];
    }
    if (n > body_n) memcpy(out + body_n, data + body_n, n - body_n);
    *out_n = n;
    return out;
}

static int bwt_encode(const uint8_t *data, size_t n, uint8_t *last, uint32_t *primary) {
    size_t *sa;
    uint8_t *s;
    size_t i;
    if (n == 0) {
        *primary = 0;
        return 0;
    }
    s = (uint8_t *)malloc(n * 2);
    sa = (size_t *)malloc(n * sizeof(size_t));
    if (!s || !sa) {
        free(s);
        free(sa);
        return -1;
    }
    memcpy(s, data, n);
    memcpy(s + n, data, n);
    for (i = 0; i < n; i++) sa[i] = i;
    /* insertion sort by rotation (fine for research sizes) */
    for (i = 1; i < n; i++) {
        size_t key = sa[i], j = i;
        while (j > 0 && memcmp(s + sa[j - 1], s + key, n) > 0) {
            sa[j] = sa[j - 1];
            j--;
        }
        sa[j] = key;
    }
    for (i = 0; i < n; i++) {
        last[i] = data[(sa[i] + n - 1) % n];
        if (sa[i] == 0) *primary = (uint32_t)i;
    }
    free(s);
    free(sa);
    return 0;
}

static uint8_t *bwt_decode(const uint8_t *last, size_t n, uint32_t primary, size_t *out_n) {
    size_t *order;
    uint8_t *out;
    size_t i, p;
    if (n == 0) {
        *out_n = 0;
        return (uint8_t *)malloc(1);
    }
    if (primary >= n) return NULL;
    order = (size_t *)malloc(n * sizeof(size_t));
    out = (uint8_t *)malloc(n);
    if (!order || !out) {
        free(order);
        free(out);
        return NULL;
    }
    for (i = 0; i < n; i++) order[i] = i;
    /* stable sort indices by last[i] */
    for (i = 1; i < n; i++) {
        size_t key = order[i], j = i;
        while (j > 0 && (last[order[j - 1]] > last[key] ||
                         (last[order[j - 1]] == last[key] && order[j - 1] > key))) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    p = primary;
    for (i = 0; i < n; i++) {
        p = order[p];
        out[i] = last[p];
    }
    free(order);
    *out_n = n;
    return out;
}

static uint8_t *deflate_enc(const uint8_t *data, size_t n, size_t *out_n) {
    uLongf dest_len = compressBound((uLong)n);
    uint8_t *out = (uint8_t *)malloc(dest_len);
    if (!out) return NULL;
    if (compress2(out, &dest_len, data, (uLong)n, 9) != Z_OK) {
        free(out);
        return NULL;
    }
    *out_n = (size_t)dest_len;
    return out;
}

static uint8_t *deflate_dec(const uint8_t *data, size_t n, size_t *out_n) {
    uLongf dest_len = n * 8 + 64;
    uint8_t *out = NULL;
    int rc;
    for (;;) {
        free(out);
        out = (uint8_t *)malloc(dest_len);
        if (!out) return NULL;
        rc = uncompress(out, &dest_len, data, (uLong)n);
        if (rc == Z_OK) {
            *out_n = (size_t)dest_len;
            return out;
        }
        if (rc != Z_BUF_ERROR) {
            free(out);
            return NULL;
        }
        dest_len *= 2;
    }
}

static uint8_t *backend_enc(uint8_t mid, const uint8_t *data, size_t n, size_t *out_n) {
    if (mid == BAV_M_STORE) {
        uint8_t *o = (uint8_t *)malloc(n ? n : 1);
        if (o && n) memcpy(o, data, n);
        *out_n = n;
        return o;
    }
    if (mid == BAV_M_DEFLATE) return deflate_enc(data, n, out_n);
    return NULL;
}

static uint8_t *backend_dec(uint8_t mid, const uint8_t *data, size_t n, size_t *out_n) {
    if (mid == BAV_M_STORE) {
        uint8_t *o = (uint8_t *)malloc(n ? n : 1);
        if (o && n) memcpy(o, data, n);
        *out_n = n;
        return o;
    }
    if (mid == BAV_M_DEFLATE) return deflate_dec(data, n, out_n);
    return NULL;
}

static void best_backend(const uint8_t *data, size_t n, uint8_t *mid, uint8_t **payload, size_t *plen) {
    size_t s_len = 0, d_len = 0;
    uint8_t *s = backend_enc(BAV_M_STORE, data, n, &s_len);
    uint8_t *d = backend_enc(BAV_M_DEFLATE, data, n, &d_len);
    if (d && (!s || d_len < s_len || (d_len == s_len && BAV_M_DEFLATE < BAV_M_STORE))) {
        free(s);
        *mid = BAV_M_DEFLATE;
        *payload = d;
        *plen = d_len;
    } else {
        free(d);
        *mid = BAV_M_STORE;
        *payload = s;
        *plen = s_len;
    }
}

static void pack_header(uint8_t *h, uint8_t method, uint64_t orig, uint32_t crc) {
    memcpy(h, BAV_MAGIC, 4);
    h[4] = BAV_FORMAT_VERSION;
    h[5] = method;
    h[6] = 0;
    memcpy(h + 7, &orig, 8);
    memcpy(h + 15, &crc, 4);
}

uint8_t *bav_compress(const uint8_t *data, size_t n, bav_method_t method, size_t *out_len) {
    uint32_t crc = bav_crc32(data, n);
    uint64_t orig = (uint64_t)n;
    uint8_t best_mid = BAV_M_STORE;
    uint8_t *best = NULL;
    size_t best_len = (size_t)-1;
    size_t i;

    typedef struct {
        uint8_t mid;
        uint8_t *p;
        size_t n;
    } cand_t;
    cand_t cands[64];
    size_t nc = 0;

    if (method == BAV_METHOD_STORE) {
        cands[nc].mid = BAV_M_STORE;
        cands[nc].p = backend_enc(BAV_M_STORE, data, n, &cands[nc].n);
        nc++;
    } else if (method == BAV_METHOD_DEFLATE) {
        cands[nc].mid = BAV_M_DEFLATE;
        cands[nc].p = backend_enc(BAV_M_DEFLATE, data, n, &cands[nc].n);
        nc++;
    } else {
        /* store + deflate */
        cands[nc].mid = BAV_M_STORE;
        cands[nc].p = backend_enc(BAV_M_STORE, data, n, &cands[nc].n);
        nc++;
        cands[nc].mid = BAV_M_DEFLATE;
        cands[nc].p = backend_enc(BAV_M_DEFLATE, data, n, &cands[nc].n);
        if (cands[nc].p) nc++;

        /* prefilter MTF / RLE0 */
        if (n >= 32) {
            uint8_t fids[] = {BAV_F_MTF, BAV_F_RLE0, BAV_F_MTF_RLE0};
            size_t fi;
            for (fi = 0; fi < 3 && nc < 60; fi++) {
                uint8_t *filtered = NULL;
                size_t flen = 0;
                uint8_t bmid;
                uint8_t *payload = NULL;
                size_t plen = 0;
                uint8_t *wrapped;
                if (fids[fi] == BAV_F_MTF) {
                    filtered = (uint8_t *)malloc(n);
                    if (!filtered) continue;
                    bav_mtf_encode(data, filtered, n);
                    flen = n;
                } else if (fids[fi] == BAV_F_RLE0) {
                    filtered = rle0_encode(data, n, &flen);
                } else {
                    uint8_t *tmp = (uint8_t *)malloc(n);
                    if (!tmp) continue;
                    bav_mtf_encode(data, tmp, n);
                    filtered = rle0_encode(tmp, n, &flen);
                    free(tmp);
                }
                if (!filtered) continue;
                if (flen > n * 2 + 64) {
                    free(filtered);
                    continue;
                }
                best_backend(filtered, flen, &bmid, &payload, &plen);
                free(filtered);
                if (!payload) continue;
                wrapped = (uint8_t *)malloc(2 + plen);
                if (!wrapped) {
                    free(payload);
                    continue;
                }
                wrapped[0] = fids[fi];
                wrapped[1] = bmid;
                memcpy(wrapped + 2, payload, plen);
                free(payload);
                cands[nc].mid = BAV_M_PREFILTER;
                cands[nc].p = wrapped;
                cands[nc].n = 2 + plen;
                nc++;
            }
        }

        /* transpose */
        {
            size_t widths[] = {2, 4, 8, 16};
            size_t wi;
            for (wi = 0; wi < 4 && nc < 60; wi++) {
                size_t w = widths[wi];
                size_t tn;
                uint8_t *t, bmid, *payload = NULL, *wrapped;
                size_t plen = 0;
                if (n < w * 4) continue;
                t = transpose(data, n, w, &tn);
                if (!t) continue;
                best_backend(t, tn, &bmid, &payload, &plen);
                free(t);
                if (!payload) continue;
                wrapped = (uint8_t *)malloc(2 + plen);
                if (!wrapped) {
                    free(payload);
                    continue;
                }
                wrapped[0] = (uint8_t)w;
                wrapped[1] = bmid;
                memcpy(wrapped + 2, payload, plen);
                free(payload);
                cands[nc].mid = BAV_M_TRANSPOSE;
                cands[nc].p = wrapped;
                cands[nc].n = 2 + plen;
                nc++;
            }
        }

        /* BWT for medium sizes */
        if (n >= 64 && n <= BWT_MAX_BYTES && nc < 60) {
            uint8_t *last = (uint8_t *)malloc(n);
            uint32_t primary = 0;
            if (last && bwt_encode(data, n, last, &primary) == 0) {
                uint8_t *mtf = (uint8_t *)malloc(n);
                if (mtf) {
                    uint8_t bmid, *payload = NULL, *wrapped;
                    size_t plen = 0;
                    bav_mtf_encode(last, mtf, n);
                    best_backend(mtf, n, &bmid, &payload, &plen);
                    free(mtf);
                    if (payload) {
                        wrapped = (uint8_t *)malloc(6 + plen);
                        if (wrapped) {
                            wrapped[0] = BAV_BWT_F_MTF;
                            memcpy(wrapped + 1, &primary, 4);
                            wrapped[5] = bmid;
                            memcpy(wrapped + 6, payload, plen);
                            free(payload);
                            cands[nc].mid = BAV_M_BWT;
                            cands[nc].p = wrapped;
                            cands[nc].n = 6 + plen;
                            nc++;
                        } else
                            free(payload);
                    }
                }
            }
            free(last);
        }
    }

    for (i = 0; i < nc; i++) {
        if (!cands[i].p) continue;
        if (cands[i].n < best_len || (cands[i].n == best_len && cands[i].mid < best_mid)) {
            best_len = cands[i].n;
            best_mid = cands[i].mid;
            best = cands[i].p;
        }
    }
    for (i = 0; i < nc; i++) {
        if (cands[i].p && cands[i].p != best) free(cands[i].p);
    }
    if (!best) {
        best = backend_enc(BAV_M_STORE, data, n, &best_len);
        best_mid = BAV_M_STORE;
    }
    {
        uint8_t *frame = (uint8_t *)malloc(BAV_HEADER_SIZE + best_len);
        if (!frame) {
            free(best);
            return NULL;
        }
        pack_header(frame, best_mid, orig, crc);
        memcpy(frame + BAV_HEADER_SIZE, best, best_len);
        free(best);
        *out_len = BAV_HEADER_SIZE + best_len;
        return frame;
    }
}

uint8_t *bav_decompress(const uint8_t *frame, size_t frame_len, size_t *out_len) {
    uint8_t mid;
    uint64_t orig;
    uint32_t crc, got;
    const uint8_t *payload;
    size_t plen;
    uint8_t *data = NULL;
    size_t dlen = 0;

    if (frame_len < BAV_HEADER_SIZE) return NULL;
    if (memcmp(frame, BAV_MAGIC, 4) != 0) return NULL;
    if (frame[4] != BAV_FORMAT_VERSION) return NULL;
    mid = frame[5];
    memcpy(&orig, frame + 7, 8);
    memcpy(&crc, frame + 15, 4);
    payload = frame + BAV_HEADER_SIZE;
    plen = frame_len - BAV_HEADER_SIZE;

    if (mid == BAV_M_TRANSPOSE) {
        size_t width, tn;
        uint8_t backend, *transformed, *u;
        if (plen < 2) return NULL;
        width = payload[0];
        backend = payload[1];
        transformed = backend_dec(backend, payload + 2, plen - 2, &tn);
        if (!transformed) return NULL;
        u = untranspose(transformed, tn, width, &dlen);
        free(transformed);
        data = u;
    } else if (mid == BAV_M_PREFILTER) {
        uint8_t fid, backend, *filtered, *undone = NULL;
        size_t fn;
        if (plen < 2) return NULL;
        fid = payload[0];
        backend = payload[1];
        filtered = backend_dec(backend, payload + 2, plen - 2, &fn);
        if (!filtered) return NULL;
        if (fid == BAV_F_MTF) {
            undone = (uint8_t *)malloc(fn);
            if (undone) {
                bav_mtf_decode(filtered, undone, fn);
                dlen = fn;
            }
        } else if (fid == BAV_F_RLE0) {
            undone = rle0_decode(filtered, fn, &dlen);
        } else if (fid == BAV_F_MTF_RLE0) {
            size_t rn;
            uint8_t *r = rle0_decode(filtered, fn, &rn);
            if (r) {
                undone = (uint8_t *)malloc(rn);
                if (undone) {
                    bav_mtf_decode(r, undone, rn);
                    dlen = rn;
                }
                free(r);
            }
        }
        free(filtered);
        data = undone;
    } else if (mid == BAV_M_BWT) {
        uint8_t flags, backend, *transformed, *tmp;
        uint32_t primary;
        size_t tn;
        if (plen < 6) return NULL;
        flags = payload[0];
        memcpy(&primary, payload + 1, 4);
        backend = payload[5];
        transformed = backend_dec(backend, payload + 6, plen - 6, &tn);
        if (!transformed) return NULL;
        if (flags & BAV_BWT_F_RLE0) {
            tmp = rle0_decode(transformed, tn, &tn);
            free(transformed);
            transformed = tmp;
            if (!transformed) return NULL;
        }
        if (flags & BAV_BWT_F_MTF) {
            tmp = (uint8_t *)malloc(tn);
            if (!tmp) {
                free(transformed);
                return NULL;
            }
            bav_mtf_decode(transformed, tmp, tn);
            free(transformed);
            transformed = tmp;
        }
        data = bwt_decode(transformed, tn, primary, &dlen);
        free(transformed);
    } else {
        data = backend_dec(mid, payload, plen, &dlen);
    }

    if (!data) return NULL;
    if ((uint64_t)dlen != orig) {
        free(data);
        return NULL;
    }
    got = bav_crc32(data, dlen);
    if (got != crc) {
        free(data);
        return NULL;
    }
    *out_len = dlen;
    return data;
}
