// Layer 0 unit tests: texture packs (xenos/renderer/texture_pack.h) - DDS reading and writing, names, fit rules and the
// pack index. Game-free, GPU-free. KiltedKernel's code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xenos/renderer/texture_pack.h>

namespace {

using namespace xenos::renderer;
using namespace xenos::renderer::texpack;

std::vector<uint8_t> LegacyHeader(uint32_t w, uint32_t h, uint32_t mips, uint32_t pf_flags, uint32_t fourcc, uint32_t bits,
                                  uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  std::vector<uint8_t> d(128, 0);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
  std::memcpy(d.data(), "DDS ", 4);
  put(4, 124);
  put(12, h);
  put(16, w);
  put(28, mips);
  put(76, 32);
  put(80, pf_flags);
  put(84, fourcc);
  put(88, bits);
  put(92, r);
  put(96, g);
  put(100, b);
  put(104, a);
  return d;
}

std::vector<uint8_t> Dx10Header(uint32_t w, uint32_t h, uint32_t mips, uint32_t dxgi) {
  std::vector<uint8_t> d = LegacyHeader(w, h, mips, 0x4, 0x30315844u /* DX10 */, 0, 0, 0, 0, 0);
  d.resize(148, 0);
  const uint32_t ext[5] = {dxgi, 3, 0, 1, 0};
  std::memcpy(d.data() + 128, ext, sizeof(ext));
  return d;
}

}  // namespace

TEST_CASE("texture_pack: hash from a pack file's name", "[texture_pack]") {
  uint64_t h = 0;
  REQUIRE(HashFromName("0031EB540D50E47E_rgba_128x128.dds", h));
  REQUIRE(h == 0x0031EB540D50E47Eull);
  REQUIRE(HashFromName("0031eb540d50e47e.dds", h));
  REQUIRE(HashFromName("0031EB540D50E47E", h));
  REQUIRE_FALSE(HashFromName("0031EB540D50E47", h));        // 15 digits
  REQUIRE_FALSE(HashFromName("0031EB540D50E47EX.dds", h));  // a 17th character that is not a separator
  REQUIRE_FALSE(HashFromName("readme.txt", h));
}

TEST_CASE("texture_pack: written DDS reads back (BC1 legacy header, BC4 DX10)", "[texture_pack]") {
  std::vector<uint8_t> level(8 * 4 * 2);  // 16x8 BC1: 4x2 blocks of 8 bytes
  for (size_t i = 0; i < level.size(); ++i) level[i] = uint8_t(i * 7);
  const std::vector<uint8_t> dds = WriteDds(gpu::Format::kBC1, 16, 8, level.data(), level.size());
  REQUIRE(dds.size() == 128 + level.size());
  DdsImage img;
  std::string why;
  REQUIRE(ParseDds(dds.data(), dds.size(), img, why));
  REQUIRE(img.format == gpu::Format::kBC1);
  REQUIRE(img.width == 16);
  REQUIRE(img.height == 8);
  REQUIRE(img.mips == 1);
  REQUIRE(img.RowBytes(0) == 32);
  REQUIRE(img.Rows(0) == 2);
  REQUIRE(img.data == level);

  std::vector<uint8_t> bc4(8 * 2 * 2);
  const std::vector<uint8_t> dds4 = WriteDds(gpu::Format::kBC4, 8, 8, bc4.data(), bc4.size());
  REQUIRE(dds4.size() == 148 + bc4.size());
  REQUIRE(ParseDds(dds4.data(), dds4.size(), img, why));
  REQUIRE(img.format == gpu::Format::kBC4);
}

TEST_CASE("texture_pack: BC7 with its mip chain, and a file shorter than its mip count", "[texture_pack]") {
  // 16x16 BC7: levels 16 (4x4 blocks), 8 (2x2), 4, 2, 1 (1 block each): 16 + 4 + 1 + 1 + 1 blocks of 16 bytes
  std::vector<uint8_t> d = Dx10Header(16, 16, 5, 98);
  d.resize(d.size() + (16 + 4 + 1 + 1 + 1) * 16, 0x11);
  DdsImage img;
  std::string why;
  REQUIRE(ParseDds(d.data(), d.size(), img, why));
  REQUIRE(img.format == gpu::Format::kBC7);
  REQUIRE(img.mips == 5);
  REQUIRE(img.offsets[1] == 16 * 16);
  REQUIRE(img.offsets[4] == (16 + 4 + 1 + 1) * 16);
  d.resize(d.size() - 2 * 16);  // the last two levels missing: three kept
  REQUIRE(ParseDds(d.data(), d.size(), img, why));
  REQUIRE(img.mips == 3);
}

TEST_CASE("texture_pack: BGRA and alpha-less RGB are read as RGBA", "[texture_pack]") {
  std::vector<uint8_t> d = LegacyHeader(1, 1, 1, 0x40 | 0x1, 0, 32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
  const uint8_t bgra[4] = {10, 20, 30, 40};
  d.insert(d.end(), bgra, bgra + 4);
  DdsImage img;
  std::string why;
  REQUIRE(ParseDds(d.data(), d.size(), img, why));
  REQUIRE(img.format == gpu::Format::kR8G8B8A8Unorm);
  REQUIRE(img.data == std::vector<uint8_t>{30, 20, 10, 40});

  std::vector<uint8_t> x = LegacyHeader(1, 1, 1, 0x40, 0, 32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0);
  const uint8_t rgbx[4] = {1, 2, 3, 0};
  x.insert(x.end(), rgbx, rgbx + 4);
  REQUIRE(ParseDds(x.data(), x.size(), img, why));
  REQUIRE(img.data == std::vector<uint8_t>{1, 2, 3, 255});
}

TEST_CASE("texture_pack: refused files", "[texture_pack]") {
  DdsImage img;
  std::string why;
  const uint8_t junk[200] = {};
  REQUIRE_FALSE(ParseDds(junk, sizeof(junk), img, why));
  std::vector<uint8_t> cube = Dx10Header(4, 4, 1, 71);
  const uint32_t misc_cube = 4;
  std::memcpy(cube.data() + 136, &misc_cube, 4);
  cube.resize(cube.size() + 8);
  REQUIRE_FALSE(ParseDds(cube.data(), cube.size(), img, why));
  std::vector<uint8_t> odd = Dx10Header(4, 4, 1, 26);  // R11G11B10_FLOAT: not a pack format
  odd.resize(odd.size() + 64);
  REQUIRE_FALSE(ParseDds(odd.data(), odd.size(), img, why));
}

TEST_CASE("texture_pack: which replacements fit a guest texture", "[texture_pack]") {
  DdsImage img;
  img.format = gpu::Format::kBC7;
  img.block = true;
  img.unit_bytes = 16;
  std::string why;
  img.width = 512, img.height = 256, img.mips = 10;
  REQUIRE(FitsGuest(256, 128, 9, img, why));   // 2x, every level the game samples
  REQUIRE(FitsGuest(512, 256, 1, img, why));   // 1x
  img.mips = 9;
  REQUIRE_FALSE(FitsGuest(256, 128, 9, img, why));  // the guest's last level missing
  img.mips = 1;
  REQUIRE(FitsGuest(256, 128, 1, img, why));  // no mips in the game: one level is enough
  REQUIRE_FALSE(FitsGuest(256, 256, 1, img, why));  // another aspect
  img.width = 3 * 256, img.height = 3 * 128, img.mips = 20;
  REQUIRE(FitsGuest(256, 128, 1, img, why));  // 3x
  img.width = 16 * 64, img.height = 16 * 64;
  REQUIRE_FALSE(FitsGuest(64, 64, 1, img, why));  // over 8x
}

TEST_CASE("texture_pack: pack folders - first folder wins, dump and variant folders skipped", "[texture_pack]") {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "cogbox_texture_pack_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  auto touch = [](const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
  };
  touch(root / "a" / "4D53082D" / "replace" / "00000000000000AA_18_256x256.dds");
  touch(root / "a" / "4D53082D" / "dump" / "00000000000000BB_18_256x256.dds");
  touch(root / "a" / "4D53082D" / "replace_keyboard" / "00000000000000CC.dds");
  touch(root / "b" / "00000000000000AA.dds");
  touch(root / "b" / "textures" / "00000000000000DD.DDS");
  touch(root / "b" / "notes.txt");
  PackIndex index;
  index.Build((root / "a").string() + ";" + (root / "b").string() + "; ;" + (root / "missing").string());
  REQUIRE(index.size() == 2);
  REQUIRE(index.Find(0xAA) != nullptr);
  REQUIRE(index.Find(0xAA)->string().find("replace") != std::string::npos);  // folder a's, listed first
  REQUIRE(index.Find(0xBB) == nullptr);
  REQUIRE(index.Find(0xCC) == nullptr);
  REQUIRE(index.Find(0xDD) != nullptr);
  REQUIRE(index.folders().size() == 3);
  REQUIRE(index.folders()[0].second == 1);
  REQUIRE(index.folders()[1].second == 1);  // its AA was already taken
  REQUIRE(index.folders()[2].second == 0);
  fs::remove_all(root, ec);
}
