// Layer 0 (game-free): Xenos shader microcode -> DXBC. See dxbc_gen.h. Each section below names the hlsl.cpp helper or
// generated line it reproduces; the maths rules and their reasons are documented there (and in notes/38), not repeated.
// Our code (MIT); the assembler it drives is the SDK's (Xenia, BSD).

#include <xenos/dxbc_gen.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

#include <rex/graphics/format/dxbc.h>

namespace xenos::dxbc_gen {

namespace {

// ---- The DXBC container checksum ----
// MD5 (RFC 1321, the RSA Data Security, Inc. MD5 Message-Digest Algorithm) over the container after its 20-byte
// fourcc + hash, with Direct3D's own finish: the bit count goes first in the last block (or into an extra block when
// fewer than 8 bytes are left), and the last dword is (bits >> 2) | 1. Written here from that description (the SDK's
// copy, thirdparty/dxbc/DXBCChecksum.cpp, lives inside its GPU plug-in DLL and is not exported); the local tests check
// it against that copy on every shader of the corpus.
void Md5Block(uint32_t st[4], const uint8_t* p) {
  static constexpr uint32_t kK[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static constexpr uint32_t kR[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
  uint32_t m[16];
  for (int i = 0; i < 16; ++i)
    m[i] = uint32_t(p[4 * i]) | (uint32_t(p[4 * i + 1]) << 8) | (uint32_t(p[4 * i + 2]) << 16) | (uint32_t(p[4 * i + 3]) << 24);
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    const uint32_t r = kR[(i >> 4) * 4 + (i & 3)];
    const uint32_t t = a + f + kK[i] + m[g];
    a = d;
    d = c;
    c = b;
    b = b + ((t << r) | (t >> (32 - r)));
  }
  st[0] += a;
  st[1] += b;
  st[2] += c;
  st[3] += d;
}
void DxbcChecksum(const uint8_t* container, uint32_t size_bytes, uint32_t hash[4]) {
  uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  const uint8_t* p = container + 20;
  const uint32_t size = size_bytes - 20, bits = size * 8, full = size & ~63u, last = size - full;
  for (uint32_t i = 0; i < full; i += 64) Md5Block(st, p + i);
  uint8_t blk[64] = {};
  const uint32_t tail = (bits >> 2) | 1;
  if (last >= 56) {
    std::memcpy(blk, p + full, last);
    blk[last] = 0x80;
    Md5Block(st, blk);
    uint8_t extra[64] = {};
    std::memcpy(extra, &bits, 4);
    std::memcpy(extra + 60, &tail, 4);
    Md5Block(st, extra);
  } else {
    std::memcpy(blk, &bits, 4);
    if (last) std::memcpy(blk + 4, p + full, last);
    blk[4 + last] = 0x80;
    std::memcpy(blk + 60, &tail, 4);
    Md5Block(st, blk);
  }
  std::memcpy(hash, st, 16);
}

namespace dx = rex::graphics::dxbc;
using namespace ::xenos::ucode;

using dx::Dest;
using dx::Index;
using dx::Src;

// Swizzle from four component numbers.
constexpr uint32_t Sw(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | (y << 2) | (z << 4) | (w << 6); }
constexpr uint32_t kX = 0b0001, kY = 0b0010, kZ = 0b0100, kW = 0b1000, kXY = 0b0011, kYZ = 0b0110, kXYZ = 0b0111,
                   kXYZW = 0b1111;

// Opcodes the SDK's enum leaves out (D3D10_SB_OPCODE_*).
constexpr uint32_t kOpBreakC = 3, kOpIShR = 42, kOpResInfo = 61, kOpSample = 69, kOpSampleB = 74;

// Constant buffer range IDs equal their registers (b0-b3).
constexpr uint32_t kCbSystem = 0, kCbFloat = 1, kCbBoolLoop = 2, kCbFetch = 3;
// Job 634: the tex_post_affine constants, after the 48 fetch-constant vectors (xenos::hlsl::TexPostAffine).
constexpr uint32_t kTexAffineBase = 48, kTexAffineVec = xenos::hlsl::kTexPostAffineVectors;

// Bits of float constants used as immediates.
constexpr uint32_t kOneBits = 0x3F800000u;
constexpr uint32_t kFltMaxBits = 0x7F7FFFFFu;
std::atomic<uint64_t> g_rule_hits[3];  // patch 0087 (CanaryRuleHits)

struct Gen {
  Gen(const Program& program, const hlsl::HlslOptions& options, bool final_pass)
      : p(program), opt(options), final(final_pass) {}
  const Program& p;
  const hlsl::HlslOptions& opt;
  const bool final;  // the second pass: the first only gathers the facts below
  // Patch 0087: the newer Canary rules this shader used (bit 0 tfetch1D XY, 1 mulsc toward zero, 2 float16 NaN -> 0).
  uint32_t rule_bits = 0;

  // ---- Facts (gathered by pass 1, used by pass 2) ----
  uint32_t reg_count = 1;
  bool dynamic_regs = false;
  uint32_t float_constants = 0;
  uint64_t constants_read[4] = {};
  bool constants_relative = false;
  bool memexport = false;
  bool writes_depth = false;
  bool any_kill = false;
  bool uses_pc_loop = false;
  std::set<uint32_t> stream_constants;
  hlsl::Translation meta;
  std::string unsupported;

  // Pass 2 inputs from pass 1.
  uint32_t regs = 0;                        // Xenos register count (r0..regs-1)
  bool dyn = false;                         // registers in the indexable temp x0
  uint32_t compact_l = 0, compact_h = 0;    // float-constant compaction (equal = none)

  // ---- Code ----
  std::vector<uint32_t> code;
  dx::Statistics stat{};
  dx::Assembler a{code, stat};

  // Temps: the Xenos registers (r0..regs-1, unless in x0), then fixed system temps, then a scratch stack.
  uint32_t base = 0;
  uint32_t next_temp = 0, max_temp = 0;
  uint32_t St() const { return base + 0; }      // x ps, y p0 (0 / ~0), z a0 (int), w aL (int)
  uint32_t Fl() const { return base + 1; }      // x pc, y guard, z loop depth, w call depth
  uint32_t Misc() const { return base + 2; }    // x register LOD, y vertex fetch address (dwords), z eM written mask
  uint32_t GradH() const { return base + 3; }
  uint32_t GradV() const { return base + 4; }
  uint32_t PosOrDepth() const { return base + 5; }  // VS xe_opos, PS xe_odepth
  uint32_t Pts() const { return base + 6; }         // oPts (dropped)
  uint32_t Oexport() const { return base + 7; }     // export to an unknown target (dropped)
  uint32_t Oc(uint32_t i) const { return base + 8 + i; }  // PS xe_oc[0-3]
  uint32_t EA() const { return base + 12; }
  static constexpr uint32_t kFixedTemps = 13;
  // Indexable temps: x0 the registers (dynamic addressing), x1[4] the loop / call stacks (x iter, y loop constant,
  // z return address), x2[5] eM0-eM4.
  bool uses_x1 = false;

  uint32_t Push() {
    const uint32_t t = next_temp++;
    max_temp = std::max(max_temp, next_temp);
    return t;
  }
  struct Scope {
    Gen& g;
    uint32_t saved;
    explicit Scope(Gen& gen) : g(gen), saved(gen.next_temp) {}
    ~Scope() { g.next_temp = saved; }
  };

  static Dest R(uint32_t t, uint32_t mask = kXYZW) { return Dest::R(t, mask); }
  static Src S(uint32_t t, uint32_t swizzle = Src::kXYZW) { return Src::R(t, swizzle); }
  static Src S1(uint32_t t, uint32_t c) { return Src::R(t).Select(c); }
  static Src LU(uint32_t x) { return Src::LU(x); }
  static Src LU4(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return Src::LU(x, y, z, w); }
  static Src LI(int32_t x) { return Src::LI(x); }
  static Src LF(float x) { return Src::LF(x); }
  static Src LF4(float x, float y, float z, float w) { return Src::LF(x, y, z, w); }

  // Constant buffer reads (SM 5.1: range ID, register, element).
  static Src Cb(uint32_t id, uint32_t element, uint32_t swizzle = Src::kXYZW) { return Src::CB(id, id, element, swizzle); }
  static Src CbRel(uint32_t id, uint32_t temp, uint32_t comp, uint32_t offset = 0) {
    return Src::CB(id, id, Index(temp, comp, offset));
  }
  static Src Sys(uint32_t element, uint32_t swizzle = Src::kXYZW) { return Cb(kCbSystem, element, swizzle); }
  // Fetch constant dword j (xe_fetch_dw with a constant j).
  static Src FetchDw(uint32_t j) { return Cb(kCbFetch, (j >> 2) % 48).Select(j & 3); }

  void Fail(const std::string& why) {
    if (unsupported.empty()) unsupported = why;
  }
  void Simplify(const char* feature, const std::string& note) {
    for (const auto& s : meta.simplifications)
      if (s.feature == feature) return;
    meta.simplifications.push_back({feature, note});
  }

  // Emits an instruction the SDK's assembler has no helper for: one destination, sources written as given.
  struct RawSrc {
    Src src;
    bool is_integer;
    uint32_t mask;
    bool force_vector;
  };
  void Raw(uint32_t opcode, const Dest& dest, std::initializer_list<RawSrc> srcs, uint32_t opcode_extra = 0) {
    uint32_t length = dest.GetLength();
    for (const RawSrc& s : srcs) length += s.src.GetLength(s.mask, s.force_vector);
    code.push_back(opcode | opcode_extra | ((1u + length) << 24));
    dest.Write(code);
    for (const RawSrc& s : srcs) s.src.Write(code, s.is_integer, s.mask, s.force_vector);
    ++stat.instruction_count;
  }

  // ishr (the assembler has no helper for it).
  void IShR(const Dest& dest, const Src& value, const Src& shift) {
    const uint32_t mask = dest.GetMask();
    Raw(kOpIShR, dest, {{value, true, mask, false}, {shift, true, mask, false}});
  }

  // ---- Small building blocks (hlsl.cpp prelude helpers) ----

  // xe_ftz_bits: denormals to a signed zero, on the bits.
  void FtzBits(uint32_t d, const Src& v, uint32_t mask) {
    Scope sc(*this);
    const uint32_t e = Push(), s = Push();
    a.OpAnd(R(e, mask), v, LU(0x7F800000u));
    a.OpAnd(R(s, mask), v, LU(0x80000000u));
    a.OpMovC(R(d, mask), S(e), v, S(s));
  }
  // xe_order_key: sign-magnitude to a signed integer order (-0 and +0 -> 0).
  void OrderKey(uint32_t d, const Src& u, uint32_t mask) {
    Scope sc(*this);
    const uint32_t m = Push(), s = Push();
    a.OpAnd(R(m, mask), u, LU(0x7FFFFFFFu));
    IShR(R(s, mask), u, LU(31));
    a.OpXOr(R(m, mask), S(m), S(s));
    a.OpIAdd(R(d, mask), S(m), -S(s));
  }
  // !xe_nan_bits: (u & 0x7FFFFFFF) <= 0x7F800000.
  void NotNanBits(uint32_t d, const Src& u, uint32_t mask) {
    Scope sc(*this);
    const uint32_t m = Push();
    a.OpAnd(R(m, mask), u, LU(0x7FFFFFFFu));
    a.OpUGE(R(d, mask), LU(0x7F800000u), S(m));
  }
  // xe_max / xe_max1 (is_max) and xe_min / xe_min1, per component of the mask.
  void MaxMin(uint32_t d, const Src& af, const Src& bf, uint32_t mask, bool is_max) {
    if (opt.dxbc_canary_minmax) {  // patch 0059: Canary's SM3 form
      Scope sc(*this);
      const uint32_t c = Push();
      if (is_max) a.OpGE(R(c, mask), af, bf);
      else a.OpLT(R(c, mask), af, bf);
      a.OpMovC(R(d, mask), S(c), af, bf);
      return;
    }
    Scope sc(*this);
    const uint32_t ua = Push(), ub = Push(), ka = Push(), kb = Push(), na = Push(), nb = Push();
    FtzBits(ua, af, mask);
    FtzBits(ub, bf, mask);
    OrderKey(ka, S(ua), mask);
    OrderKey(kb, S(ub), mask);
    NotNanBits(na, S(ua), mask);
    NotNanBits(nb, S(ub), mask);
    if (is_max) a.OpIGE(R(ka, mask), S(ka), S(kb));
    else a.OpILT(R(ka, mask), S(ka), S(kb));
    a.OpAnd(R(ka, mask), S(ka), S(na));
    a.OpAnd(R(ka, mask), S(ka), S(nb));
    a.OpMovC(R(d, mask), S(ka), S(ua), S(ub));
  }
  // xe_mul / xe_mul1: the legacy product (+0 when either factor is +-0).
  void Mul(uint32_t d, const Src& x, const Src& y, uint32_t mask) {
    Scope sc(*this);
    const uint32_t pr = Push(), z = Push();
    a.OpMul(R(pr, mask), x, y);
    if (opt.dxbc_mul_movc) {
      // Job 634: three instructions - movc tests the bits of min(|x|, |y|), so a zero factor (either sign: the abs) gives
      // +0 and anything else the product. Differs from the eq form only for a denormal factor (its bits are not 0,
      // where eq flushes it) times Inf / NaN: NaN instead of 0.
      a.OpMin(R(z, mask), x.Abs(), y.Abs());
      a.OpMovC(R(d, mask), S(z), S(pr), LF(0.0f));
      return;
    }
    if (opt.dxbc_mul_min_abs) {
      // Job 634: four instructions instead of five - min(|x|, |y|) is 0 exactly when x or y is (a NaN factor gives the
      // other one; both NaN gives NaN, which is not 0), and the product AND (that min != 0) is +0 or the product. The
      // same bits as the eq / eq / or / movc form (checked on the CPU over 9M operand pairs, job 634).
      a.OpMin(R(z, mask), x.Abs(), y.Abs());
      a.OpNE(R(z, mask), S(z), LF(0.0f));
      a.OpAnd(R(d, mask), S(pr), S(z));
      return;
    }
    const uint32_t z2 = Push();
    a.OpEq(R(z, mask), x, LF(0.0f));
    a.OpEq(R(z2, mask), y, LF(0.0f));
    a.OpOr(R(z, mask), S(z), S(z2));
    a.OpMovC(R(d, mask), S(z), LF(0.0f), S(pr));
  }
  // Patch 0087 (Canary 3390fc2, --gow2_renderer_mulsc_round_toward_zero): d.x = x * y rounded toward zero. The exact
  // error of the product comes from a Veltkamp split (4097) and a Dekker error sum - DXBC's mad may not stay fused -
  // and when it has the sign opposite to the product, the product was rounded away from zero: one step back (bits - 1).
  // A zero product (the SM3 zero rule) and Inf / NaN (error NaN) stay as they are.
  void MulTowardZero(uint32_t d, const Src& x, const Src& y) {
    Scope sc(*this);
    const uint32_t s = Push(), r = Push();
    const Src prod = S1(d, 0);
    a.OpMul(R(s, kX), x, LF(4097.0f));
    a.OpAdd(R(s, kY), S1(s, 0), -x);
    a.OpAdd(R(s, kX), S1(s, 0), -S1(s, 1));  // x high
    a.OpAdd(R(s, kY), x, -S1(s, 0));         // x low
    a.OpMul(R(s, kZ), y, LF(4097.0f));
    a.OpAdd(R(s, kW), S1(s, 2), -y);
    a.OpAdd(R(s, kZ), S1(s, 2), -S1(s, 3));  // y high
    a.OpAdd(R(s, kW), y, -S1(s, 2));         // y low
    a.OpMul(R(r, kX), S1(s, 0), S1(s, 2));
    a.OpAdd(R(r, kX), S1(r, 0), -prod);
    a.OpMul(R(r, kY), S1(s, 0), S1(s, 3));
    a.OpAdd(R(r, kX), S1(r, 0), S1(r, 1));
    a.OpMul(R(r, kY), S1(s, 1), S1(s, 2));
    a.OpAdd(R(r, kX), S1(r, 0), S1(r, 1));
    a.OpMul(R(r, kY), S1(s, 1), S1(s, 3));
    a.OpAdd(R(r, kX), S1(r, 0), S1(r, 1));
    a.OpMul(R(r, kX), S1(r, 0), prod);
    a.OpLT(R(r, kX), S1(r, 0), LF(0.0f));
    a.OpIAdd(R(r, kY), prod, LI(-1));
    a.OpMovC(R(d, kX), S1(r, 0), S1(r, 1), prod);
    rule_bits |= 2u;
  }
  // xe_sat / xe_sat4.
  void Sat(uint32_t d, const Src& v, uint32_t mask) {
    if (opt.dxbc_canary_minmax) {  // patch 0059: DXBC saturate (NaN -> 0), as Canary
      a.OpMov(R(d, mask), v, true);
      return;
    }
    Scope sc(*this);
    const uint32_t u = Push(), k = Push(), nn = Push(), c = Push();
    FtzBits(u, v, mask);
    OrderKey(k, S(u), mask);
    NotNanBits(nn, S(u), mask);
    a.OpILT(R(c, mask), S(k), LI(int32_t(kOneBits)));
    a.OpMovC(R(u, mask), S(c), S(u), LU(kOneBits));
    a.OpILT(R(c, mask), LI(0), S(k));
    a.OpAnd(R(c, mask), S(c), S(nn));
    a.OpMovC(R(d, mask), S(c), S(u), LU(0));
  }
  // xe_r21 (patch 0037), or a pass-through without --gow2_renderer_canary_r21 (patch 0046).
  void R21(uint32_t d, const Src& v) {
    if (!opt.canary_r21) {
      a.OpMov(R(d, kX), v);
      return;
    }
    if (opt.dxbc_short_r21) {
      // Job 634: nine instructions instead of eleven - the round half up of the low two bits as (v + 2) & ~3 (equal to
      // t + 4 when (v & 3) >= 2, else t), the same overflow and Inf / NaN rules. The same bits for all 2^32 inputs.
      Scope sc(*this);
      const uint32_t t = Push(), r = Push(), c = Push();
      a.OpAnd(R(t, kX), v, LU(~3u));
      a.OpIAdd(R(r, kX), v, LU(2));
      a.OpAnd(R(r, kX), S1(r, 0), LU(~3u));
      a.OpAnd(R(c, kX), S1(r, 0), LU(0x7F800000u));
      a.OpIEq(R(c, kX), S1(c, 0), LU(0x7F800000u));
      a.OpMovC(R(r, kX), S1(c, 0), S1(t, 0), S1(r, 0));
      a.OpAnd(R(c, kX), v, LU(0x7F800000u));
      a.OpIEq(R(c, kX), S1(c, 0), LU(0x7F800000u));
      a.OpMovC(R(d, kX), S1(c, 0), v, S1(r, 0));
      return;
    }
    Scope sc(*this);
    const uint32_t e = Push(), t = Push(), u = Push(), f = Push(), g = Push();
    a.OpAnd(R(e, kX), v, LU(0x7F800000u));
    a.OpIEq(R(e, kX), S(e), LU(0x7F800000u));
    a.OpAnd(R(t, kX), v, LU(~3u));
    a.OpIAdd(R(u, kX), S(t), LU(4));
    a.OpAnd(R(f, kX), S(u), LU(0x7F800000u));
    a.OpIEq(R(f, kX), S(f), LU(0x7F800000u));
    a.OpMovC(R(u, kX), S(f), S(t), S(u));
    a.OpAnd(R(g, kX), v, LU(3));
    a.OpUGE(R(g, kX), S(g), LU(2));
    a.OpMovC(R(g, kX), S(g), S(u), S(t));
    a.OpMovC(R(d, kX), S(e), v, S(g));
  }
  // isinf(v) || isnan(v) on the bits: the exponent is all ones.
  void ExpAllOnes(uint32_t d, const Src& v, uint32_t mask) {
    a.OpAnd(R(d, mask), v, LU(0x7F800000u));
    a.OpIEq(R(d, mask), S(d), LU(0x7F800000u));
  }
  // isinf(v) on the bits.
  void IsInf(uint32_t d, const Src& v, uint32_t mask) {
    a.OpAnd(R(d, mask), v, LU(0x7FFFFFFFu));
    a.OpIEq(R(d, mask), S(d), LU(0x7F800000u));
  }
  // xe_clamp_inf (to +-FLT_MAX) or xe_zero_inf (to +-0), keeping the sign bit.
  void ReplaceInf(uint32_t d, const Src& v, bool to_max) {
    Scope sc(*this);
    const uint32_t i = Push(), s = Push();
    IsInf(i, v, kX);
    a.OpAnd(R(s, kX), v, LU(0x80000000u));
    if (to_max) a.OpOr(R(s, kX), S(s), LU(kFltMaxBits));
    a.OpMovC(R(d, kX), S(i), S(s), v);
  }
  // xe_ftoi: NaN -> 0, saturated to the int32 range.
  void FtoI(uint32_t d, const Src& v, uint32_t mask) {
    Scope sc(*this);
    const uint32_t t = Push(), c = Push();
    a.OpFToI(R(t, mask), v);
    a.OpGE(R(c, mask), v, LF(2147483648.0f));
    a.OpMovC(R(t, mask), S(c), LU(0x7FFFFFFFu), S(t));
    a.OpGE(R(c, mask), LF(-2147483648.0f), v);
    a.OpMovC(R(t, mask), S(c), LU(0x80000000u), S(t));
    a.OpNE(R(c, mask), v, v);
    a.OpMovC(R(d, mask), S(c), LU(0), S(t));
  }
  // xe_a0_round / xe_a0_floor into the a0 register (St().z).
  void SetA0(const Src& v, bool round) {
    Scope sc(*this);
    const uint32_t t = Push(), c = Push();
    a.OpLT(R(c, kX), v, LF(255.0f));
    a.OpMovC(R(t, kX), S(c), v, LF(255.0f));
    a.OpLT(R(c, kX), LF(-256.0f), v);
    a.OpMovC(R(t, kX), S(c), S(t), LF(-256.0f));
    if (round) a.OpAdd(R(t, kX), S(t), LF(0.5f));
    a.OpRoundNI(R(t, kX), S(t));
    a.OpFToI(R(St(), kZ), S(t, Src::kXXXX));
  }
  // xe_swap(v, e) with e in a component: 8-in-16 for 1 / 2, 16-in-32 for 2 / 3.
  void Swap(uint32_t d, const Src& v, const Src& e, uint32_t mask) {
    if (opt.dxbc_short_swap) {
      // Job 634: 12 instructions instead of 17 - 8-in-16 for e = 1 or 2 is ((e + 1) & 2) != 0, 16-in-32 for e = 2 or 3
      // is (e & 2) != 0 (e is 0-3). The same bits for every value and mode (2^32 x 4, job 634).
      Scope sc(*this);
      const uint32_t c = Push(), t = Push(), u = Push();
      a.OpAnd(R(t, mask), v, LU(0x00FF00FFu));
      a.OpIShL(R(t, mask), S(t), LU(8));
      a.OpUShR(R(u, mask), v, LU(8));
      a.OpAnd(R(u, mask), S(u), LU(0x00FF00FFu));
      a.OpOr(R(t, mask), S(t), S(u));
      a.OpIAdd(R(c, mask), e, LU(1));
      a.OpAnd(R(c, mask), S(c), LU(2));
      a.OpMovC(R(d, mask), S(c), S(t), v);
      a.OpAnd(R(c, mask), e, LU(2));
      a.OpIShL(R(t, mask), S(d), LU(16));
      a.OpUShR(R(u, mask), S(d), LU(16));
      a.OpOr(R(t, mask), S(t), S(u));
      a.OpMovC(R(d, mask), S(c), S(t), S(d));
      return;
    }
    Scope sc(*this);
    const uint32_t c = Push(), t = Push(), u = Push();
    a.OpMov(R(d, mask), v);
    // 8-in-16.
    a.OpIEq(R(c, mask), e, LU(1));
    a.OpIEq(R(t, mask), e, LU(2));
    a.OpOr(R(c, mask), S(c), S(t));
    a.OpAnd(R(t, mask), S(d), LU(0x00FF00FFu));
    a.OpIShL(R(t, mask), S(t), LU(8));
    a.OpUShR(R(u, mask), S(d), LU(8));
    a.OpAnd(R(u, mask), S(u), LU(0x00FF00FFu));
    a.OpOr(R(t, mask), S(t), S(u));
    a.OpMovC(R(d, mask), S(c), S(t), S(d));
    // 16-in-32.
    a.OpIEq(R(c, mask), e, LU(2));
    a.OpIEq(R(t, mask), e, LU(3));
    a.OpOr(R(c, mask), S(c), S(t));
    a.OpIShL(R(t, mask), S(d), LU(16));
    a.OpUShR(R(u, mask), S(d), LU(16));
    a.OpOr(R(t, mask), S(t), S(u));
    a.OpMovC(R(d, mask), S(c), S(t), S(d));
  }
  // xe_al(iter, lc) into d.x (int).
  void LoopAddress(uint32_t d, const Src& iter, const Src& lc) {
    Scope sc(*this);
    const uint32_t st = Push(), b = Push();
    a.OpIBFE(R(st, kX), LU(8), LU(16), lc);
    a.OpUBFE(R(b, kX), LU(8), LU(8), lc);
    a.OpIMAd(R(d, kX), iter, S(st, Src::kXXXX), S(b, Src::kXXXX));
    a.OpIMax(R(d, kX), S(d, Src::kXXXX), LI(-256));
    a.OpIMin(R(d, kX), S(d, Src::kXXXX), LI(256));
  }
  // xe_degamma1 per component.
  void Degamma(uint32_t d, const Src& g, uint32_t mask) {
    Scope sc(*this);
    const uint32_t x = Push(), l = Push(), t = Push(), c = Push();
    Sat(x, g, mask);
    a.OpMul(R(x, mask), S(x), LF(255.0f));
    if (opt.dxbc_short_degamma) {
      // Job 634: l = x + max(x - 64, 0) + 2 max(x - 96, 0) + 4 max(x - 192, 0) - the same four segments (x, 2x - 64,
      // 4x - 256, 8x - 1024) without the compares and selects, 11 instructions instead of 15. Every step is exact for
      // x = sat(g) * 255, so the same bits for every float g in [0, 1] (all 1,065,353,217 checked, job 634).
      a.OpAdd(R(t, mask), S(x), LF(-64.0f));
      a.OpMax(R(t, mask), S(t), LF(0.0f));
      a.OpAdd(R(l, mask), S(x), S(t));
      a.OpAdd(R(t, mask), S(x), LF(-96.0f));
      a.OpMax(R(t, mask), S(t), LF(0.0f));
      a.OpMul(R(t, mask), S(t), LF(2.0f));
      a.OpAdd(R(l, mask), S(l), S(t));
      a.OpAdd(R(t, mask), S(x), LF(-192.0f));
      a.OpMax(R(t, mask), S(t), LF(0.0f));
      a.OpMul(R(t, mask), S(t), LF(4.0f));
      a.OpAdd(R(l, mask), S(l), S(t));
      a.OpDiv(R(d, mask), S(l), LF(1023.0f));
      return;
    }
    // 512 + (x - 192) * 8
    a.OpAdd(R(l, mask), S(x), LF(-192.0f));
    a.OpMul(R(l, mask), S(l), LF(8.0f));
    a.OpAdd(R(l, mask), S(l), LF(512.0f));
    // x < 192: 128 + (x - 96) * 4
    a.OpAdd(R(t, mask), S(x), LF(-96.0f));
    a.OpMul(R(t, mask), S(t), LF(4.0f));
    a.OpAdd(R(t, mask), S(t), LF(128.0f));
    a.OpLT(R(c, mask), S(x), LF(192.0f));
    a.OpMovC(R(l, mask), S(c), S(t), S(l));
    // x < 96: 64 + (x - 64) * 2
    a.OpAdd(R(t, mask), S(x), LF(-64.0f));
    a.OpMul(R(t, mask), S(t), LF(2.0f));
    a.OpAdd(R(t, mask), S(t), LF(64.0f));
    a.OpLT(R(c, mask), S(x), LF(96.0f));
    a.OpMovC(R(l, mask), S(c), S(t), S(l));
    // x < 64: x
    a.OpLT(R(c, mask), S(x), LF(64.0f));
    a.OpMovC(R(l, mask), S(c), S(x), S(l));
    a.OpDiv(R(d, mask), S(l), LF(1023.0f));
  }

  // If on a uint condition component (non-zero = true).
  void IfNz(const Src& c, bool when_nonzero = true) { a.OpIf(when_nonzero, c); }

  // ---- Registers and operands ----
  void NoteRegister(uint32_t index, Addressing addressing) {
    if (addressing == Addressing::kLoopRelative) dynamic_regs = true;
    reg_count = std::max(reg_count, index + 1);
  }
  void NoteConstant(const Operand& op) {
    if (op.addressing != Addressing::kAbsolute) {
      float_constants = 256;
      constants_relative = true;
    } else {
      float_constants = std::max(float_constants, std::min<uint32_t>(op.index + 1, 256));
      if (op.index < 256) constants_read[op.index >> 6] |= 1ull << (op.index & 63);
    }
  }
  uint32_t ConstantSlot(uint32_t index) const {
    if (compact_l == compact_h) return index;
    return index < compact_l ? index : compact_l + (index - compact_h);
  }
  // Index temp for an aL-relative register: (index + aL) & 63, in t.x.
  void RelRegIndex(uint32_t t, uint32_t index) {
    a.OpIAdd(R(t, kX), S1(St(), 3), LU(index));
    a.OpAnd(R(t, kX), S(t, Src::kXXXX), LU(63));
  }
  // The register as a source / destination. `rel` is a temp holding the relative index in .x (aL-relative only).
  Src RegSrc(uint32_t index, uint32_t rel, uint32_t swizzle = Src::kXYZW) const {
    if (!final) return Src::R(0, swizzle);
    if (dyn) return rel != UINT32_MAX ? Src::X(0, Index(rel, 0), swizzle) : Src::X(0, index & 63, swizzle);
    return Src::R(std::min(index, regs - 1), swizzle);
  }
  Dest RegDest(uint32_t index, uint32_t rel, uint32_t mask) const {
    if (!final) return Dest::R(0, mask);
    if (dyn) return rel != UINT32_MAX ? Dest::X(0, Index(rel, 0), mask) : Dest::X(0, index & 63, mask);
    return Dest::R(std::min(index, regs - 1), mask);
  }
  // abs() / -() of an operand as the bit operations DXIL's fabs / fneg are: a `mov` with DXBC source modifiers may
  // flush a denormal on some implementations (vkd3d + lavapipe did: -|denormal| came out +0, not -0).
  void SignModifiers(uint32_t d, uint32_t mask, const Operand& op) {
    if (op.absolute) a.OpAnd(R(d, mask), S(d), LU(0x7FFFFFFFu));
    if (op.negate) a.OpXOr(R(d, mask), S(d), LU(0x80000000u));
  }
  // -v on the bits (DXIL fneg) into t.c; returns t.c as a source.
  Src NegBits(uint32_t t, uint32_t c, const Src& v) {
    a.OpXOr(R(t, 1u << c), v, LU(0x80000000u));
    return S1(t, c);
  }
  // Reads an operand into temp d (all four components, swizzled; modifiers applied) - hlsl.cpp Operand4.
  // `component` >= 0 reads only that component of the operand into d.x - hlsl.cpp Operand1.
  // Patch 0059: `want` = the components the caller uses (per-component vector ops: the result's used components).
  void ReadOperand(uint32_t d, const Operand& op, int component = -1, uint32_t want = kXYZW) {
    Scope sc(*this);
    uint32_t swz = 0;
    if (component < 0) {
      for (uint32_t i = 0; i < 4; ++i)
        swz |= (uint32_t(op.components[std::min(i, op.component_count - 1)]) & 3) << (i * 2);
    } else {
      swz = (uint32_t(op.components[uint32_t(component)]) & 3) * 0b01010101;
    }
    const uint32_t mask = component < 0 ? want : kX;
    if (op.source == Source::kRegister) {
      uint32_t rel = UINT32_MAX;
      if (op.addressing == Addressing::kLoopRelative) {
        rel = Push();
        RelRegIndex(rel, op.index);
      }
      a.OpMov(R(d, mask), RegSrc(op.index, rel, swz));
      SignModifiers(d, mask, op);
      return;
    }
    NoteConstant(op);
    if (op.addressing == Addressing::kAbsolute) {
      a.OpMov(R(d, mask), Cb(kCbFloat, ConstantSlot(op.index), swz));
      SignModifiers(d, mask, op);
      return;
    }
    // xe_c_read(index + a0 / aL): 0 outside [0, 256). The index is clamped before the read (a root constant buffer view
    // has no bounds check), the result selected after.
    const uint32_t i = Push(), c = Push();
    a.OpIAdd(R(i, kX), S1(St(), op.addressing == Addressing::kAddressRegisterRelative ? 2 : 3), LU(op.index));
    a.OpULT(R(c, kX), S(i, Src::kXXXX), LU(256));
    a.OpUMin(R(i, kX), S(i, Src::kXXXX), LU(255));
    a.OpMov(R(d, mask), CbRel(kCbFloat, i, 0).Swizzle(swz));
    a.OpMovC(R(d, mask), S(c, Src::kXXXX), S(d), LF(0.0f));
    SignModifiers(d, mask, op);
  }

  // Patch 0060 (opt.dxbc_direct_operands): an operand with no abs / negate and absolute addressing as a source in place -
  // the register (r / x0) or the float constant, swizzled as ReadOperand would (`component` >= 0: that component,
  // replicated). False = the caller reads it into a temp (ReadOperand).
  bool DirectOperand(const Operand& op, int component, Src& out) {
    if (!opt.dxbc_direct_operands || op.absolute || op.negate || op.addressing != Addressing::kAbsolute) return false;
    if (op.source != Source::kRegister && op.source != Source::kConstantFloat) return false;
    uint32_t swz = 0;
    if (component < 0) {
      for (uint32_t i = 0; i < 4; ++i)
        swz |= (uint32_t(op.components[std::min(i, op.component_count - 1)]) & 3) << (i * 2);
    } else {
      swz = (uint32_t(op.components[uint32_t(component)]) & 3) * 0b01010101;
    }
    if (op.source == Source::kRegister) {
      out = RegSrc(op.index, UINT32_MAX, swz);
      return true;
    }
    NoteConstant(op);
    out = Cb(kCbFloat, ConstantSlot(op.index), swz);
    return true;
  }

  // ---- Kill ----
  void EmitKill(const Src& nonzero_cond) {
    if (p.is_vertex_shader) {
      Simplify("kill_in_vertex_shader", "kill op in a vertex shader has no effect (value still written)");
      return;
    }
    any_kill = true;
    a.OpDiscard(true, nonzero_cond);
  }

  // ---- ALU (hlsl.cpp EmitAlu) ----
  static bool VectorChangesState(AluVectorOpcode op) {
    return (op >= AluVectorOpcode::kSetpEqPush && op <= AluVectorOpcode::kKillNe) || op == AluVectorOpcode::kMaxA;
  }

  // Dot product of the legacy products, summed from +0 in order (xe_dp4 / xe_dp3 / xe_dp2add), into d.x.
  void Dot(uint32_t d, const Src& x, const Src& y, uint32_t n, const Src* add_c) {
    Scope sc(*this);
    const uint32_t m = Push();
    Mul(m, x, y, (1u << n) - 1);
    a.OpAdd(R(d, kX), LF(0.0f), S1(m, 0));
    for (uint32_t i = 1; i < n; ++i) a.OpAdd(R(d, kX), S(d, Src::kXXXX), S1(m, i));
    if (add_c) a.OpAdd(R(d, kX), S(d, Src::kXXXX), add_c->SelectFromSwizzled(0));  // 0060: keeps an operand's swizzle
  }

  // xe_cube(s) into d.
  void Cube(uint32_t d, uint32_t s) {
    Scope sc(*this);
    // x = s.z, y = s.w, z = s.x.
    const Src x = S1(s, 2), y = S1(s, 3), z = S1(s, 0);
    const uint32_t c = Push(), n = Push(), r1 = Push(), r2 = Push(), r3 = Push(), t = Push();
    // c.x = az >= ax && az >= ay, c.y = ay >= ax.
    a.OpGE(R(c, kX), z.Abs(), x.Abs());
    a.OpGE(R(t, kX), z.Abs(), y.Abs());
    a.OpAnd(R(c, kX), S(c, Src::kXXXX), S(t, Src::kXXXX));
    a.OpGE(R(c, kY), y.Abs(), x.Abs());
    // n.x = z < 0, n.y = y < 0, n.z = x < 0.
    a.OpLT(R(n, kX), z, LF(0.0f));
    a.OpLT(R(n, kY), y, LF(0.0f));
    a.OpLT(R(n, kZ), x, LF(0.0f));
    // Negations on the bits (DXIL fneg): ng = (-x, -y, -z).
    const uint32_t ng = Push();
    const Src nx = NegBits(ng, 0, x), ny = NegBits(ng, 1, y), nz = NegBits(ng, 2, z);
    // r1 = (-y, n ? -x : x, z, n ? 5 : 4)
    a.OpMov(R(r1, kX), ny);
    a.OpMovC(R(r1, kY), S1(n, 0), nx, x);
    a.OpMov(R(r1, kZ), z);
    a.OpMovC(R(r1, kW), S1(n, 0), LF(5.0f), LF(4.0f));
    // r2 = (n ? -z : z, x, y, n ? 3 : 2)
    a.OpMovC(R(r2, kX), S1(n, 1), nz, z);
    a.OpMov(R(r2, kY), x);
    a.OpMov(R(r2, kZ), y);
    a.OpMovC(R(r2, kW), S1(n, 1), LF(3.0f), LF(2.0f));
    // r3 = (-y, n ? z : -z, x, n ? 1 : 0)
    a.OpMov(R(r3, kX), ny);
    a.OpMovC(R(r3, kY), S1(n, 2), z, nz);
    a.OpMov(R(r3, kZ), x);
    a.OpMovC(R(r3, kW), S1(n, 2), LF(1.0f), LF(0.0f));
    a.OpMovC(R(r2), S1(c, 1), S(r2), S(r3));
    a.OpMovC(R(d), S1(c, 0), S(r1), S(r2));
    a.OpMul(R(d, kZ), S(d), LF(2.0f));
  }

  // xe_max4(s) into d.x.
  void Max4(uint32_t d, uint32_t s) {
    Scope sc(*this);
    const uint32_t u = Push(), k = Push(), nn = Push(), g = Push(), h = Push(), t = Push();
    FtzBits(u, S(s), kXYZW);
    OrderKey(k, S(u), kXYZW);
    NotNanBits(nn, S(u), kXYZW);
    // g = (x>=y, x>=z, x>=w, y>=z), h.xy = (y>=w, z>=w), each with both not NaN.
    a.OpIGE(R(g), S(k, Sw(0, 0, 0, 1)), S(k, Sw(1, 2, 3, 2)));
    a.OpAnd(R(g), S(g), S(nn, Sw(0, 0, 0, 1)));
    a.OpAnd(R(g), S(g), S(nn, Sw(1, 2, 3, 2)));
    a.OpIGE(R(h, kXY), S(k, Sw(1, 2, 0, 0)), S(k, Sw(3, 3, 0, 0)));
    a.OpAnd(R(h, kXY), S(h), S(nn, Sw(1, 2, 0, 0)));
    a.OpAnd(R(h, kXY), S(h), S(nn, Sw(3, 3, 0, 0)));
    // cx = g.x & g.y & g.z, cy = g.w & h.x, cz = h.y.
    a.OpAnd(R(t, kX), S1(g, 0), S1(g, 1));
    a.OpAnd(R(t, kX), S(t, Src::kXXXX), S1(g, 2));
    a.OpAnd(R(t, kY), S1(g, 3), S1(h, 0));
    a.OpMovC(R(d, kX), S1(h, 1), S1(u, 2), S1(u, 3));
    a.OpMovC(R(d, kX), S1(t, 1), S1(u, 1), S(d, Src::kXXXX));
    a.OpMovC(R(d, kX), S1(t, 0), S1(u, 0), S(d, Src::kXXXX));
  }

  // The destination of an export (hlsl.cpp ExportTarget) as a temp or output register; component writes go through
  // WriteExportComponent.
  enum class ExportKind { kOutput, kTemp, kEM };
  struct ExportDst {
    ExportKind kind = ExportKind::kTemp;
    uint32_t index = 0;
  };
  ExportDst ExportTarget(const Result& r) {
    switch (r.target) {
      case Target::kInterpolator:
        if (r.index >= 16) Fail(std::format("interpolator export {}", r.index));
        return {ExportKind::kOutput, std::min(r.index, 15u)};
      case Target::kPosition: return {ExportKind::kTemp, PosOrDepth()};
      case Target::kPointSizeEdgeFlagKillVertex:
        Simplify("point_size_export", "oPts written; point size / edge flag / kill vertex are dropped");
        return {ExportKind::kTemp, Pts()};
      case Target::kColor:
        if (r.index >= 4) Fail(std::format("colour export {}", r.index));
        return {ExportKind::kTemp, Oc(std::min(r.index, 3u))};
      case Target::kDepth: return {ExportKind::kTemp, PosOrDepth()};
      case Target::kExportAddress:
        memexport = true;
        return {ExportKind::kTemp, EA()};
      case Target::kExportData:
        memexport = true;
        if (r.index < 5) return {ExportKind::kEM, r.index};
        return {ExportKind::kTemp, Oexport()};
      default: return {ExportKind::kTemp, Oexport()};
    }
  }
  Dest ExportDest(const ExportDst& e, uint32_t mask) const {
    if (!final) return Dest::R(0, mask);
    switch (e.kind) {
      case ExportKind::kOutput: return Dest::O(e.index, mask);
      case ExportKind::kEM: return Dest::X(2, e.index, mask);
      default: return Dest::R(e.index, mask);
    }
  }

  bool scalar_in_ps_ = false;  // job 634: this slot's scalar result is ps (St().x) itself, not a copy in sr
  void EmitAlu(const Alu& al) {
    const bool vec_writes = al.vector_result.write_mask != 0;
    const bool do_vector = !al.IsVectorOpDefaultNop() && (vec_writes || VectorChangesState(al.vector_opcode));
    const bool do_scalar = al.scalar_opcode != AluScalarOpcode::kRetainPrev || al.scalar_result.write_mask;
    if (!do_vector && !do_scalar && !vec_writes) return;
    if (al.predicated) a.OpIf(al.predicate_condition, S1(St(), 1));
    Scope sc(*this);
    for (uint32_t i = 0; i < al.vector_operand_count; ++i)
      if (al.vector_operands[i].source == Source::kRegister)
        NoteRegister(al.vector_operands[i].index, al.vector_operands[i].addressing);
    for (uint32_t i = 0; i < al.scalar_operand_count; ++i)
      if (al.scalar_operands[i].source == Source::kRegister)
        NoteRegister(al.scalar_operands[i].index, al.scalar_operands[i].addressing);
    // Read every source first (both operations see the registers as they were before this slot).
    const uint32_t s0 = Push(), s1 = Push(), s2 = Push(), sab = Push(), vr = Push(), sr = Push();
    // Patch 0059 (Canary computes only used_result_components): a per-component vector op reads and computes only the
    // components its result write uses - the others are never read (register writes take write_mask, exports the
    // written components that are not constant 0 / 1). Same values in every component that is used.
    const uint32_t vm = do_vector ? VectorUsedMask(al) : kXYZW;
    Src vsrc[3] = {S(s0), S(s1), S(s2)};
    if (do_vector) {
      const uint32_t sv[3] = {s0, s1, s2};
      const uint32_t read_mask = al.vector_opcode == AluVectorOpcode::kMaxA ? kXYZW : vm;  // maxa also reads s0.w
      // Cube and max4 work on their operand's temp (by index); every other op takes the sources as operands.
      const bool direct_ok = al.vector_opcode != AluVectorOpcode::kCube && al.vector_opcode != AluVectorOpcode::kMax4;
      for (uint32_t i = 0; i < al.vector_operand_count; ++i) {
        if (direct_ok && DirectOperand(al.vector_operands[i], -1, vsrc[i])) continue;  // patch 0060
        ReadOperand(sv[i], al.vector_operands[i], -1, read_mask);
      }
    }
    Src sa = S1(sab, 0), sb = S1(sab, 1);
    // Patch 0060: a scalar source in place, else read into a temp and packed into sab.x / sab.y as before.
    auto scalar_source = [&](const Operand& op, int component, uint32_t sab_component, Src& out) {
      if (DirectOperand(op, component, out)) return;
      Scope ts(*this);
      const uint32_t t = Push();
      ReadOperand(t, op, component);
      a.OpMov(R(sab, 1u << sab_component), S(t, Src::kXXXX));
    };
    if (do_scalar && al.scalar_operand_count == 1) {
      scalar_source(al.scalar_operands[0], 0, 0, sa);
      if (al.scalar_operands[0].component_count == 2) scalar_source(al.scalar_operands[0], 1, 1, sb);
    } else if (do_scalar && al.scalar_operand_count == 2) {
      scalar_source(al.scalar_operands[0], 0, 0, sa);
      scalar_source(al.scalar_operands[1], 0, 1, sb);
    }

    // Job 634: the zero for a nop's register writes only when something writes it.
    if (!do_vector && (vec_writes || !opt.dxbc_coalesce_moves)) a.OpMov(R(vr), LF(0.0f));
    scalar_in_ps_ = false;
    if (do_vector) EmitVectorOp(al, vsrc[0], vsrc[1], vsrc[2], s0, vr, vm);

    if (do_scalar) EmitScalarOp(al, sa, sb, sr);

    // Writes.
    const Result& v = al.vector_result;
    const Result& s = al.scalar_result;
    if (v.target == Target::kRegister) {
      if (v.write_mask) {
        NoteRegister(v.index, v.addressing);
        Scope ws(*this);
        uint32_t rel = UINT32_MAX;
        if (v.addressing == Addressing::kLoopRelative) {
          rel = Push();
          RelRegIndex(rel, v.index);
        }
        a.OpMov(RegDest(v.index, rel, v.write_mask), S(vr));
      }
      if (s.write_mask && s.target == Target::kRegister) {
        NoteRegister(s.index, s.addressing);
        Scope ws(*this);
        uint32_t rel = UINT32_MAX;
        if (s.addressing == Addressing::kLoopRelative) {
          rel = Push();
          RelRegIndex(rel, s.index);
        }
        a.OpMov(RegDest(s.index, rel, s.write_mask), scalar_in_ps_ ? S(St(), Src::kXXXX) : S(sr, Src::kXXXX));
      }
    } else if (v.write_mask || s.write_mask) {
      if (v.target == Target::kExportAddress) {
        EmitMemexportFlush();  // xe_memexport_flush() before the new address
        for (uint32_t i = 0; i < al.vector_operand_count; ++i) {
          const Operand& op = al.vector_operands[i];
          if (op.source == Source::kConstantFloat && op.addressing == Addressing::kAbsolute) stream_constants.insert(op.index);
        }
        for (uint32_t i = 0; i < al.scalar_operand_count; ++i) {
          const Operand& op = al.scalar_operands[i];
          if (op.source == Source::kConstantFloat && op.addressing == Addressing::kAbsolute) stream_constants.insert(op.index);
        }
      }
      const ExportDst dst = ExportTarget(v);
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bit = 1u << i;
        if (v.write_mask & bit) {
          const Swizzle c = v.components[i];
          const Src val = c == Swizzle::k0 ? LF(0.0f) : c == Swizzle::k1 ? LF(1.0f) : S(vr);
          a.OpMov(ExportDest(dst, bit), val);
        } else if (s.write_mask & bit) {
          a.OpMov(ExportDest(dst, bit), scalar_in_ps_ ? S(St(), Src::kXXXX) : S(sr, Src::kXXXX));
        }
      }
      if (v.target == Target::kDepth || s.target == Target::kDepth) writes_depth = true;
      if (v.target == Target::kExportData && v.index < 5)
        a.OpOr(R(Misc(), kZ), S1(Misc(), 2), LU(1u << v.index));
    }
    if (al.predicated) a.OpEndIf();
  }

  // Patch 0059: the vector ops that work per component (result component i from source components i only).
  static bool PerComponentVectorOp(AluVectorOpcode op) {
    switch (op) {
      case AluVectorOpcode::kAdd: case AluVectorOpcode::kMul: case AluVectorOpcode::kMax: case AluVectorOpcode::kMin:
      case AluVectorOpcode::kSeq: case AluVectorOpcode::kSgt: case AluVectorOpcode::kSge: case AluVectorOpcode::kSne:
      case AluVectorOpcode::kFrc: case AluVectorOpcode::kTrunc: case AluVectorOpcode::kFloor: case AluVectorOpcode::kMad:
      case AluVectorOpcode::kCndEq: case AluVectorOpcode::kCndGe: case AluVectorOpcode::kCndGt: case AluVectorOpcode::kMaxA:
        return true;
      default: return false;
    }
  }
  // Patch 0059: the components of the vector result the writes use (see EmitAlu's writes); all four for the others.
  static uint32_t VectorUsedMask(const Alu& al) {
    if (!PerComponentVectorOp(al.vector_opcode)) return kXYZW;
    const Result& v = al.vector_result;
    uint32_t used = 0;
    if (v.target == Target::kRegister) {
      used = v.write_mask & 0xFu;
    } else {
      for (uint32_t i = 0; i < 4; ++i)
        if ((v.write_mask >> i) & 1u)
          if (v.components[i] != Swizzle::k0 && v.components[i] != Swizzle::k1) used |= 1u << i;
    }
    return used ? used : kX;  // nothing used (a maxa for a0 only): one component keeps the op well-formed
  }

  // Patch 0060: v0-v2 are the sources (temps or, with dxbc_direct_operands, operands in place); s0t is v0's temp, for
  // the ops that index it (cube, max4 always read into it).
  void EmitVectorOp(const Alu& al, const Src& v0, const Src& v1, const Src& v2, uint32_t s0t, uint32_t vr, uint32_t m) {
    Scope sc(*this);
    const uint32_t t = Push(), u = Push();
    switch (al.vector_opcode) {
      case AluVectorOpcode::kAdd: a.OpAdd(R(vr, m), v0, v1); break;
      case AluVectorOpcode::kMul: Mul(vr, v0, v1, m); break;
      case AluVectorOpcode::kMax: MaxMin(vr, v0, v1, m, true); break;
      case AluVectorOpcode::kMin: MaxMin(vr, v0, v1, m, false); break;
      case AluVectorOpcode::kSeq:
        a.OpEq(R(t, m), v0, v1);
        a.OpAnd(R(vr, m), S(t), LU(kOneBits));
        break;
      case AluVectorOpcode::kSgt:
        a.OpLT(R(t, m), v1, v0);
        a.OpAnd(R(vr, m), S(t), LU(kOneBits));
        break;
      case AluVectorOpcode::kSge:
        a.OpGE(R(t, m), v0, v1);
        a.OpAnd(R(vr, m), S(t), LU(kOneBits));
        break;
      case AluVectorOpcode::kSne:
        a.OpNE(R(t, m), v0, v1);
        a.OpAnd(R(vr, m), S(t), LU(kOneBits));
        break;
      case AluVectorOpcode::kFrc:
        a.OpRoundNI(R(t, m), v0);
        a.OpAdd(R(vr, m), v0, -S(t));
        break;
      case AluVectorOpcode::kTrunc: a.OpRoundZ(R(vr, m), v0); break;
      case AluVectorOpcode::kFloor: a.OpRoundNI(R(vr, m), v0); break;
      case AluVectorOpcode::kMad:
        Mul(t, v0, v1, m);
        a.OpAdd(R(vr, m), S(t), v2);
        break;
      case AluVectorOpcode::kCndEq:
        a.OpEq(R(t, m), v0, LF(0.0f));
        a.OpMovC(R(vr, m), S(t), v1, v2);
        break;
      case AluVectorOpcode::kCndGe:
        a.OpGE(R(t, m), v0, LF(0.0f));
        a.OpMovC(R(vr, m), S(t), v1, v2);
        break;
      case AluVectorOpcode::kCndGt:
        a.OpLT(R(t, m), LF(0.0f), v0);
        a.OpMovC(R(vr, m), S(t), v1, v2);
        break;
      case AluVectorOpcode::kDp4:
        Dot(t, v0, v1, 4, nullptr);
        a.OpMov(R(vr), S(t, Src::kXXXX));
        break;
      case AluVectorOpcode::kDp3:
        Dot(t, v0, v1, 3, nullptr);
        a.OpMov(R(vr), S(t, Src::kXXXX));
        break;
      case AluVectorOpcode::kDp2Add: {
        const Src c = v2;
        Dot(t, v0, v1, 2, &c);
        a.OpMov(R(vr), S(t, Src::kXXXX));
        break;
      }
      case AluVectorOpcode::kCube: Cube(vr, s0t); break;
      case AluVectorOpcode::kMax4:
        Simplify("max4_order", "max4 uses rexglue's >= priority; the hardware order is unresolved (notes/38)");
        Max4(t, s0t);
        a.OpMov(R(vr), S(t, Src::kXXXX));
        break;
      case AluVectorOpcode::kSetpEqPush:
      case AluVectorOpcode::kSetpNePush:
      case AluVectorOpcode::kSetpGtPush:
      case AluVectorOpcode::kSetpGePush: {
        // t.xy = (s0.w == 0, s0.x == 0); u.xy = the s1 condition on (w, x).
        a.OpEq(R(t, kXY), v0.SwizzleSwizzled(Sw(3, 0, 0, 0)), LF(0.0f));
        switch (al.vector_opcode) {
          case AluVectorOpcode::kSetpEqPush: a.OpEq(R(u, kXY), v1.SwizzleSwizzled(Sw(3, 0, 0, 0)), LF(0.0f)); break;
          case AluVectorOpcode::kSetpNePush: a.OpNE(R(u, kXY), v1.SwizzleSwizzled(Sw(3, 0, 0, 0)), LF(0.0f)); break;
          case AluVectorOpcode::kSetpGtPush: a.OpLT(R(u, kXY), LF(0.0f), v1.SwizzleSwizzled(Sw(3, 0, 0, 0))); break;
          default: a.OpGE(R(u, kXY), v1.SwizzleSwizzled(Sw(3, 0, 0, 0)), LF(0.0f)); break;
        }
        a.OpAnd(R(t, kXY), S(t), S(u));
        a.OpMov(R(St(), kY), S1(t, 0));
        a.OpAdd(R(u, kX), v0.SelectFromSwizzled(0), LF(1.0f));
        a.OpMovC(R(vr), S1(t, 1), LF(0.0f), S1(u, 0));
        break;
      }
      case AluVectorOpcode::kKillEq:
      case AluVectorOpcode::kKillGt:
      case AluVectorOpcode::kKillGe:
      case AluVectorOpcode::kKillNe:
        switch (al.vector_opcode) {
          case AluVectorOpcode::kKillEq: a.OpEq(R(t), v0, v1); break;
          case AluVectorOpcode::kKillGt: a.OpLT(R(t), v1, v0); break;
          case AluVectorOpcode::kKillGe: a.OpGE(R(t), v0, v1); break;
          default: a.OpNE(R(t), v0, v1); break;
        }
        a.OpOr(R(t, kX), S1(t, 0), S1(t, 1));
        a.OpOr(R(t, kX), S1(t, 0), S1(t, 2));
        a.OpOr(R(t, kX), S1(t, 0), S1(t, 3));
        a.OpAnd(R(vr), S1(t, 0), LU(kOneBits));
        break;
      case AluVectorOpcode::kDst:
        a.OpMov(R(vr, kX), LF(1.0f));
        Mul(t, v0.SelectFromSwizzled(1), v1.SelectFromSwizzled(1), kX);
        a.OpMov(R(vr, kY), S1(t, 0));
        a.OpMov(R(vr, kZ), v0.SelectFromSwizzled(2));
        a.OpMov(R(vr, kW), v1.SelectFromSwizzled(3));
        break;
      case AluVectorOpcode::kMaxA:
        SetA0(v0.SelectFromSwizzled(3), true);
        MaxMin(vr, v0, v1, m, true);
        break;
      default:
        Simplify("unknown_vector_opcode", std::format("vector opcode {} emitted as 0", uint32_t(al.vector_opcode)));
        a.OpMov(R(vr), LF(0.0f));
        break;
    }
    if (al.vector_result.clamp) Sat(vr, S(vr), kXYZW);
    if (al.vector_opcode >= AluVectorOpcode::kKillEq && al.vector_opcode <= AluVectorOpcode::kKillNe) {
      // if (vr.x != 0.0) discard;
      a.OpNE(R(t, kX), S1(vr, 0), LF(0.0f));
      EmitKill(S1(t, 0));
    }
  }

  void EmitScalarOp(const Alu& al, const Src& sa, const Src& sb, uint32_t sr) {  // patch 0060: sources as operands
    Scope sc(*this);
    const Src ps = S1(St(), 0);
    const uint32_t e = Push(), t = Push(), c = Push();
    bool sets_ps = true;
    switch (al.scalar_opcode) {
      case AluScalarOpcode::kAdds: case AluScalarOpcode::kAddsc0: case AluScalarOpcode::kAddsc1:
        a.OpAdd(R(e, kX), sa, sb);
        break;
      case AluScalarOpcode::kAddsPrev: a.OpAdd(R(e, kX), sa, ps); break;
      case AluScalarOpcode::kMuls: case AluScalarOpcode::kMulsc0: case AluScalarOpcode::kMulsc1:
        Mul(e, sa, sb, kX);
        if (opt.mulsc_round_toward_zero && al.scalar_opcode != AluScalarOpcode::kMuls) MulTowardZero(e, sa, sb);
        break;
      case AluScalarOpcode::kMulsPrev: Mul(e, sa, ps, kX); break;
      case AluScalarOpcode::kMulsPrev2: {
        // xe_muls_prev2: -FLT_MAX when ps == -FLT_MAX, ps or b is Inf / NaN, or !(b > 0); else xe_mul1(a, ps).
        a.OpEq(R(c, kX), ps, LU(0xFF7FFFFFu));
        ExpAllOnes(t, ps, kX);
        a.OpOr(R(c, kX), S1(c, 0), S1(t, 0));
        ExpAllOnes(t, sb, kX);
        a.OpOr(R(c, kX), S1(c, 0), S1(t, 0));
        a.OpLT(R(t, kX), LF(0.0f), sb);
        a.OpNot(R(t, kX), S1(t, 0));
        a.OpOr(R(c, kX), S1(c, 0), S1(t, 0));
        Mul(e, sa, ps, kX);
        a.OpMovC(R(e, kX), S1(c, 0), LU(0xFF7FFFFFu), S1(e, 0));
        break;
      }
      case AluScalarOpcode::kMaxs: MaxMin(e, sa, sb, kX, true); break;
      case AluScalarOpcode::kMins: MaxMin(e, sa, sb, kX, false); break;
      case AluScalarOpcode::kSeqs:
        a.OpEq(R(t, kX), sa, LF(0.0f));
        a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits));
        break;
      case AluScalarOpcode::kSgts:
        a.OpLT(R(t, kX), LF(0.0f), sa);
        a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits));
        break;
      case AluScalarOpcode::kSges:
        a.OpGE(R(t, kX), sa, LF(0.0f));
        a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits));
        break;
      case AluScalarOpcode::kSnes:
        a.OpNE(R(t, kX), sa, LF(0.0f));
        a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits));
        break;
      case AluScalarOpcode::kFrcs:
        a.OpRoundNI(R(t, kX), sa);
        a.OpAdd(R(e, kX), sa, -S1(t, 0));
        break;
      case AluScalarOpcode::kTruncs: a.OpRoundZ(R(e, kX), sa); break;
      case AluScalarOpcode::kFloors: a.OpRoundNI(R(e, kX), sa); break;
      case AluScalarOpcode::kExp:
        a.OpExp(R(t, kX), sa);
        R21(e, S1(t, 0));
        break;
      case AluScalarOpcode::kLogc:
        // xe_logc: r = xe_r21(log2(a)); -Inf -> -FLT_MAX.
        a.OpLog(R(t, kX), sa);
        R21(e, S1(t, 0));
        a.OpIEq(R(c, kX), S1(e, 0), LU(0xFF800000u));
        a.OpMovC(R(e, kX), S1(c, 0), LU(0xFF7FFFFFu), S1(e, 0));
        break;
      case AluScalarOpcode::kLog:
        a.OpLog(R(t, kX), sa);
        R21(e, S1(t, 0));
        break;
      case AluScalarOpcode::kRcpc:
      case AluScalarOpcode::kRcpf:
      case AluScalarOpcode::kRcp:
        a.OpDiv(R(t, kX), LF(1.0f), sa);
        R21(e, S1(t, 0));
        if (al.scalar_opcode == AluScalarOpcode::kRcpc) ReplaceInf(e, S1(e, 0), true);
        if (al.scalar_opcode == AluScalarOpcode::kRcpf) ReplaceInf(e, S1(e, 0), false);
        break;
      case AluScalarOpcode::kRsqc:
      case AluScalarOpcode::kRsqf:
      case AluScalarOpcode::kRsq:
        a.OpSqRt(R(t, kX), sa);
        a.OpDiv(R(t, kX), LF(1.0f), S1(t, 0));
        R21(e, S1(t, 0));
        if (al.scalar_opcode == AluScalarOpcode::kRsqc) ReplaceInf(e, S1(e, 0), true);
        if (al.scalar_opcode == AluScalarOpcode::kRsqf) ReplaceInf(e, S1(e, 0), false);
        break;
      case AluScalarOpcode::kMaxAs:
        SetA0(sa, true);
        MaxMin(e, sa, sb, kX, true);
        break;
      case AluScalarOpcode::kMaxAsf:
        SetA0(sa, false);
        MaxMin(e, sa, sb, kX, true);
        break;
      case AluScalarOpcode::kSubs: case AluScalarOpcode::kSubsc0: case AluScalarOpcode::kSubsc1:
        a.OpAdd(R(e, kX), sa, -sb);
        break;
      case AluScalarOpcode::kSubsPrev: a.OpAdd(R(e, kX), sa, -ps); break;
      case AluScalarOpcode::kSetpEq:
      case AluScalarOpcode::kSetpNe:
      case AluScalarOpcode::kSetpGt:
      case AluScalarOpcode::kSetpGe:
        switch (al.scalar_opcode) {
          case AluScalarOpcode::kSetpEq: a.OpEq(R(St(), kY), sa, LF(0.0f)); break;
          case AluScalarOpcode::kSetpNe: a.OpNE(R(St(), kY), sa, LF(0.0f)); break;
          case AluScalarOpcode::kSetpGt: a.OpLT(R(St(), kY), LF(0.0f), sa); break;
          default: a.OpGE(R(St(), kY), sa, LF(0.0f)); break;
        }
        a.OpMovC(R(e, kX), S1(St(), 1), LF(0.0f), LF(1.0f));
        break;
      case AluScalarOpcode::kSetpInv:
        // p0 = sa == 1; p0 ? 0 : (sa == 0 ? 1 : sa)
        a.OpEq(R(St(), kY), sa, LF(1.0f));
        a.OpEq(R(c, kX), sa, LF(0.0f));
        a.OpMovC(R(e, kX), S1(c, 0), LF(1.0f), sa);
        a.OpMovC(R(e, kX), S1(St(), 1), LF(0.0f), S1(e, 0));
        break;
      case AluScalarOpcode::kSetpPop:
        // p0 = (sa - 1) <= 0; p0 ? 0 : sa - 1
        a.OpAdd(R(t, kX), sa, LF(-1.0f));
        a.OpGE(R(St(), kY), LF(0.0f), S1(t, 0));
        a.OpMovC(R(e, kX), S1(St(), 1), LF(0.0f), S1(t, 0));
        break;
      case AluScalarOpcode::kSetpClr:
        a.OpMov(R(St(), kY), LU(0));
        a.OpMov(R(e, kX), LU(kFltMaxBits));
        break;
      case AluScalarOpcode::kSetpRstr:
        a.OpEq(R(St(), kY), sa, LF(0.0f));
        a.OpMovC(R(e, kX), S1(St(), 1), LF(0.0f), sa);
        break;
      case AluScalarOpcode::kKillsEq: a.OpEq(R(t, kX), sa, LF(0.0f)); a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits)); break;
      case AluScalarOpcode::kKillsGt: a.OpLT(R(t, kX), LF(0.0f), sa); a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits)); break;
      case AluScalarOpcode::kKillsGe: a.OpGE(R(t, kX), sa, LF(0.0f)); a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits)); break;
      case AluScalarOpcode::kKillsNe: a.OpNE(R(t, kX), sa, LF(0.0f)); a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits)); break;
      case AluScalarOpcode::kKillsOne: a.OpEq(R(t, kX), sa, LF(1.0f)); a.OpAnd(R(e, kX), S1(t, 0), LU(kOneBits)); break;
      case AluScalarOpcode::kSqrt:
        a.OpSqRt(R(t, kX), sa);
        R21(e, S1(t, 0));
        break;
      case AluScalarOpcode::kSin: a.OpSinCos(R(e, kX), Dest::Null(), sa); break;
      case AluScalarOpcode::kCos: a.OpSinCos(Dest::Null(), R(e, kX), sa); break;
      case AluScalarOpcode::kRetainPrev: sets_ps = false; break;
      default:
        Simplify("unknown_scalar_opcode", std::format("scalar opcode {} emitted as 0", uint32_t(al.scalar_opcode)));
        a.OpMov(R(e, kX), LF(0.0f));
        break;
    }
    if (sets_ps) a.OpMov(R(St(), kX), S1(e, 0));
    if (al.scalar_opcode >= AluScalarOpcode::kKillsEq && al.scalar_opcode <= AluScalarOpcode::kKillsOne) {
      a.OpNE(R(t, kX), ps, LF(0.0f));
      EmitKill(S1(t, 0));
    }
    // The clamp applies to the written value; ps keeps the unclamped result.
    if (al.scalar_result.clamp) Sat(sr, ps, kX);
    else if (opt.dxbc_coalesce_moves) scalar_in_ps_ = true;  // job 634: the writes read ps itself (unchanged until then)
    else a.OpMov(R(sr, kX), ps);
  }

  // ---- Resources ----
  // SM 5.1 range IDs are given in order of first use; RDEF and the declarations list them.
  struct SrvBinding {
    uint32_t reg;
    enum class Kind { kTex2D, kTexCube, kTex3D, kRaw } kind;
  };
  std::vector<SrvBinding> srvs;
  std::vector<uint32_t> samplers;  // registers
  bool uses_uav = false;
  uint32_t SrvId(uint32_t reg, SrvBinding::Kind kind) {
    for (uint32_t i = 0; i < srvs.size(); ++i)
      if (srvs[i].reg == reg) return i;
    srvs.push_back({reg, kind});
    return uint32_t(srvs.size() - 1);
  }
  uint32_t SamplerId(uint32_t reg) {
    for (uint32_t i = 0; i < samplers.size(); ++i)
      if (samplers[i] == reg) return i;
    samplers.push_back(reg);
    return uint32_t(samplers.size() - 1);
  }
  Src Tex(uint32_t reg, SrvBinding::Kind kind) { return Src::T(SrvId(reg, kind), reg); }
  Src Smp(uint32_t reg) { return Src::S(SamplerId(reg), reg); }
  Src Uav(uint32_t swizzle = Src::kXYZW) {
    uses_uav = true;
    return Src::U(0, 0, swizzle);
  }
  Dest UavDest(uint32_t mask) {
    uses_uav = true;
    return Dest::U(0, 0, mask);
  }

  // resinfo (float return) at mip 0 into d.mask.
  void ResInfo(uint32_t d, uint32_t mask, const Src& tex) {
    Raw(kOpResInfo, R(d, mask), {{LU(0), true, 0b0000, false}, {tex, false, mask, true}});
  }

  // ---- Vertex fetch (hlsl.cpp EmitVertexFetch / xe_vf_word / xe_vf_unpack) ----
  // xe_vf_word: the word at dword address dw.x (fetch constant dwords in fc.xy) into d.x.
  void VertexWord(uint32_t d, uint32_t dw, uint32_t fc) {
    Scope sc(*this);
    const uint32_t t = Push(), ring = Push();
    a.OpMov(R(d, kX), LU(0));
    // in = dw >= base && dw - base < size
    a.OpUShR(R(t, kX), S1(fc, 0), LU(2));                       // base
    a.OpUGE(R(t, kY), S1(dw, 0), S1(t, 0));
    a.OpIAdd(R(t, kZ), S1(dw, 0), -S1(t, 0));                    // dw - base
    a.OpUBFE(R(t, kW), LU(24), LU(2), S1(fc, 1));                // size
    a.OpULT(R(t, kZ), S1(t, 2), S1(t, 3));
    a.OpAnd(R(t, kY), S1(t, 1), S1(t, 2));
    a.OpIf(true, S1(t, 1));
    {
      // ring = (fdw1 bit 31) && (fdw0 & 3) == 3; a guest address is masked to 512 MB.
      a.OpAnd(R(ring, kX), S1(fc, 0), LU(3));
      a.OpIEq(R(ring, kX), S1(ring, 0), LU(3));
      a.OpAnd(R(ring, kY), S1(fc, 1), LU(0x80000000u));
      a.OpAnd(R(ring, kX), S1(ring, 0), S1(ring, 1));
      a.OpIf(true, S1(ring, 0));
      a.OpIShL(R(t, kX), S1(dw, 0), LU(2));
      a.OpLdRaw(R(d, kX), S1(t, 0), Tex(97, SrvBinding::Kind::kRaw));
      a.OpElse();
      a.OpAnd(R(t, kX), S1(dw, 0), LU(0x07FFFFFFu));
      a.OpIShL(R(t, kX), S1(t, 0), LU(2));
      if (memexport_final) a.OpLdRaw(R(d, kX), S1(t, 0), Uav());
      else a.OpLdRaw(R(d, kX), S1(t, 0), Tex(96, SrvBinding::Kind::kRaw));
      a.OpEndIf();
      a.OpAnd(R(t, kX), S1(fc, 1), LU(3));
      Swap(d, S1(d, 0), S1(t, 0), kX);
    }
    a.OpEndIf();
  }
  bool memexport_final = false;  // pass 2: the shader exports (vertex words are read through u0)

  // xe_unorm_recip(m) as a constant.
  static float UnormRecip(uint32_t m) {
    switch (m) {
      case 1u: return 1.0f;
      case 3u: return std::bit_cast<float>(0x3EAAAAABu);
      case 127u: return std::bit_cast<float>(0x3C010204u);
      case 255u: return std::bit_cast<float>(0x3B808081u);
      case 511u: return std::bit_cast<float>(0x3B004020u);
      case 1023u: return std::bit_cast<float>(0x3A802008u);
      case 2047u: return std::bit_cast<float>(0x3A001002u);
      case 32767u: return std::bit_cast<float>(0x38000100u);
      case 65535u: return std::bit_cast<float>(0x37800080u);
      default: return 1.0f / float(m);
    }
  }
  // xe_unpack_packed(w, width, offset, sgn, norm, nozero) into d.c (all but w constant).
  void UnpackPacked(uint32_t d, uint32_t c, const Src& w, uint32_t width, uint32_t offset, bool sgn, bool norm,
                    bool nozero) {
    Scope sc(*this);
    const uint32_t t = Push();
    const uint32_t m = 1u << c;
    if (sgn) {
      a.OpIShL(R(t, kX), w, LU(32 - (width + offset)));
      IShR(R(t, kX), S1(t, 0), LU(32 - width));
      a.OpIToF(R(d, m), S1(t, 0));
      if (norm) {
        if (nozero) {
          const float z = UnormRecip((1u << width) - 1);
          a.OpMul(R(d, m), S1(d, c), LF(2.0f * z));
          a.OpAdd(R(d, m), S1(d, c), LF(z));
        } else {
          a.OpMul(R(d, m), S1(d, c), LF(UnormRecip((1u << (width - 1)) - 1)));
          a.OpLT(R(t, kX), LF(-1.0f), S1(d, c));
          a.OpMovC(R(d, m), S1(t, 0), S1(d, c), LF(-1.0f));
        }
      }
    } else {
      a.OpUBFE(R(t, kX), LU(width), LU(offset), w);
      a.OpUToF(R(d, m), S1(t, 0));
      if (norm) a.OpMul(R(d, m), S1(d, c), LF(UnormRecip((1u << width) - 1)));
    }
  }
  void Unpack32(uint32_t d, uint32_t c, const Src& w, bool sgn, bool norm, bool nozero) {
    const uint32_t m = 1u << c;
    if (sgn) {
      a.OpIToF(R(d, m), w);
      if (!norm) return;
      a.OpMul(R(d, m), S1(d, c), LU(0x30000000u));
      if (nozero) a.OpAdd(R(d, m), S1(d, c), LU(0x2F800000u));
      return;
    }
    a.OpUToF(R(d, m), w);
    if (norm) a.OpMul(R(d, m), S1(d, c), LU(0x2F800000u));
  }
  // xe_half_ext(h) (h = w >> shift & 0xFFFF) into d.c.
  void HalfExt(uint32_t d, uint32_t c, const Src& w, uint32_t shift) {
    Scope sc(*this);
    const uint32_t h = Push(), e = Push(), b = Push();
    const uint32_t m = 1u << c;
    a.OpUBFE(R(h, kX), LU(16), LU(shift), w);
    a.OpUBFE(R(e, kX), LU(5), LU(10), S1(h, 0));
    a.OpAnd(R(b, kX), S1(h, 0), LU(0x8000u));
    a.OpIShL(R(b, kX), S1(b, 0), LU(16));
    a.OpIAdd(R(e, kY), S1(e, 0), LU(112));
    a.OpIShL(R(e, kY), S1(e, 1), LU(23));
    a.OpAnd(R(e, kZ), S1(h, 0), LU(1023));
    a.OpIShL(R(e, kZ), S1(e, 2), LU(13));
    a.OpOr(R(e, kY), S1(e, 1), S1(e, 2));
    a.OpOr(R(e, kY), S1(e, 1), S1(b, 0));
    a.OpMovC(R(d, m), S1(e, 0), S1(e, 1), S1(b, 0));
  }

  // Writes value temp v through a fetch-style result (hlsl.cpp StoreFetch).
  void StoreFetch(const Result& r, uint32_t v) {
    if (!r.write_mask) return;
    Scope sc(*this);
    uint32_t rel = UINT32_MAX;
    if (r.addressing == Addressing::kLoopRelative) {
      rel = Push();
      RelRegIndex(rel, r.index);
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(r.write_mask & (1u << i))) continue;
      const Swizzle s = r.components[i];
      const Src val = s == Swizzle::k0 ? LF(0.0f) : s == Swizzle::k1 ? LF(1.0f) : S1(v, uint32_t(s) & 3);
      a.OpMov(RegDest(r.index, rel, 1u << i), val);
    }
  }

  void EmitVertexFetch(const VertexFetch& f) {
    meta.uses_shared_memory = true;
    if (f.predicated) a.OpIf(f.predicate_condition, S1(St(), 1));
    Scope sc(*this);
    const uint32_t vf = f.fetch_constant;
    const uint32_t fc = Push(), vv = Push(), vw = Push(), dw = Push();
    a.OpMov(R(fc, kX), FetchDw(vf * 2));
    a.OpMov(R(fc, kY), FetchDw(vf * 2 + 1));
    if (!f.mini) {
      NoteRegister(f.address.index, f.address.addressing);
      const uint32_t idx = Push();
      ReadOperand(idx, f.address, 0);
      if (f.index_rounded) a.OpAdd(R(idx, kX), S1(idx, 0), LF(0.5f));
      a.OpRoundNI(R(idx, kX), S1(idx, 0));
      FtoI(idx, S1(idx, 0), kX);
      // xe_vfa = (fdw0 >> 2) + uint(vidx) * stride
      a.OpUShR(R(idx, kY), S1(fc, 0), LU(2));
      a.OpIMAd(R(Misc(), kY), S1(idx, 0), LU(f.stride), S1(idx, 1));
    }
    if (f.data_format == 0) {
      a.OpMov(R(vv), LF(0.0f));
    } else {
      uint32_t words = 1;
      switch (f.data_format) {
        case 26: case 32: case 34: case 37: words = 2; break;
        case 57: words = 3; break;
        case 35: case 38: words = 4; break;
        default: words = 1; break;
      }
      a.OpMov(R(vw), LU(0));
      for (uint32_t i = 0; i < words; ++i) {
        const uint32_t w = Push();
        a.OpIAdd(R(dw, kX), S1(Misc(), 1), LU(uint32_t(f.offset + int32_t(i))));
        VertexWord(w, dw, fc);
        a.OpMov(R(vw, 1u << i), S1(w, 0));
      }
      const bool sg = f.is_signed, nm = !f.is_integer, nz = f.signed_rf_mode;
      const Src wx = S1(vw, 0), wy = S1(vw, 1), wz = S1(vw, 2), ww = S1(vw, 3);
      a.OpMov(R(vv), LF(0.0f));
      switch (f.data_format) {
        case 6:
          for (uint32_t c = 0; c < 4; ++c) UnpackPacked(vv, c, wx, 8, 8 * c, sg, nm, nz);
          break;
        case 7:
          UnpackPacked(vv, 0, wx, 10, 0, sg, nm, nz);
          UnpackPacked(vv, 1, wx, 10, 10, sg, nm, nz);
          UnpackPacked(vv, 2, wx, 10, 20, sg, nm, nz);
          UnpackPacked(vv, 3, wx, 2, 30, sg, nm, nz);
          break;
        case 16:
          UnpackPacked(vv, 0, wx, 11, 0, sg, nm, nz);
          UnpackPacked(vv, 1, wx, 11, 11, sg, nm, nz);
          UnpackPacked(vv, 2, wx, 10, 22, sg, nm, nz);
          break;
        case 17:
          UnpackPacked(vv, 0, wx, 10, 0, sg, nm, nz);
          UnpackPacked(vv, 1, wx, 11, 10, sg, nm, nz);
          UnpackPacked(vv, 2, wx, 11, 21, sg, nm, nz);
          break;
        case 25:
          UnpackPacked(vv, 0, wx, 16, 0, sg, nm, nz);
          UnpackPacked(vv, 1, wx, 16, 16, sg, nm, nz);
          break;
        case 26:
          UnpackPacked(vv, 0, wx, 16, 0, sg, nm, nz);
          UnpackPacked(vv, 1, wx, 16, 16, sg, nm, nz);
          UnpackPacked(vv, 2, wy, 16, 0, sg, nm, nz);
          UnpackPacked(vv, 3, wy, 16, 16, sg, nm, nz);
          break;
        case 31:
          HalfExt(vv, 0, wx, 0);
          HalfExt(vv, 1, wx, 16);
          break;
        case 32:
          HalfExt(vv, 0, wx, 0);
          HalfExt(vv, 1, wx, 16);
          HalfExt(vv, 2, wy, 0);
          HalfExt(vv, 3, wy, 16);
          break;
        case 33: Unpack32(vv, 0, wx, sg, nm, nz); break;
        case 34:
          Unpack32(vv, 0, wx, sg, nm, nz);
          Unpack32(vv, 1, wy, sg, nm, nz);
          break;
        case 35:
          Unpack32(vv, 0, wx, sg, nm, nz);
          Unpack32(vv, 1, wy, sg, nm, nz);
          Unpack32(vv, 2, wz, sg, nm, nz);
          Unpack32(vv, 3, ww, sg, nm, nz);
          break;
        case 36: a.OpMov(R(vv, kX), wx); break;
        case 37: a.OpMov(R(vv, kXY), S(vw)); break;
        case 38: a.OpMov(R(vv), S(vw)); break;
        case 57: a.OpMov(R(vv, kXYZ), S(vw)); break;
        default: break;
      }
      if (f.exp_adjust) a.OpMul(R(vv), S(vv), LF(float(std::ldexp(1.0, f.exp_adjust))));
    }
    NoteRegister(f.result.index, f.result.addressing);
    StoreFetch(f.result, vv);
    if (f.predicated) a.OpEndIf();
  }

  // ---- Texture fetch (hlsl.cpp EmitTextureFetch / xe_tex_post / xe_cube_dir) ----

  // xe_fixed_bits(fmt.x) into d (uint4).
  void FixedBits(uint32_t d, const Src& fmt) {
    struct Entry {
      std::initializer_list<uint32_t> formats;
      uint32_t w[4];
    };
    static const Entry kTable[] = {
        {{2, 8, 9, 58, 59}, {8, 0, 0, 0}},   {{3}, {5, 5, 5, 1}},         {{4}, {5, 6, 5, 0}},
        {{5}, {5, 5, 6, 0}},                 {{6, 14, 50, 62}, {8, 8, 8, 8}}, {{7, 54}, {10, 10, 10, 2}},
        {{10, 49, 60}, {8, 8, 0, 0}},        {{13, 25}, {16, 16, 0, 0}},  {{15}, {4, 4, 4, 4}},
        {{16, 55}, {11, 11, 10, 0}},         {{17, 56}, {10, 11, 11, 0}}, {{21, 26}, {16, 16, 16, 16}},
        {{24}, {16, 0, 0, 0}},
    };
    a.OpSwitch(fmt);
    for (const Entry& e : kTable) {
      for (uint32_t f : e.formats) a.OpCase(LU(f));
      a.OpMov(R(d), LU4(e.w[0], e.w[1], e.w[2], e.w[3]));
      a.OpBreak();
    }
    a.OpDefault();
    a.OpMov(R(d), LU(0));
    a.OpBreak();
    a.OpEndSwitch();
  }

  // Job 634 (opt.tex_post_affine): xe_tex_post from per-slot constants the renderer works out from the fetch constant
  // (xenos::hlsl::TexPostAffine - its layout is described there): every mode becomes a multiply-add with constants and a
  // select per step, ~45 instructions a fetch instead of ~100 (the driver's pipeline build time follows the DXBC
  // size). The same values bit for bit as the tex_post_constants path (checked on the CPU over 367M cases, job 634).
  void TexPostAffine(uint32_t v, uint32_t slot) {
    Scope sc(*this);
    const uint32_t base = kTexAffineBase + slot * kTexAffineVec;
    const uint32_t f = Push(), c = Push(), g = Push(), t = Push(), q = Push(), y = Push();
    a.OpMov(R(f), Cb(kCbFetch, base));                     // the step masks of the four components
    // Gamma (s == 3).
    Degamma(g, S(v), kXYZW);
    a.OpAnd(R(c), S(f), LU(1));
    a.OpMovC(R(v), S(c), S(g), S(v));
    // Unsigned biased (s == 2): (v * A1 + B1) / D1 - (v * (2h - 1) - h) / (h - 1), or v * 2 - 1 (A1 2, B1 -1, D1 1).
    a.OpMul(R(t), S(v), Cb(kCbFetch, base + 1));
    a.OpAdd(R(t), S(t), Cb(kCbFetch, base + 2));
    a.OpDiv(R(t), S(t), Cb(kCbFetch, base + 3));
    a.OpAnd(R(c), S(f), LU(2));
    a.OpMovC(R(v), S(c), S(t), S(v));
    // Fixed point, not integer: the point-sampled 4-6 bit decode n * (e + 1) / (e * e), n = round(v * (e - 1)), with
    // P = e - 1 (step 16); then 16 fractional bits (step 32, sg == 0) or the clamp to -1 (step 64, sg == 2).
    a.OpMul(R(q), S(v), Cb(kCbFetch, base + 6));
    a.OpRoundNE(R(q), S(q));
    a.OpAdd(R(t), Cb(kCbFetch, base + 6), LF(2.0f));       // e + 1
    a.OpMul(R(q), S(q), S(t));
    a.OpAdd(R(t), Cb(kCbFetch, base + 6), LF(1.0f));       // e
    a.OpMul(R(t), S(t), S(t));                             // e * e
    a.OpDiv(R(q), S(q), S(t));
    a.OpAnd(R(c), S(f), LU(16));
    a.OpMovC(R(q), S(c), S(q), S(v));
    a.OpMul(R(y), S(q), LF(65536.0f));
    a.OpRoundNE(R(y), S(y));
    a.OpDiv(R(y), S(y), LF(65536.0f));
    a.OpMax(R(t), S(v), LF(-1.0f));
    a.OpAnd(R(c), S(f), LU(32));
    a.OpMovC(R(v), S(c), S(y), S(v));
    a.OpAnd(R(c), S(f), LU(64));
    a.OpMovC(R(v), S(c), S(t), S(v));
    // Integer num_format (step 4): v * A2 + B2, rounded when point sampled (step 8).
    a.OpMul(R(t), S(v), Cb(kCbFetch, base + 4));
    a.OpAdd(R(t), S(t), Cb(kCbFetch, base + 5));
    a.OpRoundNE(R(q), S(t));
    a.OpAnd(R(c), S(f), LU(8));
    a.OpMovC(R(t), S(c), S(q), S(t));
    a.OpAnd(R(c), S(f), LU(4));
    a.OpMovC(R(v), S(c), S(t), S(v));
    // Exponent adjust: 2^((int(dw3 << 13)) >> 26).
    a.OpMul(R(v), S(v), Cb(kCbFetch, base + 7));
  }

  // xe_tex_post(v, slot) in place on temp v.
  void TexPost(uint32_t v, uint32_t slot) {
    if (opt.tex_post_affine && opt.tex_num_format && opt.tex_post_constants) {  // job 634
      TexPostAffine(v, slot);
      return;
    }
    Scope sc(*this);
    const uint32_t D = Push(), sw = Push(), s = Push(), t = Push(), u = Push(), c = Push();
    const bool tpc = opt.tex_num_format && opt.tex_post_constants;  // patch 0105
    if (!tpc) {
    a.OpMov(R(D, kX), FetchDw(slot * 6));
    a.OpMov(R(D, kY), FetchDw(slot * 6 + 1));
    a.OpMov(R(D, kZ), FetchDw(slot * 6 + 3));
    }
    // Per component: sw (the source component, 4 / 5 = constant) and s (the sign mode).
    if (tpc) {
      // Patch 0105: s from the renderer's per-slot dwords (TexPostConstants); sw is not needed.
      a.OpMov(R(D, kX), Cb(kCbFetch, 48 + slot / 2).Select((slot & 1) * 2));
      a.OpMov(R(D, kY), Cb(kCbFetch, 48 + slot / 2).Select((slot & 1) * 2 + 1));
      a.OpMov(R(D, kZ), FetchDw(slot * 6 + 3));  // dw3: the exponent adjust below
      a.OpUBFE(R(s), LU(2), LU4(0, 4, 8, 12), S1(D, 0));
    } else if (opt.tex_sign_by_swizzle) {
      a.OpUBFE(R(sw), LU(3), LU4(1, 4, 7, 10), S1(D, 2));
      a.OpIMAd(R(t), S(sw), LU(2), LU(2));
      a.OpUShR(R(s), S1(D, 0), S(t));
      a.OpAnd(R(s), S(s), LU(3));
      a.OpULT(R(c), S(sw), LU(4));
      a.OpMovC(R(s), S(c), S(s), LU(0));
    } else {
      a.OpMov(R(sw), LU4(0, 1, 2, 3));
      a.OpUBFE(R(s), LU(2), LU4(2, 4, 6, 8), S1(D, 0));
    }
    if (!opt.tex_num_format) {
      // if (s == 2) v = v * 2 - 1; else if (s == 3) v = xe_degamma1(v);
      a.OpMul(R(t), S(v), LF(2.0f));
      a.OpAdd(R(t), S(t), LF(-1.0f));
      Degamma(u, S(v), kXYZW);
      a.OpIEq(R(c), S(s), LU(2));
      a.OpMovC(R(v), S(c), S(t), S(v));
      a.OpIEq(R(c), S(s), LU(3));
      a.OpMovC(R(v), S(c), S(u), S(v));
    } else {
      const uint32_t fb = Push(), w = Push(), sg = Push(), wd = Push(), fl = Push(), car = Push(), h = Push();
      if (tpc) {  // patch 0105: sg, wd and the fixed / integer / point-sampled masks from TexPostConstants
        a.OpUBFE(R(sg), LU(2), LU4(2, 6, 10, 14), S1(D, 0));
        a.OpUBFE(R(wd), LU(8), LU4(0, 8, 16, 24), S1(D, 1));
        a.OpUBFE(R(t, kXYZ), LU(1), LU4(16, 17, 18, 0), S1(D, 0));
        a.OpINE(R(fl, kXYZ), S(t), LU(0));
      } else {
      // fb = xe_fixed_bits(dw1 & 63); fl.x fixed, fl.y integer, fl.z point sampled, fl.w last.
      a.OpAnd(R(t, kX), S1(D, 1), LU(63));
      FixedBits(fb, S1(t, 0));
      a.OpINE(R(t), S(fb), LU(0));
      a.OpOr(R(t, kX), S1(t, 0), S1(t, 1));
      a.OpOr(R(t, kX), S1(t, 0), S1(t, 2));
      a.OpOr(R(fl, kX), S1(t, 0), S1(t, 3));
      a.OpAnd(R(t, kY), S1(D, 2), LU(1));
      a.OpINE(R(fl, kY), S1(t, 1), LU(0));
      // pt = ((dw3 >> 19) & 15) == 0 && (mipf == 0 || mipf == 2) && ((dw3 >> 25) & 7) == 0
      a.OpUBFE(R(t, kXYZ), LU4(4, 2, 3, 0), LU4(19, 23, 25, 0), S1(D, 2));
      a.OpIEq(R(u, kX), S1(t, 0), LU(0));
      a.OpIEq(R(u, kY), S1(t, 1), LU(0));
      a.OpIEq(R(u, kZ), S1(t, 1), LU(2));
      a.OpOr(R(u, kY), S1(u, 1), S1(u, 2));
      a.OpIEq(R(u, kZ), S1(t, 2), LU(0));
      a.OpAnd(R(u, kX), S1(u, 0), S1(u, 1));
      a.OpAnd(R(fl, kZ), S1(u, 0), S1(u, 2));
      // last = fb.w ? 3 : (fb.z ? 2 : (fb.y ? 1 : 0))
      a.OpMovC(R(t, kX), S1(fb, 1), LU(1), LU(0));
      a.OpMovC(R(t, kX), S1(fb, 2), LU(2), S1(t, 0));
      a.OpMovC(R(fl, kW), S1(fb, 3), LU(3), S1(t, 0));
      // w = sw < 4 && fixed ? fb[min(sw, last)] : 0
      a.OpUMin(R(t), S(sw), S1(fl, 3));
      a.OpIEq(R(c), S(t), LU(2));
      a.OpMovC(R(w), S(c), S1(fb, 2), S1(fb, 3));
      a.OpIEq(R(c), S(t), LU(1));
      a.OpMovC(R(w), S(c), S1(fb, 1), S(w));
      a.OpIEq(R(c), S(t), LU(0));
      a.OpMovC(R(w), S(c), S1(fb, 0), S(w));
      a.OpULT(R(c), S(sw), LU(4));
      a.OpAnd(R(c), S(c), S1(fl, 0));
      a.OpMovC(R(w), S(c), S(w), LU(0));
      // ok16 (u) = w != 0 && w <= 16; carries = ok16; s == 3 -> false; else !integer && s == 0 -> carries && pt &&
      // 4 <= w <= 6.
      a.OpINE(R(u), S(w), LU(0));
      a.OpUGE(R(t), LU(16), S(w));
      a.OpAnd(R(u), S(u), S(t));
      a.OpUGE(R(t), S(w), LU(4));
      a.OpUGE(R(h), LU(6), S(w));
      a.OpAnd(R(t), S(t), S(h));
      a.OpAnd(R(t), S(t), S(u));
      a.OpAnd(R(t), S(t), S1(fl, 2));                     // carries && pt && 4..6
      a.OpIEq(R(c), S(s), LU(0));
      a.OpNot(R(h), S1(fl, 1));
      a.OpAnd(R(c), S(c), S(h, Src::kXXXX));               // !integer && s == 0
      a.OpMovC(R(car), S(c), S(t), S(u));
      a.OpIEq(R(c), S(s), LU(3));
      a.OpMovC(R(car), S(c), LU(0), S(car));
      // sg = sw < 4 && fixed && ok16 && !(s == 3 && integer) ? s : 0
      a.OpAnd(R(t), S(c), S1(fl, 1));                      // s == 3 && integer
      a.OpNot(R(t), S(t));
      a.OpAnd(R(t), S(t), S(u));
      a.OpULT(R(h), S(sw), LU(4));
      a.OpAnd(R(t), S(t), S(h));
      a.OpAnd(R(t), S(t), S1(fl, 0));
      a.OpMovC(R(sg), S(t), S(s), LU(0));
      // wd = carries ? w : 0
      a.OpMovC(R(wd), S(car), S(w), LU(0));
      }  // patch 0105
      // s == 2: wd >= 2 ? (v * (2h - 1) - h) / (h - 1) with h = 2^(wd - 1) : v * 2 - 1.
      a.OpIAdd(R(t), S(wd), LI(-1));
      a.OpIShL(R(t), LU(1), S(t));
      a.OpUToF(R(h), S(t));
      a.OpMul(R(t), S(h), LF(2.0f));
      a.OpAdd(R(t), S(t), LF(-1.0f));
      a.OpMul(R(t), S(v), S(t));
      a.OpAdd(R(t), S(t), -S(h));
      a.OpAdd(R(u), S(h), LF(-1.0f));
      a.OpDiv(R(t), S(t), S(u));
      a.OpMul(R(u), S(v), LF(2.0f));
      a.OpAdd(R(u), S(u), LF(-1.0f));
      a.OpUGE(R(c), S(wd), LU(2));
      a.OpMovC(R(t), S(c), S(t), S(u));
      Degamma(u, S(v), kXYZW);
      a.OpIEq(R(c), S(s), LU(2));
      a.OpMovC(R(v), S(c), S(t), S(v));
      a.OpIEq(R(c), S(s), LU(3));
      a.OpMovC(R(v), S(c), S(u), S(v));
      // if (fixed)
      a.OpIf(true, S1(fl, 0));
      a.OpIf(false, S1(fl, 1));
      {
        // !integer: sg != 0 -> (sg == 2 ? max(v, -1) : v); else (wd != 0 -> n * (e + 1) / (e * e) with
        // n = round(v * (e - 1)), e = 2^wd), then round(v * 65536) / 65536.
        const uint32_t x = Push();
        a.OpIShL(R(t), LU(1), S(wd));
        a.OpUToF(R(h), S(t));                                // e
        a.OpAdd(R(t), S(h), LF(-1.0f));
        a.OpMul(R(t), S(v), S(t));
        a.OpRoundNE(R(t), S(t));                             // n
        a.OpAdd(R(u), S(h), LF(1.0f));
        a.OpMul(R(t), S(t), S(u));
        a.OpMul(R(u), S(h), S(h));
        a.OpDiv(R(t), S(t), S(u));
        a.OpINE(R(c), S(wd), LU(0));
        a.OpMovC(R(x), S(c), S(t), S(v));
        a.OpMul(R(x), S(x), LF(65536.0f));
        a.OpRoundNE(R(x), S(x));
        a.OpDiv(R(x), S(x), LF(65536.0f));
        a.OpMax(R(t), S(v), LF(-1.0f));
        a.OpIEq(R(c), S(sg), LU(2));
        a.OpMovC(R(t), S(c), S(t), S(v));
        a.OpINE(R(c), S(sg), LU(0));
        a.OpMovC(R(v), S(c), S(t), S(x));
      }
      a.OpElse();
      {
        // integer: wf = wd ? wd : 1; shift = sg == 1 || sg == 2 ? wf - 1 : wf; sc = float((1 << shift) - 1);
        // v = v * max(sc, 0.5) + (min(sc, 0.5) - 0.5); if (pt) v = round(v).
        a.OpMovC(R(t), S(wd), S(wd), LU(1));
        a.OpIEq(R(c), S(sg), LU(1));
        a.OpIEq(R(u), S(sg), LU(2));
        a.OpOr(R(c), S(c), S(u));
        a.OpIAdd(R(u), S(t), LI(-1));
        a.OpMovC(R(t), S(c), S(u), S(t));
        a.OpIShL(R(t), LU(1), S(t));
        a.OpIAdd(R(t), S(t), LI(-1));
        a.OpUToF(R(h), S(t));
        a.OpMax(R(t), S(h), LF(0.5f));
        a.OpMin(R(u), S(h), LF(0.5f));
        a.OpAdd(R(u), S(u), LF(-0.5f));
        a.OpMul(R(t), S(v), S(t));
        a.OpAdd(R(v), S(t), S(u));
        a.OpRoundNE(R(t), S(v));
        a.OpMovC(R(v), S1(fl, 2), S(t), S(v));
      }
      a.OpEndIf();
      a.OpEndIf();
    }
    // Exponent adjust: v * exp2(float(int(dw3 << 13) >> 26)).
    a.OpIBFE(R(t, kX), LU(6), LU(13), S1(D, 2));
    a.OpIToF(R(t, kX), S1(t, 0));
    a.OpExp(R(t, kX), S1(t, 0));
    a.OpMul(R(v), S(v), S(t, Src::kXXXX));
  }

  // xe_cube_dir(c.xyz) into d.xyz.
  void CubeDir(uint32_t d, uint32_t c) {
    Scope sc(*this);
    const uint32_t st = Push(), f = Push(), r0 = Push(), r1 = Push(), r2 = Push();
    // sc = c.x * 2 - 3, tc = c.y * 2 - 3
    a.OpMul(R(st, kXY), S(c), LF(2.0f));
    a.OpAdd(R(st, kXY), S(st), LF(-3.0f));
    // face = min(uint(max(c.z, 0)), 5); f.x = n (odd), f.y = axis
    a.OpMax(R(f, kX), S1(c, 2), LF(0.0f));
    a.OpFToU(R(f, kX), S1(f, 0));
    a.OpUMin(R(f, kX), S1(f, 0), LU(5));
    a.OpUShR(R(f, kY), S1(f, 0), LU(1));
    a.OpAnd(R(f, kX), S1(f, 0), LU(1));
    const Src scs = S1(st, 0), tcs = S1(st, 1), n = S1(f, 0);
    const uint32_t ng = Push();
    const Src nsc = NegBits(ng, 0, scs), ntc = NegBits(ng, 1, tcs);
    // axis 0: (n ? -1 : 1, -tc, n ? sc : -sc)
    a.OpMovC(R(r0, kX), n, LF(-1.0f), LF(1.0f));
    a.OpMov(R(r0, kY), ntc);
    a.OpMovC(R(r0, kZ), n, scs, nsc);
    // axis 1: (sc, n ? -1 : 1, n ? -tc : tc)
    a.OpMov(R(r1, kX), scs);
    a.OpMovC(R(r1, kY), n, LF(-1.0f), LF(1.0f));
    a.OpMovC(R(r1, kZ), n, ntc, tcs);
    // axis 2: (n ? -sc : sc, -tc, n ? -1 : 1)
    a.OpMovC(R(r2, kX), n, nsc, scs);
    a.OpMov(R(r2, kY), ntc);
    a.OpMovC(R(r2, kZ), n, LF(-1.0f), LF(1.0f));
    a.OpIEq(R(f, kZ), S1(f, 1), LU(0));
    a.OpIEq(R(f, kW), S1(f, 1), LU(1));
    a.OpMovC(R(d, kXYZ), S1(f, 3), S(r1), S(r2));
    a.OpMovC(R(d, kXYZ), S1(f, 2), S(r0), S(d));
  }

  // Reads coordinate components into tc (hlsl.cpp: Operand1 of each).
  void ReadCoords(uint32_t tc, const Operand& op, uint32_t n, bool one_d) {
    Scope sc(*this);
    const uint32_t t = Push();
    a.OpMov(R(tc), LF(0.0f));
    const uint32_t count = one_d ? 1 : n;
    for (uint32_t i = 0; i < count; ++i) {
      ReadOperand(t, op, int(i));
      a.OpMov(R(tc, 1u << i), S1(t, 0));
    }
  }

  void EmitTextureFetch(const TextureFetch& f) {
    if (f.predicated) a.OpIf(f.predicate_condition, S1(St(), 1));
    Scope sc(*this);
    NoteRegister(f.coordinates.index, f.coordinates.addressing);
    const uint32_t k = f.fetch_constant;
    const uint32_t tc = Push(), tsz = Push(), tv = Push(), t = Push(), tdir = Push();
    auto zero_result = [&]() {
      if (f.has_result) {
        NoteRegister(f.result.index, f.result.addressing);
        a.OpMov(R(tv), LF(0.0f));
        StoreFetch(f.result, tv);
      }
    };
    switch (f.opcode) {
      case FetchOpcode::kSetTextureLod:
        ReadOperand(t, f.coordinates, 0);
        a.OpMov(R(Misc(), kX), S1(t, 0));
        break;
      case FetchOpcode::kSetTextureGradientsHorz:
      case FetchOpcode::kSetTextureGradientsVert: {
        const uint32_t g = f.opcode == FetchOpcode::kSetTextureGradientsHorz ? GradH() : GradV();
        for (uint32_t i = 0; i < 3; ++i) {
          ReadOperand(t, f.coordinates, int(i));
          a.OpMov(R(g, 1u << i), S1(t, 0));
        }
        break;
      }
      case FetchOpcode::kTextureFetch:
      case FetchOpcode::kGetTextureComputedLod: {
        const bool cube = f.dimension == FetchDimension::kCube;
        const bool is3d = f.dimension == FetchDimension::k3DOrStacked;
        Src tex = Src::LU(0);
        SrvBinding::Kind kind;
        uint32_t reg;
        if (cube) {
          meta.texture_slots_cube |= 1u << k;
          kind = SrvBinding::Kind::kTexCube;
          reg = 32 + k;
        } else if (is3d) {
          meta.texture_slots_3d |= 1u << k;
          kind = SrvBinding::Kind::kTex3D;
          reg = 64 + k;
          Simplify("tfetch3d_stacked_as_3d", "3D/stacked fetch always samples a Texture3D (stacked 2D arrays not handled)");
        } else {
          meta.texture_slots_2d |= 1u << k;
          kind = SrvBinding::Kind::kTex2D;
          reg = k;
        }
        tex = Tex(reg, kind);
        const uint32_t n = cube || is3d ? 3 : 2;
        const bool one_d = f.dimension == FetchDimension::k1D;
        const bool one_d_xy = one_d && opt.canary_tfetch1d_xy && f.coordinates.component_count >= 2;
        ReadCoords(tc, f.coordinates, n, one_d && !one_d_xy);
        if (one_d_xy) {
          // Patch 0087 (Canary fbdb1f2 / 7cd4794): the decoder gave this tfetch1D two coordinates; Y is kept when the
          // fetch constant says the texture is not 1D (dword 5 bits 9-10), else 0 as before.
          a.OpUBFE(R(t, kX), LU(2), LU(9), FetchDw(6 * k + 5));
          a.OpMovC(R(tc, kY), S1(t, 0), S1(tc, 1), LF(0.0f));
          rule_bits |= 1u;
        }
        const float ox = f.offset_x + 1.5f / 1024.0f, oy = f.offset_y + 1.5f / 1024.0f, oz = f.offset_z + 1.5f / 1024.0f;
        uint32_t coord = tc;
        if (cube) {
          ResInfo(tsz, kXY, tex);
          a.OpDiv(R(t, kXY), LF4(ox, oy, 0.0f, 0.0f), S(tsz));
          a.OpAdd(R(tc, kXY), S(tc), S(t));
          CubeDir(tdir, tc);
          coord = tdir;
        } else if (is3d) {
          ResInfo(tsz, kXYZ, tex);
          if (f.unnormalized_coordinates) a.OpDiv(R(tc, kXYZ), S(tc), S(tsz));
          a.OpDiv(R(t, kXYZ), LF4(ox, oy, oz, 0.0f), S(tsz));
          a.OpAdd(R(tc, kXYZ), S(tc), S(t));
        } else {
          ResInfo(tsz, kXY, tex);
          // Stage 7 resolution scale (bit k of xe_ndc_offset.w): tsz /= xe_ndc_scale.w.
          a.OpUBFE(R(t, kX), LU(1), LU(k), Sys(2).Select(3));
          a.OpDiv(R(t, kYZ), S(tsz, Sw(0, 0, 1, 0)), Sys(1).Select(3));
          a.OpMovC(R(tsz, kXY), S1(t, 0), S(t, Sw(1, 2, 0, 0)), S(tsz));
          if (f.unnormalized_coordinates && f.opcode == FetchOpcode::kTextureFetch) {
            // Patch 0040: one locked mip (dword 4 MipMinLevel == MipMaxLevel): tsz = max(floor(tsz / 2^mip), 1).
            const uint32_t m = Push();
            a.OpMov(R(m, kX), FetchDw(6 * k + 4));
            a.OpUBFE(R(m, kYZ), LU(4), LU4(0, 2, 6, 0), S1(m, 0));
            a.OpIEq(R(m, kW), S1(m, 1), S1(m, 2));
            a.OpIShL(R(m, kX), LU(1), S1(m, 1));
            a.OpUToF(R(m, kX), S1(m, 0));
            a.OpDiv(R(t, kXY), S(tsz), S(m, Src::kXXXX));
            a.OpRoundNI(R(t, kXY), S(t));
            a.OpMax(R(t, kXY), S(t), LF(1.0f));
            a.OpMovC(R(tsz, kXY), S1(m, 3), S(t), S(tsz));
          }
          if (f.unnormalized_coordinates) a.OpDiv(R(tc, kXY), S(tc), S(tsz));
          a.OpDiv(R(t, kXY), LF4(ox, oy, 0.0f, 0.0f), S(tsz));
          a.OpAdd(R(tc, kXY), S(tc), S(t));
        }
        if (f.offset_x != 0.0f || f.offset_y != 0.0f || f.offset_z != 0.0f) {
          Simplify("tex_offset_mip0", "fetch offset applied in mip-0 texels (Canary's way; hardware formula unknown)");
        }
        if (f.mag_filter != TextureFilter::kUseFetchConst || f.min_filter != TextureFilter::kUseFetchConst ||
            f.mip_filter != TextureFilter::kUseFetchConst || f.aniso_filter != AnisoFilter::kUseFetchConst ||
            f.vol_mag_filter != TextureFilter::kUseFetchConst || f.vol_min_filter != TextureFilter::kUseFetchConst) {
          Simplify("tex_filter_override", "per-instruction filter override ignored (the fetch constant's sampler is used)");
        }
        const Src smp = Smp(k);
        const uint32_t amask = (1u << n) - 1;
        if (f.opcode == FetchOpcode::kGetTextureComputedLod) {
          a.OpMov(R(tv), LF(0.0f));
          if (p.is_vertex_shader) {
            Simplify("tex_lod_query_vs", "getCompTexLOD in a vertex shader returns 0");
          } else {
            a.OpLOD(R(t, kX), S(coord), n, tex, smp);
            a.OpMov(R(tv, kX), S1(t, 0));
          }
        } else {
          if (f.use_register_gradients) {
            a.OpSampleD(R(tv), S(coord), n, tex, smp, S(GradH()), S(GradV()), n);
          } else if (!f.use_computed_lod || p.is_vertex_shader) {
            if (f.use_register_lod) {
              a.OpAdd(R(t, kX), LF(f.lod_bias), S1(Misc(), 0));
              a.OpSampleL(R(tv), S(coord), n, tex, smp, S1(t, 0));
            } else {
              a.OpSampleL(R(tv), S(coord), n, tex, smp, LF(f.lod_bias));
            }
          } else if (f.lod_bias != 0.0f || f.use_register_lod) {
            if (f.use_register_lod) a.OpAdd(R(t, kX), LF(f.lod_bias), S1(Misc(), 0));
            else a.OpMov(R(t, kX), LF(f.lod_bias));
            Raw(kOpSampleB, R(tv), {{S(coord), false, amask, false}, {tex, false, kXYZW, true}, {smp, false, 0, false},
                                    {S1(t, 0), false, 0, false}});
          } else {
            Raw(kOpSample, R(tv), {{S(coord), false, amask, false}, {tex, false, kXYZW, true}, {smp, false, 0, false}});
          }
          TexPost(tv, k);
        }
        if (f.has_result) {
          NoteRegister(f.result.index, f.result.addressing);
          StoreFetch(f.result, tv);
        }
        break;
      }
      case FetchOpcode::kGetTextureWeights: {
        const bool cube = f.dimension == FetchDimension::kCube;
        const bool is3d = f.dimension == FetchDimension::k3DOrStacked;
        if (cube) {
          Simplify("tex_get_weights_cube", "getWeights of a cube map returns 0");
          zero_result();
          break;
        }
        const float ox = f.offset_x + 1.5f / 1024.0f, oy = f.offset_y + 1.5f / 1024.0f, oz = f.offset_z + 1.5f / 1024.0f;
        if (is3d) {
          meta.texture_slots_3d |= 1u << k;
          const Src tex = Tex(64 + k, SrvBinding::Kind::kTex3D);
          ReadCoords(tc, f.coordinates, 3, false);
          ResInfo(tsz, kXYZ, tex);
          if (f.unnormalized_coordinates) a.OpDiv(R(tc, kXYZ), S(tc), S(tsz));
          a.OpDiv(R(t, kXYZ), LF4(ox, oy, oz, 0.0f), S(tsz));
          a.OpAdd(R(tc, kXYZ), S(tc), S(t));
          a.OpMul(R(t, kXYZ), S(tc), S(tsz));
          a.OpAdd(R(t, kXYZ), S(t), LF(-0.5f));
          a.OpMov(R(tv), LF(0.0f));
          a.OpFrc(R(tv, kXYZ), S(t));
        } else {
          meta.texture_slots_2d |= 1u << k;
          const Src tex = Tex(k, SrvBinding::Kind::kTex2D);
          ReadCoords(tc, f.coordinates, 2, f.dimension == FetchDimension::k1D);
          ResInfo(tsz, kXY, tex);
          a.OpUBFE(R(t, kX), LU(1), LU(k), Sys(2).Select(3));
          a.OpDiv(R(t, kYZ), S(tsz, Sw(0, 0, 1, 0)), Sys(1).Select(3));
          a.OpMovC(R(tsz, kXY), S1(t, 0), S(t, Sw(1, 2, 0, 0)), S(tsz));
          if (f.unnormalized_coordinates) a.OpDiv(R(tc, kXY), S(tc), S(tsz));
          a.OpDiv(R(t, kXY), LF4(ox, oy, 0.0f, 0.0f), S(tsz));
          a.OpAdd(R(tc, kXY), S(tc), S(t));
          a.OpMul(R(t, kXY), S(tc), S(tsz));
          a.OpAdd(R(t, kXY), S(t), LF(-0.5f));
          a.OpMov(R(tv), LF(0.0f));
          a.OpFrc(R(tv, kXY), S(t));
          if (f.dimension == FetchDimension::k1D) a.OpMov(R(tv, kY), LF(0.0f));
        }
        if (f.has_result) {
          NoteRegister(f.result.index, f.result.addressing);
          StoreFetch(f.result, tv);
        }
        break;
      }
      case FetchOpcode::kGetTextureGradients:
        Simplify("tex_get_gradients", "getGradients returns 0 (not implemented)");
        zero_result();
        break;
      case FetchOpcode::kGetTextureBorderColorFrac:
        Simplify("tex_border_color_frac", "getBCF returns 0 (not implemented)");
        zero_result();
        break;
      default:
        Simplify("unknown_fetch_opcode", std::format("fetch opcode {} ignored", uint32_t(f.opcode)));
        break;
    }
    if (f.predicated) a.OpEndIf();
  }

  // ---- Control flow (hlsl.cpp EmitStraightLine / EmitPcLoop) ----
  void EmitExecBody(const ControlFlow& cf) {
    for (const Instruction& ins : cf.instructions) {
      switch (ins.kind) {
        case Instruction::Kind::kAlu: EmitAlu(ins.alu); break;
        case Instruction::Kind::kVertexFetch: EmitVertexFetch(ins.vfetch); break;
        case Instruction::Kind::kTextureFetch: EmitTextureFetch(ins.tfetch); break;
      }
    }
  }
  static bool IsExec(CfOpcode op) {
    switch (op) {
      case CfOpcode::kExec: case CfOpcode::kExecEnd: case CfOpcode::kCondExec: case CfOpcode::kCondExecEnd:
      case CfOpcode::kCondExecPred: case CfOpcode::kCondExecPredEnd: case CfOpcode::kCondExecPredClean:
      case CfOpcode::kCondExecPredCleanEnd:
        return true;
      default:
        return false;
    }
  }
  // The condition of an exec or a branch as 0 / ~0 in t.x; false when unconditional.
  bool CondValue(uint32_t t, ControlFlow::ExecType type, uint32_t bool_constant, bool condition) {
    if (type == ControlFlow::ExecType::kConditional) {
      const uint32_t k = bool_constant;
      a.OpAnd(R(t, kX), Cb(kCbBoolLoop, k >> 7).Select((k >> 5) & 3), LU(1u << (k & 31)));
      if (condition) a.OpINE(R(t, kX), S1(t, 0), LU(0));
      else a.OpIEq(R(t, kX), S1(t, 0), LU(0));
      return true;
    }
    if (type == ControlFlow::ExecType::kPredicated) {
      if (condition) a.OpMov(R(t, kX), S1(St(), 1));
      else a.OpNot(R(t, kX), S1(St(), 1));
      return true;
    }
    return false;
  }
  // Opens the `if` of an exec's condition; returns whether one was opened.
  bool OpenExec(const ControlFlow& cf) {
    Scope sc(*this);
    const uint32_t t = Push();
    if (!CondValue(t, cf.exec_type, cf.bool_constant, cf.condition)) return false;
    a.OpIf(true, S1(t, 0));
    return true;
  }

  void EmitStraightLine() {
    // Patch 0059: the loop exists only for a conditional exec_end's `break`; without one, no loop.
    bool needs_loop = !opt.dxbc_loop_only_if_needed;
    for (const ControlFlow& cf : p.control_flow)
      if (IsExec(cf.opcode) && cf.exec_end && cf.exec_type != ControlFlow::ExecType::kUnconditional) needs_loop = true;
    if (!needs_loop) {
      size_t dead_from = SIZE_MAX;
      for (const ControlFlow& cf : p.control_flow) {
        if (!IsExec(cf.opcode)) continue;
        const bool c = OpenExec(cf);
        EmitExecBody(cf);
        if (c) a.OpEndIf();
        // An unconditional exec_end: the rest is generated (same metadata) but cut, as in the loop form below.
        if (cf.exec_end && dead_from == SIZE_MAX) dead_from = code.size();
      }
      if (dead_from != SIZE_MAX) code.resize(dead_from);
      return;
    }
    a.OpLoop();
    // After an unconditional exec_end the rest is unreachable: it is still generated (for the same metadata as the HLSL
    // path) but not kept - code after a `break` in the same block is invalid in structured SPIR-V and unusual in DXBC.
    size_t dead_from = SIZE_MAX;
    for (const ControlFlow& cf : p.control_flow) {
      if (!IsExec(cf.opcode)) continue;
      const bool c = OpenExec(cf);
      EmitExecBody(cf);
      if (cf.exec_end && c) a.OpBreak();
      if (c) a.OpEndIf();
      if (cf.exec_end && !c && dead_from == SIZE_MAX) dead_from = code.size();  // the loop end is the break
    }
    if (dead_from != SIZE_MAX) code.resize(dead_from);
    EndOnePassLoop();
  }
  // Ends a loop body that runs once unless it `continue`s: `breakc_nz` on an all-ones value, then `endloop`. Same
  // behaviour as `break; endloop`; written this way because a `break` directly before `endloop` (valid DXBC) is
  // turned into invalid SPIR-V by vkd3d 1.2, which the local tests use to check this translator's output.
  void EndOnePassLoop() {
    Scope sc(*this);
    const uint32_t t = Push();
    a.OpMov(R(t, kX), LU(~0u));
    code.push_back(kOpBreakC | (1u << 18) | (uint32_t(1 + S1(t, 0).GetLength(0)) << 24));
    S1(t, 0).Write(code, true, 0);
    ++stat.instruction_count;
    a.OpEndLoop();
  }

  void SetPc(uint32_t v) { a.OpMov(R(Fl(), kX), LU(v)); }

  // Job 634 (opt.dxbc_structured_jumps): a program whose only flow control is forward jumps (1,210 of the 1,211 jumping
  // shaders of a Judgment run - no loops, calls or returns) as nested if / else blocks instead of the pc loop: a jump
  // over [k + 1, t) becomes `if (!jump) { ... }`, and one whose skipped part ends in an unconditional jump to u becomes
  // `if (!jump) { [k + 1, t - 1) } else { [t, u) }`. No loop, guard counter or per-block pc compare for the driver to
  // work through. Only when every jump stays inside the block it starts in and no exec ends the program inside a block
  // (StructuredOk, a dry run of the same walk); the others keep the pc loop.
  int CfPos(uint32_t index) const {  // position in p.control_flow of a control-flow index; size() = past the end
    for (size_t i = 0; i < p.control_flow.size(); ++i)
      if (p.control_flow[i].index == index) return int(i);
    uint32_t last = 0;
    for (const ControlFlow& cf : p.control_flow) last = std::max(last, cf.index);
    return index > last ? int(p.control_flow.size()) : -1;
  }
  bool StructuredOk(int from, int to, bool top) const {
    for (int k = from; k < to; ++k) {
      const ControlFlow& cf = p.control_flow[size_t(k)];
      switch (cf.opcode) {
        case CfOpcode::kLoopStart: case CfOpcode::kLoopEnd: case CfOpcode::kCondCall: case CfOpcode::kReturn:
          return false;
        case CfOpcode::kCondJmp: {
          const int t = CfPos(cf.target);
          if (t <= k || t > to) return false;  // backward, outside this block, or unknown
          int skip_end = t, else_end = -1;
          if (t - 1 > k) {
            const ControlFlow& j = p.control_flow[size_t(t - 1)];
            if (j.opcode == CfOpcode::kCondJmp && j.branch_type == ControlFlow::ExecType::kUnconditional) {
              const int u = CfPos(j.target);
              if (u >= t && u <= to) {
                skip_end = t - 1;
                else_end = u;
              }
            }
          }
          if (!StructuredOk(k + 1, skip_end, false)) return false;
          if (else_end >= 0 && !StructuredOk(t, else_end, false)) return false;
          k = (else_end >= 0 ? else_end : t) - 1;
          break;
        }
        default:
          if (IsExec(cf.opcode) && cf.exec_end) {
            // Only an unconditional end at the top level (the rest is cut, as in EmitStraightLine).
            if (!top || cf.exec_type != ControlFlow::ExecType::kUnconditional) return false;
          }
          break;
      }
    }
    return true;
  }
  // Emits [from, to).
  void EmitStructured(int from, int to) {
    for (int k = from; k < to; ++k) {
      const ControlFlow& cf = p.control_flow[size_t(k)];
      if (cf.opcode == CfOpcode::kCondJmp) {
        const int t = CfPos(cf.target);
        int skip_end = t, else_end = -1;
        if (t - 1 > k) {
          const ControlFlow& j = p.control_flow[size_t(t - 1)];
          if (j.opcode == CfOpcode::kCondJmp && j.branch_type == ControlFlow::ExecType::kUnconditional) {
            const int u = CfPos(j.target);
            if (u >= t && u <= to) {
              skip_end = t - 1;
              else_end = u;
            }
          }
        }
        Scope sc(*this);
        const uint32_t u = Push();
        if (!CondValue(u, cf.branch_type, cf.bool_constant, cf.condition)) {
          // Always taken: [k + 1, t) never runs; with an else part, that part always does.
          if (else_end >= 0) EmitStructured(t, else_end);
        } else {
          a.OpIf(false, S1(u, 0));
          EmitStructured(k + 1, skip_end);
          if (else_end >= 0) {
            a.OpElse();
            EmitStructured(t, else_end);
          }
          a.OpEndIf();
        }
        k = (else_end >= 0 ? else_end : t) - 1;
        continue;
      }
      if (!IsExec(cf.opcode)) continue;
      const bool c = OpenExec(cf);
      EmitExecBody(cf);
      if (c) a.OpEndIf();
      // An unconditional end at the top level (StructuredOk): the rest is unreachable - cut after it is generated.
      if (cf.exec_end && structured_dead_from_ == SIZE_MAX) structured_dead_from_ = code.size();
    }
  }
  size_t structured_dead_from_ = SIZE_MAX;

  void EmitPcLoop() {
    uses_pc_loop = true;
    uses_x1 = true;
    SetPc(0);
    a.OpMov(R(Fl(), kY), LU(0));
    a.OpLoop();
    {
      Scope sc(*this);
      const uint32_t t = Push();
      // while (xe_guard++ < 65536u)
      a.OpUGE(R(t, kX), S1(Fl(), 1), LU(65536));
      a.OpIf(true, S1(t, 0));
      a.OpBreak();
      a.OpEndIf();
      a.OpIAdd(R(Fl(), kY), S1(Fl(), 1), LU(1));
    }
    for (const ControlFlow& cf : p.control_flow) {
      const uint32_t k = cf.index;
      {
        Scope sc(*this);
        const uint32_t t = Push();
        a.OpIEq(R(t, kX), S1(Fl(), 0), LU(k));
        a.OpIf(true, S1(t, 0));
      }
      if (IsExec(cf.opcode)) {
        const bool c = OpenExec(cf);
        EmitExecBody(cf);
        if (cf.exec_end) {
          SetPc(0xFFFFFFFFu);
          a.OpBreak();
        }
        if (c) a.OpEndIf();
        if (!cf.exec_end || c) SetPc(k + 1);  // (after an unconditional break it would be unreachable)
      } else {
        Scope sc(*this);
        const uint32_t t = Push(), u = Push(), it = Push(), lc = Push();
        const Src depth = S1(Fl(), 2), call_depth = S1(Fl(), 3);
        switch (cf.opcode) {
          case CfOpcode::kLoopStart: {
            const uint32_t lk = cf.loop_constant;
            a.OpMov(R(lc, kX), Cb(kCbBoolLoop, 2 + ((lk >> 2) & 7)).Select(lk & 3));
            if (cf.repeat) {
              // it = depth > 0 ? iter[depth - 1] : 0 (index clamped to 0 when depth is 0)
              a.OpIAdd(R(u, kX), depth, LI(-1));
              a.OpIMax(R(u, kX), S1(u, 0), LI(0));
              a.OpMov(R(it, kX), Src::X(1, Index(u, 0)).Select(0));
              a.OpMovC(R(it, kX), depth, S1(it, 0), LU(0));
            } else {
              a.OpMov(R(it, kX), LU(0));
            }
            // if (depth >= 4 || it >= (lc & 0xFF)) pc = target;
            a.OpUGE(R(t, kX), depth, LU(4));
            a.OpAnd(R(u, kX), S1(lc, 0), LU(0xFF));
            a.OpUGE(R(u, kX), S1(it, 0), S1(u, 0));
            a.OpOr(R(t, kX), S1(t, 0), S1(u, 0));
            a.OpIf(true, S1(t, 0));
            SetPc(cf.target);
            a.OpElse();
            a.OpMov(Dest::X(1, Index(Fl(), 2), kX), S1(it, 0));
            a.OpMov(Dest::X(1, Index(Fl(), 2), kY), S1(lc, 0));
            a.OpIAdd(R(Fl(), kZ), depth, LU(1));
            LoopAddress(u, S1(it, 0), S1(lc, 0));
            a.OpMov(R(St(), kW), S1(u, 0));
            SetPc(k + 1);
            a.OpEndIf();
            if (cf.target <= k) a.OpContinue();
            break;
          }
          case CfOpcode::kLoopEnd: {
            a.OpIf(true, depth);
            {
              // d = depth - 1; it = ++iter[d];
              const uint32_t d = Push();
              a.OpIAdd(R(d, kX), depth, LI(-1));
              a.OpMov(R(it, kX), Src::X(1, Index(d, 0)).Select(0));
              a.OpIAdd(R(it, kX), S1(it, 0), LU(1));
              a.OpMov(Dest::X(1, Index(d, 0), kX), S1(it, 0));
              a.OpMov(R(lc, kX), Src::X(1, Index(d, 0)).Select(1));
              // if (it < (lc[d] & 0xFF) [&& !(p0 == condition)]) { aL = xe_al(it, lc[d]); pc = target; continue; }
              a.OpAnd(R(t, kX), S1(lc, 0), LU(0xFF));
              a.OpULT(R(t, kX), S1(it, 0), S1(t, 0));
              if (cf.predicated_break) {
                if (cf.condition) a.OpNot(R(u, kX), S1(St(), 1));
                else a.OpMov(R(u, kX), S1(St(), 1));
                a.OpAnd(R(t, kX), S1(t, 0), S1(u, 0));
              }
              a.OpIf(true, S1(t, 0));
              LoopAddress(u, S1(it, 0), S1(lc, 0));
              a.OpMov(R(St(), kW), S1(u, 0));
              SetPc(cf.target);
              a.OpContinue();
              a.OpEndIf();
              // depth = d; aL = d > 0 ? xe_al(iter[d - 1], lc[d - 1]) : 0;
              a.OpMov(R(Fl(), kZ), S1(d, 0));
              a.OpIAdd(R(u, kX), S1(d, 0), LI(-1));
              a.OpIMax(R(u, kX), S1(u, 0), LI(0));
              a.OpMov(R(it, kX), Src::X(1, Index(u, 0)).Select(0));
              a.OpMov(R(lc, kX), Src::X(1, Index(u, 0)).Select(1));
              LoopAddress(t, S1(it, 0), S1(lc, 0));
              a.OpMovC(R(St(), kW), S1(d, 0), S1(t, 0), LI(0));
            }
            a.OpEndIf();
            SetPc(k + 1);
            break;
          }
          case CfOpcode::kCondCall: {
            // if (c && call_depth < 4) { ret[call_depth++] = k + 1; pc = target; } else pc = k + 1;
            a.OpULT(R(t, kX), call_depth, LU(4));
            if (CondValue(u, cf.branch_type, cf.bool_constant, cf.condition)) a.OpAnd(R(t, kX), S1(t, 0), S1(u, 0));
            a.OpIf(true, S1(t, 0));
            a.OpMov(Dest::X(1, Index(Fl(), 3), kZ), LU(k + 1));
            a.OpIAdd(R(Fl(), kW), call_depth, LU(1));
            SetPc(cf.target);
            if (cf.target <= k) a.OpContinue();
            a.OpElse();
            SetPc(k + 1);
            a.OpEndIf();
            break;
          }
          case CfOpcode::kReturn:
            a.OpIf(true, call_depth);
            a.OpIAdd(R(Fl(), kW), call_depth, LI(-1));
            a.OpMov(R(Fl(), kX), Src::X(1, Index(Fl(), 3)).Select(2));
            a.OpContinue();
            a.OpEndIf();
            SetPc(k + 1);
            break;
          case CfOpcode::kCondJmp:
            if (!CondValue(u, cf.branch_type, cf.bool_constant, cf.condition)) {
              SetPc(cf.target);
              if (cf.target <= k) a.OpContinue();
            } else {
              a.OpIf(true, S1(u, 0));
              SetPc(cf.target);
              if (cf.target <= k) a.OpContinue();
              a.OpElse();
              SetPc(k + 1);
              a.OpEndIf();
            }
            break;
          default:
            SetPc(k + 1);
            break;
        }
      }
      a.OpEndIf();
    }
    EndOnePassLoop();
  }

  // ---- Memory export (hlsl.cpp kPreludeMemexport), emitted in place at each flush (as Canary: no subroutines) ----

  // xe_mx_fixed(v, width, sg, it) for a constant width into d.x; sg / it are 0 / ~0 in components.
  void MxFixed(uint32_t d, const Src& v, uint32_t width, const Src& sg, const Src& it) {
    if (width == 0) {
      a.OpMov(R(d, kX), LU(0));
      return;
    }
    Scope sc(*this);
    const uint32_t r = Push(), q = Push(), c = Push(), os = Push();
    // Signed.
    const float hi = float((1u << (width - 1)) - 1u);
    a.OpMax(R(r, kX), v, LF(-hi - 1.0f));
    a.OpMin(R(r, kX), S1(r, 0), LF(hi));
    a.OpMax(R(q, kX), v, LF(-1.0f));
    a.OpMin(R(q, kX), S1(q, 0), LF(1.0f));
    a.OpMul(R(q, kX), S1(q, 0), LF(hi));
    a.OpNE(R(c, kX), v, v);
    a.OpMovC(R(q, kX), S1(c, 0), LF(0.0f), S1(q, 0));
    a.OpMovC(R(r, kX), it, S1(r, 0), S1(q, 0));
    a.OpLT(R(c, kX), S1(r, 0), LF(0.0f));
    a.OpMovC(R(c, kX), S1(c, 0), LF(-0.5f), LF(0.5f));
    a.OpAdd(R(r, kX), S1(r, 0), S1(c, 0));
    a.OpFToI(R(os, kX), S1(r, 0));
    a.OpAnd(R(os, kX), S1(os, 0), LU(width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u)));
    // Unsigned.
    const float mx = width >= 32 ? 4294967295.0f : float((1u << width) - 1u);
    a.OpMax(R(r, kX), v, LF(0.0f));
    a.OpMin(R(r, kX), S1(r, 0), LF(mx));
    a.OpMov(R(q, kX), v, true);
    a.OpMul(R(q, kX), S1(q, 0), LF(mx));
    a.OpNE(R(c, kX), v, v);
    a.OpMovC(R(q, kX), S1(c, 0), LF(0.0f), S1(q, 0));
    a.OpMovC(R(r, kX), it, S1(r, 0), S1(q, 0));
    a.OpAdd(R(r, kX), S1(r, 0), LF(0.5f));
    a.OpFToU(R(r, kX), S1(r, 0));
    a.OpMovC(R(d, kX), sg, S1(os, 0), S1(r, 0));
  }
  // xe_mx_pack(v.(c0 c1 c2 c3), widths, sg, it) into d.x.
  void MxPack(uint32_t d, uint32_t v, const uint32_t comps[4], const uint32_t w[4], const Src& sg, const Src& it) {
    Scope sc(*this);
    const uint32_t f = Push();
    MxFixed(d, S1(v, comps[0]), w[0], sg, it);
    uint32_t shift = w[0];
    for (uint32_t i = 1; i < 4; ++i) {
      if (w[i] == 0) continue;
      MxFixed(f, S1(v, comps[i]), w[i], sg, it);
      a.OpIShL(R(f, kX), S1(f, 0), LU(shift));
      a.OpOr(R(d, kX), S1(d, 0), S1(f, 0));
      shift += w[i];
    }
  }
  // xe_f16x(v) into d.x.
  void F16x(uint32_t d, const Src& v_in) {
    Scope sc(*this);
    const uint32_t h = Push(), t = Push();
    Src v = v_in;
    if (opt.memexport_f16_nan_zero) {  // patch 0087 (Canary 3ff230d): NaN -> 0 (eq is false only for NaN)
      const uint32_t z = Push();
      a.OpEq(R(z, kX), v_in, v_in);
      a.OpAnd(R(z, kX), v_in, S1(z, 0));
      v = S1(z, 0);
      rule_bits |= 4u;
    }
    a.OpF32ToF16(R(h, kX), v);
    a.OpMax(R(t, kX), v, LF(-131008.0f));
    a.OpMin(R(t, kX), S1(t, 0), LF(131008.0f));
    a.OpMul(R(t, kX), S1(t, 0), LF(0.5f));
    a.OpF32ToF16(R(t, kX), S1(t, 0));
    a.OpIAdd(R(t, kX), S1(t, 0), LU(0x0400u));
    a.OpAnd(R(t, kY), S1(h, 0), LU(0x7C00u));
    a.OpIEq(R(t, kY), S1(t, 1), LU(0x7C00u));
    a.OpMovC(R(d, kX), S1(t, 1), S1(t, 0), S1(h, 0));
  }

  void EmitMemexportFlush() {
    Scope sc(*this);
    const uint32_t w = Push(), c = Push(), ea = Push(), m = Push(), bic = Push(), i = Push(), v = Push(), d = Push(),
                   sz = Push(), t = Push(), u = Push();
    // written = eM written; cleared.
    a.OpMov(R(w, kX), S1(Misc(), 2));
    a.OpMov(R(Misc(), kZ), LU(0));
    // if (written == 0 || (xe_sys0.x & 128) == 0) return;
    a.OpIEq(R(c, kX), S1(w, 0), LU(0));
    a.OpAnd(R(c, kY), Sys(0).Select(0), LU(128));
    a.OpIEq(R(c, kY), S1(c, 1), LU(0));
    a.OpOr(R(c, kX), S1(c, 0), S1(c, 1));
    a.OpIf(false, S1(c, 0));
    // The stream constant must have the expected markers.
    a.OpMov(R(ea), S(EA()));
    a.OpUShR(R(c), S(ea), LU4(30, 23, 23, 23));
    a.OpINE(R(c), S(c), LU4(1, 0x96, 0x96, 0x96));
    a.OpOr(R(c, kX), S1(c, 0), S1(c, 1));
    a.OpOr(R(c, kX), S1(c, 0), S1(c, 2));
    a.OpOr(R(c, kX), S1(c, 0), S1(c, 3));
    a.OpIf(false, S1(c, 0));
    // m: x fmt, y endian, z signed, w integer; bic: x base, y index, z count, w red/blue swap.
    a.OpUBFE(R(m), LU4(6, 3, 1, 1), LU4(8, 0, 16, 17), S1(ea, 2));
    a.OpINE(R(m, kZ | kW), S(m), LU(0));
    a.OpAnd(R(bic, kX), S1(ea, 0), LU(0x3FFFFFFFu));
    a.OpIShL(R(bic, kX), S1(bic, 0), LU(2));
    a.OpAnd(R(bic, kX), S1(bic, 0), LU(0x1FFFFFFFu));
    a.OpAnd(R(bic, kY | kZ), S(ea, Sw(0, 1, 3, 0)), LU(0x7FFFFFu));
    a.OpUBFE(R(bic, kW), LU(1), LU(19), S1(ea, 2));
    const Src sg = S1(m, 2), it = S1(m, 3);
    a.OpMov(R(i, kX), LU(0));
    a.OpLoop();
    {
      a.OpUGE(R(c, kX), S1(i, 0), LU(5));
      a.OpIf(true, S1(c, 0));
      a.OpBreak();
      a.OpEndIf();
      // idx = index + i (i.y); this element: written bit set and idx < count.
      a.OpIAdd(R(i, kY), S1(bic, 1), S1(i, 0));
      a.OpUShR(R(c, kX), S1(w, 0), S1(i, 0));
      a.OpAnd(R(c, kX), S1(c, 0), LU(1));
      a.OpULT(R(c, kY), S1(i, 1), S1(bic, 2));
      a.OpAnd(R(c, kX), S1(c, 0), S1(c, 1));
      a.OpIf(true, S1(c, 0));
      {
        a.OpMov(R(v), Src::X(2, Index(i, 0)));
        a.OpMovC(R(v), S1(bic, 3), S(v, Sw(2, 1, 0, 3)), S(v));
        a.OpMov(R(d), LU(0));
        a.OpMov(R(sz, kX), LU(0xFF));
        a.OpSwitch(S1(m, 0));
        auto pack_case = [&](std::initializer_list<uint32_t> fmts, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
                             uint32_t size_log2) {
          for (uint32_t f : fmts) a.OpCase(LU(f));
          const uint32_t comps[4] = {0, 1, 2, 3}, ws[4] = {w0, w1, w2, w3};
          MxPack(d, v, comps, ws, sg, it);
          a.OpMov(R(sz, kX), LU(size_log2));
          a.OpBreak();
        };
        pack_case({2, 8, 9}, 8, 0, 0, 0, 0);
        pack_case({3}, 5, 5, 5, 1, 1);
        pack_case({4}, 5, 6, 5, 0, 1);
        pack_case({5}, 5, 5, 6, 0, 1);
        pack_case({6, 14, 50}, 8, 8, 8, 8, 2);
        pack_case({7, 54}, 10, 10, 10, 2, 2);
        pack_case({10}, 8, 8, 0, 0, 1);
        pack_case({15}, 4, 4, 4, 4, 1);
        pack_case({16, 55}, 11, 11, 10, 0, 2);
        pack_case({17, 56}, 10, 11, 11, 0, 2);
        pack_case({24}, 16, 0, 0, 0, 1);
        pack_case({25}, 16, 16, 0, 0, 2);
        {
          a.OpCase(LU(26));
          const uint32_t ws[4] = {16, 16, 0, 0};
          const uint32_t xy[4] = {0, 1, 0, 1}, zw[4] = {2, 3, 2, 3};
          MxPack(t, v, xy, ws, sg, it);
          a.OpMov(R(d, kX), S1(t, 0));
          MxPack(t, v, zw, ws, sg, it);
          a.OpMov(R(d, kY), S1(t, 0));
          a.OpMov(R(sz, kX), LU(3));
          a.OpBreak();
        }
        {
          a.OpCase(LU(30));
          F16x(d, S1(v, 0));
          a.OpMov(R(sz, kX), LU(1));
          a.OpBreak();
        }
        for (uint32_t f16 : {31u, 32u}) {
          a.OpCase(LU(f16));
          for (uint32_t j = 0; j < (f16 == 31 ? 1u : 2u); ++j) {
            F16x(t, S1(v, 2 * j));
            F16x(u, S1(v, 2 * j + 1));
            a.OpIShL(R(u, kX), S1(u, 0), LU(16));
            a.OpOr(R(d, 1u << j), S1(t, 0), S1(u, 0));
          }
          a.OpMov(R(sz, kX), LU(f16 == 31 ? 2 : 3));
          a.OpBreak();
        }
        a.OpCase(LU(36));
        a.OpMov(R(d, kX), S1(v, 0));
        a.OpMov(R(sz, kX), LU(2));
        a.OpBreak();
        a.OpCase(LU(37));
        a.OpMov(R(d, kXY), S(v));
        a.OpMov(R(sz, kX), LU(3));
        a.OpBreak();
        a.OpCase(LU(38));
        a.OpMov(R(d), S(v));
        a.OpMov(R(sz, kX), LU(4));
        a.OpBreak();
        a.OpDefault();
        a.OpBreak();
        a.OpEndSwitch();
        a.OpINE(R(c, kX), S1(sz, 0), LU(0xFF));
        a.OpIf(true, S1(c, 0));
        {
          // Endian: 4 -> d.yxwz, 5 -> d.wzyx, both then 2; then the swap within each dword.
          a.OpMov(R(t, kX), S1(m, 1));
          a.OpIEq(R(c, kX), S1(t, 0), LU(4));
          a.OpMovC(R(d), S1(c, 0), S(d, Sw(1, 0, 3, 2)), S(d));
          a.OpMovC(R(t, kX), S1(c, 0), LU(2), S1(t, 0));
          a.OpIEq(R(c, kX), S1(t, 0), LU(5));
          a.OpMovC(R(d), S1(c, 0), S(d, Sw(3, 2, 1, 0)), S(d));
          a.OpMovC(R(t, kX), S1(c, 0), LU(2), S1(t, 0));
          Swap(d, S(d), S1(t, 0), kXYZW);
          // addr = base + (idx << size_log2); skipped past 512 MB.
          a.OpIShL(R(t, kX), S1(i, 1), S1(sz, 0));
          a.OpIAdd(R(t, kX), S1(t, 0), S1(bic, 0));
          a.OpIShL(R(t, kY), LU(1), S1(sz, 0));
          a.OpIAdd(R(t, kY), S1(t, 0), S1(t, 1));
          a.OpUGE(R(c, kX), LU(0x20000000u), S1(t, 1));
          a.OpIf(true, S1(c, 0));
          {
            a.OpUGE(R(c, kX), LU(1), S1(sz, 0));
            a.OpIf(true, S1(c, 0));
            {
              // shift = (addr & 3) * 8, width = 8 << size_log2, mask = ((1 << width) - 1) << shift.
              a.OpAnd(R(u, kX), S1(t, 0), LU(3));
              a.OpIShL(R(u, kX), S1(u, 0), LU(3));
              a.OpIShL(R(u, kY), LU(8), S1(sz, 0));
              a.OpIShL(R(u, kY), LU(1), S1(u, 1));
              a.OpIAdd(R(u, kY), S1(u, 1), LI(-1));
              a.OpIShL(R(u, kY), S1(u, 1), S1(u, 0));
              a.OpAnd(R(u, kZ), S1(t, 0), LU(~3u));
              a.OpNot(R(u, kW), S1(u, 1));
              a.OpAtomicAnd(UavDest(0), S1(u, 2), 1, S1(u, 3));
              a.OpIShL(R(u, kW), S1(d, 0), S1(u, 0));
              a.OpAnd(R(u, kW), S1(u, 3), S1(u, 1));
              a.OpAtomicOr(UavDest(0), S1(u, 2), 1, S1(u, 3));
            }
            a.OpElse();
            {
              a.OpIEq(R(c, kX), S1(sz, 0), LU(2));
              a.OpIf(true, S1(c, 0));
              a.OpStoreRaw(UavDest(kX), S1(t, 0), S(d));
              a.OpElse();
              a.OpIEq(R(c, kX), S1(sz, 0), LU(3));
              a.OpIf(true, S1(c, 0));
              a.OpStoreRaw(UavDest(kXY), S1(t, 0), S(d));
              a.OpElse();
              a.OpStoreRaw(UavDest(kXYZW), S1(t, 0), S(d));
              a.OpEndIf();
              a.OpEndIf();
            }
            a.OpEndIf();
          }
          a.OpEndIf();
        }
        a.OpEndIf();
      }
      a.OpEndIf();
      a.OpIAdd(R(i, kX), S1(i, 0), LU(1));
    }
    a.OpEndLoop();
    a.OpEndIf();
    a.OpEndIf();
  }

  // ---- Shader body: prologue, translated program, epilogue (hlsl.cpp TranslateToHlsl) ----
  void EmitMain() {
    const bool vs = p.is_vertex_shader;
    // Every register and system temp starts at 0.
    for (uint32_t i = 0; i < regs; ++i) a.OpMov(RegDest(i, UINT32_MAX, kXYZW), LF(0.0f));
    if (opt.dxbc_salt) a.OpMov(R(base, kX), LU(opt.dxbc_salt));  // patch 0059: dead (cleared next), bench only
    for (uint32_t i = 0; i < kFixedTemps; ++i) a.OpMov(R(base + i), LU(0));
    if (uses_x1_final)
      for (uint32_t i = 0; i < 4; ++i) a.OpMov(Dest::X(1, i), LU(0));
    if (memexport_final)
      for (uint32_t i = 0; i < 5; ++i) a.OpMov(Dest::X(2, i), LU(0));
    if (vs) {
      for (uint32_t i = 0; i < 16; ++i) a.OpMov(Dest::O(i), LF(0.0f));
      Scope sc(*this);
      const uint32_t t = Push();
      if (opt.tess_quad_segments) {
        // Quad-patch tessellation grid (hlsl.cpp): r0 = (patch + offset, u, v, 0).
        const uint32_t n = opt.tess_quad_segments;
        const uint32_t q = Push(), r = Push();
        const Src vid = Src::V1D(0).Select(0);
        a.OpUDiv(R(q, kX), R(r, kX), vid, LU(6 * n * n));           // q.x = patch, r.x = vid % per
        a.OpUDiv(R(q, kY), Dest::Null(), S1(r, 0), LU(6));          // q.y = cell
        a.OpUDiv(Dest::Null(), R(q, kZ), vid, LU(6));               // q.z = corner
        // du = corner 1, 3, 4; dv = corner 2, 4, 5.
        a.OpIEq(R(t), S1(q, 2), LU4(1, 3, 4, 0));
        a.OpOr(R(t, kX), S1(t, 0), S1(t, 1));
        a.OpOr(R(t, kX), S1(t, 0), S1(t, 2));
        a.OpAnd(R(t, kX), S1(t, 0), LU(1));                        // du
        a.OpIEq(R(r), S1(q, 2), LU4(2, 4, 5, 0));
        a.OpOr(R(r, kX), S1(r, 0), S1(r, 1));
        a.OpOr(R(r, kX), S1(r, 0), S1(r, 2));
        a.OpAnd(R(t, kY), S1(r, 0), LU(1));                        // dv
        a.OpUDiv(R(r, kY), R(r, kX), S1(q, 1), LU(n));              // r.y = cell / n, r.x = cell % n
        a.OpIAdd(R(r, kX), S1(r, 0), S1(t, 0));
        a.OpIAdd(R(r, kY), S1(r, 1), S1(t, 1));
        a.OpUToF(R(r, kXY), S(r));
        a.OpDiv(R(r, kXY), S(r), LF(float(n)));
        a.OpIAdd(R(q, kX), S1(q, 0), Sys(0).Select(1));
        a.OpAnd(R(q, kX), S1(q, 0), LU(0xFFFFFFu));
        a.OpUToF(R(q, kX), S1(q, 0));
        a.OpMov(R(t), LF(0.0f));
        a.OpMov(R(t, kX), S1(q, 0));
        a.OpMov(R(t, kYZ), S(r, Sw(0, 0, 1, 0)));
        a.OpMov(RegDest(0, UINT32_MAX, kXYZW), S(t));
      } else if (opt.vertex_index_clamp) {
        a.OpIAdd(R(t, kX), Src::V1D(0).Select(0), Sys(0).Select(1));
        a.OpAnd(R(t, kX), S1(t, 0), LU(0xFFFFFFu));
        a.OpUMax(R(t, kX), S1(t, 0), Sys(5).Select(0));
        a.OpUMin(R(t, kX), S1(t, 0), Sys(5).Select(1));
        a.OpUToF(RegDest(0, UINT32_MAX, kX), S1(t, 0));
      } else {
        a.OpIAdd(R(t, kX), Src::V1D(0).Select(0), Sys(0).Select(1));
        a.OpIToF(RegDest(0, UINT32_MAX, kX), S1(t, 0));
      }
    } else {
      Scope sc(*this);
      const uint32_t t = Push(), pg = Push(), q = Push();
      const uint32_t n = std::min(regs, 16u);
      // Interpolators into r0-r15 per xe_sys0.z.
      if (opt.dxbc_slim_prologue) {
        // Patch 0060: four bit tests per instruction and a select per register (no branches); the registers are 0.
        for (uint32_t i = 0; i < n; ++i) {
          if (i % 4 == 0)
            a.OpAnd(R(t), Sys(0).Select(2), LU4(1u << i, 2u << i, 4u << i, 8u << i));
          a.OpMovC(RegDest(i, UINT32_MAX, kXYZW), S1(t, i % 4), Src::V1D(i), LF(0.0f));
        }
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          a.OpAnd(R(t, kX), Sys(0).Select(2), LU(1u << i));
          a.OpIf(true, S1(t, 0));
          a.OpMov(RegDest(i, UINT32_MAX, kXYZW), Src::V1D(i));
          a.OpEndIf();
        }
      }
      // Pixel parameters (ps_param_gen).
      a.OpAnd(R(t, kX), Sys(0).Select(3), LU(0x80000000u));
      a.OpIf(true, S1(t, 0));
      {
        a.OpUBFE(R(t, kY), LU(4), LU(8), Sys(0).Select(3));
        a.OpUMax(R(t, kY), S1(t, 1), LU(1));
        a.OpUToF(R(t, kY), S1(t, 1));                                 // xe_rs
        a.OpMov(R(pg), LF(0.0f));
        a.OpDiv(R(q, kXY), Src::V1D(16), S(t, Src::kYYYY));
        a.OpAdd(R(q, kXY), S(q), LF(-0.5f));
        a.OpRoundNI(R(pg, kXY), Src::V1D(16));
        a.OpLT(R(t, kZ), LF(1.0f), S1(t, 1));
        a.OpMovC(R(pg, kXY), S1(t, 2), S(q), S(pg));
        // Back-facing in the sign of X (xe_sys0.x bit 3), lines in the sign of Z (bit 4).
        a.OpAnd(R(t, kZ), Sys(0).Select(0), LU(8));
        a.OpNot(R(t, kW), Src::V1D(17).Select(0));
        a.OpAnd(R(t, kZ), S1(t, 2), S1(t, 3));
        a.OpOr(R(q, kX), S1(pg, 0), LU(0x80000000u));
        a.OpMovC(R(pg, kX), S1(t, 2), S1(q, 0), S1(pg, 0));
        a.OpAnd(R(t, kZ), Sys(0).Select(0), LU(16));
        a.OpMovC(R(pg, kZ), S1(t, 2), LU(0x80000000u), S1(pg, 2));
        a.OpAnd(R(t, kW), Sys(0).Select(3), LU(15));
        if (opt.dxbc_slim_prologue) {
          // Patch 0060: four index compares per instruction and a select per register (no branches).
          const uint32_t e = Push();
          for (uint32_t i = 0; i < n; ++i) {
            if (i % 4 == 0) a.OpIEq(R(e), S1(t, 3), LU4(i, i + 1, i + 2, i + 3));
            a.OpMovC(RegDest(i, UINT32_MAX, kXYZW), S1(e, i % 4), S(pg), RegSrc(i, UINT32_MAX));
          }
        } else {
          for (uint32_t i = 0; i < n; ++i) {
            a.OpIEq(R(t, kZ), S1(t, 3), LU(i));
            a.OpIf(true, S1(t, 2));
            a.OpMov(RegDest(i, UINT32_MAX, kXYZW), S(pg));
            a.OpEndIf();
          }
        }
      }
      a.OpEndIf();
      // xe_odepth = (xe_sys0.x & 64 ? position.z * 2 : position.z, 0, 0, 0) (the shader cache's D24FS8 remap).
      a.OpAnd(R(t, kX), Sys(0).Select(0), LU(64));
      a.OpMul(R(t, kY), Src::V1D(16).Select(2), LF(2.0f));
      a.OpMovC(R(PosOrDepth(), kX), S1(t, 0), S1(t, 1), Src::V1D(16).Select(2));
    }

    // The program.
    bool needs_pc = false;
    for (const ControlFlow& cf : p.control_flow) {
      if (cf.opcode == CfOpcode::kLoopStart || cf.opcode == CfOpcode::kLoopEnd || cf.opcode == CfOpcode::kCondCall ||
          cf.opcode == CfOpcode::kReturn || cf.opcode == CfOpcode::kCondJmp) {
        needs_pc = true;
      }
    }
    if (needs_pc && opt.dxbc_structured_jumps && StructuredOk(0, int(p.control_flow.size()), true)) {
      // Job 634: forward jumps only - nested ifs. After an unconditional exec_end (top level only) the rest is
      // generated (the same metadata) but cut, as in EmitStraightLine.
      structured_dead_from_ = SIZE_MAX;
      EmitStructured(0, int(p.control_flow.size()));
      if (structured_dead_from_ != SIZE_MAX) code.resize(structured_dead_from_);
    } else if (needs_pc) {
      EmitPcLoop();
    } else {
      EmitStraightLine();
    }
    if (!p.issues.empty()) {
      for (const auto& is : p.issues) Simplify("decoder_issue", IssueKindName(is.kind));
    }
    if (memexport) EmitMemexportFlush();

    Scope sc(*this);
    const uint32_t t = Push(), u = Push();
    if (vs) {
      const uint32_t pos = PosOrDepth();
      // Position completion (Xenia's CompleteVertexOrDomainShader).
      a.OpAnd(R(t, kX), Sys(0).Select(0), LU(1));
      a.OpDiv(R(t, kY), LF(1.0f), S1(pos, 3));
      a.OpMovC(R(pos, kW), S1(t, 0), S1(pos, 3), S1(t, 1));
      a.OpAnd(R(t, kX), Sys(0).Select(0), LU(2));
      a.OpMul(R(u, kXY), S(pos), S(pos, Src::kWWWW));
      a.OpMovC(R(pos, kXY), S1(t, 0), S(u), S(pos));
      a.OpAnd(R(t, kX), Sys(0).Select(0), LU(4));
      a.OpMul(R(u, kZ), S(pos), S(pos, Src::kWWWW));
      a.OpMovC(R(pos, kZ), S1(t, 0), S(u), S(pos));
      a.OpMul(R(t, kXYZ), S(pos), Sys(1));
      a.OpMul(R(u, kXYZ), Sys(2), S(pos, Src::kWWWW));
      a.OpAdd(R(pos, kXYZ), S(t), S(u));
      a.OpMov(Dest::O(16), S(pos));
    } else {
      // Alpha test (xe_alpha_test(xe_oc[0].w)).
      const Src al = S1(Oc(0), 3), ref = Sys(4).Select(1);
      const uint32_t r = Push(), f = Push();
      a.OpAnd(R(f, kX), Sys(4).Select(0), LU(7));
      a.OpMov(R(r, kX), LU(0));                                   // 0: never
      a.OpLT(R(t, kX), al, ref);                                  // 1: a < ref
      a.OpEq(R(t, kY), al, ref);                                  // 2: a == ref
      a.OpGE(R(t, kZ), ref, al);                                  // 3: a <= ref
      a.OpLT(R(t, kW), ref, al);                                  // 4: a > ref
      a.OpNE(R(u, kX), al, ref);                                  // 5: !(a == ref)
      a.OpGE(R(u, kY), al, ref);                                  // 6: a >= ref
      const Src cands[7] = {S1(t, 0), S1(t, 1), S1(t, 2), S1(t, 3), S1(u, 0), S1(u, 1), LU(~0u)};
      for (uint32_t i = 0; i < 7; ++i) {
        a.OpIEq(R(f, kY), S1(f, 0), LU(i + 1));
        a.OpMovC(R(r, kX), S1(f, 1), cands[i], S1(r, 0));
      }
      a.OpAnd(R(f, kZ), Sys(0).Select(0), LU(32));
      a.OpNot(R(r, kX), S1(r, 0));
      a.OpAnd(R(r, kX), S1(r, 0), S1(f, 2));
      a.OpDiscard(true, S1(r, 0));
      for (uint32_t i = 0; i < 4; ++i) a.OpMul(Dest::O(i), S(Oc(i)), Sys(3).Select(i));
      if (writes_depth_final) {
        const uint32_t dp = PosOrDepth();
        a.OpAnd(R(t, kX), Sys(0).Select(0), LU(64));
        a.OpMov(R(t, kY), S1(dp, 0), true);
        a.OpMul(R(t, kY), S1(t, 1), LF(0.5f));
        a.OpMovC(Dest::ODepth(), S1(t, 0), S1(t, 1), S1(dp, 0));
      }
    }
    a.OpRet();
  }
  bool uses_x1_final = false;
  bool writes_depth_final = false;
};

// ---- Job 634 (opt.dxbc_coalesce_moves): copy coalescing on the finished SHEX code ----
// The generator computes into a scratch temp and then moves it to the destination (a quarter of all instructions were
// movs on a Judgment run). Where an instruction I writing temp t is directly followed by `mov D.m, t` (identity swizzle
// on m, no modifiers, no saturate), m is within I's mask, I works per component (or replicates one value), and none of
// the components I wrote of t is read again before it is overwritten (a forward scan; leaving the block through a loop
// end, continue or break counts as read), I writes D.m itself and the mov goes. The same values: only who holds them.
namespace coalesce {
struct Op {
  size_t at = 0, len = 0;     // whole operand tokens [at, at + len)
  uint32_t type = 0, mask = 0xF, sel = 0, comps = 0, dim = 0;
  bool mod = false, rel = false;
  uint32_t index0 = 0;
  uint32_t read_comps = 0xF;  // components a read of it can touch (from the swizzle / select)
};
inline size_t ParseOp(const std::vector<uint32_t>& c, size_t p, Op& o, std::vector<Op>* nested) {
  o.at = p;
  const uint32_t t = c[p++];
  o.comps = t & 3;
  o.sel = (t >> 2) & 3;
  o.type = (t >> 12) & 0xFF;
  o.dim = (t >> 20) & 3;
  const uint32_t cdata = (t >> 4) & 0xFF;
  if (o.comps == 2) {
    if (o.sel == 0) o.mask = cdata & 0xF;
    else if (o.sel == 1) {
      o.read_comps = 0;
      for (int i = 0; i < 4; ++i) o.read_comps |= 1u << ((cdata >> (2 * i)) & 3);
    } else o.read_comps = 1u << (cdata & 3);
  } else if (o.comps == 1) {
    o.read_comps = 1;
    o.mask = 1;
  }
  bool ext = t >> 31;
  while (ext) {
    const uint32_t e = c[p++];
    if ((e & 63) == 1 && ((e >> 6) & 0xFF)) o.mod = true;
    if ((e & 63) != 1) o.mod = true;  // anything else: leave it alone
    ext = e >> 31;
  }
  if (o.type == 4 || o.type == 5) {  // immediates
    const size_t n = (o.comps == 1 ? 1 : 4) * (o.type == 5 ? 2 : 1);
    p += n;
    o.len = p - o.at;
    return p;
  }
  for (uint32_t i = 0; i < o.dim; ++i) {
    const uint32_t r = (t >> (22 + 3 * i)) & 7;
    if (r == 0) {
      if (i == 0) o.index0 = c[p];
      ++p;
    } else if (r == 1) {
      p += 2;
    } else {
      if (r == 3) ++p;
      o.rel = true;
      Op inner;
      p = ParseOp(c, p, inner, nested);
      if (nested) nested->push_back(inner);
    }
  }
  o.len = p - o.at;
  return p;
}
struct Ins {
  size_t at = 0, len = 0, ops_at = 0;
  uint32_t op = 0;
  bool sat = false, ext = false;
  std::vector<Op> ops, rel_ops;  // rel_ops: operands inside relative indices (always reads)
};
inline bool ParseIns(const std::vector<uint32_t>& c, size_t p, Ins& in) {
  in = {};
  in.at = p;
  const uint32_t t = c[p];
  in.op = t & 0x7FF;
  in.len = (t >> 24) & 0x7F;
  if (in.op == 53) in.len = c[p + 1];
  if (!in.len) return false;
  in.sat = (t >> 13) & 1;
  in.ext = t >> 31;
  if (in.op == 53) return true;
  size_t q = p + 1;
  if (in.ext) {
    while (c[q] >> 31) ++q;
    ++q;
  }
  in.ops_at = q;
  while (q < p + in.len) {
    Op o;
    q = ParseOp(c, q, o, &in.rel_ops);
    in.ops.push_back(o);
  }
  return q == p + in.len;
}
// Opcodes that write their first operand and work per component (or replicate one value): the ones I may be.
inline bool PerComponent(uint32_t op) {
  switch (op) {
    case 0: case 1: case 14: case 15: case 16: case 17: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
    case 32: case 33: case 34: case 35: case 36: case 37: case 39: case 41: case 42: case 43: case 47: case 49:
    case 50: case 51: case 52: case 54: case 55: case 56: case 57: case 59: case 60: case 64: case 65: case 66:
    case 67: case 68: case 75: case 79: case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 130:
    case 131: case 138: case 139: case 140:
      return true;
    default: return false;
  }
}
// Opcodes whose first operand is a destination (others: every operand is read).
inline uint32_t DestCount(uint32_t op) {
  if (PerComponent(op)) return 1;
  switch (op) {
    case 77: case 38: case 78: case 81:  // sincos, imul, udiv, umul: two destinations
      return 2;
    case 45: case 46: case 61: case 69: case 70: case 71: case 72: case 73: case 74: case 108: case 165: case 163:
    case 122: case 123: case 124: case 125: case 135: case 136: case 141: case 204: case 205:
      return 1;
    default: return 0;
  }
}
inline bool FlowOut(uint32_t op) {  // leaves the block backwards / sideways: endloop, continue(c), break(c), case, default
  return op == 22 || op == 7 || op == 8 || op == 2 || op == 3 || op == 6 || op == 10 || op == 23;
}
// Whether components `live` of temp t, as written at instruction index k, can be read after it. Reads count the
// components the swizzle can pick for the components the instruction writes (per-component ops) or all it names;
// writes kill components for the rest of their block (an if / else part); after an endif the state from before the
// if returns (a kill on one path only does not count). Leaving a loop or switch, continue or break: read.
inline bool LiveAfter(const std::vector<Ins>& v, const std::vector<uint32_t>& c, size_t k, uint32_t t, uint32_t live) {
  std::vector<uint32_t> st{live};       // live set per open block
  std::vector<uint8_t> kind{0};          // 0 base, 1 if, 2 loop / switch
  std::vector<uint32_t> at_if{live};     // the live set when the block opened (for else / endif)
  for (size_t i = k + 1; i < v.size(); ++i) {
    const Ins& in = v[i];
    if (in.op == 53) continue;
    uint32_t& L = st.back();
    const uint32_t nd = DestCount(in.op);
    uint32_t dmask = 0xF;
    if (nd == 1 && !in.ops.empty() && PerComponent(in.op) && in.op != 15 && in.op != 16 && in.op != 17)
      dmask = in.ops[0].comps == 2 && in.ops[0].sel == 0 ? in.ops[0].mask : (in.ops[0].comps == 1 ? 1u : 0xFu);
    for (const Op& o : in.rel_ops)
      if (o.type == 0 && o.index0 == t && (o.read_comps & L)) return true;
    for (size_t j = nd; j < in.ops.size(); ++j) {
      const Op& o = in.ops[j];
      if (o.type != 0 || o.dim != 1 || o.index0 != t) continue;
      if (o.rel) return true;
      uint32_t rc = o.read_comps;
      if (dmask != 0xF && o.comps == 2 && o.sel == 1) {  // swizzle: only the lanes of the written components
        const uint32_t cd = (c[o.at] >> 4) & 0xFF;
        rc = 0;
        for (int q = 0; q < 4; ++q)
          if ((dmask >> q) & 1) rc |= 1u << ((cd >> (2 * q)) & 3);
      }
      if (rc & L) return true;
    }
    switch (in.op) {
      case 31:  // if
        st.push_back(L);
        kind.push_back(1);
        at_if.push_back(L);
        continue;
      case 18:  // else: back to the state at the if
        if (kind.back() == 1) st.back() = at_if.back();
        continue;
      case 21:  // endif
        if (kind.size() > 1 && kind.back() == 1) {
          st.pop_back();
          kind.pop_back();
          at_if.pop_back();
          st.back() = at_if.size() ? st.back() : st.back();
        }
        continue;
      case 48: case 76:  // loop, switch: inside, kills do not count afterwards and the code may repeat
        st.push_back(L);
        kind.push_back(2);
        at_if.push_back(L);
        continue;
      case 22: case 23:  // endloop / endswitch
        if (kind.size() > 1 && kind.back() == 2) {
          if (in.op == 22) {  // the body may run again with what we left live: still checked by the reads above
          }
          st.pop_back();
          kind.pop_back();
          at_if.pop_back();
          continue;
        }
        return true;  // the end of a loop / switch we started inside
      default: break;
    }
    if (FlowOut(in.op)) {
      bool inside_own = false;
      for (size_t q = 1; q < kind.size(); ++q) if (kind[q] == 2) inside_own = true;
      if (!inside_own) return true;
    }
    if (in.op == 62 && kind.size() == 1) return false;  // ret at our level
    for (uint32_t j = 0; j < nd && j < in.ops.size(); ++j) {
      const Op& o = in.ops[j];
      if (o.type == 0 && !o.rel && o.dim == 1 && o.index0 == t) L &= ~o.mask;
    }
    if (kind.size() == 1 && !L) return false;
  }
  return false;  // the end of the shader: temps are not outputs
}
inline uint32_t Run(std::vector<uint32_t>& c) {
  uint32_t removed = 0;
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<Ins> v;
    for (size_t p = 0; p < c.size();) {
      Ins in;
      if (!ParseIns(c, p, in)) return removed;  // something we don't parse: leave the code as it is
      v.push_back(std::move(in));
      p += v.back().len;
    }
    std::vector<uint32_t> out;
    out.reserve(c.size());
    uint32_t here = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      const Ins& I = v[i];
      bool fused = false;
      if (i + 1 < v.size() && PerComponent(I.op) && !I.ext && !I.ops.empty()) {
        const Ins& J = v[i + 1];
        const Op& id = I.ops[0];
        if (J.op == 54 && !J.sat && !J.ext && J.ops.size() == 2 && id.type == 0 && !id.rel && id.dim == 1 &&
            id.comps == 2 && id.sel == 0 && !id.mod) {
          const Op& jd = J.ops[0];
          const Op& js = J.ops[1];
          bool ok = !jd.mod && !jd.rel && jd.comps == 2 && jd.sel == 0 && (jd.type == 0 || jd.type == 2) &&
                    jd.dim == 1 && js.type == 0 && !js.rel && js.dim == 1 && js.index0 == id.index0 && !js.mod &&
                    (jd.mask & ~id.mask) == 0 && jd.mask;
          if (ok) {
            // The source must read component c for each written c (swizzle identity on the mask).
            const uint32_t st = c[js.at];
            const uint32_t jsel = (st >> 2) & 3, jcd = (st >> 4) & 0xFF;
            if ((st & 3) != 2) ok = false;
            else if (jsel == 0) ok = (jcd & 0xF) == 0xF || (jcd & 0xF) == jd.mask;  // mask-mode source: as is
            else if (jsel == 1) {
              for (int q = 0; q < 4 && ok; ++q)
                if (((jd.mask >> q) & 1) && ((jcd >> (2 * q)) & 3) != uint32_t(q)) ok = false;
            } else {
              ok = jd.mask != 0 && (jd.mask & (jd.mask - 1)) == 0 && (jcd & 3) == uint32_t(std::countr_zero(jd.mask));
            }
          }
          // A replicating op (dp2-4) writes one value to every component: any mask is fine. Otherwise the same mask
          // is required when I reads its sources by swizzle per written component (it is: DXBC ALU ops are per
          // written component), so a narrower mask only computes fewer components.
          if (ok && !LiveAfter(v, c, i + 1, id.index0, id.mask)) {
            // I with J's destination: opcode token (length changes), J's dest tokens, I's sources.
            const size_t i_dest_end = I.ops[0].at + I.ops[0].len;
            std::vector<uint32_t> ni;
            ni.push_back(c[I.at]);
            ni.insert(ni.end(), c.begin() + jd.at, c.begin() + jd.at + jd.len);
            ni.insert(ni.end(), c.begin() + i_dest_end, c.begin() + I.at + I.len);
            ni[0] = (ni[0] & ~(0x7Fu << 24)) | (uint32_t(ni.size()) << 24);
            if (ni.size() <= 0x7F) {
              out.insert(out.end(), ni.begin(), ni.end());
              ++i;
              ++here;
              fused = true;
            }
          }
        }
      }
      if (!fused) out.insert(out.end(), c.begin() + I.at, c.begin() + I.at + I.len);
    }
    c.swap(out);
    removed += here;
    if (!here) break;
  }
  return removed;
}
}  // namespace coalesce

// ---- Container (the blobs Canary's DxbcShaderTranslator::CompleteTranslation writes) ----

struct Blobs {
  std::vector<uint32_t> obj;
  uint32_t Pos() const { return uint32_t(obj.size()); }
  // Byte offset of the current end relative to blob start `start` (dwords).
  uint32_t Rel(uint32_t start) const { return (Pos() - start) * 4; }
  template <typename T>
  T& At(uint32_t pos) {
    return *reinterpret_cast<T*>(obj.data() + pos);
  }
  template <typename T>
  uint32_t Reserve(uint32_t count = 1) {
    const uint32_t pos = Pos();
    obj.resize(obj.size() + sizeof(T) / 4 * count);
    return pos;
  }
};

void WriteRdef(Blobs& b, const Gen& g, bool vs, uint32_t sys_vectors) {
  const uint32_t start = b.Pos();
  b.Reserve<dx::RdefHeader>();
  dx::AppendAlignedString(b.obj, "Gears2-Native xenos::dxbc_gen");
  // Types: uint4 arrays of the four constant buffers' sizes, float4[256].
  const uint32_t type_name_uint4 = b.Rel(start);
  dx::AppendAlignedString(b.obj, "uint4");
  const uint32_t type_name_float4 = b.Rel(start);
  dx::AppendAlignedString(b.obj, "float4");
  const uint32_t cb_sizes[4] = {sys_vectors, 256, 10, 48};
  const uint32_t types_pos = b.Reserve<dx::RdefType>(4);
  for (uint32_t i = 0; i < 4; ++i) {
    auto& t = b.At<dx::RdefType>(types_pos + i * (sizeof(dx::RdefType) / 4));
    t.variable_class = dx::RdefVariableClass::kVector;
    t.variable_type = i == 1 ? dx::RdefVariableType::kFloat : dx::RdefVariableType::kUInt;
    t.row_count = 1;
    t.column_count = 4;
    t.element_count = uint16_t(cb_sizes[i]);
    t.name_ptr = i == 1 ? type_name_float4 : type_name_uint4;
  }
  const uint32_t types_ptr = (types_pos - start) * 4;
  // One variable per constant buffer.
  static const char* kVarNames[4] = {"xe_system", "xe_c", "xe_bool_loop", "xe_fetch"};
  static const char* kCbNames[4] = {"XeSystem", "XeFloatConstants", "XeBoolLoopConstants", "XeFetchConstants"};
  uint32_t var_names[4], cb_names[4];
  for (uint32_t i = 0; i < 4; ++i) {
    var_names[i] = b.Rel(start);
    dx::AppendAlignedString(b.obj, kVarNames[i]);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    cb_names[i] = b.Rel(start);
    dx::AppendAlignedString(b.obj, kCbNames[i]);
  }
  const uint32_t vars_pos = b.Reserve<dx::RdefVariable>(4);
  for (uint32_t i = 0; i < 4; ++i) {
    auto& v = b.At<dx::RdefVariable>(vars_pos + i * (sizeof(dx::RdefVariable) / 4));
    v.name_ptr = var_names[i];
    v.size_bytes = cb_sizes[i] * 16;
    v.flags = dx::kRdefVariableFlagUsed;
    v.type_ptr = types_ptr + i * sizeof(dx::RdefType);
    v.start_texture = UINT32_MAX;
    v.start_sampler = UINT32_MAX;
  }
  const uint32_t cbs_pos = b.Reserve<dx::RdefCbuffer>(4);
  for (uint32_t i = 0; i < 4; ++i) {
    auto& c = b.At<dx::RdefCbuffer>(cbs_pos + i * (sizeof(dx::RdefCbuffer) / 4));
    c.name_ptr = cb_names[i];
    c.variable_count = 1;
    c.variables_ptr = (vars_pos - start) * 4 + i * sizeof(dx::RdefVariable);
    c.size_vector_aligned_bytes = cb_sizes[i] * 16;
    c.type = dx::RdefCbufferType::kCbuffer;
  }
  // Bindings in s#, t#, u#, cb# order.
  std::vector<uint32_t> smp_names, srv_names;
  for (uint32_t s : g.samplers) {
    smp_names.push_back(b.Rel(start));
    dx::AppendAlignedString(b.obj, std::format("xe_sampler_{}", s).c_str());
  }
  for (const auto& s : g.srvs) {
    srv_names.push_back(b.Rel(start));
    dx::AppendAlignedString(b.obj, std::format("xe_t{}", s.reg).c_str());
  }
  const uint32_t uav_name = b.Rel(start);
  dx::AppendAlignedString(b.obj, "xe_memexport_buffer");
  const uint32_t binds_pos = b.Pos();
  for (uint32_t i = 0; i < g.samplers.size(); ++i) {
    const uint32_t pos = b.Reserve<dx::RdefInputBind>();
    auto& ib = b.At<dx::RdefInputBind>(pos);
    ib.name_ptr = smp_names[i];
    ib.type = dx::RdefInputType::kSampler;
    ib.bind_point = g.samplers[i];
    ib.bind_count = 1;
    ib.id = i;
  }
  for (uint32_t i = 0; i < g.srvs.size(); ++i) {
    const uint32_t pos = b.Reserve<dx::RdefInputBind>();
    auto& ib = b.At<dx::RdefInputBind>(pos);
    ib.name_ptr = srv_names[i];
    ib.bind_point = g.srvs[i].reg;
    ib.bind_count = 1;
    ib.id = i;
    if (g.srvs[i].kind == Gen::SrvBinding::Kind::kRaw) {
      ib.type = dx::RdefInputType::kByteAddress;
      ib.return_type = dx::ResourceReturnType::kMixed;
      ib.dimension = dx::RdefDimension::kSRVBuffer;
    } else {
      ib.type = dx::RdefInputType::kTexture;
      ib.return_type = dx::ResourceReturnType::kFloat;
      ib.sample_count = UINT32_MAX;
      ib.flags = dx::kRdefInputFlags4Component;
      ib.dimension = g.srvs[i].kind == Gen::SrvBinding::Kind::kTexCube ? dx::RdefDimension::kSRVTextureCube
                     : g.srvs[i].kind == Gen::SrvBinding::Kind::kTex3D ? dx::RdefDimension::kSRVTexture3D
                                                                       : dx::RdefDimension::kSRVTexture2D;
    }
  }
  if (g.uses_uav) {
    const uint32_t pos = b.Reserve<dx::RdefInputBind>();
    auto& ib = b.At<dx::RdefInputBind>(pos);
    ib.name_ptr = uav_name;
    ib.type = dx::RdefInputType::kUAVRWByteAddress;
    ib.return_type = dx::ResourceReturnType::kMixed;
    ib.dimension = dx::RdefDimension::kUAVBuffer;
    ib.bind_point = 0;
    ib.bind_count = 1;
    ib.id = 0;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t pos = b.Reserve<dx::RdefInputBind>();
    auto& ib = b.At<dx::RdefInputBind>(pos);
    ib.name_ptr = cb_names[i];
    ib.type = dx::RdefInputType::kCbuffer;
    ib.bind_point = i;
    ib.bind_count = 1;
    ib.flags = dx::kRdefInputFlagUserPacked;
    ib.id = i;
  }
  const uint32_t binds_end = b.Pos();
  auto& h = b.At<dx::RdefHeader>(start);
  h.cbuffer_count = 4;
  h.cbuffers_ptr = (cbs_pos - start) * 4;
  h.input_bind_count = (binds_end - binds_pos) * 4 / sizeof(dx::RdefInputBind);
  h.input_binds_ptr = (binds_pos - start) * 4;
  h.shader_model = vs ? dx::RdefShaderModel::kVertexShader5_1 : dx::RdefShaderModel::kPixelShader5_1;
  h.compile_flags = dx::kCompileFlagNoPreshader | dx::kCompileFlagPreferFlowControl | dx::kCompileFlagIeeeStrictness;
  h.generator_name_ptr = sizeof(dx::RdefHeader);
  h.fourcc = dx::RdefHeader::FourCC::k5_1;
  h.InitializeSizes();
}

struct SigParam {
  const char* name;
  uint32_t semantic_index;
  dx::Name sv;
  dx::SignatureRegisterComponentType type;
  uint32_t reg;
  uint8_t mask;
  uint8_t rw_mask;
};
void WriteSignature(Blobs& b, const std::vector<SigParam>& params) {
  const uint32_t start = b.Pos();
  b.Reserve<dx::Signature>();
  const uint32_t params_pos = b.Reserve<dx::SignatureParameter>(uint32_t(params.size()));
  // Names (one copy per distinct name).
  std::vector<std::pair<std::string, uint32_t>> names;
  std::vector<uint32_t> name_ptrs;
  for (const SigParam& sp : params) {
    uint32_t ptr = UINT32_MAX;
    for (const auto& n : names)
      if (n.first == sp.name) ptr = n.second;
    if (ptr == UINT32_MAX) {
      ptr = b.Rel(start);
      dx::AppendAlignedString(b.obj, sp.name);
      names.emplace_back(sp.name, ptr);
    }
    name_ptrs.push_back(ptr);
  }
  for (uint32_t i = 0; i < params.size(); ++i) {
    auto& par = b.At<dx::SignatureParameter>(params_pos + i * (sizeof(dx::SignatureParameter) / 4));
    par.semantic_name_ptr = name_ptrs[i];
    par.semantic_index = params[i].semantic_index;
    par.system_value = params[i].sv;
    par.component_type = params[i].type;
    par.register_index = params[i].reg;
    par.mask = params[i].mask;
    par.always_reads_mask = params[i].rw_mask;
  }
  auto& h = b.At<dx::Signature>(start);
  h.parameter_count = uint32_t(params.size());
  h.parameter_info_ptr = sizeof(dx::Signature);
}

}  // namespace

DxbcTranslation TranslateToDxbc(const Program& program, const hlsl::HlslOptions& options) {
  DxbcTranslation out;
  const bool vs = program.is_vertex_shader;

  // Pass 1: the facts (registers, constants, exports) - the code is thrown away.
  Gen g1(program, options, false);
  g1.base = 0;
  g1.next_temp = g1.max_temp = Gen::kFixedTemps;
  g1.EmitMain();
  if (!g1.unsupported.empty()) {
    out.unsupported = g1.unsupported;
    return out;
  }

  // Float-constant compaction (hlsl.cpp, same rule).
  uint32_t best_l = 0, best_h = 0;
  if (options.compact_float_constants && !g1.constants_relative && !g1.memexport) {
    const auto read = [&](uint32_t i) { return (g1.constants_read[i >> 6] >> (i & 63)) & 1; };
    int prev = -1;
    for (uint32_t i = 0; i < g1.float_constants; ++i) {
      if (!read(i)) continue;
      if (int(i) - prev - 1 > int(best_h - best_l)) {
        best_l = uint32_t(prev + 1);
        best_h = i;
      }
      prev = int(i);
    }
    if (best_h - best_l < hlsl::kCompactMinGap) best_l = best_h = 0;
  }

  // Pass 2: the code.
  Gen g(program, options, true);
  g.dynamic_regs = false;
  g.regs = g1.dynamic_regs ? 64u : std::max(g1.reg_count, vs ? (options.tess_quad_segments ? 2u : 1u) : 16u);
  g.dyn = g1.dynamic_regs;
  g.compact_l = best_l;
  g.compact_h = best_h;
  g.memexport_final = g1.memexport;
  g.writes_depth_final = !vs && g1.writes_depth;
  g.uses_x1_final = g1.uses_pc_loop;
  g.base = g.dyn ? 0 : g.regs;
  g.next_temp = g.max_temp = g.base + Gen::kFixedTemps;
  g.EmitMain();
  if (!g.unsupported.empty()) {
    out.unsupported = g.unsupported;
    return out;
  }
  if (options.dxbc_coalesce_moves) {  // job 634
    const uint32_t removed = coalesce::Run(g.code);
    g.stat.instruction_count -= std::min(g.stat.instruction_count, removed);
    if (g.stat.mov_instruction_count >= removed) g.stat.mov_instruction_count -= removed;
  }

  // Metadata (hlsl.cpp's Translation fields).
  hlsl::Translation& m = out.meta;
  m = g.meta;
  m.uses_pc_loop = g.uses_pc_loop;
  m.float_constants = g.float_constants;
  m.float_constants_low = m.float_constants_high = g.float_constants;
  if (best_h != best_l) {
    m.float_constants_low = best_l;
    m.float_constants_high = best_h;
  }
  for (int i = 0; i < 4; ++i) m.float_constants_read[i] = g.constants_read[i];
  m.float_constants_relative = g.constants_relative;
  m.memexport = g.memexport;
  m.memexport_stream_constants.assign(g.stream_constants.begin(), g.stream_constants.end());
  out.writes_depth = g.writes_depth_final;
  for (const auto& s : g.srvs) out.srv_registers.push_back(s.reg);
  out.sampler_registers = g.samplers;

  // Declarations + code (SHEX).
  const uint32_t sys_vectors = (options.vertex_index_clamp && vs) ? 6 : 5;
  std::vector<uint32_t> shex;
  dx::Statistics stat = g.stat;
  dx::Assembler d(shex, stat);
  shex.push_back(dx::VersionToken(vs ? dx::ProgramType::kVertexShader : dx::ProgramType::kPixelShader, 5, 1));
  shex.push_back(0);
  // No refactoring: precise (bit-identical positions between passes, notes/42). Patch 0095: pixel shaders that don't
  // write depth may allow it (--gow2_renderer_ps_refactoring) - their colour maths isn't compared between passes.
  // Not for shaders that kill (their discard decisions stay bit-exact) or export to memory (exported words may be read
  // back as vertex data).
  d.OpDclGlobalFlags(!vs && options.ps_refactoring_allowed && !g.writes_depth_final && !g.any_kill && !g.memexport_final
                         ? dx::kGlobalFlagRefactoringAllowed
                         : 0u);
  d.OpDclConstantBuffer(Src::CB(Src::Dcl, kCbSystem, 0, 0), sys_vectors);
  d.OpDclConstantBuffer(Src::CB(Src::Dcl, kCbFloat, 1, 1), 256,
                        g.constants_relative ? dx::ConstantBufferAccessPattern::kDynamicIndexed
                                             : dx::ConstantBufferAccessPattern::kImmediateIndexed);
  d.OpDclConstantBuffer(Src::CB(Src::Dcl, kCbBoolLoop, 2, 2), 10);
  d.OpDclConstantBuffer(Src::CB(Src::Dcl, kCbFetch, 3, 3),
                       !(options.tex_num_format && options.tex_post_constants) ? 48
                       : options.tex_post_affine ? kTexAffineBase + 32 * kTexAffineVec  // job 634
                                                 : 64);  // 0105: + 32 slots x 2 dwords
  for (uint32_t i = 0; i < g.samplers.size(); ++i)
    d.OpDclSampler(Src::S(Src::Dcl, i, g.samplers[i], g.samplers[i]));
  for (uint32_t i = 0; i < g.srvs.size(); ++i) {
    const uint32_t reg = g.srvs[i].reg;
    if (g.srvs[i].kind == Gen::SrvBinding::Kind::kRaw) {
      d.OpDclResourceRaw(Src::T(Src::Dcl, i, reg, reg), 0);
    } else {
      const dx::ResourceDimension dim = g.srvs[i].kind == Gen::SrvBinding::Kind::kTexCube ? dx::ResourceDimension::kTextureCube
                                        : g.srvs[i].kind == Gen::SrvBinding::Kind::kTex3D ? dx::ResourceDimension::kTexture3D
                                                                                         : dx::ResourceDimension::kTexture2D;
      d.OpDclResource(dim, dx::ResourceReturnTypeX4Token(dx::ResourceReturnType::kFloat), Src::T(Src::Dcl, i, reg, reg), 0);
    }
  }
  dx::ShaderFeatureInfo sfi{};
  if (g.uses_uav) {
    if (vs) sfi.feature_flags[0] |= dx::kShaderFeature0_UAVsAtEveryStage;
    d.OpDclUnorderedAccessViewRaw(0, Src::U(Src::Dcl, 0, 0, 0), 0);
  }
  if (vs) {
    d.OpDclInputSGV(Dest::V1D(0, kX), dx::Name::kVertexID);
    for (uint32_t i = 0; i < 16; ++i) d.OpDclOutput(Dest::O(i));
    d.OpDclOutputSIV(Dest::O(16), dx::Name::kPosition);
  } else {
    for (uint32_t i = 0; i < 16; ++i) d.OpDclInputPS(dx::InterpolationMode::kLinear, Dest::V1D(i));
    d.OpDclInputPSSIV(dx::InterpolationMode::kLinearNoPerspective, Dest::V1D(16), dx::Name::kPosition);
    d.OpDclInputPSSGV(Dest::V1D(17, kX), dx::Name::kIsFrontFace);
    for (uint32_t i = 0; i < 4; ++i) d.OpDclOutput(Dest::O(i));
    if (out.writes_depth) d.OpDclOutput(Dest::ODepth());
  }
  d.OpDclTemps(g.max_temp);
  if (g.dyn) d.OpDclIndexableTemp(0, 64, 4);
  if (g.uses_x1_final) d.OpDclIndexableTemp(1, 4, 4);
  if (g.memexport_final) d.OpDclIndexableTemp(2, 5, 4);
  shex.insert(shex.end(), g.code.begin(), g.code.end());
  shex[1] = uint32_t(shex.size());

  // Signatures.
  using CT = dx::SignatureRegisterComponentType;
  std::vector<SigParam> isgn, osgn;
  if (vs) {
    isgn.push_back({"SV_VertexID", 0, dx::Name::kVertexID, CT::kUInt32, 0, kX, kX});
    for (uint32_t i = 0; i < 16; ++i) osgn.push_back({"TEXCOORD", i, dx::Name::kUndefined, CT::kFloat32, i, kXYZW, 0});
    osgn.push_back({"SV_Position", 0, dx::Name::kPosition, CT::kFloat32, 16, kXYZW, 0});
  } else {
    for (uint32_t i = 0; i < 16; ++i) isgn.push_back({"TEXCOORD", i, dx::Name::kUndefined, CT::kFloat32, i, kXYZW, kXYZW});
    isgn.push_back({"SV_Position", 0, dx::Name::kPosition, CT::kFloat32, 16, kXYZW, kXYZW});
    isgn.push_back({"SV_IsFrontFace", 0, dx::Name::kIsFrontFace, CT::kUInt32, 17, kX, kX});
    for (uint32_t i = 0; i < 4; ++i) osgn.push_back({"SV_Target", i, dx::Name::kUndefined, CT::kFloat32, i, kXYZW, 0});
    if (out.writes_depth) osgn.push_back({"SV_Depth", 0, dx::Name::kUndefined, CT::kFloat32, UINT32_MAX, kX, 0b1110});
  }

  // Container: RDEF, ISGN, OSGN, SHEX, SFI0, STAT.
  Blobs b;
  constexpr uint32_t kBlobCount = 6;
  b.obj.resize(sizeof(dx::ContainerHeader) / 4 + kBlobCount);
  uint32_t blob = 0;
  auto begin_blob = [&](dx::BlobHeader::FourCC fourcc) {
    b.obj[sizeof(dx::ContainerHeader) / 4 + blob++] = b.Pos() * 4;
    const uint32_t pos = b.Reserve<dx::BlobHeader>();
    b.At<dx::BlobHeader>(pos).fourcc = fourcc;
    return pos;
  };
  auto end_blob = [&](uint32_t pos) { b.At<dx::BlobHeader>(pos).size_bytes = (b.Pos() - pos - 2) * 4; };
  uint32_t bp = begin_blob(dx::BlobHeader::FourCC::kResourceDefinition);
  WriteRdef(b, g, vs, sys_vectors);
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kInputSignature);
  WriteSignature(b, isgn);
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kOutputSignature);
  WriteSignature(b, osgn);
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kShaderEx);
  b.obj.insert(b.obj.end(), shex.begin(), shex.end());
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kShaderFeatureInfo);
  b.obj.resize(b.obj.size() + sizeof(sfi) / 4);
  std::memcpy(b.obj.data() + b.Pos() - sizeof(sfi) / 4, &sfi, sizeof(sfi));
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kStatistics);
  stat.temp_register_count = g.max_temp;
  b.obj.resize(b.obj.size() + sizeof(stat) / 4);
  std::memcpy(b.obj.data() + b.Pos() - sizeof(stat) / 4, &stat, sizeof(stat));
  end_blob(bp);
  const uint32_t size_bytes = b.Pos() * 4;
  {
    auto& h = b.At<dx::ContainerHeader>(0);
    h.InitializeIdentification();
    h.size_bytes = size_bytes;
    h.blob_count = kBlobCount;
    DxbcChecksum(reinterpret_cast<const uint8_t*>(b.obj.data()), size_bytes, h.hash);
  }
  out.bytecode.resize(size_bytes);
  std::memcpy(out.bytecode.data(), b.obj.data(), size_bytes);
  out.ok = true;
  if (g.rule_bits & 1u) g_rule_hits[0].fetch_add(1, std::memory_order_relaxed);  // patch 0087
  if (g.rule_bits & 2u) g_rule_hits[1].fetch_add(1, std::memory_order_relaxed);
  if (g.rule_bits & 4u) g_rule_hits[2].fetch_add(1, std::memory_order_relaxed);
  return out;
}

RuleHits CanaryRuleHits() {
  RuleHits h;
  h.tfetch1d_xy = g_rule_hits[0].load(std::memory_order_relaxed);
  h.mulsc_round_toward_zero = g_rule_hits[1].load(std::memory_order_relaxed);
  h.memexport_f16_nan_zero = g_rule_hits[2].load(std::memory_order_relaxed);
  return h;
}


std::vector<uint8_t> RectListGeometryShader() {
  // builtin_shaders.h kRectListGs (Canary e6cd2e4 pipeline_cache.cc, BSD): a primitive with a NaN position is dropped;
  // the longest edge picks the strip order; the 4th vertex is -v[a] + v[b] + v[c] for every output.
  std::vector<uint32_t> shex;
  dx::Statistics stat{};
  dx::Assembler a(shex, stat);
  shex.push_back(dx::VersionToken(dx::ProgramType::kGeometryShader, 5, 1));
  shex.push_back(0);
  a.OpDclGlobalFlags(0);
  for (uint32_t i = 0; i < 16; ++i) a.OpDclInput(Dest::V2D(3, i));
  a.OpDclInputSIV(Dest::V2D(3, 16), dx::Name::kPosition);
  a.OpDclTemps(2);
  a.OpDclInputPrimitive(dx::Primitive::kTriangle);
  a.OpDclStream(Dest::M(0));
  a.OpDclOutputTopology(dx::PrimitiveTopology::kTriangleStrip);
  for (uint32_t i = 0; i < 16; ++i) a.OpDclOutput(Dest::O(i));
  a.OpDclOutputSIV(Dest::O(16), dx::Name::kPosition);
  a.OpDclMaxOutputVertexCount(4);
  const auto R = [](uint32_t t, uint32_t m = kXYZW) { return Dest::R(t, m); };
  const auto S = [](uint32_t t, uint32_t sw = Src::kXYZW) { return Src::R(t, sw); };
  // if (any(isnan(v[n].position))) return;
  for (uint32_t n = 0; n < 3; ++n) {
    a.OpNE(R(0), Src::V2D(n, 16), Src::V2D(n, 16));
    a.OpOr(R(0, kXY), S(0), S(0, Sw(2, 3, 0, 0)));
    a.OpOr(R(0, kX), S(0, Src::kXXXX), S(0, Src::kYYYY));
    a.OpRetC(true, S(0, Src::kXXXX));
  }
  // Squared edge lengths e12, e20, e01 into r1.xyz, each product and sum rounded on its own.
  const uint32_t ends[3][2] = {{2, 1}, {0, 2}, {1, 0}};
  for (uint32_t e = 0; e < 3; ++e) {
    a.OpAdd(R(0, kXY), Src::V2D(ends[e][0], 16), -Src::V2D(ends[e][1], 16));
    a.OpMul(R(0, kXY), S(0), S(0));
    a.OpAdd(R(1, 1u << e), S(0, Src::kXXXX), S(0, Src::kYYYY));
  }
  // if (e20 < e12 && e01 < e12) o = (0, 1, 2) else o = e01 < e20 ? (1, 2, 0) : (2, 0, 1); into r0.xyz.
  a.OpLT(R(0, kX), S(1, Src::kYYYY), S(1, Src::kXXXX));
  a.OpLT(R(0, kY), S(1, Src::kZZZZ), S(1, Src::kXXXX));
  a.OpAnd(R(0, kW), S(0, Src::kXXXX), S(0, Src::kYYYY));
  a.OpLT(R(0, kX), S(1, Src::kZZZZ), S(1, Src::kYYYY));
  a.OpMovC(R(0, kXYZ), S(0, Src::kXXXX), Src::LU(1, 2, 0, 0), Src::LU(2, 0, 1, 0));
  a.OpMovC(R(0, kXYZ), S(0, Src::kWWWW), Src::LU(0, 1, 2, 0), S(0));
  for (uint32_t k = 0; k < 3; ++k) {
    for (uint32_t j = 0; j < 17; ++j) a.OpMov(Dest::O(j), Src::V2D(Index(0, k), j));
    a.OpEmitStream(Dest::M(0));
  }
  for (uint32_t j = 0; j < 17; ++j) {
    a.OpAdd(R(1), -Src::V2D(Index(0, 0), j), Src::V2D(Index(0, 1), j));
    a.OpAdd(R(1), S(1), Src::V2D(Index(0, 2), j));
    a.OpMov(Dest::O(j), S(1));
  }
  a.OpEmitStream(Dest::M(0));
  a.OpCutStream(Dest::M(0));
  a.OpRet();
  shex[1] = uint32_t(shex.size());

  Blobs b;
  constexpr uint32_t kBlobCount = 5;  // RDEF, ISGN, OSG5, SHEX, STAT
  b.obj.resize(sizeof(dx::ContainerHeader) / 4 + kBlobCount);
  uint32_t blob = 0;
  auto begin_blob = [&](dx::BlobHeader::FourCC fourcc) {
    b.obj[sizeof(dx::ContainerHeader) / 4 + blob++] = b.Pos() * 4;
    const uint32_t pos = b.Reserve<dx::BlobHeader>();
    b.At<dx::BlobHeader>(pos).fourcc = fourcc;
    return pos;
  };
  auto end_blob = [&](uint32_t pos) { b.At<dx::BlobHeader>(pos).size_bytes = (b.Pos() - pos - 2) * 4; };
  uint32_t bp = begin_blob(dx::BlobHeader::FourCC::kResourceDefinition);
  {
    const uint32_t start = b.Pos();
    b.Reserve<dx::RdefHeader>();
    dx::AppendAlignedString(b.obj, "Gears2-Native xenos::dxbc_gen");
    auto& h = b.At<dx::RdefHeader>(start);
    h.shader_model = dx::RdefShaderModel::kGeometryShader5_1;
    h.compile_flags = dx::kCompileFlagNoPreshader | dx::kCompileFlagPreferFlowControl | dx::kCompileFlagIeeeStrictness;
    h.generator_name_ptr = sizeof(dx::RdefHeader);
    h.fourcc = dx::RdefHeader::FourCC::k5_1;
    h.InitializeSizes();
  }
  end_blob(bp);
  using CT = dx::SignatureRegisterComponentType;
  std::vector<SigParam> isgn;
  for (uint32_t i = 0; i < 16; ++i) isgn.push_back({"TEXCOORD", i, dx::Name::kUndefined, CT::kFloat32, i, kXYZW, kXYZW});
  isgn.push_back({"SV_Position", 0, dx::Name::kPosition, CT::kFloat32, 16, kXYZW, kXYZW});
  bp = begin_blob(dx::BlobHeader::FourCC::kInputSignature);
  WriteSignature(b, isgn);
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kOutputSignatureForGS);
  {
    const uint32_t start = b.Pos();
    b.Reserve<dx::Signature>();
    const uint32_t params = b.Reserve<dx::SignatureParameterForGS>(17);
    const uint32_t texcoord = b.Rel(start);
    dx::AppendAlignedString(b.obj, "TEXCOORD");
    const uint32_t position = b.Rel(start);
    dx::AppendAlignedString(b.obj, "SV_Position");
    for (uint32_t i = 0; i < 17; ++i) {
      auto& p = b.At<dx::SignatureParameterForGS>(params + i * (sizeof(dx::SignatureParameterForGS) / 4));
      p.semantic_name_ptr = i < 16 ? texcoord : position;
      p.semantic_index = i < 16 ? i : 0;
      p.system_value = i < 16 ? dx::Name::kUndefined : dx::Name::kPosition;
      p.component_type = CT::kFloat32;
      p.register_index = i;
      p.mask = kXYZW;
    }
    auto& h = b.At<dx::Signature>(start);
    h.parameter_count = 17;
    h.parameter_info_ptr = sizeof(dx::Signature);
  }
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kShaderEx);
  b.obj.insert(b.obj.end(), shex.begin(), shex.end());
  end_blob(bp);
  bp = begin_blob(dx::BlobHeader::FourCC::kStatistics);
  stat.input_primitive = dx::Primitive::kTriangle;
  stat.gs_output_topology = dx::PrimitiveTopology::kTriangleStrip;
  stat.gs_max_output_vertex_count = 4;
  b.obj.resize(b.obj.size() + sizeof(stat) / 4);
  std::memcpy(b.obj.data() + b.Pos() - sizeof(stat) / 4, &stat, sizeof(stat));
  end_blob(bp);
  const uint32_t size_bytes = b.Pos() * 4;
  auto& h = b.At<dx::ContainerHeader>(0);
  h.InitializeIdentification();
  h.size_bytes = size_bytes;
  h.blob_count = kBlobCount;
  DxbcChecksum(reinterpret_cast<const uint8_t*>(b.obj.data()), size_bytes, h.hash);
  std::vector<uint8_t> out(size_bytes);
  std::memcpy(out.data(), b.obj.data(), size_bytes);
  return out;
}

}  // namespace xenos::dxbc_gen

namespace xenos::hlsl {
// Patch 0105: the same values TexPost works out per pixel (tex_num_format path), from the slot's fetch constant.
void TexPostConstants(const uint32_t* f, bool sign_by_swizzle, uint32_t out[2]) {
  const uint32_t dw0 = f[0], dw1 = f[1], dw3 = f[3];
  uint32_t sw[4], s[4];
  for (uint32_t c = 0; c < 4; ++c) {
    if (sign_by_swizzle) {
      sw[c] = (dw3 >> (1 + 3 * c)) & 7;
      s[c] = sw[c] < 4 ? (dw0 >> (2 * sw[c] + 2)) & 3 : 0;
    } else {
      sw[c] = c;
      s[c] = (dw0 >> (2 + 2 * c)) & 3;
    }
  }
  uint32_t fb[4] = {};
  switch (dw1 & 63) {  // xe_fixed_bits (dxbc_gen FixedBits)
    case 2: case 8: case 9: case 58: case 59: fb[0] = 8; break;
    case 3: fb[0] = 5; fb[1] = 5; fb[2] = 5; fb[3] = 1; break;
    case 4: fb[0] = 5; fb[1] = 6; fb[2] = 5; break;
    case 5: fb[0] = 5; fb[1] = 5; fb[2] = 6; break;
    case 6: case 14: case 50: case 62: fb[0] = fb[1] = fb[2] = fb[3] = 8; break;
    case 7: case 54: fb[0] = 10; fb[1] = 10; fb[2] = 10; fb[3] = 2; break;
    case 10: case 49: case 60: fb[0] = 8; fb[1] = 8; break;
    case 13: case 25: fb[0] = 16; fb[1] = 16; break;
    case 15: fb[0] = fb[1] = fb[2] = fb[3] = 4; break;
    case 16: case 55: fb[0] = 11; fb[1] = 11; fb[2] = 10; break;
    case 17: case 56: fb[0] = 10; fb[1] = 11; fb[2] = 11; break;
    case 21: case 26: fb[0] = fb[1] = fb[2] = fb[3] = 16; break;
    case 24: fb[0] = 16; break;
    default: break;
  }
  const bool fixed = fb[0] || fb[1] || fb[2] || fb[3];
  const bool integer = (dw3 & 1) != 0;
  const uint32_t t0 = (dw3 >> 19) & 15, t1 = (dw3 >> 23) & 3, t2 = (dw3 >> 25) & 7;
  const bool pt = t0 == 0 && (t1 == 0 || t1 == 2) && t2 == 0;
  const uint32_t last = fb[3] ? 3 : (fb[2] ? 2 : (fb[1] ? 1 : 0));
  out[0] = (fixed ? 1u << 16 : 0) | (integer ? 1u << 17 : 0) | (pt ? 1u << 18 : 0);
  out[1] = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t m = std::min(sw[c], last);
    uint32_t w = m == 0 ? fb[0] : m == 1 ? fb[1] : m == 2 ? fb[2] : fb[3];
    if (!(sw[c] < 4 && fixed)) w = 0;
    const bool ok16 = w != 0 && w <= 16;
    bool car = (s[c] == 0 && !integer) ? (ok16 && pt && w >= 4 && w <= 6) : ok16;
    if (s[c] == 3) car = false;
    const bool sgo = !(s[c] == 3 && integer) && ok16 && sw[c] < 4 && fixed;
    const uint32_t sg = sgo ? s[c] : 0;
    const uint32_t wd = car ? w : 0;
    out[0] |= (s[c] | sg << 2) << (4 * c);
    out[1] |= wd << (8 * c);
  }
}

// Job 634: the tex_post_affine constants of one slot (dxbc_gen TexPostAffine), from TexPostConstants' two dwords.
void TexPostAffine(const uint32_t* f, bool sign_by_swizzle, uint32_t out[kTexPostAffineVectors * 4]) {
  uint32_t D[2];
  TexPostConstants(f, sign_by_swizzle, D);
  const bool fixed = (D[0] >> 16) & 1, integer = (D[0] >> 17) & 1, pt = (D[0] >> 18) & 1;
  const int32_t k = int32_t(f[3] << 13) >> 26;
  const float m = std::ldexp(1.0f, k);
  auto put = [&](uint32_t vec, uint32_t c, float x) { std::memcpy(&out[vec * 4 + c], &x, 4); };
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t s = (D[0] >> (4 * c)) & 3, sg = (D[0] >> (4 * c + 2)) & 3, wd = (D[1] >> (8 * c)) & 255;
    uint32_t fl = 0;
    float a1 = 2.0f, b1 = -1.0f, d1 = 1.0f, a2 = 1.0f, b2 = 0.0f, p = 1.0f;
    if (s == 3) fl |= 1;
    if (s == 2) {
      fl |= 2;
      if (wd >= 2) {  // the same float operations as the per-pixel form, so the same values
        const float h = float(1u << (wd - 1));
        a1 = h * 2.0f - 1.0f;
        b1 = -h;
        d1 = h - 1.0f;
      }
    }
    if (fixed && !integer) {
      if (sg == 0) {
        fl |= 32;
        if (wd) {
          fl |= 16;
          p = float(1u << wd) - 1.0f;
        }
      }
      if (sg == 2) fl |= 64;
    }
    if (fixed && integer) {
      fl |= 4;
      if (pt) fl |= 8;
      uint32_t w = wd ? wd : 1;
      if (sg == 1 || sg == 2) w -= 1;
      const float h = float((1u << (w & 31)) - 1u);
      a2 = std::max(h, 0.5f);
      b2 = std::min(h, 0.5f) - 0.5f;
    }
    out[c] = fl;
    put(1, c, a1);
    put(2, c, b1);
    put(3, c, d1);
    put(4, c, a2);
    put(5, c, b2);
    put(6, c, p);
    put(7, c, m);
  }
}
}  // namespace xenos::hlsl
