#pragma once
// LZO1X decompressor (the format Fable's .big banks use). Portable, no Windows headers.
#include <cstddef>
#include <cstdint>

namespace fo::art {
// Inflates `in` into `out` (capacity out_cap). Returns the number of bytes written, or -1 on
// corrupt input or if the output would overflow. Stops at the end-of-stream marker or when
// the input runs out, whichever comes first.
long lzo1x_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);
}  // namespace fo::art
