// Layer 0 (game-free): texture packs - see xenos/renderer/texture_pack.h. KiltedKernel's code (MIT). The pack layout
// and name rule (`<TITLEID>\replace\<hash16>_....dds`, the hash first) follow the Gears of War 2 PC port's texture packs
// (santiquiroz/gears-of-war-2-pc-port, docs/TEXTURE_PACKS.md, BSD-3-Clause) so its packs load here; no code is taken from it.
#include <xenos/renderer/texture_pack.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>

namespace xenos::renderer::texpack {
namespace {

using gpu::Format;

uint32_t U32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // DDS files are little-endian, as the host
}
void Put32(std::vector<uint8_t>& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

// DXGI_FORMAT values (dxgiformat.h) of the formats packs and dumps use.
struct DxgiEntry {
  uint32_t dxgi;
  Format format;
};
constexpr DxgiEntry kDxgi[] = {
    {71, Format::kBC1}, {72, Format::kBC1},                    // BC1_UNORM(_SRGB)
    {74, Format::kBC2}, {75, Format::kBC2},                    // BC2
    {77, Format::kBC3}, {78, Format::kBC3},                    // BC3
    {80, Format::kBC4},                                        // BC4_UNORM
    {83, Format::kBC5},                                        // BC5_UNORM
    {98, Format::kBC7}, {99, Format::kBC7},                    // BC7_UNORM(_SRGB)
    {61, Format::kR8Unorm},                                    // R8_UNORM
    {49, Format::kR8G8Unorm},                                  // R8G8_UNORM
    {28, Format::kR8G8B8A8Unorm}, {29, Format::kR8G8B8A8Unorm},  // R8G8B8A8_UNORM(_SRGB)
    {24, Format::kR10G10B10A2Unorm},                           // R10G10B10A2_UNORM
    {85, Format::kB5G6R5Unorm},                                // B5G6R5_UNORM
    {86, Format::kB5G5R5A1Unorm},                              // B5G5R5A1_UNORM
    {115, Format::kB4G4R4A4Unorm},                             // B4G4R4A4_UNORM
    {56, Format::kR16Unorm},                                   // R16_UNORM
    {35, Format::kR16G16Unorm},                                // R16G16_UNORM
    {11, Format::kR16G16B16A16Unorm},                          // R16G16B16A16_UNORM
    {54, Format::kR16Float},                                   // R16_FLOAT
    {34, Format::kR16G16Float},                                // R16G16_FLOAT
    {10, Format::kR16G16B16A16Float},                          // R16G16B16A16_FLOAT
    {41, Format::kR32Float},                                   // R32_FLOAT
    {16, Format::kR32G32Float},                                // R32G32_FLOAT
    {2, Format::kR32G32B32A32Float},                           // R32G32B32A32_FLOAT
};
constexpr uint32_t kDxgiB8G8R8A8 = 87, kDxgiB8G8R8A8Srgb = 91;

uint32_t DxgiOf(Format f) {
  for (const DxgiEntry& e : kDxgi)
    if (e.format == f) return e.dxgi;  // (the first, UNORM, entry)
  return 0;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// r95 H1: a path's characters as ASCII, every other one '?' - straight from the native string (UTF-16 on Windows), so no
// code-page conversion and nothing that throws for a name the ANSI code page cannot hold. Enough for the hash at the
// start of a pack file's name, its extension and the folder names Build skips.
std::string AsciiName(const std::filesystem::path& p) {
  using Unit = std::make_unsigned_t<std::filesystem::path::value_type>;
  const std::filesystem::path::string_type& n = p.native();
  std::string out;
  out.reserve(n.size());
  for (const auto c : n) out.push_back(static_cast<Unit>(c) < 0x80 ? char(c) : '?');
  return out;
}

bool EndsWithDds(const std::string& ascii) { return ascii.size() >= 4 && Lower(ascii.substr(ascii.size() - 4)) == ".dds"; }

}  // namespace

bool FormatUnit(Format f, bool& block, uint32_t& unit_bytes) {
  block = false;
  switch (f) {
    case Format::kBC1: case Format::kBC4: block = true; unit_bytes = 8; return true;
    case Format::kBC2: case Format::kBC3: case Format::kBC5: case Format::kBC7: block = true; unit_bytes = 16; return true;
    case Format::kR8Unorm: unit_bytes = 1; return true;
    case Format::kR8G8Unorm: case Format::kB5G6R5Unorm: case Format::kB5G5R5A1Unorm: case Format::kB4G4R4A4Unorm:
    case Format::kR16Unorm: case Format::kR16Float: unit_bytes = 2; return true;
    case Format::kR8G8B8A8Unorm: case Format::kR10G10B10A2Unorm: case Format::kR16G16Unorm: case Format::kR16G16Float:
    case Format::kR32Float: unit_bytes = 4; return true;
    case Format::kR16G16B16A16Unorm: case Format::kR16G16B16A16Float: case Format::kR32G32Float: unit_bytes = 8; return true;
    case Format::kR32G32B32A32Float: unit_bytes = 16; return true;
    default: unit_bytes = 0; return false;
  }
}

bool ParseDds(const uint8_t* p, size_t n, DdsImage& out, std::string& why) {
  out = DdsImage{};
  if (n < 128 || std::memcmp(p, "DDS ", 4) != 0 || U32(p + 4) != 124) { why = "not a DDS file"; return false; }
  const uint32_t height = U32(p + 12), width = U32(p + 16), depth = U32(p + 24);
  uint32_t mips = std::max(U32(p + 28), 1u);
  const uint32_t pf_flags = U32(p + 80), fourcc = U32(p + 84), bits = U32(p + 88);
  const uint32_t rmask = U32(p + 92), gmask = U32(p + 96), bmask = U32(p + 100), amask = U32(p + 104);
  const uint32_t caps2 = U32(p + 112);
  if (!width || !height || width > 16384 || height > 16384) { why = "bad size"; return false; }
  if (caps2 & 0x200u) { why = "a cube map"; return false; }
  if ((caps2 & 0x200000u) && depth > 1) { why = "a volume texture"; return false; }
  size_t at = 128;
  bool swap_rb = false;
  Format f = Format::kUnknown;
  if ((pf_flags & 0x4u) && fourcc == FourCC('D', 'X', '1', '0')) {  // DDPF_FOURCC, DX10 header
    if (n < 148) { why = "short DX10 header"; return false; }
    const uint32_t dxgi = U32(p + 128), dim = U32(p + 132), misc = U32(p + 136), array = U32(p + 140);
    at = 148;
    if (dim != 3) { why = "not a 2D texture"; return false; }       // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    if (misc & 0x4u) { why = "a cube map"; return false; }
    if (array > 1) { why = "a texture array"; return false; }
    if (dxgi == kDxgiB8G8R8A8 || dxgi == kDxgiB8G8R8A8Srgb) {
      f = Format::kR8G8B8A8Unorm;
      swap_rb = true;
    } else {
      for (const DxgiEntry& e : kDxgi)
        if (e.dxgi == dxgi) { f = e.format; break; }
    }
    if (f == Format::kUnknown) { why = "DXGI format " + std::to_string(dxgi) + " not supported"; return false; }
  } else if (pf_flags & 0x4u) {  // legacy FourCC
    switch (fourcc) {
      case FourCC('D', 'X', 'T', '1'): f = Format::kBC1; break;
      case FourCC('D', 'X', 'T', '2'): case FourCC('D', 'X', 'T', '3'): f = Format::kBC2; break;
      case FourCC('D', 'X', 'T', '4'): case FourCC('D', 'X', 'T', '5'): f = Format::kBC3; break;
      case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): f = Format::kBC4; break;
      case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): f = Format::kBC5; break;
      default: why = "FourCC format not supported"; return false;
    }
  } else if ((pf_flags & 0x40u) && bits == 32) {  // DDPF_RGB
    if (rmask == 0x000000FFu && gmask == 0x0000FF00u && bmask == 0x00FF0000u) f = Format::kR8G8B8A8Unorm;
    else if (rmask == 0x00FF0000u && gmask == 0x0000FF00u && bmask == 0x000000FFu) { f = Format::kR8G8B8A8Unorm; swap_rb = true; }
    else { why = "32-bit RGB layout not supported"; return false; }
    // (no alpha mask: alpha is set to opaque below)
  } else if ((pf_flags & 0x20000u) && bits == 8) {  // DDPF_LUMINANCE
    f = Format::kR8Unorm;
  } else {
    why = "pixel format not supported";
    return false;
  }
  const bool fill_alpha = f == Format::kR8G8B8A8Unorm && !(pf_flags & 0x4u) && (!(pf_flags & 0x1u) || !amask);
  out.format = f;
  out.width = width;
  out.height = height;
  if (!FormatUnit(f, out.block, out.unit_bytes)) { why = "format not supported"; return false; }
  mips = std::min(mips, 1u + uint32_t(std::bit_width(std::max(width, height)) - 1));
  out.mips = 0;
  size_t total = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    out.mips = m;
    const size_t bytes = out.LevelBytes(m);
    if (at + total + bytes > n) break;
    out.offsets.push_back(total);
    total += bytes;
    out.mips = m + 1;
  }
  if (!out.mips) { why = "file shorter than its top level"; return false; }
  out.data.assign(p + at, p + at + total);
  if (swap_rb || fill_alpha)
    for (size_t i = 0; i + 3 < out.data.size(); i += 4) {
      if (swap_rb) std::swap(out.data[i], out.data[i + 2]);
      if (fill_alpha) out.data[i + 3] = 0xFF;
    }
  return true;
}

bool ReadDds(const std::filesystem::path& file, DdsImage& out, std::string& why) {
  std::ifstream in(file, std::ios::binary);
  if (!in) { why = "cannot open"; return false; }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0 || size > (std::streamoff(1) << 30)) { why = "bad file size"; return false; }
  in.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) { why = "read failed"; return false; }
  return ParseDds(bytes.data(), bytes.size(), out, why);
}

std::vector<uint8_t> WriteDds(Format format, uint32_t width, uint32_t height, const uint8_t* level0, size_t bytes) {
  bool block = false;
  uint32_t unit = 0;
  if (!FormatUnit(format, block, unit) || !width || !height) return {};
  const uint32_t dxgi = DxgiOf(format);
  const bool legacy = format == Format::kBC1 || format == Format::kBC2 || format == Format::kBC3 ||
                      format == Format::kR8G8B8A8Unorm;
  if (!legacy && !dxgi) return {};
  const size_t head = legacy ? 128 : 148;
  std::vector<uint8_t> out(head + bytes, 0);
  std::memcpy(out.data(), "DDS ", 4);
  Put32(out, 4, 124);
  Put32(out, 8, 0x1u | 0x2u | 0x4u | 0x1000u | (block ? 0x80000u : 0x8u));  // CAPS HEIGHT WIDTH PIXELFORMAT, LINEARSIZE|PITCH
  Put32(out, 12, height);
  Put32(out, 16, width);
  const uint32_t row = (block ? (width + 3) / 4 : width) * unit;
  Put32(out, 20, block ? uint32_t(bytes) : row);
  Put32(out, 28, 1);  // one level
  Put32(out, 76, 32);
  if (legacy && format == Format::kR8G8B8A8Unorm) {
    Put32(out, 80, 0x40u | 0x1u);  // DDPF_RGB | DDPF_ALPHAPIXELS
    Put32(out, 88, 32);
    Put32(out, 92, 0x000000FFu);
    Put32(out, 96, 0x0000FF00u);
    Put32(out, 100, 0x00FF0000u);
    Put32(out, 104, 0xFF000000u);
  } else {
    Put32(out, 80, 0x4u);  // DDPF_FOURCC
    Put32(out, 84, format == Format::kBC1 ? FourCC('D', 'X', 'T', '1') : format == Format::kBC2 ? FourCC('D', 'X', 'T', '3')
                   : format == Format::kBC3 ? FourCC('D', 'X', 'T', '5') : FourCC('D', 'X', '1', '0'));
  }
  Put32(out, 108, 0x1000u);  // DDSCAPS_TEXTURE
  if (!legacy) {
    Put32(out, 128, dxgi);
    Put32(out, 132, 3);  // TEXTURE2D
    Put32(out, 140, 1);  // array size
  }
  if (bytes) std::memcpy(out.data() + head, level0, bytes);
  return out;
}

bool HashFromName(std::string_view leaf, uint64_t& hash) {
  if (leaf.size() < 16) return false;
  uint64_t h = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = leaf[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    h = (h << 4) | uint64_t(d);
  }
  if (leaf.size() > 16 && leaf[16] != '_' && leaf[16] != '.') return false;
  hash = h;
  return true;
}

bool FitsGuest(uint32_t w, uint32_t h, uint32_t mips, const DdsImage& img, std::string& why) {
  if (!w || !h || !img.width || !img.height) { why = "empty"; return false; }
  const uint32_t k = img.width / w;
  if (k < 1 || k > 8 || img.width != w * k || img.height != h * k) {
    why = std::to_string(img.width) + "x" + std::to_string(img.height) + " is not the same whole multiple (1-8x) of " +
          std::to_string(w) + "x" + std::to_string(h);
    return false;
  }
  if (mips > 1) {
    const uint32_t need = mips + uint32_t(std::bit_width(k) - 1);  // the guest's last level, k times bigger
    if (img.mips < std::min(need, 1u + uint32_t(std::bit_width(std::max(img.width, img.height)) - 1))) {
      why = std::to_string(img.mips) + " mip level(s), the game samples " + std::to_string(mips) +
            " (save it with every mip level, e.g. texconv -m 0)";
      return false;
    }
  }
  if (img.block && ((img.width & 3) || (img.height & 3))) { why = "block-compressed size not a multiple of 4"; return false; }
  return true;
}

bool FitsUpload(const DdsImage& img, std::string& why) {
  for (uint32_t level = 0; level < img.mips; ++level) {
    const size_t bytes = img.LevelBytes(level);
    if (bytes > kMaxLevelBytes) {
      why = "too big to upload (" + std::to_string((bytes + (size_t(1) << 20) - 1) >> 20) + " MB level " +
            std::to_string(level) + ", the most is " + std::to_string(kMaxLevelBytes >> 20) +
            " MB; use BC7 / BC3 or a smaller scale)";
      return false;
    }
  }
  return true;
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
  try {
#if defined(__cpp_char8_t)  // (u8path is deprecated in C++20)
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
    return std::filesystem::u8path(utf8.begin(), utf8.end());
#endif
  } catch (...) {  // not valid UTF-8: as the bytes come (never reached for the launcher's paths)
    try {
      return std::filesystem::path(std::string(utf8));
    } catch (...) {
      return {};
    }
  }
}

std::string PathToUtf8(const std::filesystem::path& p) noexcept {
  try {
    const auto u = p.u8string();  // std::u8string (C++20) or std::string
    return std::string(u.begin(), u.end());
  } catch (...) {  // e.g. a lone UTF-16 surrogate
    try {
      return AsciiName(p);
    } catch (...) {
      return "?";
    }
  }
}

void PackIndex::Walk(const std::string& folder, size_t& added) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = PathFromUtf8(folder);
  if (root.empty() || !fs::is_directory(root, ec)) return;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end_it;
  for (; !ec && it != end_it; it.increment(ec)) {
    const fs::directory_entry& de = *it;
    const std::string leaf = AsciiName(de.path().filename());
    std::error_code entry_ec;  // r95 L3: an entry that cannot be looked at is skipped, not the end of the walk
    if (de.is_directory(entry_ec)) {
      const std::string l = Lower(leaf);
      if (l == "dump" || l == ".build" || l.rfind("replace_", 0) == 0 || it.depth() >= 6) it.disable_recursion_pending();
      continue;
    }
    if (entry_ec || !EndsWithDds(leaf)) continue;
    uint64_t hash = 0;
    if (!HashFromName(leaf, hash)) continue;
    if (files_.emplace(hash, de.path()).second) ++added;
  }
  if (ec) errors_.push_back(folder + ": stopped reading it (" + ec.message() + ")");
}

void PackIndex::Build(const std::string& list) {
  files_.clear();
  folders_.clear();
  errors_.clear();
  size_t at = 0;
  while (at <= list.size()) {
    size_t end = list.find(';', at);
    if (end == std::string::npos) end = list.size();
    std::string folder = list.substr(at, end - at);
    at = end + 1;
    while (!folder.empty() && (folder.back() == ' ' || folder.back() == '"')) folder.pop_back();
    while (!folder.empty() && (folder.front() == ' ' || folder.front() == '"')) folder.erase(folder.begin());
    if (folder.empty()) continue;
    size_t added = 0;
    try {  // r95 H1: nothing in a mod's folder may take the game down - log it and go on
      Walk(folder, added);
    } catch (const std::exception& ex) {
      errors_.push_back(folder + ": " + ex.what());
    } catch (...) {
      errors_.push_back(folder + ": could not be read");
    }
    folders_.emplace_back(folder, added);
  }
}

}  // namespace xenos::renderer::texpack
