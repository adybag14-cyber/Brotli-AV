#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bav {

enum class Method { Auto, Store, Deflate, Research };

std::vector<uint8_t> compress(const std::vector<uint8_t> &data, Method method = Method::Auto);
std::vector<uint8_t> decompress(const std::vector<uint8_t> &frame);

// Research transforms (shared with tests)
std::vector<uint8_t> mtf_encode(const std::vector<uint8_t> &data);
std::vector<uint8_t> mtf_decode(const std::vector<uint8_t> &data);
uint32_t crc32(const std::vector<uint8_t> &data);

} // namespace bav
