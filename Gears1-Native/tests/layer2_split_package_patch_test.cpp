// ue3/gow1_split_package_patch.h: the Engine.xxx script patch that makes Gears 1 itself pick the two-player split side
// by side (GitHub #420, task 8b), and Gears 1's LZO package patch machinery (ue3/package_patch.h). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gow1_split_package_patch.h>
#include <ue3/package_patch.h>

#include <cstring>

namespace {
constexpr uint64_t kCond = 0x21C41C, kDefault = 0x21F1B5, kLeft = 0x21C9D0, kRight = 0x21CACD, kSub = 0x21C6C0;
constexpr size_t kPlain = 7503548;
constexpr uint32_t kBlock = 0x20000;

void Put(std::vector<uint8_t>& d, uint64_t at, std::initializer_list<uint8_t> b) {
  for (uint8_t v : b) d[at++] = v;
}
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(kPlain);
  for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 7 + (i >> 9));
  Put(d, kCond, {0x24, 0x01, 0x16, 0x18});
  Put(d, kDefault, {0, 0, 0, 1, 0, 0, 0, 0, 1});
  Put(d, kLeft, {0x24, 0x01, 0x04, 0x27});
  Put(d, kRight, {0x24, 0x01, 0x04, 0x27});
  Put(d, kSub, {0x3F, 0x66, 0x66, 0x66, 0x00, 0x05, 0x1E, 0x3F, 0x00, 0x00, 0x00});
  return d;
}
void Le32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
uint32_t GetLe32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
// The file as Gears 1 stores it: LE header (tag twice, totals), block table, blocks, zeros to 32 KB.
std::vector<uint8_t> Compress(const std::vector<uint8_t>& plain) {
  std::vector<std::vector<uint8_t>> blocks;
  for (size_t at = 0; at < plain.size(); at += kBlock)
    blocks.push_back(ue3::Lzo1xStore(plain.data() + at, std::min<size_t>(kBlock, plain.size() - at)));
  uint32_t tc = 0;
  for (auto& b : blocks) tc += uint32_t(b.size());
  std::vector<uint8_t> o;
  Le32(o, 0x9E2A83C1u); Le32(o, 0x9E2A83C1u); Le32(o, tc); Le32(o, uint32_t(plain.size()));
  for (size_t i = 0; i < blocks.size(); ++i) {
    Le32(o, uint32_t(blocks[i].size()));
    Le32(o, uint32_t(std::min<size_t>(kBlock, plain.size() - i * kBlock)));
  }
  for (auto& b : blocks) o.insert(o.end(), b.begin(), b.end());
  o.resize((o.size() + 0x7FFF) / 0x8000 * 0x8000, 0);
  return o;
}
std::vector<uint8_t> Decompress(const std::vector<uint8_t>& f) {
  const uint32_t tu = GetLe32(&f[12]), n = (tu + kBlock - 1) / kBlock;
  std::vector<uint8_t> out(tu);
  size_t p = 16 + 8 * n, w = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t cs = GetLe32(&f[16 + 8 * i]), us = GetLe32(&f[20 + 8 * i]);
    REQUIRE(ue3::Lzo1xDecompress(&f[p], cs, &out[w], us));
    p += cs; w += us;
  }
  CHECK(GetLe32(&f[8]) == p - (16 + 8 * n));
  return out;
}
}  // namespace

TEST_CASE("gow1 LZO literal store decodes back to the same bytes", "[split]") {
  for (size_t len : {size_t(1), size_t(3), size_t(4), size_t(18), size_t(19), size_t(273), size_t(274), size_t(0x20000)}) {
    std::vector<uint8_t> in(len);
    for (size_t i = 0; i < len; ++i) in[i] = uint8_t(i * 13 + 5);
    const std::vector<uint8_t> enc = ue3::Lzo1xStore(in.data(), in.size());
    std::vector<uint8_t> out(len);
    CHECK(ue3::Lzo1xDecompress(enc.data(), enc.size(), out.data(), out.size()));
    CHECK(out == in);
  }
}

TEST_CASE("gow1 split package patch parses into the five Engine.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gow1::kSplitVerticalPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 5);
  for (const auto& p : patches) CHECK(p.file == "Engine.xxx");
}

TEST_CASE("gow1 split package patch: the compressed copy decodes to eSST_2P_VERTICAL with both safe zones", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow1::kSplitVerticalPackagePatch);
  const std::vector<uint8_t> plain = Original();
  const std::vector<uint8_t> file = Compress(plain);
  std::vector<uint8_t> out;
  std::string err;
  CHECK(ue3::PatchCompressedPackage(file, "engine.XXX", patches, out, nullptr, &err) == 5);
  CHECK(err.empty());
  CHECK(out.size() % 0x8000 == 0);
  std::vector<uint8_t> want = plain;
  want[kCond + 1] = 2; want[kDefault + 8] = 2; want[kLeft + 1] = 2; want[kRight + 1] = 2;
  want[kSub + 8] = 0x66; want[kSub + 9] = 0x66; want[kSub + 10] = 0x66;   // 0.5 -> 0.9
  CHECK(Decompress(out) == want);
  std::vector<uint8_t> none;
  CHECK(ue3::PatchCompressedPackage(file, "WarfareGame.xxx", patches, none) == 0);
}

TEST_CASE("gow1 split package patch refuses a different Engine.xxx or a damaged file", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow1::kSplitVerticalPackagePatch);
  std::vector<uint8_t> plain = Original();
  plain[kRight + 1] = 3;
  std::vector<uint8_t> out;
  std::string err;
  CHECK(ue3::PatchCompressedPackage(Compress(plain), "Engine.xxx", patches, out, nullptr, &err) == -1);
  CHECK(out.empty());
  CHECK_FALSE(err.empty());
  std::vector<uint8_t> damaged = Compress(Original());
  damaged.resize(1000);
  err.clear();
  CHECK(ue3::PatchCompressedPackage(damaged, "Engine.xxx", patches, out, nullptr, &err) == -1);
  CHECK_FALSE(err.empty());
}

TEST_CASE("gow1: a bad package patch item is reported", "[split]") {
  std::string err;
  CHECK(ue3::ParsePackagePatches("Engine.xxx@0x10:2401>24", &err).empty());
  CHECK_FALSE(err.empty());
}

TEST_CASE("gow1: the table-of-contents size of a Gears 1 LZO package is its header's total", "[split]") {
  const std::vector<uint8_t> f = Compress(Original());
  CHECK(ue3::LegacyCompressedTotal(f.data()) == kPlain);
  const uint8_t be[16] = {0x9E, 0x2A, 0x83, 0xC1, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2};   // Gears 2's big-endian header
  CHECK(ue3::LegacyCompressedTotal(be) == 0);
}

// Real data (review r1): the disc's Engine.xxx, if GOW1_ENGINE_XXX names it - its blocks are real LZO1X with matches
// (M1-M4), so this checks the decoder on them, the stock bytes at every patch offset, and the patched copy. Skipped
// (with a note) when the variable isn't set: the repository holds no game files.
#include <cstdlib>
#include <fstream>
#include <iterator>
TEST_CASE("gow1 split package patch on the real TU5 Engine.xxx (GOW1_ENGINE_XXX)", "[split]") {
  const char* path = std::getenv("GOW1_ENGINE_XXX");
  if (!path || !*path) {
    WARN("GOW1_ENGINE_XXX not set - the real Engine.xxx check is skipped");
    return;
  }
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
  const std::vector<uint8_t> plain = Decompress(file);   // all 58 blocks, through the real matches
  REQUIRE(plain.size() == kPlain);
  CHECK(std::memcmp(&plain[kCond], "\x24\x01\x16\x18", 4) == 0);
  CHECK(std::memcmp(&plain[kDefault], "\x00\x00\x00\x01\x00\x00\x00\x00\x01", 9) == 0);
  CHECK(std::memcmp(&plain[kLeft], "\x24\x01\x04\x27", 4) == 0);
  CHECK(std::memcmp(&plain[kRight], "\x24\x01\x04\x27", 4) == 0);
  const auto patches = ue3::ParsePackagePatches(gow1::kSplitVerticalPackagePatch);
  std::vector<uint8_t> out;
  REQUIRE(ue3::PatchCompressedPackage(file, "Engine.xxx", patches, out) == 5);
  std::vector<uint8_t> want = plain;
  want[kCond + 1] = 2; want[kDefault + 8] = 2; want[kLeft + 1] = 2; want[kRight + 1] = 2;
  want[kSub + 8] = 0x66; want[kSub + 9] = 0x66; want[kSub + 10] = 0x66;
  CHECK(Decompress(out) == want);
  CHECK(ue3::LegacyCompressedTotal(out.data()) == kPlain);
}
