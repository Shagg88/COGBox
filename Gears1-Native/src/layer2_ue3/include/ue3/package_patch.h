// Layer 2 (Unreal Engine 3, Gears 1's 2006 engine): byte patches to a cooked script package (GitHub #420, as Gears 2's
// ue3/package_patch.h). The patched copy goes under <user data>\package-patches and is served through the mods' folder
// view (config_install.cpp); the game's files are never written.
//
// Gears 1's .xxx packages are "fully compressed" with LZO1X (Gears 2 / 3 use LZX): a little-endian header - the package
// tag 0x9E2A83C1 twice (the second where newer engines store the block size; the engine then uses 0x20000), the total
// compressed and uncompressed sizes - then one (compressed, uncompressed) pair per 0x20000-byte block, then the blocks,
// then zero padding to a 32 KB boundary (checked on TU5's Engine.xxx: 58 blocks, 1,314,080 bytes + 29,408 zeros). The copy
// keeps that format, and WarGame\Xbox360TOC.txt's line for it gets its uncompressed size from LegacyCompressedTotal
// below (the mods' view rewrites that line; a 0 there makes the game read the file as plain): only the blocks holding a patch are decoded, patched and
// stored again as one LZO1X literal run (valid LZO1X that any decoder reads); every other block is copied byte for byte.
// Each patch names the bytes it expects, so another game version is refused instead of corrupted. Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue3 {

// "<file>@<offset>:<old hex>><new hex>", ';'-separated; <offset> in the decompressed package, decimal or 0x-hex; old and
// new the same length. An optional "#<note>" after the new bytes is for the log.
struct PackagePatch {
  std::string file;  // file name, matched case-insensitively
  uint64_t offset = 0;
  std::vector<uint8_t> expect, replace;
  std::string note;
};
std::vector<PackagePatch> ParsePackagePatches(const std::string& spec, std::string* error = nullptr);

// Applies the patches meant for `file` to plain `data`; returns how many applied, -1 if any expected bytes didn't match
// (data then unchanged). `log` gets one line per patch.
int ApplyPackagePatches(std::vector<uint8_t>& data, const std::string& file, const std::vector<PackagePatch>& patches,
                        std::vector<std::string>* log = nullptr);

// A bounds-checked LZO1X decoder (exactly out_len bytes), and the literal-only encoder described above.
bool Lzo1xDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
std::vector<uint8_t> Lzo1xStore(const uint8_t* in, size_t len);

// The uncompressed total of a file starting with that header (its first 16 bytes), 0 when it isn't one; the mods' view
// writes it into the table of contents (Xbox360TOC.txt), as the disc's line for the file has it.
uint64_t LegacyCompressedTotal(const uint8_t* head16);

// The patches for `file` applied to a Gears 1 fully compressed file `in` (format above), written to `out`. Returns how
// many applied; 0 if none is meant for `file`; -1 (out empty, `error` set) on a damaged file, a decode error or a byte
// that isn't what a patch expects.
int PatchCompressedPackage(const std::vector<uint8_t>& in, const std::string& file, const std::vector<PackagePatch>& patches,
                           std::vector<uint8_t>& out, std::vector<std::string>* log = nullptr, std::string* error = nullptr);

}  // namespace ue3
