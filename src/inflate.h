#pragma once

// DEFLATE decompression, for the archives that use it.

#include <cstddef>

namespace bg3le {

// Decompresses `size` bytes at `in` into exactly `outSize` bytes at `out`.
// Accepts a zlib-wrapped stream or raw deflate. Returns false if the data
// is malformed or does not produce exactly `outSize` bytes.
bool inflate(char const* in, std::size_t size, char* out,
             std::size_t outSize);

}  // namespace bg3le
