// Synthetic UE3 containers and XMemCompress streams; no game content. Our code (MIT).
#include <catch2/catch_test_macros.hpp>
#include <ue3/shader_package_scan.h>
#include <array>
#include <tuple>

namespace {
void Be(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(v >> shift));
}
std::vector<uint8_t> Container(bool vertex) {
  std::vector<uint8_t> out;
  Be(out, 40);  // TArray byte count
  for (uint32_t v : {0x102A1100u | uint32_t(vertex), 32u, 8u, 4u, 4u, 0x10000000u, 0u, 0u, 0x12345678u, 0x87654321u})
    Be(out, v);
  return out;
}
// Stored LZX blocks, with bit headers packed MSB-first into little-endian 16-bit words.
std::vector<uint8_t> Stored(const std::vector<std::vector<uint8_t>>& blocks) {
  std::vector<uint8_t> out;
  bool first = true;
  for (const auto& block : blocks) {
    std::vector<bool> bits;
    auto add = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1); };
    if (first) add(0, 1);  // no x86 call transform
    add(3, 3);
    add(uint32_t(block.size()) >> 8, 16);
    add(uint32_t(block.size()) & 255, 8);
    while (bits.size() % 16) bits.push_back(false);
    for (size_t i = 0; i < bits.size(); i += 16) {
      uint16_t word = 0;
      for (size_t b = 0; b < 16; ++b) word = uint16_t(word << 1) | uint16_t(bits[i + b]);
      out.push_back(uint8_t(word)); out.push_back(uint8_t(word >> 8));
    }
    for (int i = 0; i < 3; ++i) { out.push_back(1); out.insert(out.end(), 3, 0); }
    out.insert(out.end(), block.begin(), block.end());
    first = false;
  }
  return out;
}
std::vector<uint8_t> Frame(const std::vector<uint8_t>& plain) {
  auto stream = Stored({plain});
  std::vector<uint8_t> out = {255, uint8_t(plain.size() >> 8), uint8_t(plain.size()),
                            uint8_t(stream.size() >> 8), uint8_t(stream.size())};
  out.insert(out.end(), stream.begin(), stream.end());
  return out;
}
}
TEST_CASE("Shader discovery retains both stages and both constant-block variants", "[shader-prebuild]") {
  auto data = Container(false), vertex = Container(true);
  data.insert(data.end(), vertex.begin(), vertex.end());
  std::vector<std::tuple<bool, uint32_t, uint32_t>> found;
  ue3::shader_prebuild::PackageScanner scanner([&](bool vs, const uint8_t*, uint32_t size, uint32_t variant) {
    found.emplace_back(vs, size, variant);
  });
  scanner.Scan(data);
  REQUIRE(found.size() == 4);
  REQUIRE(found[0] == std::tuple(false, 8u, 0u));
  REQUIRE(found[1] == std::tuple(false, 4u, 1u));
  REQUIRE(found[2] == std::tuple(true, 8u, 0u));
  REQUIRE(found[3] == std::tuple(true, 4u, 1u));
}
TEST_CASE("Shader discovery scans nested streams in uncompressed packages", "[shader-prebuild]") {
  auto plain = Container(false);
  plain.erase(plain.begin(), plain.begin() + 4);  // inner containers are packed without a TArray
  auto frame = Frame(plain);
  std::vector<uint8_t> data(4, 0);
  Be(data, uint32_t(plain.size())); Be(data, uint32_t(frame.size()));
  data.insert(data.end(), frame.begin(), frame.end());
  unsigned found = 0;
  ue3::shader_prebuild::PackageScanner scanner([&](bool, const uint8_t*, uint32_t, uint32_t) { ++found; });
  REQUIRE(scanner.Scan(data).nested == 1);
  REQUIRE(found == 2);
}
TEST_CASE("Shader discovery joins compressed chunks without hiding raw containers", "[shader-prebuild]") {
  auto plain = Container(false), frame = Frame(plain);
  std::vector<uint8_t> data;
  for (uint32_t v : {0x9E2A83C1u, 0x20000u, uint32_t(frame.size()), uint32_t(plain.size()),
                     uint32_t(frame.size()), uint32_t(plain.size())}) Be(data, v);
  data.insert(data.end(), frame.begin(), frame.end());
  auto raw = Container(true);
  data.insert(data.end(), raw.begin(), raw.end());
  unsigned pixels = 0, vertices = 0;
  ue3::shader_prebuild::PackageScanner scanner([&](bool vs, const uint8_t*, uint32_t, uint32_t) {
    ++(vs ? vertices : pixels);
  });
  const auto result = scanner.Scan(data);
  REQUIRE(result.chunks == 1);
  REQUIRE(result.failed_chunks == 0);
  REQUIRE(pixels >= 2);
  REQUIRE(vertices == 2);
}
TEST_CASE("XMemCompress does not consume padding after odd stored blocks", "[shader-prebuild]") {
  std::vector<uint8_t> a = {1, 2, 3}, b = {4, 5, 6, 7, 8};
  auto stream = Stored({a, b});
  std::array<uint8_t, 8> out{};
  REQUIRE(ue3::LzxDecompress(stream.data(), stream.size(), out.data(), out.size(), 17));
  REQUIRE(out == std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
  REQUIRE_FALSE(ue3::LzxDecompress(stream.data(), stream.size() - 1, out.data(), out.size(), 17));
}
TEST_CASE("Nested LZX streams can exceed their history window", "[shader-prebuild]") {
  std::vector<uint8_t> plain(0x22000, 0x5A), out(plain.size());
  auto stream = Stored({plain});
  REQUIRE(ue3::LzxDecompress(stream.data(), stream.size(), out.data(), out.size(), 17));
  REQUIRE(out == plain);
}
TEST_CASE("LZX also accepts padded stored blocks", "[shader-prebuild]") {
  auto stream = Stored({{1, 2, 3}, {4, 5, 6, 7, 8}});
  stream.insert(stream.begin() + 19, 0);  // four-byte header + repeat offsets + three stored bytes
  std::array<uint8_t, 8> out{};
  REQUIRE(ue3::LzxDecompress(stream.data(), stream.size(), out.data(), out.size(), 17));
  REQUIRE(out == std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
}
TEST_CASE("Malformed shader containers are rejected", "[shader-prebuild]") {
  auto data = Container(false);
  data.resize(data.size() - 1);
  unsigned found = 0;
  ue3::shader_prebuild::PackageScanner scanner([&](bool, const uint8_t*, uint32_t, uint32_t) { ++found; });
  scanner.Scan(data);
  REQUIRE(found == 0);
}
TEST_CASE("Damaged compressed packages cannot become successful empty indexes", "[shader-prebuild]") {
  std::vector<uint8_t> data;
  Be(data, 0x9E2A83C1u);
  ue3::shader_prebuild::PackageScanner scanner([](bool, const uint8_t*, uint32_t, uint32_t) {});
  REQUIRE(scanner.Scan(data).failed_chunks == 1);
}
TEST_CASE("Uncompressed UE3 package tags are not mistaken for broken compression", "[shader-prebuild]") {
  std::vector<uint8_t> data;
  for (uint32_t word : {0x9E2A83C1u, 575u, 12345u, 0u}) Be(data, word);
  ue3::shader_prebuild::PackageScanner scanner([](bool, const uint8_t*, uint32_t, uint32_t) {});
  REQUIRE(scanner.Scan(data).failed_chunks == 0);
}
