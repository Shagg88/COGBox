// Layer 2 (Unreal Engine 3 on the 360): a cooked package's export paths (ue3/package_exports.h; GitHub #145). The
// LZX blocks go through the shader prebuild reader's decoder (ue3/prebuild_lzx.h). Our code (MIT).
#include "ue3/package_exports.h"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <vector>

#include "ue3/prebuild_lzx.h"

namespace ue3 {
namespace {

constexpr uint32_t kTag = 0x9E2A83C1u;
constexpr uint32_t kMaxTables = 64u << 20;   // more than any table section (GearGame_LOC_int's ends at 22 KB)

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Bounds-checked big-endian reads over a byte range.
struct Cursor {
  const uint8_t* data;
  size_t size;
  size_t at = 0;
  bool ok = true;
  uint32_t U32() {
    if (!ok || at + 4 > size) { ok = false; return 0; }
    const uint32_t v = Be32(data + at);
    at += 4;
    return v;
  }
  int32_t I32() { return int32_t(U32()); }
  void Skip(size_t n) {
    if (!ok || n > size - at) { ok = false; return; }
    at += n;
  }
  // FString: length then 8-bit characters, or (negative length) UTF-16 code units; both include the terminator.
  // Names are ASCII; a UTF-16 one keeps its low bytes.
  std::string FString() {
    const int32_t n = I32();
    std::string s;
    if (!ok || n == 0) return s;
    const size_t chars = n > 0 ? size_t(n) : size_t(-int64_t(n));
    const size_t bytes = n > 0 ? chars : chars * 2;
    if (chars > 4096 || bytes > size - at) { ok = false; return s; }
    for (size_t i = 0; i + 1 < chars; ++i) s.push_back(char(n > 0 ? data[at + i] : data[at + 2 * i]));
    at += bytes;
    return s;
  }
};

// One XMemCompress-framed LZX block (frames "cc cc", or "FF uu uu cc cc"), as the shader prebuild reader does it.
bool FramedLzx(const uint8_t* in, size_t n, uint8_t* out, size_t out_size) {
  std::vector<uint8_t> raw;
  size_t p = 0;
  while (p + 2 <= n) {
    size_t csz;
    if (in[p] == 0xFF) {
      if (p + 5 > n) break;
      csz = size_t(in[p + 3]) << 8 | in[p + 4];
      p += 5;
    } else {
      csz = size_t(in[p]) << 8 | in[p + 1];
      p += 2;
    }
    if (!csz || p + csz > n) break;
    raw.insert(raw.end(), in + p, in + p + csz);
    p += csz;
  }
  return !raw.empty() && prebuild::PrebuildLzxDecode(raw.data(), raw.size(), out, out_size, 17);
}

}  // namespace

bool ReadPackageExportPaths(const PackageRead& read, uint64_t file_size, std::set<std::string>* out) {
  out->clear();
  // The summary: its fixed part and chunk table fit in the first 64 KB of every package seen; more is read if not.
  std::vector<uint8_t> head(size_t(std::min<uint64_t>(file_size, 0x10000)));
  if (head.size() < 64 || !read(0, head.size(), head.data())) return false;
  Cursor s{head.data(), head.size()};
  if (s.U32() != kTag) return false;
  const uint32_t version = s.U32() & 0xFFFF;   // a package compressed as a whole has 0x20000 (or the tag) here
  if (version < 543 || version >= 835) return false;   // 575 Gears 2, 828 Gears 3: no ComponentMap, no extra int
  s.Skip(4);                                           // header size
  s.FString();                                         // folder
  s.Skip(4);                                           // package flags
  const int32_t name_count = s.I32(), name_offset = s.I32(), export_count = s.I32(), export_offset = s.I32();
  s.Skip(8);                                           // import count, offset
  const int32_t depends_offset = s.I32();
  if (version >= 623) s.Skip(16);                      // import/export GUIDs offset, two counts, thumbnail table
  s.Skip(16);                                          // GUID
  const int32_t generations = s.I32();
  if (generations < 0 || generations > 4096) return false;
  s.Skip(size_t(generations) * 12);
  s.Skip(8);                                           // engine, cooker versions
  const uint32_t compression = s.U32();
  const int32_t chunk_count = s.I32();
  if (!s.ok || name_count < 0 || export_count < 0 || name_offset < 0 || export_offset < 0 || depends_offset <= 0 ||
      uint32_t(depends_offset) > kMaxTables || chunk_count < 0 || chunk_count > 4096)
    return false;
  if (s.at + size_t(chunk_count) * 16 > head.size()) {
    if (s.at + size_t(chunk_count) * 16 > file_size) return false;
    head.resize(s.at + size_t(chunk_count) * 16);
    if (!read(0, head.size(), head.data())) return false;
    s.data = head.data();
    s.size = head.size();
  }

  // The bytes [0, depends offset) of the uncompressed package: read, or decoded from the chunks that cover them.
  std::vector<uint8_t> data(size_t(depends_offset), 0);
  if (chunk_count == 0) {
    if (compression != 0 || uint64_t(depends_offset) > file_size || !read(0, data.size(), data.data())) return false;
  } else {
    if (compression != 4) return false;   // LZX only (the 360's packages)
    for (int32_t c = 0; c < chunk_count; ++c) {
      const uint32_t uo = s.U32(), us = s.U32(), co = s.U32(), cs = s.U32();
      if (!s.ok) return false;
      if (uo >= uint32_t(depends_offset)) continue;
      if (us == 0 || us > kMaxTables || cs < 16 || uint64_t(co) + cs > file_size) return false;
      std::vector<uint8_t> chunk(cs);
      if (!read(co, cs, chunk.data()) || Be32(chunk.data()) != kTag) return false;
      const uint32_t block_size = Be32(&chunk[4]), total_u = Be32(&chunk[12]);
      if (block_size == 0 || block_size > (1u << 20) || total_u != us) return false;
      const size_t blocks = (size_t(total_u) + block_size - 1) / block_size;
      if (16 + blocks * 8 > cs) return false;
      std::vector<uint8_t> plain(total_u);
      size_t src = 16 + blocks * 8, dst = 0;
      for (size_t b = 0; b < blocks; ++b) {
        const uint32_t bc = Be32(&chunk[16 + b * 8]), bu = Be32(&chunk[16 + b * 8 + 4]);
        if (bu > block_size || dst + bu > plain.size() || bc > cs - src) return false;
        if (!FramedLzx(&chunk[src], bc, &plain[dst], bu)) return false;
        src += bc;
        dst += bu;
      }
      if (dst != plain.size()) return false;
      const size_t n = std::min<size_t>(plain.size(), data.size() - uo);
      std::copy_n(plain.begin(), n, data.begin() + uo);
    }
  }

  Cursor t{data.data(), data.size()};
  std::vector<std::string> names;
  names.reserve(size_t(name_count));
  t.at = size_t(name_offset);
  if (t.at > t.size) return false;
  for (int32_t i = 0; i < name_count && t.ok; ++i) {
    names.push_back(t.FString());
    t.Skip(8);   // flags
  }
  struct Entry {
    int32_t outer;
    std::string name;
  };
  std::vector<Entry> exports;
  exports.reserve(size_t(export_count));
  t.at = size_t(export_offset);
  if (!t.ok || t.at > t.size) return false;
  for (int32_t i = 0; i < export_count && t.ok; ++i) {
    t.Skip(8);   // class, super
    const int32_t outer = t.I32(), name = t.I32(), number = t.I32();
    t.Skip(4 + 8 + 8 + 4);   // archetype, object flags, serial size and offset, export flags
    const int32_t net = t.I32();
    if (net < 0 || net > 100000) return false;
    t.Skip(size_t(net) * 4 + 16 + 4);   // NetObjectCount array, package GUID, package flags
    if (!t.ok || name < 0 || size_t(name) >= names.size() || outer < 0 || outer > export_count) return false;
    exports.push_back({outer, number == 0 ? names[size_t(name)] : names[size_t(name)] + "_" + std::to_string(number - 1)});
  }
  if (!t.ok) return false;
  for (const Entry& e : exports) {
    std::string path = e.name;
    int32_t outer = e.outer;
    for (int32_t depth = 0; outer > 0; ++depth) {
      if (depth >= export_count) return false;   // an outer loop
      path = exports[size_t(outer - 1)].name + "." + path;
      outer = exports[size_t(outer - 1)].outer;
    }
    out->insert(std::move(path));
  }
  return true;
}

bool ReadPackageExportPaths(const std::filesystem::path& file, std::set<std::string>* out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return false;
  return ReadPackageExportPaths(
      [&](uint64_t offset, size_t n, uint8_t* dst) {
        if (offset > uint64_t(size) || n > uint64_t(size) - offset) return false;
        in.clear();
        in.seekg(std::streamoff(offset));
        return bool(in.read(reinterpret_cast<char*>(dst), std::streamsize(n)));
      },
      uint64_t(size), out);
}

}  // namespace ue3
