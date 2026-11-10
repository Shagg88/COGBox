// Layer 2: the export paths of a cooked package (ue3/package_exports.h; GitHub #145), on small packages built here in
// the version-575 layout - one uncompressed, one with its tables in an LZX chunk of stored blocks. Game-free. Our code
// (MIT).
#include <catch2/catch_test_macros.hpp>
#include <ue3/package_exports.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
void U32(std::vector<uint8_t>& v, uint32_t u) {
  v.push_back(uint8_t(u >> 24)); v.push_back(uint8_t(u >> 16)); v.push_back(uint8_t(u >> 8)); v.push_back(uint8_t(u));
}
void Put32(std::vector<uint8_t>& v, size_t at, uint32_t u) {
  v[at] = uint8_t(u >> 24); v[at + 1] = uint8_t(u >> 16); v[at + 2] = uint8_t(u >> 8); v[at + 3] = uint8_t(u);
}
void Narrow(std::vector<uint8_t>& v, const std::string& s) {
  U32(v, uint32_t(s.size() + 1));
  v.insert(v.end(), s.begin(), s.end());
  v.push_back(0);
}

struct Export {
  int32_t outer;   // 1-based export index, 0 = the package
  int32_t name;    // name-table index
  int32_t number;  // 0 = none, n = _<n-1>
};
// Names: "Pkg", "Group", "Sound". Exports: Pkg.Group, Pkg.Group.Sound, Pkg.Group.Sound_1.
const std::vector<std::string> kNames = {"Pkg", "Group", "Sound"};
const std::vector<Export> kExports = {{0, 1, 0}, {1, 2, 0}, {1, 2, 2}};
const std::set<std::string> kPaths = {"Group", "Group.Sound", "Group.Sound_1"};

// The name, import and export tables, starting at file offset `base`.
std::vector<uint8_t> Tables(uint32_t base, uint32_t* name_off, uint32_t* import_off, uint32_t* export_off,
                            uint32_t* depends_off) {
  std::vector<uint8_t> t;
  *name_off = base;
  for (const auto& n : kNames) {
    Narrow(t, n);
    U32(t, 0); U32(t, 0);   // flags
  }
  *import_off = base + uint32_t(t.size());   // no imports
  *export_off = base + uint32_t(t.size());
  for (const Export& e : kExports) {
    U32(t, 0); U32(t, 0);                     // class, super
    U32(t, uint32_t(e.outer)); U32(t, uint32_t(e.name)); U32(t, uint32_t(e.number));
    U32(t, 0);                                // archetype
    U32(t, 0); U32(t, 0);                     // object flags
    U32(t, 0); U32(t, 0);                     // serial size, offset
    U32(t, 0);                                // export flags
    U32(t, 1); U32(t, 7);                     // NetObjectCount array of one
    for (int i = 0; i < 4; ++i) U32(t, 0);    // package GUID
    U32(t, 0);                                // package flags
  }
  *depends_off = base + uint32_t(t.size());
  return t;
}

// The summary, with `chunks` {uncompressed offset, size, compressed offset, size}; offsets patched by the caller.
std::vector<uint8_t> Summary(uint32_t version, uint32_t compression, const std::vector<std::array<uint32_t, 4>>& chunks,
                             size_t* counts_at) {
  std::vector<uint8_t> s;
  U32(s, 0x9E2A83C1u);
  U32(s, version);
  U32(s, 0);                       // header size
  Narrow(s, "None");               // folder
  U32(s, 0);                       // package flags
  *counts_at = s.size();
  for (int i = 0; i < 7; ++i) U32(s, 0);   // name / export / import counts and offsets, depends offset
  if (version >= 623) for (int i = 0; i < 4; ++i) U32(s, 0);
  for (int i = 0; i < 4; ++i) U32(s, 0);   // GUID
  U32(s, 1); U32(s, 3); U32(s, 3); U32(s, 0);   // one generation
  U32(s, 4638); U32(s, 87);        // engine, cooker versions
  U32(s, compression);
  U32(s, uint32_t(chunks.size()));
  for (const auto& c : chunks) for (uint32_t x : c) U32(s, x);
  return s;
}
void PatchCounts(std::vector<uint8_t>& f, size_t at, uint32_t name_off, uint32_t import_off, uint32_t export_off,
                 uint32_t depends_off) {
  Put32(f, at, uint32_t(kNames.size())); Put32(f, at + 4, name_off);
  Put32(f, at + 8, uint32_t(kExports.size())); Put32(f, at + 12, export_off);
  Put32(f, at + 16, 0); Put32(f, at + 20, import_off);
  Put32(f, at + 24, depends_off);
}

std::vector<uint8_t> Uncompressed(uint32_t version = 575) {
  size_t at = 0;
  std::vector<uint8_t> f = Summary(version, 0, {}, &at);
  uint32_t n, i, e, d;
  const auto t = Tables(uint32_t(f.size()), &n, &i, &e, &d);
  f.insert(f.end(), t.begin(), t.end());
  PatchCounts(f, at, n, i, e, d);
  return f;
}

// A stored LZX block of `data` (one block, bit headers in little-endian 16-bit words), in one XMemCompress frame
// "FF uu uu cc cc".
std::vector<uint8_t> FramedStored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> lzx;
  std::vector<bool> bits;
  auto add = [&](uint32_t v, int nbits) {
    for (int b = nbits - 1; b >= 0; --b) bits.push_back((v >> b) & 1);
  };
  add(0, 1);   // no x86 call translation
  add(3, 3);   // stored
  add(uint32_t(data.size()) >> 8, 16);
  add(uint32_t(data.size()) & 255, 8);
  while (bits.size() % 16) bits.push_back(false);
  for (size_t b = 0; b < bits.size(); b += 16) {
    uint16_t word = 0;
    for (size_t k = 0; k < 16; ++k) word = uint16_t(word << 1) | uint16_t(bits[b + k]);
    lzx.push_back(uint8_t(word));
    lzx.push_back(uint8_t(word >> 8));
  }
  for (int r = 0; r < 3; ++r) {   // R0-R2 = 1
    lzx.push_back(1);
    lzx.insert(lzx.end(), 3, 0);
  }
  lzx.insert(lzx.end(), data.begin(), data.end());
  std::vector<uint8_t> framed = {0xFF, uint8_t(data.size() >> 8), uint8_t(data.size()), uint8_t(lzx.size() >> 8),
                                 uint8_t(lzx.size())};
  framed.insert(framed.end(), lzx.begin(), lzx.end());
  return framed;
}

// The tables in one LZX chunk of one block, after the summary.
std::vector<uint8_t> Chunked() {
  size_t at = 0;
  const size_t summary = Summary(575, 4, {{0, 0, 0, 0}}, &at).size();
  uint32_t n, i, e, d;
  const auto t = Tables(uint32_t(summary), &n, &i, &e, &d);
  const auto block = FramedStored(t);
  std::vector<uint8_t> chunk;
  U32(chunk, 0x9E2A83C1u);
  U32(chunk, 0x20000);   // block size
  U32(chunk, uint32_t(block.size()));
  U32(chunk, uint32_t(t.size()));
  U32(chunk, uint32_t(block.size()));
  U32(chunk, uint32_t(t.size()));
  chunk.insert(chunk.end(), block.begin(), block.end());
  std::vector<uint8_t> f =
      Summary(575, 4, {{uint32_t(summary), uint32_t(t.size()), uint32_t(summary), uint32_t(chunk.size())}}, &at);
  REQUIRE(f.size() == summary);
  f.insert(f.end(), chunk.begin(), chunk.end());
  PatchCounts(f, at, n, i, e, d);
  return f;
}

bool Paths(const std::vector<uint8_t>& f, std::set<std::string>* out) {
  return ue3::ReadPackageExportPaths(
      [&](uint64_t off, size_t size, uint8_t* dst) {
        if (off > f.size() || size > f.size() - off) return false;
        std::memcpy(dst, f.data() + off, size);
        return true;
      },
      f.size(), out);
}
}  // namespace

TEST_CASE("Package exports: an uncompressed package's paths, with name numbers", "[package-exports]") {
  std::set<std::string> paths;
  REQUIRE(Paths(Uncompressed(), &paths));
  REQUIRE(paths == kPaths);
}

TEST_CASE("Package exports: tables in an LZX chunk", "[package-exports]") {
  std::set<std::string> paths;
  REQUIRE(Paths(Chunked(), &paths));
  REQUIRE(paths == kPaths);
}

TEST_CASE("Package exports: Gears 3's summary (version 828)", "[package-exports]") {
  std::set<std::string> paths;
  REQUIRE(Paths(Uncompressed(828), &paths));
  REQUIRE(paths == kPaths);
}

TEST_CASE("Package exports: a package compressed as a whole (Judgment's form)", "[package-exports]") {
  // The whole uncompressed package in one chunk at byte 0 (block size 0x20000 where the version would be).
  const auto plain = Uncompressed();
  const auto block = FramedStored(plain);
  std::vector<uint8_t> f;
  U32(f, 0x9E2A83C1u);
  U32(f, 0x20000);
  U32(f, uint32_t(block.size()));
  U32(f, uint32_t(plain.size()));
  U32(f, uint32_t(block.size()));
  U32(f, uint32_t(plain.size()));
  f.insert(f.end(), block.begin(), block.end());
  std::set<std::string> paths;
  REQUIRE(Paths(f, &paths));
  REQUIRE(paths == kPaths);
}

TEST_CASE("Package exports: what it refuses", "[package-exports]") {
  std::set<std::string> paths;
  auto f = Uncompressed();
  REQUIRE_FALSE(Paths(std::vector<uint8_t>(f.begin(), f.end() - 10), &paths));   // export table cut short
  REQUIRE_FALSE(Paths(Uncompressed(374), &paths));                                 // Gears 1's layout
  auto whole = f;
  Put32(whole, 4, 0x20000);                                                        // compressed as a whole
  REQUIRE_FALSE(Paths(whole, &paths));
  auto bad = f;
  Put32(bad, 0, 0x12345678);
  REQUIRE_FALSE(Paths(bad, &paths));
  auto chunk = Chunked();
  chunk.resize(chunk.size() - 4);                                                  // a chunk cut short
  REQUIRE_FALSE(Paths(chunk, &paths));
}
