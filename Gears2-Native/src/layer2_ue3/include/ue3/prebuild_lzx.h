// Layer 2 (Unreal Engine 3 on the 360): the shader prebuild reader's LZX decoder (prebuild_lzx.cpp; decoder from PR #43,
// legoliamneeson). Its own namespace and name so it never clashes with Gears 3's ue3::LzxDecompress. Our code (MIT).
#pragma once
#include <cstddef>
#include <cstdint>

namespace ue3::prebuild {

// Largest output one call decodes (the reader's own caps: a chunk block is at most 1 MB, an inner block 16 MB).
inline constexpr size_t kPrebuildLzxMaxOutput = size_t(1) << 24;

// One raw LZX stream (frame headers already removed) of exactly out_len bytes, window 2^window_bits (15..21), no reset
// interval. Tries XMemCompress's form first (no pad byte after an odd-length uncompressed block); only if that fails,
// once more with the CAB-style pad byte, and then sets *needed_padding. False: not decoded (out is then undefined).
bool PrebuildLzxDecode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, int window_bits,
                       bool* needed_padding = nullptr);

}  // namespace ue3::prebuild
