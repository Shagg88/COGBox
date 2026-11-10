// Layer 2 (Unreal Engine 3 on the 360): byte patches to a cooked script package (e.g. GearGame.xxx), for game switches
// that live in UnrealScript rather than in the executable - Gears 3's GearGame.AllowCheats is `return false;` in
// script, so no hook in the recompiled code can reach it (notes/25). The patched package is served the same way as the
// edited config bundles (config_install.cpp): an edited copy under <user data>\ini-edits, shown to the game through
// the folder overlay, with its table-of-contents line (size, MD5) rewritten. The game's files are never written.
//
// "Fully compressed" packages (the 360 .xxx files, whose table-of-contents line gives an uncompressed size) are
// decompressed first - the copy is stored plain and its line's uncompressed size becomes 0, as the disc already does
// for the packages it stores plain (e.g. Gears 3's MPCH_*_SF.xxx). Each patch names the bytes it expects, so a
// different game version or release is refused instead of corrupted. Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue3 {

// "<file>@<offset>:<old hex>><new hex>", ';'-separated; <offset> in the decompressed package, decimal or 0x-hex; old and
// new the same length, e.g. "GearGame.xxx@0xD4508E:28>27". An optional "#<note>" after the new bytes is for the log.
struct PackagePatch {
  std::string file;  // name inside the bundle folder (--ue3_ini_bundle_dir), matched case-insensitively
  uint64_t offset = 0;
  std::vector<uint8_t> expect, replace;
  std::string note;
};
std::vector<PackagePatch> ParsePackagePatches(const std::string& spec, std::string* error = nullptr);

// A 360 "fully compressed" file: big-endian tag 0x9E2A83C1, block size 0x20000, total compressed / uncompressed sizes,
// one (compressed, uncompressed) pair per block, then the blocks, each an XMemCompress LZX stream (128 KB window) in
// 32 KB frames. Returns false (and leaves `out` empty) when `in` doesn't start with that header; `error` set on a
// damaged file.
bool IsFullyCompressed(const std::vector<uint8_t>& in);
// One XMemCompress LZX stream (no reset interval) of exactly `out_len` bytes, window 2^window_bits (lzx.cpp, ours).
bool LzxDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, int window_bits);
bool DecompressFully(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* error = nullptr);

// Applies the patches meant for `file` to `data`; returns how many applied, -1 if any expected bytes didn't match (data
// then unchanged). `log` gets one line per patch.
int ApplyPackagePatches(std::vector<uint8_t>& data, const std::string& file, const std::vector<PackagePatch>& patches,
                        std::vector<std::string>* log = nullptr);

}  // namespace ue3
