#pragma once
// Small zlib reader and writer, enough for game.bin's chunks (each well under 64 KB).
// No Windows or game dependencies, so it is tested on its own (tests/shop_stock_test.cpp).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fo::util {

/** Decodes one zlib stream (RFC 1950/1951). False on any error, including a bad Adler-32. */
bool zlib_inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

/** Encodes `data` as a zlib stream of stored (uncompressed) blocks. Always succeeds. */
std::vector<uint8_t> zlib_store(const uint8_t* data, size_t n);

uint32_t adler32(const uint8_t* data, size_t n);

}  // namespace fo::util
