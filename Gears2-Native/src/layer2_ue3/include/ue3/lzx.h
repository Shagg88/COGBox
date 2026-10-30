// Shared XMemCompress LZX decoder. Our code (MIT).
#pragma once
#include <cstddef>
#include <cstdint>
namespace ue3 {
bool LzxDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, int window_bits);
}
