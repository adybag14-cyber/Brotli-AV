/* BAV full-research codec: orchestration in C, transforms in NASM, backends C libs. */
#include "kernels.h"
#include "backends.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "BAV1"
#define VER 2
#define VER_LEGACY 1
#define HV2 15
#define HV1 19

#define M_STORE 0
#define M_DEFLATE 1
#define M_LZMA 2
#define M_ZSTD 3
#define M_TRANSPOSE 4
#define M_BROTLI 5
#define M_PREFILTER 6
#define M_BWT 7
#define M_BLOCKS 8
#define M_TOKEN 9
#define M_XFORM 10
#define M_PARTS 11

#define F_MTF 1
#define F_RLE0 2
#define F_MTF_RLE0 3
#define F_SUB1 4
#define F_SUB4 5
#define F_XOR1 6
#define BWT_F_MTF 1
#define BWT_F_RLE0 2
#define BWT_F_SUB1 4
#define X_NONE 0
#define X_SUB1 1
#define X_SUB4 2
#define X_XOR1 3
#define BWT_MAX 120000

typedef struct { uint8_t mid; uint8_t *payload; size_t plen; } cand_t;

static void free_cands(cand_t *c, int n) {
    for (int i = 0; i < n; i++) free(c[i].payload);
}

static int backend_enc(uint8_t mid, const uint8_t *in, size_t n, uint8_t **out, size_t *olen) {
    switch (mid) {
    case M_STORE: return bav_backend_store(in, n, out, olen);
    case M_DEFLATE: return bav_backend_deflate(in, n, out, olen);
    case M_LZMA: return bav_backend_lzma(in, n, out, olen);
    case M_ZSTD: return bav_backend_zstd(in, n, out, olen);
    case M_BROTLI: return bav_backend_brotli(in, n, out, olen);
    default: return -1;
    }
}

static int backend_dec(uint8_t mid, const uint8_t *in, size_t n, uint8_t **out, size_t *olen, size_t exp) {
    switch (mid) {
    case M_STORE: return bav_backend_store(in, n, out, olen);
    case M_DEFLATE: return bav_backend_inflate(in, n, out, olen, exp);
    case M_LZMA: return bav_backend_unlzma(in, n, out, olen, exp);
    case M_ZSTD: return bav_backend_unzstd(in, n, out, olen, exp);
    case M_BROTLI: return bav_backend_unbrotli(in, n, out, olen, exp);
    default: return -1;
    }
}

static void best_backend(const uint8_t *in, size_t n, uint8_t *mid, uint8_t **out, size_t *olen) {
    static const uint8_t mids[] = {M_STORE, M_DEFLATE, M_LZMA, M_ZSTD, M_BROTLI};
    uint8_t best_mid = M_STORE;
    uint8_t *best = NULL;
    size_t best_len = (size_t)-1;
    for (size_t i = 0; i < sizeof(mids); i++) {
        uint8_t *p = NULL;
        size_t pl = 0;
        if (backend_enc(mids[i], in, n, &p, &pl) != 0) continue;
        if (pl < best_len || (pl == best_len && mids[i] < best_mid)) {
            free(best);
            best = p;
            best_len = pl;
            best_mid = mids[i];
        } else free(p);
    }
    if (!best) {
        bav_backend_store(in, n, &best, &best_len);
        best_mid = M_STORE;
    }
    *mid = best_mid;
    *out = best;
    *olen = best_len;
}

static uint8_t *apply_filter(const uint8_t *in, size_t n, uint8_t fid, size_t *out_n) {
    uint8_t *tmp = (uint8_t *)malloc(n ? n : 1);
    uint8_t *tmp2 = (uint8_t *)malloc(n * 2 + 64);
    if (!tmp || !tmp2) { free(tmp); free(tmp2); return NULL; }
    uint8_t *res = NULL;
    switch (fid) {
    case F_MTF:
        bav_mtf_encode_asm(in, tmp, n);
        res = tmp; *out_n = n; free(tmp2); break;
    case F_RLE0:
        *out_n = bav_rle0_encode_asm(in, tmp2, n);
        res = tmp2; free(tmp); break;
    case F_MTF_RLE0:
        bav_mtf_encode_asm(in, tmp, n);
        *out_n = bav_rle0_encode_asm(tmp, tmp2, n);
        res = tmp2; free(tmp); break;
    case F_SUB1:
        bav_sub_delta_asm(in, tmp, n, 1); res = tmp; *out_n = n; free(tmp2); break;
    case F_SUB4:
        bav_sub_delta_asm(in, tmp, n, 4); res = tmp; *out_n = n; free(tmp2); break;
    case F_XOR1:
        bav_xor_delta_asm(in, tmp, n, 1); res = tmp; *out_n = n; free(tmp2); break;
    default:
        free(tmp); free(tmp2); return NULL;
    }
    return res;
}

static int undo_filter(uint8_t *in, size_t n, uint8_t fid, uint8_t **out, size_t *out_n, size_t exp) {
    if (fid == F_MTF) {
        uint8_t *o = (uint8_t *)malloc(n ? n : 1);
        if (!o) return -1;
        bav_mtf_decode_asm(in, o, n);
        *out = o; *out_n = n; return 0;
    }
    if (fid == F_RLE0) {
        size_t cap = exp ? exp : n * 2 + 64;
        uint8_t *o = (uint8_t *)malloc(cap);
        if (!o) return -1;
        size_t got = bav_rle0_decode_asm(in, n, o, cap);
        if (got == (size_t)-1) { free(o); return -1; }
        *out = o; *out_n = got; return 0;
    }
    if (fid == F_MTF_RLE0) {
        size_t cap = exp ? exp : n * 2 + 64;
        uint8_t *mid = (uint8_t *)malloc(cap);
        if (!mid) return -1;
        size_t got = bav_rle0_decode_asm(in, n, mid, cap);
        if (got == (size_t)-1) { free(mid); return -1; }
        uint8_t *o = (uint8_t *)malloc(got ? got : 1);
        if (!o) { free(mid); return -1; }
        bav_mtf_decode_asm(mid, o, got);
        free(mid);
        *out = o; *out_n = got; return 0;
    }
    if (fid == F_SUB1 || fid == F_SUB4 || fid == F_XOR1) {
        uint8_t *o = (uint8_t *)malloc(n ? n : 1);
        if (!o) return -1;
        memcpy(o, in, n);
        size_t dist = (fid == F_SUB4) ? 4 : 1;
        if (fid == F_XOR1) bav_xor_delta_inv_asm(o, n, dist);
        else bav_sub_delta_inv_asm(o, n, dist);
        *out = o; *out_n = n; return 0;
    }
    return -1;
}

/* BWT sort in C; byte transforms remain NASM */
typedef struct { const uint8_t *s; size_t n; } bwt_ctx_t;

static int bwt_cmp(const void *a, const void *b, void *ctx) {
    const bwt_ctx_t *c = (const bwt_ctx_t *)ctx;
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    size_t n = c->n;
    for (size_t k = 0; k < n; k++) {
        uint8_t x = c->s[i + k], y = c->s[j + k];
        if (x != y) return (int)x - (int)y;
    }
    return 0;
}

#if defined(_WIN32)
/* MSVC/ mingw may lack qsort_s with context — use global */
static bwt_ctx_t g_bwt;
static int bwt_cmp_global(const void *a, const void *b) {
    return bwt_cmp(a, b, &g_bwt);
}
#endif

static int bwt_encode(const uint8_t *data, size_t n, uint8_t **last, uint32_t *primary) {
    if (n == 0) { *last = (uint8_t *)malloc(1); *primary = 0; return 0; }
    uint8_t *s = (uint8_t *)malloc(n * 2);
    size_t *sa = (size_t *)malloc(n * sizeof(size_t));
    uint8_t *L = (uint8_t *)malloc(n);
    if (!s || !sa || !L) { free(s); free(sa); free(L); return -1; }
    memcpy(s, data, n); memcpy(s + n, data, n);
    for (size_t i = 0; i < n; i++) sa[i] = i;
#if defined(_WIN32)
    g_bwt.s = s; g_bwt.n = n;
    qsort(sa, n, sizeof(size_t), bwt_cmp_global);
#else
    bwt_ctx_t ctx = {s, n};
    qsort_r(sa, n, sizeof(size_t), bwt_cmp, &ctx);
#endif
    *primary = 0;
    for (size_t i = 0; i < n; i++) {
        L[i] = data[(sa[i] + n - 1) % n];
        if (sa[i] == 0) *primary = (uint32_t)i;
    }
    free(s); free(sa);
    *last = L;
    return 0;
}

typedef struct { const uint8_t *last; } bwt_dec_ctx_t;
static bwt_dec_ctx_t g_bwt_dec;

static int bwt_dec_cmp(const void *a, const void *b) {
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    const uint8_t *last = g_bwt_dec.last;
    if (last[i] != last[j]) return (int)last[i] - (int)last[j];
    return (i < j) ? -1 : (i > j); /* stable by index */
}

static int bwt_decode(const uint8_t *last, size_t n, uint32_t primary, uint8_t **out) {
    if (n == 0) { *out = (uint8_t *)malloc(1); return 0; }
    if (primary >= n) return -1;
    size_t *order = (size_t *)malloc(n * sizeof(size_t));
    uint8_t *o = (uint8_t *)malloc(n);
    if (!order || !o) { free(order); free(o); return -1; }
    for (size_t i = 0; i < n; i++) order[i] = i;
    g_bwt_dec.last = last;
    qsort(order, n, sizeof(size_t), bwt_dec_cmp);
    size_t p = primary;
    for (size_t i = 0; i < n; i++) {
        p = order[p];
        o[i] = last[p];
    }
    free(order);
    *out = o;
    return 0;
}

static void add_cand(cand_t *cands, int *nc, int cap, uint8_t mid, uint8_t *payload, size_t plen) {
    if (*nc >= cap) { free(payload); return; }
    cands[*nc].mid = mid;
    cands[*nc].payload = payload;
    cands[*nc].plen = plen;
    (*nc)++;
}

static int cand_cmp(const void *a, const void *b) {
    const cand_t *x = (const cand_t *)a, *y = (const cand_t *)b;
    if (x->plen < y->plen) return -1;
    if (x->plen > y->plen) return 1;
    return (int)x->mid - (int)y->mid;
}

uint8_t *bav_asm_compress(const uint8_t *data, size_t n, size_t *out_len) {
    uint32_t crc = bav_crc32_asm(data, n);
    cand_t cands[256];
    int nc = 0;
    const int cap = 256;

    /* backends */
    {
        static const uint8_t mids[] = {M_STORE, M_DEFLATE, M_ZSTD, M_BROTLI};
        for (size_t i = 0; i < sizeof(mids); i++) {
            uint8_t *p = NULL; size_t pl = 0;
            if (backend_enc(mids[i], data, n, &p, &pl) == 0)
                add_cand(cands, &nc, cap, mids[i], p, pl);
        }
    }
    /* transpose */
    {
        int widths[] = {2, 3, 4, 5, 6, 8, 12, 16};
        for (int wi = 0; wi < 8; wi++) {
            int w = widths[wi];
            if (n < (size_t)w * 4) continue;
            uint8_t *tr = (uint8_t *)malloc(n);
            if (!tr) continue;
            bav_transpose_asm(data, tr, n, (size_t)w);
            uint8_t mid; uint8_t *pay = NULL; size_t plen = 0;
            best_backend(tr, n, &mid, &pay, &plen);
            free(tr);
            uint8_t *wrapped = (uint8_t *)malloc(2 + plen);
            if (!wrapped) { free(pay); continue; }
            wrapped[0] = (uint8_t)w; wrapped[1] = mid;
            memcpy(wrapped + 2, pay, plen); free(pay);
            add_cand(cands, &nc, cap, M_TRANSPOSE, wrapped, 2 + plen);
        }
    }
    /* xform */
    {
        int widths[] = {2, 4, 8, 12, 16};
        uint8_t xids[] = {X_NONE, X_SUB1, X_SUB4, X_XOR1};
        for (int wi = 0; wi < 5; wi++) {
            int w = widths[wi];
            if (n < (size_t)w * 4) continue;
            uint8_t *base = (uint8_t *)malloc(n);
            if (!base) continue;
            bav_transpose_asm(data, base, n, (size_t)w);
            for (int xi = 0; xi < 4; xi++) {
                uint8_t *tr = (uint8_t *)malloc(n);
                if (!tr) continue;
                uint8_t xid = xids[xi];
                if (xid == X_NONE) memcpy(tr, base, n);
                else if (xid == X_SUB1) bav_sub_delta_asm(base, tr, n, 1);
                else if (xid == X_SUB4) bav_sub_delta_asm(base, tr, n, 4);
                else bav_xor_delta_asm(base, tr, n, 1);
                uint8_t mid; uint8_t *pay = NULL; size_t plen = 0;
                best_backend(tr, n, &mid, &pay, &plen);
                free(tr);
                uint8_t *wrapped = (uint8_t *)malloc(3 + plen);
                if (!wrapped) { free(pay); continue; }
                wrapped[0] = (uint8_t)w; wrapped[1] = xid; wrapped[2] = mid;
                memcpy(wrapped + 3, pay, plen); free(pay);
                add_cand(cands, &nc, cap, M_XFORM, wrapped, 3 + plen);
            }
            free(base);
        }
    }
    /* prefilter */
    if (n >= 32) {
        uint8_t fids[] = {F_MTF, F_RLE0, F_MTF_RLE0, F_SUB1, F_SUB4, F_XOR1};
        for (int fi = 0; fi < 6; fi++) {
            size_t fn = 0;
            uint8_t *filt = apply_filter(data, n, fids[fi], &fn);
            if (!filt) continue;
            if (fn > n * 2 + 64) { free(filt); continue; }
            uint8_t mid; uint8_t *pay = NULL; size_t plen = 0;
            best_backend(filt, fn, &mid, &pay, &plen);
            free(filt);
            uint8_t *wrapped = (uint8_t *)malloc(2 + plen);
            if (!wrapped) { free(pay); continue; }
            wrapped[0] = fids[fi]; wrapped[1] = mid;
            memcpy(wrapped + 2, pay, plen); free(pay);
            add_cand(cands, &nc, cap, M_PREFILTER, wrapped, 2 + plen);
        }
    }
    /* BWT */
    if (n >= 64 && n <= BWT_MAX) {
        uint8_t *last = NULL; uint32_t primary = 0;
        if (bwt_encode(data, n, &last, &primary) == 0) {
            uint8_t *mtf = (uint8_t *)malloc(n);
            bav_mtf_encode_asm(last, mtf, n);
            struct { uint8_t flags; uint8_t *buf; size_t len; int own; } vars[5];
            vars[0].flags = 0; vars[0].buf = last; vars[0].len = n; vars[0].own = 0;
            vars[1].flags = BWT_F_MTF; vars[1].buf = mtf; vars[1].len = n; vars[1].own = 0;
            {
                uint8_t *r = (uint8_t *)malloc(n * 2 + 8);
                size_t rl = bav_rle0_encode_asm(mtf, r, n);
                vars[2].flags = (uint8_t)(BWT_F_MTF | BWT_F_RLE0);
                vars[2].buf = r; vars[2].len = rl; vars[2].own = 1;
            }
            {
                uint8_t *s = (uint8_t *)malloc(n);
                bav_sub_delta_asm(mtf, s, n, 1);
                vars[3].flags = (uint8_t)(BWT_F_MTF | BWT_F_SUB1);
                vars[3].buf = s; vars[3].len = n; vars[3].own = 1;
            }
            {
                uint8_t *s = (uint8_t *)malloc(n);
                bav_sub_delta_asm(mtf, s, n, 1);
                uint8_t *r = (uint8_t *)malloc(n * 2 + 8);
                size_t rl = bav_rle0_encode_asm(s, r, n);
                free(s);
                vars[4].flags = (uint8_t)(BWT_F_MTF | BWT_F_SUB1 | BWT_F_RLE0);
                vars[4].buf = r; vars[4].len = rl; vars[4].own = 1;
            }
            for (int vi = 0; vi < 5; vi++) {
                uint8_t mid; uint8_t *pay = NULL; size_t plen = 0;
                best_backend(vars[vi].buf, vars[vi].len, &mid, &pay, &plen);
                uint8_t *wrapped = (uint8_t *)malloc(6 + plen);
                if (wrapped) {
                    wrapped[0] = vars[vi].flags;
                    memcpy(wrapped + 1, &primary, 4);
                    wrapped[5] = mid;
                    memcpy(wrapped + 6, pay, plen);
                    add_cand(cands, &nc, cap, M_BWT, wrapped, 6 + plen);
                }
                free(pay);
                if (vars[vi].own) free(vars[vi].buf);
            }
            free(mtf); free(last);
        }
    }
    /* multi-block */
    if (n >= 1024) {
        int sizes[] = {1024, 2048, 3072, 4096, 6144, 8192, 16384};
        for (int si = 0; si < 7; si++) {
            size_t bs = (size_t)sizes[si];
            if (n < bs) continue;
            size_t capb = n + 64 + (n / bs + 1) * 16;
            uint8_t *buf = (uint8_t *)malloc(capb);
            if (!buf) continue;
            size_t pos = 0;
            buf[pos++] = (uint8_t)(bs & 0xff);
            buf[pos++] = (uint8_t)((bs >> 8) & 0xff);
            int ok = 1;
            for (size_t i = 0; i < n && ok; i += bs) {
                size_t cl = n - i < bs ? n - i : bs;
                uint8_t mid; uint8_t *pay = NULL; size_t plen = 0;
                best_backend(data + i, cl, &mid, &pay, &plen);
                if (pos + 5 + plen > capb) {
                    capb = (pos + 5 + plen) * 2;
                    uint8_t *nb = (uint8_t *)realloc(buf, capb);
                    if (!nb) { free(pay); ok = 0; break; }
                    buf = nb;
                }
                buf[pos++] = mid;
                memcpy(buf + pos, &plen, 4); pos += 4; /* LE on little-endian hosts */
                memcpy(buf + pos, pay, plen); pos += plen;
                free(pay);
            }
            if (ok) add_cand(cands, &nc, cap, M_BLOCKS, buf, pos);
            else free(buf);
        }
    }

    if (nc == 0) {
        uint8_t *p = NULL; size_t pl = 0;
        bav_backend_store(data, n, &p, &pl);
        add_cand(cands, &nc, cap, M_STORE, p, pl);
    }
    qsort(cands, (size_t)nc, sizeof(cand_t), cand_cmp);
    uint8_t mid = cands[0].mid;
    uint8_t *payload = cands[0].payload;
    size_t plen = cands[0].plen;
    /* free losers */
    for (int i = 1; i < nc; i++) free(cands[i].payload);

    size_t hsz = (n > 0xFFFFFFFFu) ? HV1 : HV2;
    uint8_t *frame = (uint8_t *)malloc(hsz + plen);
    if (!frame) { free(payload); return NULL; }
    memcpy(frame, MAGIC, 4);
    if (hsz == HV2) {
        frame[4] = VER;
        frame[5] = mid;
        frame[6] = 0;
        uint32_t orig = (uint32_t)n;
        memcpy(frame + 7, &orig, 4);
        memcpy(frame + 11, &crc, 4);
    } else {
        frame[4] = VER_LEGACY;
        frame[5] = mid;
        frame[6] = 0;
        uint64_t orig = (uint64_t)n;
        memcpy(frame + 7, &orig, 8);
        memcpy(frame + 15, &crc, 4);
    }
    memcpy(frame + hsz, payload, plen);
    free(payload);
    *out_len = hsz + plen;
    return frame;
}

static int decode_payload(uint8_t mid, const uint8_t *payload, size_t plen, size_t expected, uint8_t **out, size_t *out_n);

static int decode_payload(uint8_t mid, const uint8_t *payload, size_t plen, size_t expected, uint8_t **out, size_t *out_n) {
    if (mid == M_STORE || mid == M_DEFLATE || mid == M_ZSTD || mid == M_BROTLI)
        return backend_dec(mid, payload, plen, out, out_n, expected);
    if (mid == M_TRANSPOSE) {
        if (plen < 2) return -1;
        uint8_t w = payload[0], b = payload[1];
        uint8_t *tr = NULL; size_t trn = 0;
        if (backend_dec(b, payload + 2, plen - 2, &tr, &trn, expected) != 0) return -1;
        uint8_t *o = (uint8_t *)malloc(trn ? trn : 1);
        bav_untranspose_asm(tr, o, trn, w);
        free(tr);
        *out = o; *out_n = trn; return 0;
    }
    if (mid == M_XFORM) {
        if (plen < 3) return -1;
        uint8_t w = payload[0], xid = payload[1], b = payload[2];
        uint8_t *tr = NULL; size_t trn = 0;
        if (backend_dec(b, payload + 3, plen - 3, &tr, &trn, expected) != 0) return -1;
        if (xid == X_SUB1) bav_sub_delta_inv_asm(tr, trn, 1);
        else if (xid == X_SUB4) bav_sub_delta_inv_asm(tr, trn, 4);
        else if (xid == X_XOR1) bav_xor_delta_inv_asm(tr, trn, 1);
        uint8_t *o = (uint8_t *)malloc(trn ? trn : 1);
        bav_untranspose_asm(tr, o, trn, w);
        free(tr);
        *out = o; *out_n = trn; return 0;
    }
    if (mid == M_PREFILTER) {
        if (plen < 2) return -1;
        uint8_t fid = payload[0], b = payload[1];
        uint8_t *filt = NULL; size_t fn = 0;
        if (backend_dec(b, payload + 2, plen - 2, &filt, &fn, 0) != 0) return -1;
        int rc = undo_filter(filt, fn, fid, out, out_n, expected);
        free(filt);
        return rc;
    }
    if (mid == M_BWT) {
        if (plen < 6) return -1;
        uint8_t flags = payload[0];
        uint32_t primary;
        memcpy(&primary, payload + 1, 4);
        uint8_t b = payload[5];
        uint8_t *tr = NULL; size_t trn = 0;
        /* BWT last-column length equals original size when no RLE; pass expected. */
        if (backend_dec(b, payload + 6, plen - 6, &tr, &trn, expected) != 0) return -1;
        if (flags & BWT_F_RLE0) {
            size_t cap = expected ? expected : trn * 2 + 64;
            uint8_t *r = (uint8_t *)malloc(cap);
            size_t got = bav_rle0_decode_asm(tr, trn, r, cap);
            free(tr);
            if (got == (size_t)-1) { free(r); return -1; }
            tr = r; trn = got;
        }
        if (flags & BWT_F_SUB1) bav_sub_delta_inv_asm(tr, trn, 1);
        if (flags & BWT_F_MTF) {
            uint8_t *m = (uint8_t *)malloc(trn ? trn : 1);
            bav_mtf_decode_asm(tr, m, trn);
            free(tr); tr = m;
        }
        uint8_t *o = NULL;
        if (bwt_decode(tr, trn, primary, &o) != 0) { free(tr); return -1; }
        free(tr);
        *out = o; *out_n = trn; return 0;
    }
    if (mid == M_BLOCKS) {
        if (plen < 2) return -1;
        size_t pos = 2;
        size_t cap = expected ? expected : 1024;
        uint8_t *o = (uint8_t *)malloc(cap);
        size_t on = 0;
        if (!o) return -1;
        while (pos < plen) {
            if (pos + 5 > plen) { free(o); return -1; }
            uint8_t bmid = payload[pos];
            uint32_t chunk_len;
            memcpy(&chunk_len, payload + pos + 1, 4);
            pos += 5;
            if (pos + chunk_len > plen) { free(o); return -1; }
            uint8_t *chunk = NULL; size_t cn = 0;
            if (decode_payload(bmid, payload + pos, chunk_len, 0, &chunk, &cn) != 0) {
                free(o); return -1;
            }
            pos += chunk_len;
            if (on + cn > cap) {
                cap = (on + cn) * 2;
                uint8_t *nb = (uint8_t *)realloc(o, cap);
                if (!nb) { free(chunk); free(o); return -1; }
                o = nb;
            }
            memcpy(o + on, chunk, cn); on += cn; free(chunk);
        }
        *out = o; *out_n = on; return 0;
    }
    return -1;
}

uint8_t *bav_asm_decompress(const uint8_t *frame, size_t frame_len, size_t *out_len) {
    if (frame_len < 8 || memcmp(frame, MAGIC, 4) != 0) return NULL;
    uint8_t ver = frame[4], mid;
    uint64_t orig;
    uint32_t crc;
    const uint8_t *payload;
    size_t plen;
    if (ver == VER) {
        if (frame_len < HV2) return NULL;
        mid = frame[5];
        uint32_t o32; memcpy(&o32, frame + 7, 4); orig = o32;
        memcpy(&crc, frame + 11, 4);
        payload = frame + HV2; plen = frame_len - HV2;
    } else if (ver == VER_LEGACY) {
        if (frame_len < HV1) return NULL;
        mid = frame[5];
        memcpy(&orig, frame + 7, 8);
        memcpy(&crc, frame + 15, 4);
        payload = frame + HV1; plen = frame_len - HV1;
    } else return NULL;
    uint8_t *data = NULL; size_t dn = 0;
    if (decode_payload(mid, payload, plen, (size_t)orig, &data, &dn) != 0) return NULL;
    if (dn != orig || bav_crc32_asm(data, dn) != crc) { free(data); return NULL; }
    *out_len = dn;
    return data;
}
