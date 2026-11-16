// Layer 0 (game-free): texture packs (KiltedKernel). A pack is a folder of DDS files, each named after the content hash
// of the guest texture it replaces (ComputeRexTextureHash: the hash the ReXGlue-based ports use, so packs made for the
// Gears of War 2 PC port, e.g. `texture_packs\<pack>\4D53082D\replace\<hash16>_....dds`, load unchanged). A replacement
// may be the original size or a whole multiple of it (up to 8x) and needs its mip levels down to the original's last
// one. Read by texture_cache.cpp (--gow2_texture_pack_dirs); also writes the dumps packs are made from
// (--gow2_texture_pack_dump_dir).
#pragma once

#include <xenos/renderer/gpu.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xenos::renderer::texpack {

// One DDS image: 2D, one layer, its mip levels one after another as the file has them (top level first).
struct DdsImage {
  gpu::Format format = gpu::Format::kUnknown;
  uint32_t width = 0, height = 0, mips = 0;
  bool block = false;        // block-compressed: 4x4 texel blocks
  uint32_t unit_bytes = 0;   // bytes per block (block) or per texel
  std::vector<uint8_t> data;
  std::vector<size_t> offsets;  // per level, into data
  uint32_t LevelWidth(uint32_t level) const { return std::max(width >> level, 1u); }
  uint32_t LevelHeight(uint32_t level) const { return std::max(height >> level, 1u); }
  uint32_t RowBytes(uint32_t level) const {
    return (block ? (LevelWidth(level) + 3) / 4 : LevelWidth(level)) * unit_bytes;
  }
  uint32_t Rows(uint32_t level) const { return block ? (LevelHeight(level) + 3) / 4 : LevelHeight(level); }
  size_t LevelBytes(uint32_t level) const { return size_t(RowBytes(level)) * Rows(level); }
};

// Bytes per block / texel of a host format, and whether it is block-compressed; false for formats DDS files here do not
// carry (depth).
bool FormatUnit(gpu::Format f, bool& block, uint32_t& unit_bytes);

// Reads a DDS file: DX10 headers (BC1-5, BC7, R8, R8G8, R8G8B8A8 / B8G8R8A8, and the other colour formats the texture
// cache uses) and the legacy ones (DXT1/3/5, ATI1/ATI2, BC4U/BC5U, 32-bit RGBA / BGRA, 8-bit luminance). sRGB variants are
// taken as their UNORM twins (the game does its own gamma), B8G8R8A8 is turned into R8G8B8A8. Cube maps, volumes and
// arrays are refused. A file shorter than its mip count says keeps the whole levels it has.
bool ParseDds(const uint8_t* bytes, size_t size, DdsImage& out, std::string& why);
bool ReadDds(const std::filesystem::path& file, DdsImage& out, std::string& why);

// A DDS file of one level (the base level of a dump) in a host format: the legacy header for BC1/BC2/BC3 and R8G8B8A8
// (read by every DDS tool), DX10 for the rest. Empty when the format has no DDS form.
std::vector<uint8_t> WriteDds(gpu::Format format, uint32_t width, uint32_t height, const uint8_t* level0, size_t bytes);

// The hash at the start of a pack file's name: 16 hex digits, then the end of the name, '_' or '.'.
bool HashFromName(std::string_view leaf, uint64_t& hash);

// Whether a replacement fits a guest texture of w x h with `mips` levels: the same size or the same whole multiple
// (1-8x) across and down, and (when the guest has mip levels) enough levels to reach its last one.
bool FitsGuest(uint32_t w, uint32_t h, uint32_t mips, const DdsImage& img, std::string& why);

// r95 M3: the biggest mip level a replacement may have. The backends' upload rings refuse a level above half their
// size (D3D12 --upload_ring_mb 256 by default; Vulkan the same), so a bigger level would fail to upload and be read
// again on every retry. 64 MB leaves room with a smaller ring: e.g. RGBA8 4096x4096, BC7 / BC3 8192x8192.
constexpr size_t kMaxLevelBytes = size_t(64) << 20;
// Whether every level of img is small enough to upload (kMaxLevelBytes); why says how big and what to do otherwise.
bool FitsUpload(const DdsImage& img, std::string& why);

// r95 H1: paths as UTF-8 (the launcher passes UTF-8; std::filesystem::path(std::string) would take the ANSI code page
// on Windows and throw for a name it cannot represent). Neither throws: a name that cannot be converted gives "?".
std::filesystem::path PathFromUtf8(std::string_view utf8);
std::string PathToUtf8(const std::filesystem::path& p) noexcept;

// The DDS files of a list of pack folders, by hash. Folders are searched recursively (a pack root, its
// `<TITLEID>\replace` folder or the replace folder itself all work); folders named `dump`, `.build` or `replace_<variant>`
// are skipped. The first folder of the list that has a hash wins. Folder names are UTF-8; any file or folder name works
// (r95 H1: names outside the ANSI code page no longer throw). A folder that cannot be read to the end keeps what was
// found and adds a line to errors() - Build itself does not throw.
class PackIndex {
 public:
  void Build(const std::string& folders_separated_by_semicolons);
  const std::filesystem::path* Find(uint64_t hash) const {
    const auto it = files_.find(hash);
    return it == files_.end() ? nullptr : &it->second;
  }
  size_t size() const { return files_.size(); }
  bool empty() const { return files_.empty(); }
  // per folder: (folder, textures it adds), for the log
  const std::vector<std::pair<std::string, size_t>>& folders() const { return folders_; }
  // what went wrong while walking the folders (UTF-8, one line each), for the log
  const std::vector<std::string>& errors() const { return errors_; }

 private:
  void Walk(const std::string& folder, size_t& added);
  std::unordered_map<uint64_t, std::filesystem::path> files_;
  std::vector<std::pair<std::string, size_t>> folders_;
  std::vector<std::string> errors_;
};

}  // namespace xenos::renderer::texpack
