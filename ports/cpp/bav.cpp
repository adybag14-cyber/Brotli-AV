#include "bav.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

// Optional NASM kernels
extern "C" {
#ifdef BAV_USE_NASM
uint32_t bav_crc32_nasm(const uint8_t *data, size_t len, uint32_t crc);
void bav_mtf_encode_nasm(const uint8_t *in, uint8_t *out, size_t n);
void bav_mtf_decode_nasm(const uint8_t *in, uint8_t *out, size_t n);
#endif
}

namespace bav {
namespace {

constexpr uint8_t MAGIC[4] = {'B', 'A', 'V', '1'};
constexpr uint8_t FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = 19;
constexpr uint8_t M_STORE = 0, M_DEFLATE = 1, M_TRANSPOSE = 4, M_PREFILTER = 6, M_BWT = 7;
constexpr uint8_t F_MTF = 1, F_RLE0 = 2, F_MTF_RLE0 = 3;
constexpr uint8_t BWT_F_MTF = 1, BWT_F_RLE0 = 2;
constexpr size_t BWT_MAX = 120000;

std::vector<uint8_t> pack_header(uint8_t method, uint64_t orig, uint32_t crc) {
    std::vector<uint8_t> h(HEADER_SIZE);
    std::memcpy(h.data(), MAGIC, 4);
    h[4] = FORMAT_VERSION;
    h[5] = method;
    h[6] = 0;
    std::memcpy(h.data() + 7, &orig, 8);
    std::memcpy(h.data() + 15, &crc, 4);
    return h;
}

std::vector<uint8_t> zlib_enc(const std::vector<uint8_t> &data) {
    uLongf dest = compressBound((uLong)data.size());
    std::vector<uint8_t> out(dest);
    if (compress2(out.data(), &dest, data.data(), (uLong)data.size(), 9) != Z_OK)
        throw std::runtime_error("zlib compress failed");
    out.resize(dest);
    return out;
}

std::vector<uint8_t> zlib_dec(const std::vector<uint8_t> &data) {
    uLongf dest = data.size() * 8 + 64;
    for (;;) {
        std::vector<uint8_t> out(dest);
        int rc = uncompress(out.data(), &dest, data.data(), (uLong)data.size());
        if (rc == Z_OK) {
            out.resize(dest);
            return out;
        }
        if (rc != Z_BUF_ERROR) throw std::runtime_error("zlib decompress failed");
        dest *= 2;
    }
}

std::vector<uint8_t> rle0_enc(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < data.size()) {
        if (data[i] == 0) {
            size_t j = i;
            while (j < data.size() && data[j] == 0 && (j - i) < 255) j++;
            out.push_back(0);
            out.push_back(static_cast<uint8_t>(j - i));
            i = j;
        } else {
            out.push_back(data[i++]);
        }
    }
    return out;
}

std::vector<uint8_t> rle0_dec(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < data.size();) {
        if (data[i] == 0) {
            if (i + 1 >= data.size()) throw std::runtime_error("truncated RLE0");
            out.insert(out.end(), data[i + 1], 0);
            i += 2;
        } else {
            out.push_back(data[i++]);
        }
    }
    return out;
}

std::vector<uint8_t> transpose(const std::vector<uint8_t> &data, size_t width) {
    if (width <= 1 || data.size() < width * 2) return data;
    size_t n = data.size() - (data.size() % width);
    if (n == 0) return data;
    size_t rows = n / width;
    std::vector<uint8_t> out(data.size());
    for (size_t col = 0; col < width; col++) {
        size_t base = col * rows;
        for (size_t row = 0; row < rows; row++) out[base + row] = data[row * width + col];
    }
    if (data.size() > n) std::copy(data.begin() + n, data.end(), out.begin() + n);
    return out;
}

std::vector<uint8_t> untranspose(const std::vector<uint8_t> &data, size_t width) {
    if (width <= 1 || data.size() < width * 2) return data;
    size_t n = data.size() - (data.size() % width);
    size_t rows = n / width;
    std::vector<uint8_t> out(data.size());
    for (size_t col = 0; col < width; col++) {
        size_t base = col * rows;
        for (size_t row = 0; row < rows; row++) out[row * width + col] = data[base + row];
    }
    if (data.size() > n) std::copy(data.begin() + n, data.end(), out.begin() + n);
    return out;
}

std::pair<std::vector<uint8_t>, uint32_t> bwt_enc(const std::vector<uint8_t> &data) {
    size_t n = data.size();
    if (n == 0) return {{}, 0};
    std::vector<uint8_t> s(n * 2);
    std::copy(data.begin(), data.end(), s.begin());
    std::copy(data.begin(), data.end(), s.begin() + n);
    std::vector<size_t> sa(n);
    for (size_t i = 0; i < n; i++) sa[i] = i;
    std::sort(sa.begin(), sa.end(), [&](size_t a, size_t b) {
        return std::lexicographical_compare(s.begin() + a, s.begin() + a + n, s.begin() + b,
                                            s.begin() + b + n);
    });
    std::vector<uint8_t> last(n);
    uint32_t primary = 0;
    for (size_t i = 0; i < n; i++) {
        last[i] = data[(sa[i] + n - 1) % n];
        if (sa[i] == 0) primary = static_cast<uint32_t>(i);
    }
    return {last, primary};
}

std::vector<uint8_t> bwt_dec(const std::vector<uint8_t> &last, uint32_t primary) {
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

std::pair<uint8_t, std::vector<uint8_t>> best_backend(const std::vector<uint8_t> &data) {
    auto store = data;
    auto defl = zlib_enc(data);
    if (defl.size() < store.size()) return {M_DEFLATE, std::move(defl)};
    return {M_STORE, std::move(store)};
}

std::vector<uint8_t> backend_dec(uint8_t mid, const std::vector<uint8_t> &payload) {
    if (mid == M_STORE) return payload;
    if (mid == M_DEFLATE) return zlib_dec(payload);
    throw std::runtime_error("unknown backend");
}

} // namespace

uint32_t crc32(const std::vector<uint8_t> &data) {
#ifdef BAV_USE_NASM
    return bav_crc32_nasm(data.data(), data.size(), 0);
#else
    return static_cast<uint32_t>(::crc32(0L, data.data(), static_cast<uInt>(data.size())));
#endif
}

std::vector<uint8_t> mtf_encode(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out(data.size());
#ifdef BAV_USE_NASM
    bav_mtf_encode_nasm(data.data(), out.data(), data.size());
#else
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
#endif
    return out;
}

std::vector<uint8_t> mtf_decode(const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out(data.size());
#ifdef BAV_USE_NASM
    bav_mtf_decode_nasm(data.data(), out.data(), data.size());
#else
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
#endif
    return out;
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &data, Method method) {
    uint32_t c = crc32(data);
    uint64_t orig = data.size();
    struct Cand {
        uint8_t mid;
        std::vector<uint8_t> p;
    };
    std::vector<Cand> cands;

    auto push_simple = [&]() {
        cands.push_back({M_STORE, data});
        try {
            cands.push_back({M_DEFLATE, zlib_enc(data)});
        } catch (...) {
        }
    };

    if (method == Method::Store) {
        cands.push_back({M_STORE, data});
    } else if (method == Method::Deflate) {
        cands.push_back({M_DEFLATE, zlib_enc(data)});
    } else {
        push_simple();
        if (data.size() >= 32) {
            for (uint8_t fid : {F_MTF, F_RLE0, F_MTF_RLE0}) {
                std::vector<uint8_t> filtered;
                if (fid == F_MTF)
                    filtered = mtf_encode(data);
                else if (fid == F_RLE0)
                    filtered = rle0_enc(data);
                else
                    filtered = rle0_enc(mtf_encode(data));
                if (filtered.size() > data.size() * 2 + 64) continue;
                auto [bmid, payload] = best_backend(filtered);
                std::vector<uint8_t> wrapped;
                wrapped.push_back(fid);
                wrapped.push_back(bmid);
                wrapped.insert(wrapped.end(), payload.begin(), payload.end());
                cands.push_back({M_PREFILTER, std::move(wrapped)});
            }
        }
        for (size_t w : {2u, 4u, 8u, 16u}) {
            if (data.size() < w * 4) continue;
            auto t = transpose(data, w);
            auto [bmid, payload] = best_backend(t);
            std::vector<uint8_t> wrapped;
            wrapped.push_back(static_cast<uint8_t>(w));
            wrapped.push_back(bmid);
            wrapped.insert(wrapped.end(), payload.begin(), payload.end());
            cands.push_back({M_TRANSPOSE, std::move(wrapped)});
        }
        if (data.size() >= 64 && data.size() <= BWT_MAX) {
            auto [last, primary] = bwt_enc(data);
            auto mtf = mtf_encode(last);
            auto [bmid, payload] = best_backend(mtf);
            std::vector<uint8_t> wrapped;
            wrapped.push_back(BWT_F_MTF);
            for (int i = 0; i < 4; i++) wrapped.push_back(reinterpret_cast<uint8_t *>(&primary)[i]);
            wrapped.push_back(bmid);
            wrapped.insert(wrapped.end(), payload.begin(), payload.end());
            cands.push_back({M_BWT, std::move(wrapped)});
        }
    }

    if (cands.empty()) cands.push_back({M_STORE, data});
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        if (a.p.size() != b.p.size()) return a.p.size() < b.p.size();
        return a.mid < b.mid;
    });
    auto frame = pack_header(cands[0].mid, orig, c);
    frame.insert(frame.end(), cands[0].p.begin(), cands[0].p.end());
    return frame;
}

std::vector<uint8_t> decompress(const std::vector<uint8_t> &frame) {
    if (frame.size() < HEADER_SIZE) throw std::runtime_error("frame too short");
    if (std::memcmp(frame.data(), MAGIC, 4) != 0) throw std::runtime_error("bad magic");
    if (frame[4] != FORMAT_VERSION) throw std::runtime_error("bad version");
    uint8_t mid = frame[5];
    uint64_t orig;
    uint32_t crc;
    std::memcpy(&orig, frame.data() + 7, 8);
    std::memcpy(&crc, frame.data() + 15, 4);
    std::vector<uint8_t> payload(frame.begin() + HEADER_SIZE, frame.end());
    std::vector<uint8_t> data;

    if (mid == M_TRANSPOSE) {
        if (payload.size() < 2) throw std::runtime_error("transpose short");
        size_t width = payload[0];
        auto transformed =
            backend_dec(payload[1], std::vector<uint8_t>(payload.begin() + 2, payload.end()));
        data = untranspose(transformed, width);
    } else if (mid == M_PREFILTER) {
        if (payload.size() < 2) throw std::runtime_error("prefilter short");
        uint8_t fid = payload[0];
        auto filtered =
            backend_dec(payload[1], std::vector<uint8_t>(payload.begin() + 2, payload.end()));
        if (fid == F_MTF)
            data = mtf_decode(filtered);
        else if (fid == F_RLE0)
            data = rle0_dec(filtered);
        else if (fid == F_MTF_RLE0)
            data = mtf_decode(rle0_dec(filtered));
        else
            throw std::runtime_error("unknown filter");
    } else if (mid == M_BWT) {
        if (payload.size() < 6) throw std::runtime_error("bwt short");
        uint8_t flags = payload[0];
        uint32_t primary;
        std::memcpy(&primary, payload.data() + 1, 4);
        auto transformed =
            backend_dec(payload[5], std::vector<uint8_t>(payload.begin() + 6, payload.end()));
        if (flags & BWT_F_RLE0) transformed = rle0_dec(transformed);
        if (flags & BWT_F_MTF) transformed = mtf_decode(transformed);
        data = bwt_dec(transformed, primary);
    } else {
        data = backend_dec(mid, payload);
    }

    if (data.size() != orig) throw std::runtime_error("size mismatch");
    if (crc32(data) != crc) throw std::runtime_error("CRC mismatch");
    return data;
}

} // namespace bav
