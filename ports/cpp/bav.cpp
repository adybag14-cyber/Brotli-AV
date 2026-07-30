/* Full research BAV codec — C++ backup (parity with ports/csharp + src/bav). */
#include "bav.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <zlib.h>
#include <lzma.h>
#include <zstd.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

namespace bav {
namespace {

constexpr uint8_t MAGIC[4] = {'B', 'A', 'V', '1'};
constexpr uint8_t FORMAT_VERSION = 2;
constexpr uint8_t FORMAT_VERSION_LEGACY = 1;
constexpr size_t HEADER_V2 = 15;
constexpr size_t HEADER_V1 = 19;

constexpr uint8_t M_STORE = 0, M_DEFLATE = 1, M_LZMA = 2, M_ZSTD = 3, M_TRANSPOSE = 4;
constexpr uint8_t M_BROTLI = 5, M_PREFILTER = 6, M_BWT = 7, M_BLOCKS = 8, M_TOKEN = 9;
constexpr uint8_t M_XFORM = 10, M_PARTS = 11;

constexpr uint8_t F_MTF = 1, F_RLE0 = 2, F_MTF_RLE0 = 3, F_SUB1 = 4, F_SUB4 = 5, F_XOR1 = 6;
constexpr uint8_t F_SUB2 = 7, F_SUB3 = 8, F_SUB5 = 9, F_SUB6 = 10, F_SUB8 = 11, F_SUB12 = 12;
constexpr uint8_t F_SUB16 = 13, F_XOR4 = 14;

constexpr uint8_t BWT_F_MTF = 1, BWT_F_RLE0 = 2, BWT_F_SUB1 = 4;
constexpr uint8_t X_NONE = 0, X_SUB1 = 1, X_SUB4 = 2, X_XOR1 = 3;
constexpr size_t BWT_MAX = 120000;

struct Cand {
    uint8_t mid;
    std::vector<uint8_t> p;
};

static void put_u16(std::vector<uint8_t> &o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
static void put_u32(std::vector<uint8_t> &o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}
static uint16_t get_u16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static std::vector<uint8_t> make_header(uint8_t method, uint64_t orig, uint32_t crc) {
    if (orig > 0xFFFFFFFFull) {
        std::vector<uint8_t> h(HEADER_V1);
        std::memcpy(h.data(), MAGIC, 4);
        h[4] = FORMAT_VERSION_LEGACY;
        h[5] = method;
        h[6] = 0;
        std::memcpy(h.data() + 7, &orig, 8);
        std::memcpy(h.data() + 15, &crc, 4);
        return h;
    }
    std::vector<uint8_t> h;
    h.reserve(HEADER_V2);
    h.insert(h.end(), MAGIC, MAGIC + 4);
    h.push_back(FORMAT_VERSION);
    h.push_back(method);
    h.push_back(0);
    put_u32(h, static_cast<uint32_t>(orig));
    put_u32(h, crc);
    return h;
}

// ---- backends ----

static std::vector<uint8_t> store_enc(const std::vector<uint8_t> &d) { return d; }

static std::vector<uint8_t> zlib_enc(const std::vector<uint8_t> &data) {
    uLongf dest = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> out(dest ? dest : 1);
    if (compress2(out.data(), &dest, data.data(), static_cast<uLong>(data.size()), 9) != Z_OK)
        throw std::runtime_error("zlib compress failed");
    out.resize(dest);
    return out;
}

static std::vector<uint8_t> zlib_dec(const std::vector<uint8_t> &data) {
    uLongf dest = data.size() * 8 + 64;
    if (dest < 64) dest = 64;
    for (int i = 0; i < 12; i++) {
        std::vector<uint8_t> out(dest);
        int rc = uncompress(out.data(), &dest, data.data(), static_cast<uLong>(data.size()));
        if (rc == Z_OK) {
            out.resize(dest);
            return out;
        }
        if (rc != Z_BUF_ERROR) throw std::runtime_error("zlib decompress failed");
        dest *= 2;
    }
    throw std::runtime_error("zlib decompress failed");
}

static std::vector<uint8_t> xz_encode_filters(const std::vector<uint8_t> &data,
                                              lzma_filter *filters) {
    size_t bound = lzma_stream_buffer_bound(data.size());
    if (bound == 0) bound = data.size() + data.size() / 3 + 128;
    std::vector<uint8_t> out(bound);
    size_t out_pos = 0;
    lzma_ret ret = lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, nullptr, data.data(),
                                             data.size(), out.data(), &out_pos, out.size());
    if (ret != LZMA_OK) throw std::runtime_error("lzma encode failed");
    out.resize(out_pos);
    return out;
}

static std::vector<uint8_t> lzma_enc(const std::vector<uint8_t> &data) {
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 9 | LZMA_PRESET_EXTREME))
        throw std::runtime_error("lzma preset failed");

    lzma_filter filters_plain[2] = {
        {LZMA_FILTER_LZMA2, &opt},
        {LZMA_VLI_UNKNOWN, nullptr},
    };
    std::vector<uint8_t> best = xz_encode_filters(data, filters_plain);

    // Delta + LZMA2 distance search (Python parity)
    static const uint32_t dists[] = {1, 2, 3, 4, 5, 6, 8, 12, 16};
    for (uint32_t dist : dists) {
        lzma_options_delta dopt;
        dopt.type = LZMA_DELTA_TYPE_BYTE;
        dopt.dist = dist;
        lzma_options_lzma opt2;
        if (lzma_lzma_preset(&opt2, 9 | LZMA_PRESET_EXTREME)) continue;
        lzma_filter filters[3] = {
            {LZMA_FILTER_DELTA, &dopt},
            {LZMA_FILTER_LZMA2, &opt2},
            {LZMA_VLI_UNKNOWN, nullptr},
        };
        try {
            auto cand = xz_encode_filters(data, filters);
            if (cand.size() < best.size()) best = std::move(cand);
        } catch (...) {
        }
    }
    return best;
}

static std::vector<uint8_t> lzma_dec(const std::vector<uint8_t> &data) {
    uint64_t memlimit = 256ull * 1024 * 1024;
    size_t dest = data.size() * 4 + 64;
    if (dest < 64) dest = 64;
    for (int attempt = 0; attempt < 10; attempt++) {
        std::vector<uint8_t> out(dest);
        size_t in_pos = 0, out_pos = 0;
        lzma_ret ret =
            lzma_stream_buffer_decode(&memlimit, 0, nullptr, data.data(), &in_pos, data.size(),
                                      out.data(), &out_pos, out.size());
        if (ret == LZMA_OK) {
            out.resize(out_pos);
            return out;
        }
        if (ret != LZMA_BUF_ERROR) throw std::runtime_error("lzma decompress failed");
        dest *= 2;
    }
    throw std::runtime_error("lzma decompress failed");
}

static std::vector<uint8_t> zstd_enc(const std::vector<uint8_t> &data) {
    size_t bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> out(bound ? bound : 1);
    size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 22);
    if (ZSTD_isError(n)) throw std::runtime_error(ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

static std::vector<uint8_t> zstd_dec(const std::vector<uint8_t> &data) {
    unsigned long long cap = ZSTD_getFrameContentSize(data.data(), data.size());
    if (cap == ZSTD_CONTENTSIZE_ERROR || cap == ZSTD_CONTENTSIZE_UNKNOWN)
        cap = data.size() * 4 + 64;
    std::vector<uint8_t> out(static_cast<size_t>(cap) ? static_cast<size_t>(cap) : 1);
    size_t n = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(n)) throw std::runtime_error(ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

static std::vector<uint8_t> brotli_enc(const std::vector<uint8_t> &data) {
    auto once = [&](BrotliEncoderMode mode) {
        size_t bound = BrotliEncoderMaxCompressedSize(data.size());
        if (bound == 0) bound = data.size() + data.size() / 4 + 64;
        std::vector<uint8_t> out(bound);
        size_t dest = bound;
        if (!BrotliEncoderCompress(11, BROTLI_DEFAULT_WINDOW, mode, data.size(), data.data(),
                                   &dest, out.data()))
            throw std::runtime_error("brotli compress failed");
        out.resize(dest);
        return out;
    };
    auto best = once(BROTLI_MODE_GENERIC);
    try {
        auto t = once(BROTLI_MODE_TEXT);
        if (t.size() < best.size()) best = std::move(t);
    } catch (...) {
    }
    try {
        auto f = once(BROTLI_MODE_FONT);
        if (f.size() < best.size()) best = std::move(f);
    } catch (...) {
    }
    return best;
}

static std::vector<uint8_t> brotli_dec(const std::vector<uint8_t> &data) {
    size_t cap = data.size() * 16 + 1024;
    if (cap < 65536) cap = 65536;
    for (int i = 0; i < 12; i++) {
        std::vector<uint8_t> out(cap);
        size_t dest = cap;
        BrotliDecoderResult r =
            BrotliDecoderDecompress(data.size(), data.data(), &dest, out.data());
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            out.resize(dest);
            return out;
        }
        cap *= 2;
    }
    throw std::runtime_error("brotli decompress failed");
}

static std::vector<uint8_t> backend_enc(uint8_t mid, const std::vector<uint8_t> &data) {
    switch (mid) {
    case M_STORE: return store_enc(data);
    case M_DEFLATE: return zlib_enc(data);
    case M_LZMA: return lzma_enc(data);
    case M_ZSTD: return zstd_enc(data);
    case M_BROTLI: return brotli_enc(data);
    default: throw std::runtime_error("encode backend");
    }
}

static std::vector<uint8_t> backend_dec(uint8_t mid, const std::vector<uint8_t> &data) {
    switch (mid) {
    case M_STORE: return store_enc(data);
    case M_DEFLATE: return zlib_dec(data);
    case M_LZMA: return lzma_dec(data);
    case M_ZSTD: return zstd_dec(data);
    case M_BROTLI: return brotli_dec(data);
    default: throw std::runtime_error("decode backend");
    }
}

static std::vector<Cand> try_backends(const std::vector<uint8_t> &data) {
    std::vector<Cand> out;
    for (uint8_t mid : {M_STORE, M_DEFLATE, M_LZMA, M_ZSTD, M_BROTLI}) {
        try {
            out.push_back({mid, backend_enc(mid, data)});
        } catch (...) {
        }
    }
    return out;
}

static Cand best_backend(const std::vector<uint8_t> &data) {
    auto c = try_backends(data);
    if (c.empty()) return {M_STORE, data};
    std::sort(c.begin(), c.end(), [](const Cand &a, const Cand &b) {
        if (a.p.size() != b.p.size()) return a.p.size() < b.p.size();
        return a.mid < b.mid;
    });
    return std::move(c[0]);
}

// ---- transforms ----

static std::vector<uint8_t> transpose(const std::vector<uint8_t> &data, size_t width) {
    if (width <= 1 || data.size() < width * 2) return data;
    size_t n = data.size() - (data.size() % width);
    if (n == 0) return data;
    size_t rows = n / width;
    std::vector<uint8_t> out(data.size());
    for (size_t col = 0; col < width; col++) {
        size_t base = col * rows;
        for (size_t row = 0; row < rows; row++) out[base + row] = data[row * width + col];
    }
    if (data.size() > n) std::copy(data.begin() + static_cast<long>(n), data.end(),
                                   out.begin() + static_cast<long>(n));
    return out;
}

static std::vector<uint8_t> untranspose(const std::vector<uint8_t> &data, size_t width) {
    if (width <= 1 || data.size() < width * 2) return data;
    size_t n = data.size() - (data.size() % width);
    size_t rows = n / width;
    std::vector<uint8_t> out(data.size());
    for (size_t col = 0; col < width; col++) {
        size_t base = col * rows;
        for (size_t row = 0; row < rows; row++) out[row * width + col] = data[base + row];
    }
    if (data.size() > n) std::copy(data.begin() + static_cast<long>(n), data.end(),
                                   out.begin() + static_cast<long>(n));
    return out;
}

static std::vector<uint8_t> rle0_enc(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < data.size()) {
        if (data[i] == 0) {
            size_t j = i;
            while (j < data.size() && data[j] == 0 && (j - i) < 255) j++;
            out.push_back(0);
            out.push_back(static_cast<uint8_t>(j - i));
            i = j;
        } else
            out.push_back(data[i++]);
    }
    return out;
}

static std::vector<uint8_t> rle0_dec(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < data.size();) {
        if (data[i] == 0) {
            if (i + 1 >= data.size()) throw std::runtime_error("truncated RLE0");
            out.insert(out.end(), data[i + 1], 0);
            i += 2;
        } else
            out.push_back(data[i++]);
    }
    return out;
}

static std::vector<uint8_t> sub_delta(const std::vector<uint8_t> &data, int dist) {
    if (dist < 1 || data.empty()) return data;
    std::vector<uint8_t> out = data;
    for (int i = static_cast<int>(data.size()) - 1; i >= dist; i--)
        out[static_cast<size_t>(i)] =
            static_cast<uint8_t>(data[static_cast<size_t>(i)] - data[static_cast<size_t>(i - dist)]);
    return out;
}

static std::vector<uint8_t> sub_delta_inv(const std::vector<uint8_t> &data, int dist) {
    if (dist < 1 || data.empty()) return data;
    std::vector<uint8_t> out = data;
    for (size_t i = static_cast<size_t>(dist); i < data.size(); i++)
        out[i] = static_cast<uint8_t>(out[i] + out[i - static_cast<size_t>(dist)]);
    return out;
}

static std::vector<uint8_t> xor_delta(const std::vector<uint8_t> &data, int dist) {
    if (dist < 1 || data.empty()) return data;
    std::vector<uint8_t> out = data;
    for (int i = static_cast<int>(data.size()) - 1; i >= dist; i--)
        out[static_cast<size_t>(i)] =
            static_cast<uint8_t>(data[static_cast<size_t>(i)] ^ data[static_cast<size_t>(i - dist)]);
    return out;
}

static std::vector<uint8_t> xor_delta_inv(const std::vector<uint8_t> &data, int dist) {
    if (dist < 1 || data.empty()) return data;
    std::vector<uint8_t> out = data;
    for (size_t i = static_cast<size_t>(dist); i < data.size(); i++)
        out[i] = static_cast<uint8_t>(out[i] ^ out[i - static_cast<size_t>(dist)]);
    return out;
}

static int filter_sub_dist(uint8_t fid) {
    switch (fid) {
    case F_SUB1: return 1;
    case F_SUB2: return 2;
    case F_SUB3: return 3;
    case F_SUB4: return 4;
    case F_SUB5: return 5;
    case F_SUB6: return 6;
    case F_SUB8: return 8;
    case F_SUB12: return 12;
    case F_SUB16: return 16;
    default: return 0;
    }
}

} // namespace

// public mtf (may use NASM)
uint32_t crc32(const std::vector<uint8_t> &data) {
    return static_cast<uint32_t>(::crc32(0L, data.data(), static_cast<uInt>(data.size())));
}

std::vector<uint8_t> mtf_encode(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out(data.size());
    uint8_t table[256], pos[256];
    for (int i = 0; i < 256; i++) {
        table[i] = static_cast<uint8_t>(i);
        pos[i] = static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t b = data[i];
        uint8_t r = pos[b];
        out[i] = r;
        if (r) {
            for (int j = r; j > 0; j--) {
                uint8_t s = table[j - 1];
                table[j] = s;
                pos[s] = static_cast<uint8_t>(j);
            }
            table[0] = b;
            pos[b] = 0;
        }
    }
    return out;
}

std::vector<uint8_t> mtf_decode(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out(data.size());
    uint8_t table[256];
    for (int i = 0; i < 256; i++) table[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t r = data[i];
        uint8_t b = table[r];
        out[i] = b;
        if (r) {
            for (int j = r; j > 0; j--) table[j] = table[j - 1];
            table[0] = b;
        }
    }
    return out;
}

namespace {

static std::vector<uint8_t> apply_filter(const std::vector<uint8_t> &data, uint8_t fid) {
    int d = filter_sub_dist(fid);
    if (d > 0) return sub_delta(data, d);
    switch (fid) {
    case F_MTF: return mtf_encode(data);
    case F_RLE0: return rle0_enc(data);
    case F_MTF_RLE0: return rle0_enc(mtf_encode(data));
    case F_XOR1: return xor_delta(data, 1);
    case F_XOR4: return xor_delta(data, 4);
    default: throw std::runtime_error("unknown filter");
    }
}

static std::vector<uint8_t> undo_filter(const std::vector<uint8_t> &data, uint8_t fid) {
    int d = filter_sub_dist(fid);
    if (d > 0) return sub_delta_inv(data, d);
    switch (fid) {
    case F_MTF: return mtf_decode(data);
    case F_RLE0: return rle0_dec(data);
    case F_MTF_RLE0: return mtf_decode(rle0_dec(data));
    case F_XOR1: return xor_delta_inv(data, 1);
    case F_XOR4: return xor_delta_inv(data, 4);
    default: throw std::runtime_error("unknown filter");
    }
}

static std::vector<uint8_t> apply_xform(const std::vector<uint8_t> &data, uint8_t xid) {
    switch (xid) {
    case X_NONE: return data;
    case X_SUB1: return sub_delta(data, 1);
    case X_SUB4: return sub_delta(data, 4);
    case X_XOR1: return xor_delta(data, 1);
    default: throw std::runtime_error("unknown xform");
    }
}

static std::vector<uint8_t> undo_xform(const std::vector<uint8_t> &data, uint8_t xid) {
    switch (xid) {
    case X_NONE: return data;
    case X_SUB1: return sub_delta_inv(data, 1);
    case X_SUB4: return sub_delta_inv(data, 4);
    case X_XOR1: return xor_delta_inv(data, 1);
    default: throw std::runtime_error("unknown xform");
    }
}

static std::pair<std::vector<uint8_t>, uint32_t> bwt_enc(const std::vector<uint8_t> &data) {
    size_t n = data.size();
    if (n == 0) return {{}, 0};
    std::vector<uint8_t> s(n * 2);
    std::copy(data.begin(), data.end(), s.begin());
    std::copy(data.begin(), data.end(), s.begin() + static_cast<long>(n));
    std::vector<size_t> sa(n);
    for (size_t i = 0; i < n; i++) sa[i] = i;
    std::sort(sa.begin(), sa.end(), [&](size_t a, size_t b) {
        return std::lexicographical_compare(s.begin() + static_cast<long>(a),
                                            s.begin() + static_cast<long>(a + n),
                                            s.begin() + static_cast<long>(b),
                                            s.begin() + static_cast<long>(b + n));
    });
    std::vector<uint8_t> last(n);
    uint32_t primary = 0;
    for (size_t i = 0; i < n; i++) {
        last[i] = data[(sa[i] + n - 1) % n];
        if (sa[i] == 0) primary = static_cast<uint32_t>(i);
    }
    return {last, primary};
}

static std::vector<uint8_t> bwt_dec(const std::vector<uint8_t> &last, uint32_t primary) {
    size_t n = last.size();
    if (n == 0) return {};
    if (primary >= n) throw std::runtime_error("BWT primary OOR");
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return last[a] < last[b]; });
    std::vector<uint8_t> out(n);
    size_t p = primary;
    for (size_t i = 0; i < n; i++) {
        p = order[p];
        out[i] = last[p];
    }
    return out;
}

static bool is_word(uint8_t b) {
    return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '_';
}

static std::vector<std::vector<uint8_t>> token_split(const std::vector<uint8_t> &data) {
    std::vector<std::vector<uint8_t>> tokens;
    size_t i = 0;
    while (i < data.size()) {
        if (is_word(data[i])) {
            size_t j = i + 1;
            while (j < data.size() && is_word(data[j])) j++;
            tokens.emplace_back(data.begin() + static_cast<long>(i),
                                data.begin() + static_cast<long>(j));
            i = j;
        } else {
            tokens.push_back({data[i]});
            i++;
        }
    }
    return tokens;
}

static std::vector<uint8_t> varint_enc(const std::vector<uint32_t> &values) {
    std::vector<uint8_t> out;
    for (uint32_t v : values) {
        while (v >= 0x80) {
            out.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

static std::vector<uint32_t> varint_dec(const std::vector<uint8_t> &data) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < data.size()) {
        int shift = 0;
        uint32_t val = 0;
        for (;;) {
            if (i >= data.size()) throw std::runtime_error("truncated varint");
            uint8_t b = data[i++];
            val |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (b < 0x80) break;
            shift += 7;
            if (shift > 70) throw std::runtime_error("varint too long");
        }
        out.push_back(val);
    }
    return out;
}

static std::vector<uint8_t> token_encode_payload(const std::vector<uint8_t> &data) {
    auto tokens = token_split(data);
    std::map<std::string, int> counts, first;
    std::map<std::string, std::vector<uint8_t>> key_of;
    for (size_t i = 0; i < tokens.size(); i++) {
        std::string k(tokens[i].begin(), tokens[i].end());
        key_of[k] = tokens[i];
        counts[k]++;
        if (!first.count(k)) first[k] = static_cast<int>(i);
    }
    std::vector<std::string> uniq;
    for (auto &kv : counts) uniq.push_back(kv.first);
    std::sort(uniq.begin(), uniq.end(), [&](const std::string &a, const std::string &b) {
        if (counts[a] != counts[b]) return counts[a] > counts[b];
        return first[a] < first[b];
    });
    std::map<std::string, uint32_t> id_map;
    for (uint32_t i = 0; i < uniq.size(); i++) id_map[uniq[i]] = i;
    std::vector<uint32_t> ids;
    ids.reserve(tokens.size());
    for (auto &t : tokens) {
        std::string k(t.begin(), t.end());
        ids.push_back(id_map[k]);
    }
    std::vector<uint8_t> dict;
    for (auto &k : uniq) {
        auto &t = key_of[k];
        if (t.size() > 0xFFFF) throw std::runtime_error("token too long");
        put_u16(dict, static_cast<uint16_t>(t.size()));
        dict.insert(dict.end(), t.begin(), t.end());
    }
    auto d = best_backend(dict);
    auto icomp = best_backend(varint_enc(ids));
    std::vector<uint8_t> out;
    out.push_back(d.mid);
    out.push_back(icomp.mid);
    put_u32(out, static_cast<uint32_t>(d.p.size()));
    put_u32(out, static_cast<uint32_t>(icomp.p.size()));
    out.insert(out.end(), d.p.begin(), d.p.end());
    out.insert(out.end(), icomp.p.begin(), icomp.p.end());
    return out;
}

static std::vector<uint8_t> token_decode_payload(const std::vector<uint8_t> &payload) {
    if (payload.size() < 10) throw std::runtime_error("token short");
    uint8_t dmid = payload[0], imid = payload[1];
    uint32_t dlen = get_u32(payload.data() + 2);
    uint32_t ilen = get_u32(payload.data() + 6);
    size_t pos = 10;
    if (pos + dlen + ilen > payload.size()) throw std::runtime_error("token trunc");
    auto dict_blob =
        backend_dec(dmid, std::vector<uint8_t>(payload.begin() + static_cast<long>(pos),
                                               payload.begin() + static_cast<long>(pos + dlen)));
    auto id_blob = backend_dec(
        imid, std::vector<uint8_t>(payload.begin() + static_cast<long>(pos + dlen),
                                   payload.begin() + static_cast<long>(pos + dlen + ilen)));
    std::vector<std::vector<uint8_t>> uniq;
    size_t p = 0;
    while (p < dict_blob.size()) {
        if (p + 2 > dict_blob.size()) throw std::runtime_error("bad token dict");
        uint16_t ln = get_u16(dict_blob.data() + p);
        p += 2;
        if (p + ln > dict_blob.size()) throw std::runtime_error("bad token entry");
        uniq.emplace_back(dict_blob.begin() + static_cast<long>(p),
                          dict_blob.begin() + static_cast<long>(p + ln));
        p += ln;
    }
    auto ids = varint_dec(id_blob);
    std::vector<uint8_t> out;
    for (uint32_t tid : ids) {
        if (tid >= uniq.size()) throw std::runtime_error("token id OOR");
        out.insert(out.end(), uniq[tid].begin(), uniq[tid].end());
    }
    return out;
}

// ---- research families ----

static const uint8_t ALL_PREFILTERS[] = {F_MTF,   F_RLE0,  F_MTF_RLE0, F_SUB1,  F_SUB2, F_SUB3,
                                         F_SUB4,  F_SUB5,  F_SUB6,     F_SUB8,  F_SUB12, F_SUB16,
                                         F_XOR1,  F_XOR4};

static std::vector<Cand> research_transpose(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    for (size_t w : {2u, 3u, 4u, 5u, 6u, 8u, 12u, 16u}) {
        if (data.size() < w * 4) continue;
        auto b = best_backend(transpose(data, w));
        std::vector<uint8_t> wrapped;
        wrapped.push_back(static_cast<uint8_t>(w));
        wrapped.push_back(b.mid);
        wrapped.insert(wrapped.end(), b.p.begin(), b.p.end());
        r.push_back({M_TRANSPOSE, std::move(wrapped)});
    }
    return r;
}

static std::vector<Cand> research_xform(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    for (size_t w : {2u, 4u, 8u, 12u, 16u}) {
        if (data.size() < w * 4) continue;
        auto base = transpose(data, w);
        for (uint8_t xid : {X_NONE, X_SUB1, X_SUB4, X_XOR1}) {
            auto tr = apply_xform(base, xid);
            auto b = best_backend(tr);
            std::vector<uint8_t> wrapped;
            wrapped.push_back(static_cast<uint8_t>(w));
            wrapped.push_back(xid);
            wrapped.push_back(b.mid);
            wrapped.insert(wrapped.end(), b.p.begin(), b.p.end());
            r.push_back({M_XFORM, std::move(wrapped)});
        }
    }
    return r;
}

static std::vector<Cand> research_prefilter(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    if (data.size() < 32) return r;
    for (uint8_t fid : ALL_PREFILTERS) {
        try {
            auto filtered = apply_filter(data, fid);
            if (filtered.size() > data.size() * 2 + 64) continue;
            auto b = best_backend(filtered);
            std::vector<uint8_t> wrapped;
            wrapped.push_back(fid);
            wrapped.push_back(b.mid);
            wrapped.insert(wrapped.end(), b.p.begin(), b.p.end());
            r.push_back({M_PREFILTER, std::move(wrapped)});
        } catch (...) {
        }
    }
    return r;
}

static std::vector<Cand> research_bwt(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    size_t n = data.size();
    if (n < 64 || n > BWT_MAX) return r;
    auto [last, primary] = bwt_enc(data);
    auto mtf = mtf_encode(last);
    struct V {
        uint8_t flags;
        std::vector<uint8_t> tr;
    };
    std::vector<V> variants = {
        {0, last},
        {BWT_F_MTF, mtf},
        {static_cast<uint8_t>(BWT_F_MTF | BWT_F_RLE0), rle0_enc(mtf)},
        {static_cast<uint8_t>(BWT_F_MTF | BWT_F_SUB1), sub_delta(mtf, 1)},
        {static_cast<uint8_t>(BWT_F_MTF | BWT_F_SUB1 | BWT_F_RLE0), rle0_enc(sub_delta(mtf, 1))},
    };
    for (auto &v : variants) {
        auto b = best_backend(v.tr);
        std::vector<uint8_t> wrapped;
        wrapped.push_back(v.flags);
        put_u32(wrapped, primary);
        wrapped.push_back(b.mid);
        wrapped.insert(wrapped.end(), b.p.begin(), b.p.end());
        r.push_back({M_BWT, std::move(wrapped)});
    }
    return r;
}

static std::vector<Cand> research_blocks(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    size_t n = data.size();
    if (n < 1024) return r;
    for (size_t bs : {1024u, 2048u, 3072u, 4096u, 6144u, 8192u, 16384u}) {
        if (n < bs) continue;
        std::vector<uint8_t> out;
        put_u16(out, static_cast<uint16_t>(bs));
        bool ok = true;
        for (size_t i = 0; i < n; i += bs) {
            size_t len = std::min(bs, n - i);
            std::vector<uint8_t> chunk(data.begin() + static_cast<long>(i),
                                       data.begin() + static_cast<long>(i + len));
            auto b = best_backend(chunk);
            if (b.p.size() > 0xFFFFFFFFull) {
                ok = false;
                break;
            }
            out.push_back(b.mid);
            put_u32(out, static_cast<uint32_t>(b.p.size()));
            out.insert(out.end(), b.p.begin(), b.p.end());
        }
        if (ok) r.push_back({M_BLOCKS, std::move(out)});
    }
    return r;
}

static std::vector<Cand> research_parts(const std::vector<uint8_t> &data) {
    std::vector<Cand> r;
    size_t n = data.size();
    if (n < 1024) return r;
    for (int nparts : {2, 3, 4}) {
        std::vector<uint8_t> out;
        put_u16(out, static_cast<uint16_t>(nparts));
        for (int i = 0; i < nparts; i++) {
            size_t a = static_cast<size_t>(i) * n / static_cast<size_t>(nparts);
            size_t b = static_cast<size_t>(i + 1) * n / static_cast<size_t>(nparts);
            std::vector<uint8_t> chunk(data.begin() + static_cast<long>(a),
                                       data.begin() + static_cast<long>(b));
            auto best = best_backend(chunk);
            uint8_t mid = best.mid;
            auto payload = best.p;
            for (size_t w : {4u, 8u, 12u}) {
                if (chunk.size() < w * 4) continue;
                auto base = transpose(chunk, w);
                for (uint8_t xid : {X_NONE, X_SUB1, X_SUB4}) {
                    auto tr = apply_xform(base, xid);
                    auto m2 = best_backend(tr);
                    std::vector<uint8_t> xp;
                    xp.push_back(static_cast<uint8_t>(w));
                    xp.push_back(xid);
                    xp.push_back(m2.mid);
                    xp.insert(xp.end(), m2.p.begin(), m2.p.end());
                    if (xp.size() < payload.size()) {
                        mid = M_XFORM;
                        payload = std::move(xp);
                    }
                }
            }
            for (uint8_t fid : ALL_PREFILTERS) {
                try {
                    auto f = apply_filter(chunk, fid);
                    if (f.size() > chunk.size() * 2 + 64) continue;
                    auto m2 = best_backend(f);
                    std::vector<uint8_t> pref;
                    pref.push_back(fid);
                    pref.push_back(m2.mid);
                    pref.insert(pref.end(), m2.p.begin(), m2.p.end());
                    if (pref.size() < payload.size()) {
                        mid = M_PREFILTER;
                        payload = std::move(pref);
                    }
                } catch (...) {
                }
            }
            out.push_back(mid);
            put_u32(out, static_cast<uint32_t>(payload.size()));
            out.insert(out.end(), payload.begin(), payload.end());
        }
        r.push_back({M_PARTS, std::move(out)});
    }
    return r;
}

static std::vector<Cand> research_token(const std::vector<uint8_t> &data) {
    if (data.size() < 64) return {};
    size_t printable = 0;
    for (uint8_t b : data)
        if ((b >= 32 && b < 127) || b == 9 || b == 10 || b == 13) printable++;
    if (printable < data.size() * 2 / 5) return {};
    try {
        return {{M_TOKEN, token_encode_payload(data)}};
    } catch (...) {
        return {};
    }
}

static std::vector<uint8_t> decode_method_payload(uint8_t mid, const std::vector<uint8_t> &payload);

static std::vector<uint8_t> decode_bwt(const std::vector<uint8_t> &payload) {
    if (payload.size() < 6) throw std::runtime_error("BWT short");
    uint8_t flags = payload[0];
    uint32_t primary = get_u32(payload.data() + 1);
    uint8_t backend = payload[5];
    auto transformed =
        backend_dec(backend, std::vector<uint8_t>(payload.begin() + 6, payload.end()));
    if (flags & BWT_F_RLE0) transformed = rle0_dec(transformed);
    if (flags & BWT_F_SUB1) transformed = sub_delta_inv(transformed, 1);
    if (flags & BWT_F_MTF) transformed = mtf_decode(transformed);
    return bwt_dec(transformed, primary);
}

static std::vector<uint8_t> decode_blocks(const std::vector<uint8_t> &payload) {
    if (payload.size() < 2) throw std::runtime_error("blocks short");
    uint16_t bs = get_u16(payload.data());
    if (bs == 0) throw std::runtime_error("bad block size");
    size_t pos = 2;
    std::vector<uint8_t> out;
    while (pos < payload.size()) {
        if (pos + 5 > payload.size()) throw std::runtime_error("trunc block");
        uint8_t bmid = payload[pos];
        uint32_t plen = get_u32(payload.data() + pos + 1);
        pos += 5;
        if (pos + plen > payload.size()) throw std::runtime_error("trunc block payload");
        auto chunk = decode_method_payload(
            bmid, std::vector<uint8_t>(payload.begin() + static_cast<long>(pos),
                                       payload.begin() + static_cast<long>(pos + plen)));
        out.insert(out.end(), chunk.begin(), chunk.end());
        pos += plen;
    }
    return out;
}

static std::vector<uint8_t> decode_parts(const std::vector<uint8_t> &payload) {
    if (payload.size() < 2) throw std::runtime_error("parts short");
    int nparts = get_u16(payload.data());
    if (nparts == 0) throw std::runtime_error("bad nparts");
    size_t pos = 2;
    std::vector<uint8_t> out;
    for (int i = 0; i < nparts; i++) {
        if (pos + 5 > payload.size()) throw std::runtime_error("trunc part");
        uint8_t pmid = payload[pos];
        uint32_t plen = get_u32(payload.data() + pos + 1);
        pos += 5;
        if (pos + plen > payload.size()) throw std::runtime_error("trunc part payload");
        auto chunk = decode_method_payload(
            pmid, std::vector<uint8_t>(payload.begin() + static_cast<long>(pos),
                                       payload.begin() + static_cast<long>(pos + plen)));
        out.insert(out.end(), chunk.begin(), chunk.end());
        pos += plen;
    }
    return out;
}

static std::vector<uint8_t> decode_method_payload(uint8_t mid, const std::vector<uint8_t> &payload) {
    switch (mid) {
    case M_TRANSPOSE: {
        if (payload.size() < 2) throw std::runtime_error("transpose short");
        auto t = backend_dec(payload[1], std::vector<uint8_t>(payload.begin() + 2, payload.end()));
        return untranspose(t, payload[0]);
    }
    case M_XFORM: {
        if (payload.size() < 3) throw std::runtime_error("xform short");
        auto t = backend_dec(payload[2], std::vector<uint8_t>(payload.begin() + 3, payload.end()));
        return untranspose(undo_xform(t, payload[1]), payload[0]);
    }
    case M_PREFILTER: {
        if (payload.size() < 2) throw std::runtime_error("prefilter short");
        auto t = backend_dec(payload[1], std::vector<uint8_t>(payload.begin() + 2, payload.end()));
        return undo_filter(t, payload[0]);
    }
    case M_BWT: return decode_bwt(payload);
    case M_TOKEN: return token_decode_payload(payload);
    case M_BLOCKS: return decode_blocks(payload);
    case M_PARTS: return decode_parts(payload);
    case M_STORE:
    case M_DEFLATE:
    case M_LZMA:
    case M_ZSTD:
    case M_BROTLI:
        return backend_dec(mid, payload);
    default:
        throw std::runtime_error("unknown method");
    }
}

static void append_cands(std::vector<Cand> &dst, std::vector<Cand> &&src) {
    dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

} // namespace

const char *version_string() {
    return "bav-cpp 0.3.0 (backup port, full research)";
}

Method parse_method(const std::string &s) {
    if (s == "auto" || s == "research") return Method::Auto;
    if (s == "store") return Method::Store;
    if (s == "deflate") return Method::Deflate;
    if (s == "lzma") return Method::Lzma;
    if (s == "zstd") return Method::Zstd;
    if (s == "brotli") return Method::Brotli;
    throw std::runtime_error("unknown method: " + s);
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &data, Method method) {
    uint32_t c = crc32(data);
    uint64_t orig = data.size();
    std::vector<Cand> cands;

    switch (method) {
    case Method::Store:
        cands.push_back({M_STORE, store_enc(data)});
        break;
    case Method::Deflate:
        cands.push_back({M_DEFLATE, zlib_enc(data)});
        break;
    case Method::Lzma:
        cands.push_back({M_LZMA, lzma_enc(data)});
        break;
    case Method::Zstd:
        cands.push_back({M_ZSTD, zstd_enc(data)});
        break;
    case Method::Brotli:
        cands.push_back({M_BROTLI, brotli_enc(data)});
        break;
    case Method::Auto:
    case Method::Research:
    default: {
        // Parallel candidate families
        std::vector<Cand> t_backends, t_tr, t_xf, t_pf, t_bwt, t_bl, t_pt, t_tk;
        std::thread th0([&] { t_backends = try_backends(data); });
        std::thread th1([&] { t_tr = research_transpose(data); });
        std::thread th2([&] { t_xf = research_xform(data); });
        std::thread th3([&] { t_pf = research_prefilter(data); });
        std::thread th4([&] { t_bwt = research_bwt(data); });
        std::thread th5([&] { t_bl = research_blocks(data); });
        std::thread th6([&] { t_pt = research_parts(data); });
        std::thread th7([&] { t_tk = research_token(data); });
        th0.join();
        th1.join();
        th2.join();
        th3.join();
        th4.join();
        th5.join();
        th6.join();
        th7.join();
        append_cands(cands, std::move(t_backends));
        append_cands(cands, std::move(t_tr));
        append_cands(cands, std::move(t_xf));
        append_cands(cands, std::move(t_pf));
        append_cands(cands, std::move(t_bwt));
        append_cands(cands, std::move(t_bl));
        append_cands(cands, std::move(t_pt));
        append_cands(cands, std::move(t_tk));
        break;
    }
    }

    if (cands.empty()) cands.push_back({M_STORE, store_enc(data)});
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        if (a.p.size() != b.p.size()) return a.p.size() < b.p.size();
        return a.mid < b.mid;
    });
    auto frame = make_header(cands[0].mid, orig, c);
    frame.insert(frame.end(), cands[0].p.begin(), cands[0].p.end());
    return frame;
}

std::vector<uint8_t> decompress(const std::vector<uint8_t> &frame) {
    if (frame.size() < 8) throw std::runtime_error("BAV frame too short");
    if (std::memcmp(frame.data(), MAGIC, 4) != 0) throw std::runtime_error("bad magic");
    uint8_t ver = frame[4];
    uint8_t mid;
    uint64_t orig;
    uint32_t crc;
    std::vector<uint8_t> payload;
    if (ver == FORMAT_VERSION) {
        if (frame.size() < HEADER_V2) throw std::runtime_error("v2 short");
        mid = frame[5];
        orig = get_u32(frame.data() + 7);
        crc = get_u32(frame.data() + 11);
        payload.assign(frame.begin() + static_cast<long>(HEADER_V2), frame.end());
    } else if (ver == FORMAT_VERSION_LEGACY) {
        if (frame.size() < HEADER_V1) throw std::runtime_error("v1 short");
        mid = frame[5];
        std::memcpy(&orig, frame.data() + 7, 8);
        std::memcpy(&crc, frame.data() + 15, 4);
        payload.assign(frame.begin() + static_cast<long>(HEADER_V1), frame.end());
    } else
        throw std::runtime_error("unsupported version");

    auto data = decode_method_payload(mid, payload);
    if (data.size() != orig) throw std::runtime_error("size mismatch");
    if (crc32(data) != crc) throw std::runtime_error("CRC32 mismatch");
    return data;
}

} // namespace bav
