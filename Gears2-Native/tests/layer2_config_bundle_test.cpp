// Unit tests for the UE3 config bundle reader / writer / edits (ue3/config_bundle.h, notes/64), on a small bundle built
// here in the same format as GearGame\CookedXenon\Coalesced_*.bin. Game-free. Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "ue3/config_bundle.h"

namespace {
void I32(std::vector<uint8_t>& v, int32_t x) {
  const uint32_t u = uint32_t(x);
  v.push_back(uint8_t(u >> 24)); v.push_back(uint8_t(u >> 16)); v.push_back(uint8_t(u >> 8)); v.push_back(uint8_t(u));
}
void Wide(std::vector<uint8_t>& v, const std::string& s) {  // UTF-16LE with terminator, negative length
  I32(v, -int32_t(s.size() + 1));
  for (char c : s) { v.push_back(uint8_t(c)); v.push_back(0); }
  v.push_back(0); v.push_back(0);
}
void Narrow(std::vector<uint8_t>& v, const std::string& s) {
  I32(v, int32_t(s.size() + 1));
  for (char c : s) v.push_back(uint8_t(c));
  v.push_back(0);
}
// Two files: an engine ini with [SystemSettings] (one narrow value, one empty value) and a camera ini.
std::vector<uint8_t> Sample() {
  std::vector<uint8_t> v;
  I32(v, 2);
  Wide(v, "..\\GearGame\\Config\\Xe-GearEngine.ini");
  I32(v, 1);
  Wide(v, "SystemSettings");
  I32(v, 3);
  Wide(v, "MotionBlur"); Wide(v, "True");
  Wide(v, "MaxAnisotropy"); Narrow(v, "4");
  Wide(v, "Empty"); I32(v, 0);
  Wide(v, "..\\GearGame\\Config\\GearCamera.ini");
  I32(v, 2);
  Wide(v, "GearGame.GameplayCam_Default");
  I32(v, 1);
  Wide(v, "FOVAngle"); Wide(v, "80");
  Wide(v, "GearGame.GameplayCam_Targeting");
  I32(v, 2);
  Wide(v, "FOVAngle"); Wide(v, "50   // zoomed");
  Wide(v, "Other"); Wide(v, "x");
  return v;
}
}  // namespace

TEST_CASE("config bundle: read and write back byte for byte", "[ue3][config]") {
  const std::vector<uint8_t> b = Sample();
  ue3::ConfigBundle c;
  std::string err;
  REQUIRE(ue3::ParseConfigBundle(b, c, &err));
  REQUIRE(c.files.size() == 2);
  CHECK(c.files[0].sections[0].keys[1].value.Utf8() == "4");
  CHECK_FALSE(c.files[0].sections[0].keys[1].value.wide);
  CHECK_FALSE(c.files[0].sections[0].keys[2].value.present);
  CHECK(ue3::WriteConfigBundle(c) == b);
  std::vector<uint8_t> cut(b.begin(), b.end() - 3);
  CHECK_FALSE(ue3::ParseConfigBundle(cut, c, &err));
}

TEST_CASE("config bundle: set, scale, add", "[ue3][config]") {
  ue3::ConfigBundle c;
  REQUIRE(ue3::ParseConfigBundle(Sample(), c));
  std::string err;
  const auto edits = ue3::ParseConfigEdits(
      "Xe-GearEngine.ini|SystemSettings|MotionBlur=False; xe-gearengine.ini|systemsettings|MaxAnisotropy=16;"
      "GearCamera.ini|*|FOVAngle*=1.25; Xe-GearEngine.ini|SystemSettings|DepthOfField=False;"
      "Xe-GearEngine.ini|NewSection|Key=1; bad entry", &err);
  CHECK(edits.size() == 5);
  CHECK_FALSE(err.empty());  // "bad entry" reported
  std::vector<std::string> log;
  CHECK(ue3::ApplyConfigEdits(c, edits, &log) == 6);  // 2 set, 2 scaled, 2 added
  const auto& sys = c.files[0].sections[0];
  CHECK(sys.keys[0].value.Utf8() == "False");
  CHECK(sys.keys[0].value.wide);
  CHECK(sys.keys[1].value.Utf8() == "16");
  CHECK(sys.keys.back().key.Utf8() == "DepthOfField");
  CHECK(c.files[0].sections.back().name.Utf8() == "NewSection");
  CHECK(c.files[1].sections[0].keys[0].value.Utf8() == "100");
  CHECK(c.files[1].sections[1].keys[0].value.Utf8() == "62.5   // zoomed");  // the comment is kept
  CHECK(c.files[1].sections[1].keys[1].value.Utf8() == "x");                // other keys untouched
  // The edited bundle reads back.
  ue3::ConfigBundle again;
  CHECK(ue3::ParseConfigBundle(ue3::WriteConfigBundle(c), again));
}

namespace {
std::string Hex(const ue3::Sha1Digest& d) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (uint8_t b : d) s += k[b >> 4], s += k[b & 15];
  return s;
}
ue3::Sha1Digest Sha(const std::string& s) { return ue3::Sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
}  // namespace

TEST_CASE("SHA-1 (FIPS 180 test vectors)", "[ue3][config]") {
  CHECK(Hex(Sha("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(Hex(Sha("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(Hex(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(Hex(Sha(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  CHECK(Hex(Sha(std::string(64, 'x'))) == Hex(Sha(std::string(64, 'x'))));
}

TEST_CASE("start-up hash table entry found by name and hash", "[ue3][config]") {
  // "<name>\0" + 20-byte hash entries, as in the game image (job 257).
  const ue3::Sha1Digest a = Sha("bundle one"), b = Sha("bundle two");
  std::vector<uint8_t> img(64, 0xEE);
  auto put = [&](const std::string& name, const ue3::Sha1Digest& h) {
    img.insert(img.end(), name.begin(), name.end());
    img.push_back(0);
    img.insert(img.end(), h.begin(), h.end());
  };
  put("coalesced_int.bin", a);
  put("coalesced_deu.bin", b);
  put("coalesced_int.bin", b);  // same name, other hash: not ours
  const auto hits = ue3::FindFileHash(img.data(), img.size(), "coalesced_int.bin", a);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0] == 64 + 18);
  CHECK(ue3::FindFileHash(img.data(), img.size(), "coalesced_fra.bin", a).empty());
  CHECK(ue3::FindFileHash(img.data(), img.size(), "Coalesced_int.bin", a).empty());  // stored lower case
}
