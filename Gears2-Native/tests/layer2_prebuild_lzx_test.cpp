// Layer 2: the shader prebuild reader's LZX decoder (ue3/prebuild_lzx.h). Synthetic streams, no game content. The
// stream builder and cases are from PR #43's tests/layer2_shader_prebuild_test.cpp (legoliamneeson). Our code (MIT).
#include <catch2/catch_test_macros.hpp>
#include <ue3/prebuild_lzx.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
// Stored (uncompressed) LZX blocks, bit headers packed MSB-first into little-endian 16-bit words. pad_odd adds the
// CAB-style pad byte after each odd-length block (XMemCompress writes none).
std::vector<uint8_t> Stored(const std::vector<std::vector<uint8_t>>& blocks, bool pad_odd = false) {
  std::vector<uint8_t> out;
  bool first = true;
  for (const auto& block : blocks) {
    std::vector<bool> bits;
    auto add = [&](uint32_t v, int n) {
      for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1);
    };
    if (first) add(0, 1);  // no x86 call translation
    add(3, 3);
    add(uint32_t(block.size()) >> 8, 16);
    add(uint32_t(block.size()) & 255, 8);
    while (bits.size() % 16) bits.push_back(false);
    for (size_t i = 0; i < bits.size(); i += 16) {
      uint16_t word = 0;
      for (size_t b = 0; b < 16; ++b) word = uint16_t(word << 1) | uint16_t(bits[i + b]);
      out.push_back(uint8_t(word));
      out.push_back(uint8_t(word >> 8));
    }
    for (int i = 0; i < 3; ++i) {  // R0-R2 = 1
      out.push_back(1);
      out.insert(out.end(), 3, 0);
    }
    out.insert(out.end(), block.begin(), block.end());
    if (pad_odd && (block.size() & 1)) out.push_back(0);
    first = false;
  }
  return out;
}
using ue3::prebuild::PrebuildLzxDecode;
}  // namespace

TEST_CASE("Prebuild LZX: no pad byte after an odd stored block (XMemCompress)", "[shader-prebuild]") {
  const auto stream = Stored({{1, 2, 3}, {4, 5, 6, 7, 8}});
  std::array<uint8_t, 8> out{};
  bool padded = true;
  REQUIRE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), out.size(), 17, &padded));
  REQUIRE(out == std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
  REQUIRE_FALSE(padded);
  REQUIRE_FALSE(PrebuildLzxDecode(stream.data(), stream.size() - 1, out.data(), out.size(), 17));
}

TEST_CASE("Prebuild LZX: CAB-style padding only as the second attempt", "[shader-prebuild]") {
  const auto stream = Stored({{1, 2, 3}, {4, 5, 6, 7, 8}}, true);
  std::array<uint8_t, 8> out{};
  bool padded = false;
  REQUIRE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), out.size(), 17, &padded));
  REQUIRE(out == std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
  REQUIRE(padded);
}

TEST_CASE("Prebuild LZX: a stream longer than its window", "[shader-prebuild]") {
  std::vector<uint8_t> plain(0x22000, 0x5A), out(plain.size());
  const auto stream = Stored({plain});
  REQUIRE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), out.size(), 17));
  REQUIRE(out == plain);
}

TEST_CASE("Prebuild LZX: output-size guard and bad arguments", "[shader-prebuild]") {
  const auto stream = Stored({{1, 2, 3}});
  std::vector<uint8_t> out(ue3::prebuild::kPrebuildLzxMaxOutput + 1);
  REQUIRE_FALSE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), out.size(), 17));
  REQUIRE_FALSE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), 0, 17));
  REQUIRE_FALSE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), 3, 14));  // window bits 15..21
  REQUIRE(PrebuildLzxDecode(stream.data(), stream.size(), out.data(), 3, 17));
}
