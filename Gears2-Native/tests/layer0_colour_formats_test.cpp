// Unit tests for xenos/colour_formats.h (M2 colour). Game-free: every input is synthetic.
// Golden tables (tests/golden/layer0_colour_formats_golden.inc) come from tools/golden/gn_colour_formats_golden.cpp,
// which runs verbatim Xenia Canary e6cd2e4bc code (xenos.cc Float7e3To32, the d3d12_render_target_cache.cc clear
// switch, pixel_formats.xesli XePackFixed / XeUnpackR16G16Edram compiled as C++) and an op-for-op execution of the
// DXBC 7e3 encoder (dxbc_shader_translator_om.cc 3816-3889), and checks this header against them exhaustively.
// Hand cases cite the file:line whose formula gives the expected value (Canary paths relative to src/xenia/gpu/; "SDK
// 32bpp disasm" = rexglue SDK src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h).
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "golden/layer0_colour_formats_golden.inc"
#include "xenos/colour_formats.h"
#include "xenos/edram.h"

namespace xc = xenos::colour;
using xc::ColorFormat;
using xc::DestFormat;
using xc::NumFormat;
using xc::Reference;

namespace {
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
float F(uint32_t u) { return std::bit_cast<float>(u); }
uint64_t Fnv(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xFF;
    h *= 0x100000001B3ull;
  }
  return h;
}
// RB_COPY_DEST_INFO from fields (SDK registers.h 872-879).
uint32_t DestInfo(DestFormat f, NumFormat nf = NumFormat::kUnsignedRepeatingFraction, int32_t bias = 0,
                  bool swap = false, xc::Endian128 e = xc::Endian128::kNone) {
  return uint32_t(e) | (uint32_t(f) << 7) | (uint32_t(nf) << 13) | ((uint32_t(bias) & 63u) << 16) |
         (uint32_t(swap) << 24);
}
// Independent 7e3 reference: the nearest representable value in [0, 31.875] (computed in double), ties to the even
// code. Codes are monotonic in value, so a binary search over Float7e3To32's table works.
uint32_t Nearest7e3(double v) {
  if (!(v > 0.0)) return 0;  // NaN, negative, zero
  if (v >= 31.875) return 0x3FF;
  uint32_t lo = 0, hi = 0x3FF;  // value(lo) <= v < value(hi)
  while (hi - lo > 1) {
    const uint32_t mid = (lo + hi) / 2;
    (double(xc::Float7e3To32(mid)) <= v ? lo : hi) = mid;
  }
  const double dl = v - double(xc::Float7e3To32(lo)), dh = double(xc::Float7e3To32(hi)) - v;
  if (dl < dh) return lo;
  if (dh < dl) return hi;
  return (lo & 1) ? hi : lo;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("7e3 decode matches Canary Float7e3To32 for all 1024 codes", "[colour_formats]") {
  for (uint32_t c = 0; c < 1024; ++c) REQUIRE(Bits(xc::Float7e3To32(c)) == golden_colour::kFloat7e3To32[c]);
  // xenos.cc 104-120 by hand: exponent bias 3, 7-bit mantissa, denormals below 0.25.
  CHECK(xc::Float7e3To32(0x180) == 1.0f);
  CHECK(xc::Float7e3To32(0x3FF) == 31.875f);
  CHECK(xc::Float7e3To32(0x080) == 0.25f);
  CHECK(xc::Float7e3To32(0x001) == 0.25f / 128.0f);
  CHECK(xc::Float7e3To32(0x07F) == 0.25f * 127.0f / 128.0f);
  CHECK(xc::Float7e3To32(0x400 | 0x180) == 1.0f);  // only the low 10 bits are used
}

TEST_CASE("7e3 encode: golden spots, stride hash, round trip, nearest-even", "[colour_formats]") {
  for (const auto& s : golden_colour::kEncode7e3Spots) {
    INFO("in 0x" << std::hex << s.in_bits);
    CHECK(xc::Float32To7e3(F(s.in_bits)) == s.code);
  }
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; i += 0x1000) h = Fnv(h, xc::Float32To7e3(F(uint32_t(i))));
  CHECK(h == golden_colour::kEncode7e3Stride4096Fnv);
  // Every code survives decode -> encode.
  for (uint32_t c = 0; c < 1024; ++c) REQUIRE(xc::Float32To7e3(xc::Float7e3To32(c)) == c);
  // PreClamped == Unclamped inside [0, 31.875].
  for (uint32_t c = 0; c < 1024; ++c) REQUIRE(xc::PreClampedFloat32To7e3(xc::Float7e3To32(c)) == c);
  // Every fp16 value (the host render-target storage) against the independent nearest-even search.
  for (uint32_t hb = 0; hb < 0x10000; ++hb) {
    const float f = xc::HalfToFloat(uint16_t(hb));
    INFO("half 0x" << std::hex << hb);
    REQUIRE(xc::Float32To7e3(f) == Nearest7e3(double(f)));
  }
  // Ties go to even (om.cc 3866-3872, "(b + 0x7FFF + ((b >> 16) & 1)) >> 16").
  CHECK(xc::Float32To7e3(1.0f + 1.0f / 256.0f) == 0x180);
  CHECK(xc::Float32To7e3(1.0f + 3.0f / 256.0f) == 0x182);
  // Clamp: max(x, 0) then min(x, 31.875), NaN -> 0 (om.cc 3875-3889).
  CHECK(xc::Float32To7e3(-5.0f) == 0);
  CHECK(xc::Float32To7e3(-0.0f) == 0);
  CHECK(xc::Float32To7e3(std::nanf("")) == 0);
  CHECK(xc::Float32To7e3(INFINITY) == 0x3FF);
  CHECK(xc::Float32To7e3(100.0f) == 0x3FF);
}

TEST_CASE("7e3 alpha, word pack/unpack, render-target clamp", "[colour_formats]") {
  // uint(saturate(a) * 3 + 0.5) (d3d12_render_target_cache.cc 3509-3517).
  CHECK(xc::FloatToAlpha2(0.0f) == 0);
  CHECK(xc::FloatToAlpha2(0.16f) == 0);
  CHECK(xc::FloatToAlpha2(0.17f) == 1);
  CHECK(xc::FloatToAlpha2(0.5f) == 2);
  CHECK(xc::FloatToAlpha2(0.83f) == 2);
  CHECK(xc::FloatToAlpha2(0.84f) == 3);
  CHECK(xc::FloatToAlpha2(1.0f) == 3);
  CHECK(xc::FloatToAlpha2(7.0f) == 3);
  CHECK(xc::FloatToAlpha2(-1.0f) == 0);
  CHECK(xc::FloatToAlpha2(std::nanf("")) == 0);
  for (uint32_t a = 0; a < 4; ++a) CHECK(xc::FloatToAlpha2(xc::Alpha2ToFloat(a)) == a);
  CHECK(xc::Alpha2ToFloat(1) == 1.0f / 3.0f);
  // Word layout: R bits 0-9, G 10-19, B 20-29, A 30-31 (pixel_formats.xesli 403-419).
  CHECK(xc::Pack7e3({1.0f, 2.0f, 0.25f, 1.0f}) == (0x180u | (0x200u << 10) | (0x080u << 20) | (3u << 30)));
  for (uint32_t c = 0; c < 1024; c += 7) {
    for (uint32_t a = 0; a < 4; ++a) {
      const uint32_t w = c | (((c * 3) & 0x3FF) << 10) | (((c * 5 + 1) & 0x3FF) << 20) | (a << 30);
      REQUIRE(xc::Pack7e3(xc::Unpack7e3(w)) == w);
    }
  }
  const xc::Float4 wide{40.0f, -1.0f, 3.0f, 1.5f};
  CHECK(xc::Clamp7e3RenderTargetRange(wide, false) == wide);
  CHECK(xc::Clamp7e3RenderTargetRange(wide, true) == xc::Float4{31.875f, 0.0f, 3.0f, 1.0f});
}

TEST_CASE("Host fp16 7e3 render-target value -> guest 7e3 bits (M2 comparison)", "[colour_formats]") {
  // fp16: 1.0 = 0x3C00, 2.0 = 0x4000, 0.5 = 0x3800, 40.0 = 0x5100, -1.0 = 0xBC00, NaN 0x7E00, Inf 0x7C00.
  CHECK(xc::Host7e3Fp16ToGuest(0x3C00, 0x4000, 0x3800, 0x3800) ==
        (0x180u | (0x200u << 10) | (0x100u << 20) | (2u << 30)));
  CHECK(xc::Host7e3Fp16ToGuest(0x5100, 0xBC00, 0x7E00, 0x3C00) == (0x3FFu | (3u << 30)));
  CHECK(xc::Host7e3Fp16ToGuest(0x7C00, 0, 0, 0) == 0x3FFu);
  // Alpha keeps full fp16 precision on the host RT; quantised only here: 0.6 -> 2, 0.2 -> 1.
  CHECK((xc::Host7e3Fp16ToGuest(0, 0, 0, xc::FloatToHalf(0.6f)) >> 30) == 2);
  CHECK((xc::Host7e3Fp16ToGuest(0, 0, 0, xc::FloatToHalf(0.2f)) >> 30) == 1);
  // A host value between two 7e3 codes quantises like the guest would: fp16 1.0 + 3/1024 -> 0x180 (nearest).
  CHECK((xc::Host7e3Fp16ToGuest(0x3C03, 0, 0, 0) & 0x3FF) == 0x180);
  CHECK((xc::Host7e3Fp16ToGuest(0x3C05, 0, 0, 0) & 0x3FF) == 0x181);
  // 64-bit form, R in the low 16 bits.
  CHECK(xc::Host7e3Fp16ToGuest(uint64_t(0x3C00) | (uint64_t(0x3800) << 48)) == (0x180u | (2u << 30)));
  // Guest -> host -> guest is lossless for every RGB code and alpha (all 7e3 values are exact in fp16).
  for (uint32_t c = 0; c < 1024; ++c) {
    for (uint32_t a = 0; a < 4; ++a) {
      const uint32_t w = c | ((1023 - c) << 10) | (((c * 9) & 0x3FF) << 20) | (a << 30);
      REQUIRE(xc::Host7e3Fp16ToGuest(xc::Guest7e3ToHostFp16(w)) == w);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("fp16 conversions", "[colour_formats]") {
  // Every half survives f16tof32 -> f32tof16 (NaN payloads included).
  for (uint32_t h = 0; h < 0x10000; ++h) REQUIRE(xc::FloatToHalf(xc::HalfToFloat(uint16_t(h))) == h);
  CHECK(xc::HalfToFloat(0x3C00) == 1.0f);
  CHECK(xc::HalfToFloat(0x0001) == std::ldexp(1.0f, -24));
  CHECK(xc::HalfToFloat(0x03FF) == std::ldexp(1023.0f, -24));
  CHECK(xc::HalfToFloat(0x7BFF) == 65504.0f);
  CHECK(Bits(xc::HalfToFloat(0x8000)) == 0x80000000u);
  // f32tof16 round to nearest even, no clamp (xesl.xesli 1237).
  CHECK(xc::FloatToHalf(65504.0f) == 0x7BFF);
  CHECK(xc::FloatToHalf(65519.996f) == 0x7BFF);
  CHECK(xc::FloatToHalf(65520.0f) == 0x7C00);
  CHECK(xc::FloatToHalf(-1e10f) == 0xFC00);
  CHECK(xc::FloatToHalf(1.0f + 1.0f / 2048.0f) == 0x3C00);              // tie -> even
  CHECK(xc::FloatToHalf(1.0f + 3.0f / 2048.0f) == 0x3C02);              // tie -> even
  CHECK(xc::FloatToHalf(std::ldexp(1.0f, -25)) == 0x0000);              // tie at half the smallest denormal
  CHECK(xc::FloatToHalf(std::ldexp(1.5f, -25)) == 0x0001);
  CHECK(xc::FloatToHalf(std::ldexp(1.0f, -14) * (1023.5f / 1024.0f)) == 0x0400);  // denormal rounds up to normal
  CHECK((xc::FloatToHalf(std::nanf("")) & 0x7C00) == 0x7C00);
  CHECK((xc::FloatToHalf(std::nanf("")) & 0x3FF) != 0);
#if defined(__FLT16_MAX__) && !defined(_WIN32)
  // The compiler's own IEEE binary32 -> binary16 conversion as an independent reference. Not on Windows: clang
  // targeting MSVC has no compiler-rt builtins for _Float16 (__extendhfsf2 / __truncsfhf2 are unresolved at link, job 108).
  for (uint32_t h = 0; h < 0x10000; ++h) {
    _Float16 x;
    const uint16_t hh = uint16_t(h);
    __builtin_memcpy(&x, &hh, 2);
    const float ref = float(x);
    if (ref != ref) continue;
    REQUIRE(Bits(xc::HalfToFloat(hh)) == Bits(ref));
  }
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; i += 4093) {
    const float f = F(uint32_t(i));
    if (f != f) continue;
    const _Float16 x = _Float16(f);
    uint16_t ref;
    __builtin_memcpy(&ref, &x, 2);
    INFO("float 0x" << std::hex << i);
    REQUIRE(xc::FloatToHalf(f) == ref);
  }
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("16_16 fixed point: decode -32...32, SDK vs Canary clamp", "[colour_formats]") {
  uint64_t hc = 0xCBF29CE484222325ull, hs = hc;
  for (uint32_t c = 0; c < 0x10000; ++c) {
    hc = Fnv(hc, Bits(xc::Fixed16ResolveDecode(c, Reference::kCanary)));
    hs = Fnv(hs, Bits(xc::Fixed16ResolveDecode(c, Reference::kSdk)));
  }
  CHECK(hc == golden_colour::kFixed16DecodeCanaryFnv);
  CHECK(hs == golden_colour::kFixed16DecodeSdkFnv);
  // xenos.h 303-306 "0x8001 = -32, 0x7FFF = 32"; pixel_formats.xesli 422-447 max(-32, i * 32/32767).
  CHECK(xc::Fixed16ResolveDecode(0x7FFF, Reference::kCanary) == 32.0f);
  CHECK(xc::Fixed16ResolveDecode(0x8001, Reference::kCanary) == -32.0f);
  CHECK(xc::Fixed16ResolveDecode(0x8000, Reference::kCanary) == -32.0f);
  CHECK(xc::Fixed16ResolveDecode(0x0000, Reference::kCanary) == 0.0f);
  // SDK 32bpp disasm: mul l(0.000977) [0x3a800100]; max l(-1.0): everything below -1 is -1.
  CHECK(xc::Fixed16ResolveDecode(0x7FFF, Reference::kSdk) == 32.0f);
  CHECK(xc::Fixed16ResolveDecode(0x8001, Reference::kSdk) == -1.0f);
  CHECK(xc::Fixed16ResolveDecode(0xFC00, Reference::kSdk) == -1.0f);  // -1024 -> -1.00003 -> -1
  CHECK(xc::Fixed16ResolveDecode(0xFC01, Reference::kSdk) == float(-1023) * (32.0f / 32767.0f));
  CHECK(xc::Fixed16ResolveDecode(0x10000 | 0x0400, Reference::kSdk) == float(1024) * (32.0f / 32767.0f));
  CHECK(xc::Fixed16RovDecode(0x8000) == -32768.0f * (32.0f / 32767.0f));
}

TEST_CASE("16_16 fixed point: guest encode and the host SNORM (exp bias -5) equivalence", "[colour_formats]") {
  // om.cc 1600-1625: v * 32767/32 +- 0.5, ftoi; [-32, 32] clamp first.
  CHECK(xc::Fixed16GuestEncode(32.0f) == 0x7FFF);
  CHECK(xc::Fixed16GuestEncode(-32.0f) == 0x8001);
  CHECK(xc::Fixed16GuestEncode(100.0f) == 0x7FFF);
  CHECK(xc::Fixed16GuestEncode(-100.0f) == 0x8001);
  CHECK(xc::Fixed16GuestEncode(0.0f) == 0x0000);
  CHECK(xc::Fixed16GuestEncode(1.0f) == 0x0400);  // 1023.96875 + 0.5 -> 1024
  CHECK(xc::Fixed16GuestEncode(std::nanf("")) == 0x8001);
  CHECK(xc::HostSnorm16FromShaderValue(32.0f) == 0x7FFF);
  CHECK(xc::HostSnorm16FromShaderValue(-32.0f) == 0x8001);
  CHECK(xc::HostSnorm16FromShaderValue(-40.0f) == 0x8001);
  CHECK(xc::HostSnorm16FromShaderValue(std::nanf("")) == 0);
  // Every guest code except 0x8000 (SNORM's second -1) decodes to a value both encoders map back to the same bits:
  // host SNORM bits == guest bits.
  for (uint32_t c = 0; c < 0x10000; ++c) {
    if (c == 0x8000) continue;
    const float v = xc::Fixed16ResolveDecode(c, Reference::kCanary);
    INFO("code 0x" << std::hex << c);
    REQUIRE(xc::HostSnorm16FromShaderValue(v) == c);
    REQUIRE(xc::Fixed16GuestEncode(v) == c);
    REQUIRE(xc::HostSnorm16ToGuestValue(c, Reference::kCanary) == v);
  }
  // Arbitrary shader values (a 2,000,001-point sweep of [-32, 32]): the guest ROV formula rounds v * 1023.96875 to
  // float before adding 0.5, the host SNORM conversion rounds the exact product (to even here). They differ by one
  // code, only within 1/1024 of a .5 point - about 0.07% of values (1298 of the sweep with g++/clang++ x86-64).
  uint32_t differ = 0;
  for (uint32_t i = 0; i <= 2000000; ++i) {
    const float v = -32.0f + 64.0f * float(i) / 2000000.0f;
    const uint32_t g = xc::Fixed16GuestEncode(v), h = xc::HostSnorm16FromShaderValue(v);
    if (g == h) continue;
    ++differ;
    const double scaled = std::fabs(double(v) * (32767.0 / 32.0));
    INFO("v " << v);
    REQUIRE(std::fabs(scaled - std::floor(scaled) - 0.5) < 1.0 / 1024.0);
    const bool one_code_apart = ((g - h) & 0xFFFFu) == 1u || ((h - g) & 0xFFFFu) == 1u;
    REQUIRE(one_code_apart);
  }
  CHECK(differ < 2000);
}

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("unorm / signed / integer packing (XePackFixed) and the SDK unorm-only pack", "[colour_formats]") {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t hb = 0; hb < 0x10000; ++hb) {
    const float v = xc::HalfToFloat(uint16_t(hb));
    for (uint32_t bits : {2u, 8u, 10u, 11u, 16u}) {
      for (uint32_t nf = 0; nf < 4; ++nf) h = Fnv(h, xc::PackFixedCanary(v, bits, NumFormat(nf)));
    }
  }
  CHECK(h == golden_colour::kPackFixedHalvesFnv);
  // saturate * max + 0.5 (pixel_formats.xesli 121-123; SDK 32bpp disasm mov_sat / mad / ftou).
  CHECK(xc::FloatToUNorm(0.5f, 8) == 128);
  CHECK(xc::FloatToUNorm(127.4f / 255.0f, 8) == 127);
  CHECK(xc::FloatToUNorm(2.0f, 8) == 255);
  CHECK(xc::FloatToUNorm(-1.0f, 8) == 0);
  CHECK(xc::FloatToUNorm(std::nanf(""), 10) == 0);
  CHECK(xc::FloatToUNorm(1.0f, 16) == 0xFFFF);
  for (uint32_t b = 0; b < 256; ++b) REQUIRE(xc::FloatToUNorm(xc::UNorm8ToFloat(b), 8) == b);
  for (uint32_t b = 0; b < 1024; ++b) REQUIRE(xc::FloatToUNorm(xc::UNorm10ToFloat(b), 10) == b);
  // Signed: clamp(-1, 1) * 127 +- 0.5, truncated, masked (pixel_formats.xesli 108-113).
  CHECK(xc::PackFixed(-0.5f, 8, NumFormat::kSignedRepeatingFraction, Reference::kCanary) == 0xC0);  // -64
  CHECK(xc::PackFixed(-1.0f, 16, NumFormat::kSignedRepeatingFraction, Reference::kCanary) == 0x8001);
  CHECK(xc::PackFixed(-0.5f, 8, NumFormat::kSignedRepeatingFraction, Reference::kSdk) == 0x00);  // SDK: unorm
  CHECK(xc::PackFixed(300.0f, 8, NumFormat::kUnsignedInteger, Reference::kCanary) == 255);
  CHECK(xc::PackFixed(-300.0f, 8, NumFormat::kSignedInteger, Reference::kCanary) == 0x80);
}

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("RB_COPY_DEST_INFO decode and exponent bias", "[colour_formats]") {
  const uint32_t reg = DestInfo(DestFormat::k_16_16_16_16_FLOAT, NumFormat::kFloat, -5, true, xc::Endian128::k8in32);
  const xc::CopyDestInfo d = xc::DecodeCopyDestInfo(reg);
  CHECK(d.endian == xc::Endian128::k8in32);
  CHECK(d.format == DestFormat::k_16_16_16_16_FLOAT);
  CHECK(d.number == NumFormat::kFloat);
  CHECK(d.exp_bias == -5);
  CHECK(d.swap);
  CHECK(xc::CopyDestExpBias(0x3Fu << 16) == -1);
  CHECK(xc::CopyDestExpBias(0x20u << 16) == -32);
  CHECK(xc::CopyDestExpBias(0x1Fu << 16) == 31);
  CHECK(xc::ExpBiasFactor(0) == 1.0f);
  CHECK(xc::ExpBiasFactor(-5) == 1.0f / 32.0f);
  CHECK(xc::ExpBiasFactor(31) == std::ldexp(1.0f, 31));
  CHECK(xc::ExpBiasFactor(-32) == std::ldexp(1.0f, -32));
  // draw_util.cc 1666-1675: the -1...1 hack adds 5 (capped at 31) for 16_16 / 16_16_16_16 only.
  CHECK(xc::ResolveExpBias(-5, ColorFormat::k_16_16, true) == 0);
  CHECK(xc::ResolveExpBias(30, ColorFormat::k_16_16_16_16, true) == 31);
  CHECK(xc::ResolveExpBias(-5, ColorFormat::k_8_8_8_8, true) == -5);
  CHECK(xc::ResolveExpBias(-5, ColorFormat::k_16_16, false) == -5);
}

TEST_CASE("endian swap on store", "[colour_formats]") {
  using E = xc::Endian128;
  CHECK(xc::EndianSwap32(0x00112233, E::kNone) == 0x00112233u);
  CHECK(xc::EndianSwap32(0x00112233, E::k8in16) == 0x11003322u);
  CHECK(xc::EndianSwap32(0x00112233, E::k8in32) == 0x33221100u);
  CHECK(xc::EndianSwap32(0x00112233, E::k16in32) == 0x22330011u);
  CHECK(xc::EndianSwap64(0x0011223344556677ull, E::k8in64) == 0x7766554433221100ull);
  CHECK(xc::EndianSwap64(0x0011223344556677ull, E::k8in32) == 0x3322110077665544ull);
}

TEST_CASE("colour resolve pipeline, hand cases", "[colour_formats]") {
  const xc::ResolveOptions sdk{Reference::kSdk}, can{Reference::kCanary};
  SECTION("8888 one sample is the identity, with 8in32 swap") {
    const uint32_t w = 0x80402010;
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, &w, 1, DestInfo(DestFormat::k_8_8_8_8), sdk) == w);
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, &w, 1, DestInfo(DestFormat::k_8_8_8_8), can) == w);
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, &w, 1,
                                 DestInfo(DestFormat::k_8_8_8_8, {}, 0, false, xc::Endian128::k8in32), sdk) ==
          0x10204080u);
    // R/B swap (resolve.xesli 807-810).
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, &w, 1, DestInfo(DestFormat::k_8_8_8_8, {}, 0, true), sdk) ==
          0x80102040u);
  }
  SECTION("4x average: one red sample of four -> 0.25 -> 64, no intermediate rounding") {
    const uint32_t w[4] = {0x000000FF, 0, 0, 0};
    // (1/255*255 ... ) (1 + 0 + 0 + 0) * (1 * 0.5 * 0.5) = 0.25; 0.25 * 255 + 0.5 = 64.25 -> 64.
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, w, 4, DestInfo(DestFormat::k_8_8_8_8), sdk) == 0x40u);
    const uint32_t w2[4] = {0x01, 0x02, 0x02, 0x02};  // 7/4 = 1.75 -> 2 (rounded once at the end)
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, w2, 4, DestInfo(DestFormat::k_8_8_8_8), sdk) == 0x02u);
  }
  SECTION("8888_GAMMA source: Canary PWL-degammas, the SDK does not") {
    const uint32_t w = 0xFF000080;  // R = 128, A = 255
    const uint32_t info = DestInfo(DestFormat::k_8_8_8_8, {}, 0);
    // SDK: 128/255 * 255 + 0.5 -> 128 (and it's a raw copy anyway).
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8_GAMMA, &w, 1, info, sdk) == 0xFF000080u);
    // Canary (resolve.xesli 721-733, gamma.h): 128/255 -> piece 96..192: 512 - 256 = 256, + trunc(256 * 4/1024) = 257,
    // / 1023 = 0.25122; * 255 + 0.5 = 64.56 -> 64. Alpha stays 255.
    CHECK(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8_GAMMA, &w, 1, info, can) == 0xFF000040u);
    CHECK(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8_GAMMA, info, true, Reference::kSdk));
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8_GAMMA, info, true, Reference::kCanary));
  }
  SECTION("7e3 source -> 16_16_16_16_FLOAT, with swap") {
    const uint32_t w = 0x180u | (0x200u << 10) | (0u << 20) | (3u << 30);  // 1, 2, 0, alpha 1
    const uint64_t r = xc::ResolveColourPixel(ColorFormat::k_2_10_10_10_FLOAT, &w, 1,
                                              DestInfo(DestFormat::k_16_16_16_16_FLOAT, NumFormat::kFloat, 0, true), sdk);
    CHECK(r == (0x0000ull | (0x4000ull << 16) | (0x3C00ull << 32) | (0x3C00ull << 48)));
  }
  SECTION("16_16 source, signed dest, exp bias -5: SDK and Canary differ") {
    const uint32_t w = 0x8001u | (0x4000u << 16);  // R = -32, G = 16384 * 32/32767
    const uint32_t info = DestInfo(DestFormat::k_16_16, NumFormat::kSignedRepeatingFraction, -5);
    // Canary: R -32 / 32 = -1 -> -1 * 32767 - 0.5 -> 0x8001; G 16.0005 / 32 = 0.500015 -> * 32767 + 0.5 = 16384.5
    // -> 16384 (pixel_formats.xesli 108-113).
    CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, info, can) == (0x8001u | (0x4000u << 16)));
    // SDK: R max(-1, -32) = -1, / 32 -> -0.03125 -> unorm 0; G 0.500015 * 65535 + 0.5 (SDK 32bpp disasm case 25).
    float g = (float(16384) * (32.0f / 32767.0f)) * (1.0f / 32.0f) * 65535.0f;
    g = g + 0.5f;
    CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, info, sdk) == (uint32_t(g) << 16));
  }
  SECTION("2x 32_FLOAT average and bias") {
    const uint32_t w[2] = {Bits(1.0f), Bits(2.0f)};
    CHECK(xc::ResolveColourPixel(ColorFormat::k_32_FLOAT, w, 2, DestInfo(DestFormat::k_32_FLOAT, NumFormat::kFloat),
                                 sdk) == Bits(1.5f));
    CHECK(xc::ResolveColourPixel(ColorFormat::k_32_FLOAT, w, 2,
                                 DestInfo(DestFormat::k_32_FLOAT, NumFormat::kFloat, 2), sdk) == Bits(6.0f));
  }
  SECTION("fp16 dest passes NaN and Inf through, no clamp") {
    const uint32_t w = 0x7E00u | (0x7C00u << 16);  // 16_16_FLOAT: NaN, +Inf
    const uint32_t r = uint32_t(xc::ResolveColourPixel(
        ColorFormat::k_16_16_FLOAT, &w, 1, DestInfo(DestFormat::k_16_16_FLOAT, NumFormat::kFloat, 1), sdk));
    CHECK((r & 0x7C00u) == 0x7C00u);
    CHECK((r & 0x3FFu) != 0u);
    CHECK((r >> 16) == 0x7C00u);
    const uint32_t big = Bits(70000.0f);
    CHECK(xc::ResolveColourPixel(ColorFormat::k_32_FLOAT, &big, 1,
                                 DestInfo(DestFormat::k_16_16_FLOAT, NumFormat::kFloat), sdk) == 0x7C00u);
  }
  SECTION("64bpp 16_16_16_16 source, 4x, unorm dest") {
    // Samples (dword0, dword1): R low16 of dword0, G high16, B low16 of dword1, A high16 (pixel_formats.xesli 442-447).
    const uint32_t w[8] = {0x7FFF, 0, 0x7FFF, 0, 0, 0, 0, 0};  // R = 32 in two samples
    // (32 + 32) * 0.25 = 16 -> unorm saturate -> 0xFFFF; bias -5: 0.5 -> 0.5 * 65535 + 0.5 = 32768.
    CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16_16_16, w, 4, DestInfo(DestFormat::k_16_16_16_16), sdk) == 0xFFFFull);
    CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16_16_16, w, 4, DestInfo(DestFormat::k_16_16_16_16, {}, -5), sdk) ==
          32768ull);
  }
  SECTION("16_16_EDRAM dest has no SDK case (raw float bits of R)") {
    const uint32_t w = Bits(0.5f);
    CHECK(xc::ResolveColourPixel(ColorFormat::k_32_FLOAT, &w, 1, DestInfo(DestFormat::k_16_16_EDRAM), sdk) == Bits(0.5f));
    CHECK(xc::ResolveColourPixel(ColorFormat::k_32_FLOAT, &w, 1, DestInfo(DestFormat::k_16_16_EDRAM), can) == 0x8000u);
  }
}

TEST_CASE("raw fast-path condition", "[colour_formats]") {
  const uint32_t plain = DestInfo(DestFormat::k_8_8_8_8);
  for (Reference r : {Reference::kSdk, Reference::kCanary}) {
    CHECK(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, plain, true, r));
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, plain, false, r));  // averaging
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, DestInfo(DestFormat::k_8_8_8_8, {}, 1), true, r));
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_2_10_10_10_FLOAT, DestInfo(DestFormat::k_2_10_10_10), true, r));
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_16_16, DestInfo(DestFormat::k_16_16), true, r));
    CHECK(xc::ColourResolveIsRaw(ColorFormat::k_2_10_10_10_AS_10_10_10_10,
                                 DestInfo(DestFormat::k_2_10_10_10_AS_16_16_16_16), true, r));
    CHECK(xc::ColourResolveIsRaw(ColorFormat::k_32_32_FLOAT, DestInfo(DestFormat::k_32_32_FLOAT, NumFormat::kFloat),
                                 true, r));
  }
  // Number format: Canary needs it to match, the SDK ignores it.
  const uint32_t signed8 = DestInfo(DestFormat::k_8_8_8_8, NumFormat::kSignedRepeatingFraction);
  CHECK(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, signed8, true, Reference::kSdk));
  CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, signed8, true, Reference::kCanary));
  CHECK(xc::ColourResolveIsRaw(ColorFormat::k_16_16_FLOAT, DestInfo(DestFormat::k_16_16_FLOAT), true, Reference::kSdk));
  CHECK_FALSE(
      xc::ColourResolveIsRaw(ColorFormat::k_16_16_FLOAT, DestInfo(DestFormat::k_16_16_FLOAT), true, Reference::kCanary));
}

// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("colour <-> depth raw bits and the 40-sample column swap", "[colour_formats]") {
  CHECK(xc::DepthStencilFromColourWord(0xAABBCCDDu) == xc::DepthStencilBits{0xAABBCCu, 0xDDu});
  CHECK(xc::ColourWordFromDepthStencil(0xAABBCC, 0xDD) == 0xAABBCCDDu);
  CHECK(xc::ColourWordFromDepthStencil(0x1AABBCC, 0x1DD) == 0xAABBCCDDu);  // masked to 24 / 8 bits
  for (uint32_t w : {0u, 1u, 0xFFFFFFFFu, 0x12345678u, 0x80000000u}) {
    const auto ds = xc::DepthStencilFromColourWord(w);
    CHECK(xc::ColourWordFromDepthStencil(ds.depth24, ds.stencil) == w);
  }
  using xenos::edram::Msaa;
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k1X, false) == 40);
  CHECK(xc::SwapTileHalfColumns(39, Msaa::k1X, false) == 79);
  CHECK(xc::SwapTileHalfColumns(40, Msaa::k1X, false) == 0);
  CHECK(xc::SwapTileHalfColumns(79, Msaa::k1X, false) == 39);
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k2X, false) == 40);
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k4X, false) == 20);
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k1X, true) == 20);
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k4X, true) == 10);
  CHECK(xc::SwapTileHalfColumns(0, Msaa::k1X, false, 2) == 80);
  // Same as the depth swap in the tested edram::OffsetBytes: the colour address of the swapped column equals the
  // depth address of the original one (1x, 32bpp, one tile row).
  xenos::edram::AddressParams pc, pd;
  pc.pitch_tiles = pd.pitch_tiles = 16;
  pd.is_depth = true;
  for (uint32_t x = 0; x < 80; ++x) {
    for (uint32_t y : {0u, 7u, 15u}) {
      REQUIRE(xenos::edram::OffsetBytes(x, y, 0, pd) ==
              xenos::edram::OffsetBytes(xc::SwapTileHalfColumns(x, Msaa::k1X, false), y, 0, pc));
    }
  }
}

TEST_CASE("resolve-clear values", "[colour_formats]") {
  // 64bpp order: d3d12_render_target_cache.cc 1535-1543 (Canary) vs SDK render_target_cache.cpp 1349.
  CHECK(xc::ColourClearValue(true, 0x11111111, 0x22222222, Reference::kCanary) == 0x1111111122222222ull);
  CHECK(xc::ColourClearValue(true, 0x11111111, 0x22222222, Reference::kSdk) == 0x2222222211111111ull);
  CHECK(uint32_t(xc::ColourClearValue(false, 0x11111111, 0x22222222, Reference::kCanary)) == 0x11111111u);
  CHECK(uint32_t(xc::ColourClearValue(false, 0x11111111, 0x22222222, Reference::kSdk)) == 0x11111111u);
  CHECK(xc::DepthClearValue(0xFFFFFF00u) == xc::DepthStencilBits{0xFFFFFFu, 0u});
  // Per-format host clear colour against the verbatim Canary switch (5385-5465; the SDK's 4575-4645 is identical).
  for (const auto& c : golden_colour::kClearCases) {
    const xc::HostColourClear m = xc::HostColourClearValue(ColorFormat(c.format), c.value);
    INFO("format " << c.format << " value 0x" << std::hex << c.value);
    for (int j = 0; j < 4; ++j) CHECK(Bits(m.rgba[j]) == c.rgba_bits[j]);
    CHECK(m.via_drawing == c.via_drawing);
  }
  // 16-bit formats as raw bits, not / 32 ("Disregarding the current -32...32 vs. -1...1 settings").
  const auto s16 = xc::HostColourClearValue(ColorFormat::k_16_16, 0x80017FFFull);
  CHECK(s16.rgba[0] == 32767.0f);
  CHECK(s16.rgba[1] == 32769.0f);
  CHECK(s16.through_uint_view);
  const auto f32 = xc::HostColourClearValue(ColorFormat::k_32_FLOAT, 0x3F800001ull);
  CHECK(f32.via_drawing);
  CHECK_FALSE(xc::HostColourClearValue(ColorFormat::k_32_FLOAT, 0x00FFFFFFull).via_drawing);
}

// ---------------------------------------------------------------------------------------------------------------------
// The colour resolves Gears of War 2 issues (coordinator, 2026-10-21): 8888->8888, 16_16_FLOAT->16_16_FLOAT,
// 16_16->16_16, 16_16_16_16_FLOAT->16_16_16_16_FLOAT, 7e3->16_16_16_16_FLOAT with exp_bias -3; 1x, no gamma.
TEST_CASE("Gears 2 resolve paths", "[colour_formats]") {
  for (Reference r : {Reference::kSdk, Reference::kCanary}) {
    const xc::ResolveOptions o{r};
    INFO("canary " << (r == Reference::kCanary));
    // 8888 -> 8888, bias 0, 1 sample: raw in both, and the full path is the identity for every byte value.
    CHECK(xc::ColourResolveIsRaw(ColorFormat::k_8_8_8_8, DestInfo(DestFormat::k_8_8_8_8), true, r));
    for (uint32_t b = 0; b < 256; ++b) {
      const uint32_t w = b | ((255 - b) << 8) | (((b * 7) & 255) << 16) | (((b * 13) & 255) << 24);
      REQUIRE(xc::ResolveColourPixel(ColorFormat::k_8_8_8_8, &w, 1, DestInfo(DestFormat::k_8_8_8_8), o) == w);
    }
    // fp16 -> fp16 (raw when the number format is float; Canary needs kFloat, the SDK any). Full path is lossless
    // for every half except NaN payloads, which are hardware-defined.
    for (DestFormat df : {DestFormat::k_16_16_FLOAT, DestFormat::k_16_16_16_16_FLOAT}) {
      const ColorFormat sf = df == DestFormat::k_16_16_FLOAT ? ColorFormat::k_16_16_FLOAT : ColorFormat::k_16_16_16_16_FLOAT;
      CHECK(xc::ColourResolveIsRaw(sf, DestInfo(df, NumFormat::kFloat), true, r));
      for (uint32_t h = 0; h < 0x10000; ++h) {
        const uint32_t w[2] = {h | ((h ^ 0x8000u) << 16), (h ^ 0x1234u) | (h << 16)};
        auto is_nan = [](uint32_t x) { return (x & 0x7C00u) == 0x7C00u && (x & 0x3FFu) != 0u; };
        if (is_nan(h) || is_nan(h ^ 0x1234u)) continue;
        const uint64_t want = df == DestFormat::k_16_16_FLOAT ? w[0] : (uint64_t(w[0]) | (uint64_t(w[1]) << 32));
        REQUIRE(xc::ResolveColourPixel(sf, w, 1, DestInfo(df, NumFormat::kFloat), o) == want);
      }
    }
    // 16_16 -> 16_16 is never raw (not bitwise equivalent, xenos.h 613-642), so it always goes through the
    // conversion.
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_16_16, DestInfo(DestFormat::k_16_16), true, r));
  }
  // 16_16 -> 16_16 values. Source R = 0x7FFF (32), G = 0xC000 (-16384 -> -16.0005).
  const uint32_t w = 0x7FFFu | (0xC000u << 16);
  const xc::ResolveOptions sdk{Reference::kSdk}, can{Reference::kCanary};
  // SDK (32bpp disasm case 4 decode max(-1); case 25 unorm encode): R 32 -> saturate 1 -> 0xFFFF; G -1 -> 0.
  CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, DestInfo(DestFormat::k_16_16), sdk) == 0x0000FFFFu);
  CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1,
                               DestInfo(DestFormat::k_16_16, NumFormat::kSignedRepeatingFraction), sdk) == 0x0000FFFFu);
  // Canary, unsigned fraction: same as the SDK here (G -16 saturates to 0).
  CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, DestInfo(DestFormat::k_16_16), can) == 0x0000FFFFu);
  // Canary, signed fraction with bias -5 (the inferred-lighting pattern): R 32/32 = 1 -> 0x7FFF; G -16.0005/32 =
  // -0.500015 -> * 32767 - 0.5 = -16384.5 -> -16384 = 0xC000. Bits survive. The SDK (unorm only) gives R 1.0 -> 0xFFFF,
  // G -1/32 -> 0: negative values are lost.
  const uint32_t signed_m5 = DestInfo(DestFormat::k_16_16, NumFormat::kSignedRepeatingFraction, -5);
  CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, signed_m5, can) == (0x7FFFu | (0xC000u << 16)));
  CHECK(xc::ResolveColourPixel(ColorFormat::k_16_16, &w, 1, signed_m5, sdk) == 0x0000FFFFu);
  // 7e3 -> 16_16_16_16_FLOAT, exp_bias -3 (x 1/8, exact): RGB = 7e3 value / 8 as fp16, alpha = (a2 / 3) / 8.
  const uint32_t info = DestInfo(DestFormat::k_16_16_16_16_FLOAT, NumFormat::kFloat, -3);
  for (Reference r : {Reference::kSdk, Reference::kCanary}) {
    CHECK_FALSE(xc::ColourResolveIsRaw(ColorFormat::k_2_10_10_10_FLOAT, info, true, r));
    for (uint32_t c = 0; c < 1024; ++c) {
      for (uint32_t a = 0; a < 4; ++a) {
        const uint32_t p = c | (((c * 3) & 0x3FF) << 10) | (((1023 - c) & 0x3FF) << 20) | (a << 30);
        const uint64_t got = xc::ResolveColourPixel(ColorFormat::k_2_10_10_10_FLOAT, &p, 1, info, {r});
        // Hand formula: resolve.xesli 886 (x 2^-3), pixel_formats.xesli 330-338 (f32tof16 per component).
        auto h = [](float v) { return uint64_t(xc::FloatToHalf(v * 0.125f)); };
        const uint64_t want = h(xc::Float7e3To32(c)) | (h(xc::Float7e3To32(c * 3)) << 16) |
                              (h(xc::Float7e3To32(1023 - c)) << 32) | (h(float(a) * (1.0f / 3.0f)) << 48);
        REQUIRE(got == want);
        // RGB / 8 is exact in fp16 for every 7e3 code (smallest 2^-11 / 8 = 2^-14 * 2^-... stays representable).
        REQUIRE(xc::HalfToFloat(uint16_t(got)) * 8.0f == xc::Float7e3To32(c));
      }
    }
  }
  // Hand values: 1.0 (0x180) -> 0.125 = fp16 0x3000; 31.875 (0x3FF) -> 3.984375 = 0x43F8; alpha 3 -> 0.125 = 0x3000;
  // alpha 1 -> 1/24 -> fp16 0x2955.
  const uint32_t p = 0x180u | (0x3FFu << 10) | (0u << 20) | (3u << 30);
  CHECK(xc::ResolveColourPixel(ColorFormat::k_2_10_10_10_FLOAT, &p, 1, info, sdk) ==
        (0x3000ull | (0x43F8ull << 16) | (0x0000ull << 32) | (0x3000ull << 48)));
  CHECK(xc::FloatToHalf((1.0f / 3.0f) * 0.125f) == 0x2955);
}
