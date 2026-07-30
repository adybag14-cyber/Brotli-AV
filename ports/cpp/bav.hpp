#pragma once
/* Full research BAV codec — C++ backup port (parity with C# / Python gen2+). */
#include <cstdint>
#include <string>
#include <vector>

namespace bav {

enum class Method { Auto, Store, Deflate, Lzma, Zstd, Brotli, Research };

// Compress *data* into a BAV1/v2 frame. Default method is Auto (full research).
std::vector<uint8_t> compress(const std::vector<uint8_t> &data, Method method = Method::Auto);

// Decompress a BAV frame (v1 or v2); verifies CRC32 and original size.
std::vector<uint8_t> decompress(const std::vector<uint8_t> &frame);

Method parse_method(const std::string &s);

// Exposed for tests / NASM parity
uint32_t crc32(const std::vector<uint8_t> &data);
std::vector<uint8_t> mtf_encode(const std::vector<uint8_t> &data);
std::vector<uint8_t> mtf_decode(const std::vector<uint8_t> &data);

const char *version_string();

} // namespace bav
