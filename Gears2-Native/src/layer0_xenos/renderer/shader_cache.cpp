// Layer 0 (game-free): see xenos/renderer/shader_cache.h. Our code (MIT).
#include <xenos/renderer/shader_cache.h>

#include <xenos/dev_tools.h>
#include <xenos/dxbc_gen.h>
#include <xenos/dxbc_spirv.h>
#include <xenos/hlsl.h>
#include <xenos/renderer/native_hdr.h>
#include <xenos/ucode.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>  // path_to_utf8 (F-12)
#include <rex/logging/macros.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <system_error>
#ifdef GN_SPIRV_OPT
#include <spirv-tools/optimizer.hpp>
#endif

// Notes/80 developer diagnostic: the translated HLSL of the listed shaders (ucode hashes, hex, comma-separated) is written
// to --gow2_renderer_hlsl_dump_dir as <vs|ps>-<hash>.hlsl, whatever back end compiles them.
REXCVAR_DEFINE_STRING(gow2_renderer_hlsl_dump_hashes, "", "Gears2",
                      "Developer (notes/80): comma list of shader ucode hashes whose translated HLSL is written out");
REXCVAR_DEFINE_STRING(gow2_renderer_hlsl_dump_dir, "", "Gears2", "Developer: folder for --gow2_renderer_hlsl_dump_hashes");
REXCVAR_DEFINE_STRING(gow2_pcss, "off", "Gears2",
                      "Notes/80 s.16 (Gears 3): soft shadows (PCSS) for the dynamic shadows - off | high (quality high, "
                      "radius 0.35) | ultra (quality ultra, radius 0.25, the shadow maps drawn at the render scale). The "
                      "three shadow-projection shaders get a blocker search and a filter as wide as the blocker's distance");
REXCVAR_DEFINE_INT32(gow2_pcss_mode, 1, "Gears2",
                     "Notes/80 s.16 (developer): the start-up state of the PCSS shadow shaders - 0 the game's filter, 1 PCSS, "
                     "2 split view (left the game's, right PCSS), 3 coverage view; F12 toggles the filter, Shift+F12 the view "
                     "(with --gow2_ao_debug_keys)");
REXCVAR_DEFINE_INT32(gow2_pcss_quality, 2, "Gears2",
                     "Notes/80 s.16 (developer): the PCSS quality - 0 low, 1 medium, 2 high, 3 ultra (blocker-search / filter samples "
                     "8/16, 16/24, 16/32, 32/64); Ctrl+F12 cycles it");
REXCVAR_DEFINE_DOUBLE(gow2_pcss_radius, 0.5, "Gears2",
                      "Notes/80 s.16 (developer): the PCSS radius - scales the penumbra, its widest and the blocker search (1.0, 0.75, "
                      "0.5, 0.35 or 0.25, the nearest is taken); Alt+F12 cycles it");
REXCVAR_DEFINE_STRING(gow2_renderer_hlsl_override_dir, "", "Gears2",
                      "Developer (notes/80 s.16): folder of <vs|ps>-<hash>.hlsl files used in place of those shaders' "
                      "translated HLSL (same resources and constants; the HLSL compiler path, not DXBC)");

namespace xenos::renderer {

// Notes/80 s.16 (--gow2_pcss): Gears 3's three dynamic-shadow projection shaders keep their 9-tap 3x3 filter; PCSS is
// computed from the same receiver depth and shadow-map coordinate and replaces the filter's combined result (r0.x)
// before the game's fade / modulate maths. Live state from the system constants: xe_sys1.z bit 0 PCSS on, bits 1-2 the
// view (0 normal, 1 split: left the game's filter, right PCSS, 2 coverage: the right half shadowed where a shadow pass
// draws), bits 3-4 the quality (blocker-search texels 16/32/32/64 and bilinear filter gathers 8/12/16/32; a pixel
// whose whole search is blockers skips the filter); the penumbra in world units from the shadow matrix (orthographic:
// depth gap x angle; perspective: similar triangles with a light size), in texels at the receiver; xe_sys1.w the radius, which
// scales the penumbra growth, its widest and the blocker search. Distances in the game's shadow-map texels, so a map
// drawn at the render scale gives finer samples, not a different shape.
// The shadow matrix: the receiver's shadow-space position is col0 * a + col1 * b + col2 * c + konst (a, b, c its
// view-space coordinates; what each column is varies by shader), components depth / u / w as `dz` / `du` / `dw`.
struct PcssMatrix { int col0, col1, col2, konst; char dz, du, dw; };
static std::string PcssBlock(uint32_t slot, const char* sharp, const char* one, const PcssMatrix& m) {
  std::string b = R"({  // PCSS (notes/80 s.16)
  const uint pc_st = xe_sys1.z;
  const uint pc_view = (pc_st >> 1u) & 3u, pc_q = (pc_st >> 3u) & 3u;
  const float pc_rs = asfloat(xe_sys1.w) > 0.0 ? asfloat(xe_sys1.w) : 1.0;
  float2 pc_ssz; xe_tex_2d_0.GetDimensions(pc_ssz.x, pc_ssz.y);
  const bool pc_right = xe_in.position.x >= pc_ssz.x * 0.5;
  if (pc_view == 1u ? pc_right : (pc_st & 1u) != 0u) {
    const uint nb = pc_q == 0u ? 8u : (pc_q == 3u ? 32u : 16u);
    const uint nf = pc_q == 0u ? 8u : (pc_q == 1u ? 12u : (pc_q == 2u ? 16u : 32u));  // bilinear gathers
    float2 gsz; xe_tex_2d_@S.GetDimensions(gsz.x, gsz.y);
    if ((asuint(xe_ndc_offset.w) >> @Su) & 1u) gsz /= xe_ndc_scale.w;
    float2 hsz; xe_tex_2d_@S.GetDimensions(hsz.x, hsz.y);  // the host texture (a scaled map: more texels)
    const float2 texel = 1.0 / gsz;
    const float sharp = @K, one = @O;
    const float spin = frac(sin(dot(floor(xe_in.position.xy), float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    const float dec0 = xe_tex_post(float4(0.0, 0.0, 0.0, 0.0), @Su).x, dec1 = xe_tex_post(float4(1.0, 0.0, 0.0, 0.0), @Su).x - dec0;
    // The light, from the shadow matrix: orthographic (the sun, directional lights: no w row) or perspective (spot lights).
    const float3 rz = float3(xe_c[@A].@Z, xe_c[@B].@Z, xe_c[@C].@Z), ru = float3(xe_c[@A].@U, xe_c[@B].@U, xe_c[@C].@U);
    const float3 rw = float3(xe_c[@A].@W, xe_c[@B].@W, xe_c[@C].@W);
    const float cz = xe_c[@N].@Z, cw = xe_c[@N].@W;
    const bool persp = dot(rw, rw) > 1e-8;
    const float ru_len = max(length(ru), 1e-20), rz_len = max(length(rz), 1e-20);
    const float alpha = persp ? dot(rz, rw) / dot(rw, rw) : 0.0, beta = cz - alpha * cw;  // perspective: depth = alpha * w + beta
    // Blocker search: a gather per sample (4 texels).
    const uint ng = max(nb / 2u, 4u);
    float bsum = 0.0, bn = 0.0, csum = 0.0;
    [loop] for (uint i = 0u; i < ng; ++i) {
      const float a = spin + float(i) * 2.39996323, rr = sqrt((float(i) + 0.5) / float(ng));
      const float2 o = float2(cos(a), sin(a)) * rr * (8.0 * pc_rs) * texel;
      const float4 g = xe_tex_2d_@S.GatherRed(xe_sampler_@S, pc_uv + o) * dec1 + dec0;
      const float4 c = saturate((g - pc_recv) * sharp + one);
      csum += dot(c, 1.0);
      const float4 blk = float4(c < 0.5);
      bn += dot(blk, 1.0);
      bsum += dot(blk, g);
    }
    const float nt = float(ng * 4u);
    float lit = 1.0;
    if (bn >= nt) {
      lit = csum / nt;  // every texel searched is a blocker: deep in the shadow, the search's own average, no filter
    } else if (bn > 0.0) {
      // The penumbra in world units at the receiver, then in the game's texels. Orthographic: the depth gap (world) x the
      // light's angular size. Perspective: (receiver - blocker) / blocker distance x the light's size (similar triangles).
      const float db = bsum / bn;
      float pen;
      if (!persp) {
        pen = (pc_recv - db) / rz_len * (0.2 * pc_rs) * ru_len * gsz.x;
      } else {
        const float lr = max(pc_w, 1e-4), lb = max((db - beta) / alpha, 1e-4);
        pen = max(lr - lb, 0.0) / lb * (40.0 * pc_rs) * ru_len * gsz.x / lr;
      }
      pen = clamp(pen, 1.0, max(1.0, 12.0 * pc_rs));
      // Filter: bilinear-weighted soft comparisons, a gather each, on a spiral rotated per pixel.
      float sum = 0.0;
      [loop] for (uint j = 0u; j < nf; ++j) {
        const float a = spin + float(j) * 2.39996323, rr = sqrt((float(j) + 0.5) / float(nf));
        const float2 suv = pc_uv + float2(cos(a), sin(a)) * rr * pen * texel;
        const float4 g = xe_tex_2d_@S.GatherRed(xe_sampler_@S, suv) * dec1 + dec0;
        const float4 c = saturate((g - pc_recv) * sharp + one);
        const float2 f = frac(suv * hsz - 0.5);
        sum += lerp(lerp(c.w, c.z, f.x), lerp(c.x, c.y, f.x), f.y);
      }
      lit = sum / float(nf);
    }
    r[0].x = lit;
  }
  if (pc_view == 2u && pc_right) r[0].x = 0.0;
}
)";
  const auto put = [&b](const std::string& from, const std::string& to) {
    for (size_t at = b.find(from); at != std::string::npos; at = b.find(from, at + to.size())) b.replace(at, from.size(), to);
  };
  put("@S", std::to_string(slot));
  put("@K", sharp);
  put("@O", one);
  put("@A", std::to_string(m.col0));
  put("@B", std::to_string(m.col1));
  put("@C", std::to_string(m.col2));
  put("@N", std::to_string(m.konst));
  put("@Z", std::string(1, m.dz));
  put("@U", std::string(1, m.du));
  put("@W", std::string(1, m.dw));
  return b;
}

// The PCSS patch for one of the three shaders (translated HLSL in), or false when the text is not the shape it was
// written against (a translator change) - the shader then keeps the game's filter.
static bool ApplyPcss(uint64_t ucode_hash, std::string& hlsl) {
  const auto once = [&hlsl](const std::string& a) {
    const size_t at = hlsl.find(a);
    return at != std::string::npos && hlsl.find(a, at + 1) == std::string::npos;
  };
  const auto insert_before = [&hlsl](const std::string& a, const std::string& text) { hlsl.insert(hlsl.find(a), text); };
  const auto insert_after = [&hlsl](const std::string& a, const std::string& text) { hlsl.insert(hlsl.find(a) + a.size(), text); };
  if (ucode_hash == 0x3C5E875D17B49A36ull) {  // the light-attenuation buffer: one straight block, cf 2 - cf 7 the filter
    const std::string cf2 = "    // cf 2\n";
    const std::string tail = "      {\n        float4 s0 = r[0].xxxx;\n        float4 s1 = r[0].xxxx;\n        float4 s2 = -(xe_c[12].yyyy);";
    if (!once(cf2) || !once(tail)) return false;
    insert_before(cf2, "    const float pc_recv = xe_min(r[0].xxxx, xe_c[12].zzzz).x;\n"
                       "    const float2 pc_uv = r[0].yz * xe_rcp(r[0].w);\n"
                       "    const float pc_w = r[0].w;\n");
    // r0 = c8.zxyw * x + c9.zxyw * y + c10.zxyw * z + c11.zxyw: depth in the constants' z, u in x, w in w
    insert_before(tail, PcssBlock(1, "xe_c[7].z", "xe_c[12].y", PcssMatrix{8, 9, 10, 11, 'z', 'x', 'w'}));
    return true;
  }
  struct Modulated { uint64_t hash; int reg; const char* clamp; const char* sharp; const char* one; PcssMatrix m; };
  static const Modulated kModulated[] = {  // modulated shadows: a state machine, the coordinate ready at step 2
      {0xCA3F4A808B09D6F4ull, 5, "xe_c[18].zzzz", "xe_c[12].z", "xe_c[18].y", {13, 14, 15, 16, 'z', 'x', 'w'}},
      {0x857CECDEB64A657Full, 6, "xe_c[20].zzzz", "xe_c[14].z", "xe_c[20].y", {15, 16, 17, 18, 'z', 'x', 'w'}},
  };
  for (const Modulated& m : kModulated) {
    if (ucode_hash != m.hash) continue;
    const std::string top = "  [loop] while (xe_guard++ < 65536u) {\n";
    const std::string step2 = "    if (xe_pc == 2u) {\n";
    const std::string step9_end = "      xe_pc = 10u;\n";
    if (!once(top) || !once(step2) || !once(step9_end)) return false;
    const std::string reg = "r[" + std::to_string(m.reg) + "]";
    insert_before(top, "  float pc_recv = 0.0, pc_w = 1.0;\n  float2 pc_uv = float2(0.0, 0.0);\n");
    insert_after(step2, "      pc_recv = xe_min(" + reg + ".zzzz, " + m.clamp + ").x;\n      pc_uv = " + reg + ".xy * xe_rcp(" +
                            reg + ".w);\n      pc_w = " + reg + ".w;\n");
    insert_before(step9_end, PcssBlock(2, m.sharp, m.one, m.m));
    return true;
  }
  return false;
}
static bool PcssShader(uint64_t ucode_hash) {
  return ucode_hash == 0x3C5E875D17B49A36ull || ucode_hash == 0xCA3F4A808B09D6F4ull || ucode_hash == 0x857CECDEB64A657Full;
}
static bool PcssOn() {  // --gow2_pcss high / ultra
  static const bool on = REXCVAR_GET(gow2_pcss) == "high" || REXCVAR_GET(gow2_pcss) == "ultra";
  return on;
}

// Developer (--gow2_renderer_hlsl_override_dir): the replacement HLSL for this shader, empty if there is none.
static std::string HlslOverride(uint64_t ucode_hash, bool is_vertex) {
  static const std::string dir = REXCVAR_GET(gow2_renderer_hlsl_override_dir);
  if (dir.empty()) return {};
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(ucode_hash));
  std::ifstream f(std::filesystem::path(dir) / ((is_vertex ? "vs-" : "ps-") + std::string(hex) + ".hlsl"), std::ios::binary);
  if (!f) return {};
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

namespace {

double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Analyse(const ucode::Program& program, GuestShader& s) {
  using ucode::Instruction;
  using ucode::Target;
  auto reg = [&](uint32_t index, ucode::Addressing addressing) {
    if (addressing != ucode::Addressing::kAbsolute) s.dynamic_addressing = true;
    else s.register_bound = std::max(s.register_bound, index + 1);
  };
  auto operand = [&](const ucode::Operand& o) {
    if (o.source == ucode::Source::kRegister) reg(o.index, o.addressing);
  };
  int color0_writes = 0;
  int32_t color0_constant = -1;
  auto result = [&](const ucode::Result& r) {
    if (r.target == Target::kRegister && r.write_mask) reg(r.index, r.addressing);
    if (r.target == Target::kColor && r.index == 0 && r.write_mask) ++color0_writes;
    switch (r.target) {
      case Target::kDepth: if (r.write_mask) s.writes_depth = true; break;
      case Target::kColor: if (r.index < 4) s.color_targets_written |= 1u << r.index; break;
      case Target::kExportAddress: case Target::kExportData: s.memexport = true; break;
      default: break;
    }
  };
  for (const ucode::ControlFlow& cf : program.control_flow) {
    for (const Instruction& ins : cf.instructions) {
      switch (ins.kind) {
        case Instruction::Kind::kAlu: {
          const ucode::Alu& a = ins.alu;
          const uint32_t v = uint32_t(a.vector_opcode);
          if (!a.IsVectorOpDefaultNop()) {
            if (v >= uint32_t(ucode::AluVectorOpcode::kKillEq) && v <= uint32_t(ucode::AluVectorOpcode::kKillNe))
              s.kills = true;
          }
          const uint32_t sc = uint32_t(a.scalar_opcode);
          if (sc >= uint32_t(ucode::AluScalarOpcode::kKillsEq) && sc <= uint32_t(ucode::AluScalarOpcode::kKillsOne))
            s.kills = true;
          result(a.vector_result);
          result(a.scalar_result);
          // oC0 = c[k]: Xenos "mov" is max(x, x); both operands the same absolute float constant, plain swizzle.
          if (a.vector_result.target == Target::kColor && a.vector_result.index == 0 && !a.predicated &&
              a.vector_opcode == ucode::AluVectorOpcode::kMax && a.vector_operand_count == 2 &&
              a.vector_result.write_mask == 0xF && !a.vector_result.clamp) {
            const ucode::Operand& o0 = a.vector_operands[0];
            const ucode::Operand& o1 = a.vector_operands[1];
            if (o0.source == ucode::Source::kConstantFloat && o1.source == o0.source && o0.index == o1.index &&
                o0.addressing == ucode::Addressing::kAbsolute && o1.addressing == o0.addressing &&
                o0.IsStandardSwizzle() && o1.IsStandardSwizzle() && !o0.negate && !o1.negate && !o0.absolute &&
                !o1.absolute)
              color0_constant = int32_t(o0.index);
          }
          for (uint32_t i = 0; i < a.vector_operand_count && i < 3; ++i) operand(a.vector_operands[i]);
          for (uint32_t i = 0; i < a.scalar_operand_count && i < 2; ++i) operand(a.scalar_operands[i]);
          break;
        }
        case Instruction::Kind::kVertexFetch:
          result(ins.vfetch.result);
          operand(ins.vfetch.address);
          if (ins.vfetch.data_format < 64) s.vfetch_formats |= uint64_t(1) << ins.vfetch.data_format;
          if (ins.vfetch.fetch_constant < 96)
            s.vfetch_constants[ins.vfetch.fetch_constant >> 5] |= 1u << (ins.vfetch.fetch_constant & 31);
          break;
        case Instruction::Kind::kTextureFetch:
          if (ins.tfetch.has_result) {
            result(ins.tfetch.result);
            if (ins.tfetch.result.target == Target::kColor) color0_writes += 2;  // not a constant colour
          }
          operand(ins.tfetch.coordinates);
          if (ins.tfetch.opcode == ucode::FetchOpcode::kTextureFetch && ins.tfetch.fetch_constant < 32)
            s.texture_slots |= 1u << ins.tfetch.fetch_constant;
          break;
      }
    }
  }
  if (!s.is_vertex && color0_writes == 1 && color0_constant >= 0 && color0_constant < 256)
    s.constant_color0 = color0_constant;
}

const char* StageProfile(gpu::ShaderStage stage) {
  switch (stage) {
    case gpu::ShaderStage::kVertex: return "vs";
    case gpu::ShaderStage::kPixel: return "ps";
    case gpu::ShaderStage::kGeometry: return "gs";
    case gpu::ShaderStage::kCompute: return "cs";
  }
  return "xx";
}

}  // namespace

ShaderCache::ShaderCache(gpu::Backend& backend, const ShaderCompilerFactory& factory, std::filesystem::path dir,
                         uint32_t threads)
    : backend_(backend), dir_(std::move(dir)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  dir_ok_ = std::filesystem::is_directory(dir_, ec);
  if (!dir_ok_) REXLOG_WARN("gow2_renderer: shader cache folder {} not usable; compiling in memory only", rex::path_to_utf8(dir_));
  if (factory) sync_compiler_ = factory();
  tag_ = sync_compiler_ ? sync_compiler_->CacheTag(backend_.GetShaderIL()) : std::string("no-compiler");
  if (!sync_compiler_ || !factory) return;
  if (!threads) {
    const uint32_t hw = std::thread::hardware_concurrency();
    threads = hw > 3 ? hw - 2 : 1;
  }
  for (uint32_t i = 0; i < threads; ++i) {
    std::unique_ptr<ShaderCompiler> c = factory();
    if (!c) break;
    worker_compilers_.push_back(std::move(c));
  }
  for (auto& c : worker_compilers_) workers_.emplace_back([this, raw = c.get()] { Worker(raw); });
  stats_.threads = uint32_t(workers_.size());
}

ShaderCache::~ShaderCache() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : workers_) t.join();
  spv_wake_.notify_all();
  for (std::thread& t : spv_workers_) t.join();
  {
    std::lock_guard<std::mutex> lock(ucode_mutex_);  // patch 0014: the writer finishes its queue first
    ucode_stop_ = true;
  }
  ucode_cv_.notify_all();
  if (ucode_thread_.joinable()) ucode_thread_.join();
  if (pack_on_) {
    const PackInfo p = GetPackInfo();
    REXLOG_INFO("gow2_renderer: shader pack at exit: {} shaders, {:.1f} MB, {} added this session, {} bad reads",
                p.shaders, double(p.bytes) / 1048576.0, p.added, p.bad);
    std::lock_guard<std::mutex> lock(pack_mutex_);
    pack_.close();
  }
}

std::string ShaderCache::CompilerDescription() const {
  if (!sync_compiler_) return "no compiler";
  return sync_compiler_->Describe() + ", " + std::to_string(workers_.size()) + " compile thread(s)";
}

std::string ShaderCache::FileName(const std::string& stem, const std::string& hlsl) const {
  uint64_t key = Fnv1a64(hlsl.data(), hlsl.size());
  key = Fnv1a64(tag_.data(), tag_.size(), key);
  char name[112];
  std::snprintf(name, sizeof(name), "%s-%016llX.%s", stem.c_str(), static_cast<unsigned long long>(key),
                backend_.GetShaderIL() == gpu::ShaderIL::kDxil ? "dxil" : "spv");
  return name;
}

bool ShaderCache::LoadFromDisk(const std::string& name, std::vector<uint8_t>& bytecode, bool* from_pack) {
  if (from_pack) *from_pack = false;
  if (!dir_ok_) return false;
  if (PackFind(name, bytecode)) {
    if (from_pack) *from_pack = true;
    return true;
  }
  // Patch 0015: one read of the known size (was a byte-at-a-time istreambuf_iterator copy).
  std::ifstream f(dir_ / name, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamoff size = f.tellg();
  if (size <= 0 || size > (std::streamoff(64) << 20)) return false;
  bytecode.resize(size_t(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(bytecode.data()), size);
  if (!f) {
    bytecode.clear();
    return false;
  }
  PackAppend(name, bytecode);  // an existing cache moves into the pack as it is used
  return true;
}

namespace {
constexpr char kPackMagic[8] = {'G', 'N', 'S', 'H', 'P', 'A', 'C', 'K'};
constexpr uint32_t kPackVersion = 1;
constexpr uint32_t kRecordMagic = 0x43455253u;  // "SREC"
constexpr uint64_t kPackHeaderSize = 16;
struct PackRecordHeader {
  uint32_t magic, name_size, data_size, reserved;
  uint64_t checksum;
};
static_assert(sizeof(PackRecordHeader) == 24, "pack record header layout");
uint64_t PackChecksum(const std::string& name, const uint8_t* data, size_t size) {
  return Fnv1a64(data, size, Fnv1a64(name.data(), name.size()));
}
}  // namespace

void ShaderCache::SetPack(bool on) {
  std::lock_guard<std::mutex> lock(pack_mutex_);
  if (!on || !dir_ok_ || pack_on_) return;
  const auto t0 = std::chrono::steady_clock::now();
  const std::filesystem::path path = dir_ / "shaders.pack";
  std::error_code ec;
  const uint64_t size = std::filesystem::exists(path, ec) ? uint64_t(std::filesystem::file_size(path, ec)) : 0;
  uint64_t good = 0;
  if (size) {
    // Index pass: record headers and names only; each bytecode is checked against its checksum when it is read.
    std::ifstream in(path, std::ios::binary);
    char header[kPackHeaderSize] = {};
    in.read(header, sizeof(header));
    uint32_t version = 0;
    std::memcpy(&version, header + 8, 4);
    if (!in || std::memcmp(header, kPackMagic, 8) != 0 || version != kPackVersion) {
      REXLOG_WARN("gow2_renderer: {} is not a shader pack this build reads; shader pack off (loose files only)",
                  rex::path_to_utf8(path));
      return;
    }
    good = kPackHeaderSize;
    std::string name;
    while (good + sizeof(PackRecordHeader) <= size) {
      PackRecordHeader h{};
      in.seekg(std::streamoff(good));
      in.read(reinterpret_cast<char*>(&h), sizeof(h));
      if (!in || h.magic != kRecordMagic || !h.name_size || h.name_size > 255 || !h.data_size ||
          h.data_size > (64u << 20))
        break;
      const uint64_t end = good + sizeof(h) + h.name_size + h.data_size;
      if (end > size) break;
      name.assign(h.name_size, '\0');
      in.read(name.data(), h.name_size);
      if (!in) break;
      pack_index_[name] = PackEntry{good + sizeof(h) + h.name_size, h.data_size, h.checksum};
      good = end;
    }
  }
  if (!size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    char header[kPackHeaderSize] = {};
    std::memcpy(header, kPackMagic, 8);
    std::memcpy(header + 8, &kPackVersion, 4);
    out.write(header, sizeof(header));
    if (!out) {
      REXLOG_WARN("gow2_renderer: can't create {}; shader pack off (loose files only)", rex::path_to_utf8(path));
      return;
    }
    good = kPackHeaderSize;
  } else if (good < size) {
    // A torn last record (the game was closed while it was written): cut it off; the shader is compiled again.
    std::filesystem::resize_file(path, good, ec);
    REXLOG_WARN("gow2_renderer: shader pack: cut a torn record at byte {} of {} ({})", good, size,
                ec ? ec.message() : std::string("ok"));
    if (ec) return;
  }
  pack_.open(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!pack_) {
    REXLOG_WARN("gow2_renderer: can't open {}; shader pack off (loose files only)", rex::path_to_utf8(path));
    pack_index_.clear();
    return;
  }
  pack_end_ = good;
  pack_on_ = true;
  REXLOG_INFO("gow2_renderer: shader pack {}: {} shaders, {:.1f} MB, indexed in {:.0f} ms", rex::path_to_utf8(path),
              pack_index_.size(), double(good) / 1048576.0, MsSince(t0));
}

bool ShaderCache::PackFind(const std::string& name, std::vector<uint8_t>& bytecode) {
  if (!pack_on_) return false;
  std::lock_guard<std::mutex> lock(pack_mutex_);
  auto it = pack_index_.find(name);
  if (it == pack_index_.end()) return false;
  bytecode.resize(it->second.size);
  pack_.clear();
  pack_.seekg(std::streamoff(it->second.offset));
  pack_.read(reinterpret_cast<char*>(bytecode.data()), std::streamsize(bytecode.size()));
  if (!pack_ || PackChecksum(name, bytecode.data(), bytecode.size()) != it->second.checksum) {
    pack_.clear();
    bytecode.clear();
    pack_index_.erase(it);  // read the loose file (or compile) instead; a good copy is appended again
    ++pack_bad_;
    return false;
  }
  return true;
}

void ShaderCache::PackAppend(const std::string& name, const std::vector<uint8_t>& bytecode) {
  if (!pack_on_ || bytecode.empty() || name.empty() || name.size() > 255) return;
  std::lock_guard<std::mutex> lock(pack_mutex_);
  if (pack_index_.count(name)) return;
  PackRecordHeader h{kRecordMagic, uint32_t(name.size()), uint32_t(bytecode.size()), 0,
                     PackChecksum(name, bytecode.data(), bytecode.size())};
  pack_.clear();
  pack_.seekp(std::streamoff(pack_end_));
  pack_.write(reinterpret_cast<const char*>(&h), sizeof(h));
  pack_.write(name.data(), std::streamsize(name.size()));
  pack_.write(reinterpret_cast<const char*>(bytecode.data()), std::streamsize(bytecode.size()));
  pack_.flush();
  if (!pack_) {
    pack_.clear();  // not indexed; the next record overwrites from the same place
    return;
  }
  pack_index_[name] = PackEntry{pack_end_ + sizeof(h) + name.size(), uint32_t(bytecode.size()), h.checksum};
  pack_end_ += sizeof(h) + name.size() + bytecode.size();
  ++pack_added_;
}

ShaderCache::PackInfo ShaderCache::GetPackInfo() {
  std::lock_guard<std::mutex> lock(pack_mutex_);
  PackInfo r;
  r.open = pack_on_;
  r.shaders = pack_index_.size();
  r.bytes = pack_end_;
  r.added = pack_added_;
  r.bad = pack_bad_;
  return r;
}

void ShaderCache::SaveToDisk(const std::string& name, const std::vector<uint8_t>& bytecode, const std::string& hlsl,
                             const std::string& messages, bool ok) {
  if (!dir_ok_) return;
  if (!ok) {
    // Keep the failing source next to the cache for diagnosis (game-derived: out\ is never committed).
    std::ofstream(dir_ / (name + ".failed.hlsl"), std::ios::binary) << hlsl << "\n/*\n" << messages << "\n*/\n";
    return;
  }
  if (pack_on_) {  // patch 0015: the pack only (no new loose file to open)
    PackAppend(name, bytecode);
    return;
  }
  // Unique temporary name per thread, then rename: a half-written file is never read.
  const std::filesystem::path tmp =
      dir_ / (name + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp");
  bool written = false;
  {
    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytecode.data()), std::streamsize(bytecode.size()));
    f.flush();
    written = f.good();
  }
  std::error_code ec;
  // 0.6.1 (second review S1): a full disk left a cut-off file under the final name, loaded on every later run
  if (!written) { std::filesystem::remove(tmp, ec); return; }
  std::filesystem::rename(tmp, dir_ / name, ec);
  if (ec) std::filesystem::remove(tmp, ec);
}

void ShaderCache::Worker(ShaderCompiler* compiler) {
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty() || !warm_queue_.empty(); });
      if (stop_) return;
      // Live misses (what is on screen now) first, then the start-up warm-up backlog.
      auto& q = queue_.empty() ? warm_queue_ : queue_;
      job = std::move(q.front());
      q.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    job->ok = compiler->Compile(job->hlsl, job->stage, backend_.GetShaderIL(), job->bytecode, job->messages);
    job->ms = MsSince(t0);
    SaveToDisk(job->name, job->bytecode, job->hlsl, job->messages, job->ok);
    job->hlsl.clear();
    job->hlsl.shrink_to_fit();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_.push_back(std::move(job));
    }
    done_cv_.notify_all();
  }
}

void ShaderCache::FinishJob(std::unique_ptr<Job> job) {
  --in_flight_;
  if (job->spirv_job) {  // 0.6.2: a conversion, not a compile - its own counters
    ++spv_jobs_done_;
    spv_jobs_ms_ += job->ms;
    if (!job->ok) ++spv_jobs_failed_;
    if ((spv_jobs_done_ & (spv_jobs_done_ - 1)) == 0 || in_flight_ == 0)
      REXLOG_INFO("gow2_renderer: SPIR-V jobs: {} done ({} failed), {:.0f} ms of conversion on the background threads, {} still to do",
                  spv_jobs_done_, spv_jobs_failed_, spv_jobs_ms_, in_flight_);
  } else {
    stats_.compile_ms += job->ms;
  }
  GuestShader& s = *job->shader;
  if (job->ok && !job->bytecode.empty()) {
    if (!job->spirv_job) ++stats_.compiled;
    if (!job->messages.empty() && stats_.compiled <= 3)
      REXLOG_INFO("gow2_renderer: compiler messages for {}: {}", job->name, job->messages.substr(0, 400));
    s.handle = backend_.CreateShader(job->stage, job->bytecode.data(), job->bytecode.size());
    s.state = s.handle ? ShaderState::kReady : ShaderState::kFailed;
    if (!s.handle) s.error = "backend rejected the bytecode";
  } else {
    s.state = ShaderState::kFailed;
    s.error = job->messages.substr(0, job->messages.find('\n'));
  }
  if (s.state == ShaderState::kFailed) {
    ++stats_.errors;
    REXLOG_WARN("gow2_renderer: SHADER-FAIL {}: {}", job->name, s.error);
  }
}

bool ShaderCache::WaitReady(const GuestShader& s, double budget_ms) {
  if (s.state != ShaderState::kPending) return true;
  if (budget_ms <= 0.0) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(budget_ms);
  // Still queued: compile it here (the caller's own compiler) instead of waiting for a free worker.
  std::unique_ptr<Job> mine;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* q : {&queue_, &warm_queue_}) {
      for (auto it = q->begin(); it != q->end(); ++it)
        if ((*it)->shader == &s) {
          mine = std::move(*it);
          q->erase(it);
          break;
        }
      if (mine) break;
    }
    if (!mine) {  // 0.6.2: a SPIR-V job is never converted here (45 ms and more against these budgets) - it goes first
      for (auto it = spv_warm_queue_.begin(); it != spv_warm_queue_.end(); ++it)
        if ((*it)->shader == &s) {
          spv_queue_.push_front(std::move(*it));
          spv_warm_queue_.erase(it);
          spv_wake_.notify_one();
          break;
        }
    }
  }
  if (mine) {
    if (sync_compiler_) {
      const auto t0 = std::chrono::steady_clock::now();
      mine->ok = sync_compiler_->Compile(mine->hlsl, mine->stage, backend_.GetShaderIL(), mine->bytecode, mine->messages);
      mine->ms = MsSince(t0);
      SaveToDisk(mine->name, mine->bytecode, mine->hlsl, mine->messages, mine->ok);
      FinishJob(std::move(mine));
      return s.state != ShaderState::kPending;
    }
    std::lock_guard<std::mutex> lock(mutex_);  // no compiler of our own: give it back to the workers
    queue_.push_front(std::move(mine));
    wake_.notify_one();
  }
  // On a worker (or handed back): wait until it is done, within the budget.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait_until(lock, deadline, [&] {
      for (const auto& j : done_)
        if (j->shader == &s) return true;
      return false;
    });
  }
  {  // its own job first (Poll has a time budget)
    std::unique_ptr<Job> own;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = done_.begin(); it != done_.end(); ++it)
        if ((*it)->shader == &s) {
          own = std::move(*it);
          done_.erase(it);
          break;
        }
    }
    if (own) FinishJob(std::move(own));
  }
  Poll();
  return s.state != ShaderState::kPending;
}

void ShaderCache::SaveUcode(uint64_t ucode_hash, bool is_vertex, const uint32_t* dwords, size_t dword_count) {
  if (!record_ucode_ || !dwords || !dword_count) return;
  char name[48];
  std::snprintf(name, sizeof(name), "%s-%016llX.ucode", is_vertex ? "vs" : "ps", static_cast<unsigned long long>(ucode_hash));
  // Patch 0014: queued for the writer thread (the file checks and writes no longer run on the caller's thread).
  {
    std::lock_guard<std::mutex> lock(ucode_mutex_);
    UcodeItem item;
    item.path = dir_ / "ucode" / name;
    item.dwords.assign(dwords, dwords + dword_count);
    item.hash = ucode_hash;
    item.is_vertex = is_vertex;
    ucode_queue_.push_back(std::move(item));
    if (!ucode_thread_.joinable()) ucode_thread_ = std::thread([this] { UcodeWriter(); });
  }
  ucode_cv_.notify_one();
}

// Patch 0085: ucode/recorded.pack - every recorded shader in one file, so the start-up warm-up reads one file instead
// of thousands (job 388: reading ~13,500 .ucode files took ~15 s even on 8 threads). Layout: "GNUCPAK1" (8 bytes),
// then records of {u64 ucode hash, u32 flags (bit 0 = vertex), u32 dword count, dwords}. Records are appended by the
// writer thread; a record cut short (the process ended mid-write) ends the valid part, and the writer cuts the file
// back to it before appending. The .ucode files stay as before (tools read them).
static constexpr char kUcodePackMagic[8] = {'G', 'N', 'U', 'C', 'P', 'A', 'K', '1'};
struct UcodePackRecord {
  uint64_t hash = 0;
  bool is_vertex = false;
  size_t offset = 0;  // of the dwords, in the buffer
  uint32_t count = 0;
};
// The records of a pack image; returns the bytes of whole records (0 = no valid header).
static size_t ParseUcodePack(const std::vector<char>& b, std::vector<UcodePackRecord>* out) {
  if (b.size() < 8 || std::memcmp(b.data(), kUcodePackMagic, 8) != 0) return 0;
  size_t at = 8;
  while (at + 16 <= b.size()) {
    UcodePackRecord r;
    uint32_t flags = 0;
    std::memcpy(&r.hash, b.data() + at, 8);
    std::memcpy(&flags, b.data() + at + 8, 4);
    std::memcpy(&r.count, b.data() + at + 12, 4);
    if (!r.count || r.count > (1u << 20) || (flags & ~1u) || at + 16 + size_t(r.count) * 4 > b.size()) break;
    r.is_vertex = (flags & 1u) != 0;
    r.offset = at + 16;
    if (out) out->push_back(r);
    at += 16 + size_t(r.count) * 4;
  }
  return at;
}
static bool ReadWholeFile(const std::filesystem::path& p, std::vector<char>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamoff n = f.tellg();
  if (n < 0 || n > (std::streamoff(1) << 31)) return false;
  out.resize(size_t(n));
  f.seekg(0);
  return n == 0 || bool(f.read(out.data(), n));
}

void ShaderCache::UcodeWriter() {
  std::error_code ec;
  bool dir_made = false;
  const std::filesystem::path pack_path = dir_ / "ucode" / "recorded.pack";
  std::ofstream pack;
  std::unordered_map<uint64_t, bool> in_pack;  // key: hash ^ (vertex << 63)
  bool pack_ready = false;
  for (;;) {
    UcodeItem item;
    {
      std::unique_lock<std::mutex> lock(ucode_mutex_);
      ucode_cv_.wait(lock, [this] { return ucode_stop_ || !ucode_queue_.empty(); });
      if (ucode_queue_.empty()) break;  // stop requested and nothing left
      item = std::move(ucode_queue_.front());
      ucode_queue_.pop_front();
    }
    const std::filesystem::path& path = item.path;
    if (!dir_made) {
      std::filesystem::create_directories(path.parent_path(), ec);
      dir_made = true;
    }
    if (item.file && !std::filesystem::exists(path, ec)) {
      std::filesystem::path tmp = path;
      tmp += ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(item.dwords.data()), std::streamsize(item.dwords.size() * sizeof(uint32_t)));
      }
      std::filesystem::rename(tmp, path, ec);
      if (ec) std::filesystem::remove(tmp, ec);
    }
    // The pack: opened once, after cutting it back to its whole records (or started anew).
    if (!pack_ready) {
      pack_ready = true;
      std::vector<char> b;
      std::vector<UcodePackRecord> recs;
      size_t valid = 0;
      const bool read_ok = ReadWholeFile(pack_path, b);
      if (read_ok) valid = ParseUcodePack(b, &recs);
      for (const auto& r : recs) in_pack[r.hash ^ (uint64_t(r.is_vertex) << 63)] = true;
      // Cut or restart the pack only when it was read and found bad (or is not there at all); a pack that exists but
      // could not be read (locked, I/O error) is left alone and not recorded to this run.
      const bool unreadable = !read_ok && std::filesystem::exists(pack_path, ec);
      if (unreadable) {
        REXLOG_WARN("gow2_renderer: can't read {}; left as it is, not recorded to this run", rex::path_to_utf8(pack_path));
      } else if (valid == 0) {
        std::ofstream f(pack_path, std::ios::binary | std::ios::trunc);
        f.write(kUcodePackMagic, 8);
      } else if (valid < b.size()) {
        std::filesystem::resize_file(pack_path, valid, ec);
      }
      if (!unreadable) pack.open(pack_path, std::ios::binary | std::ios::app);
      else pack.setstate(std::ios::failbit);  // "!pack" below: nothing written
    }
    const uint64_t key = item.hash ^ (uint64_t(item.is_vertex) << 63);
    if (!pack || in_pack.count(key) || item.dwords.empty()) continue;
    in_pack[key] = true;
    const uint32_t flags = item.is_vertex ? 1u : 0u, count = uint32_t(item.dwords.size());
    pack.write(reinterpret_cast<const char*>(&item.hash), 8);
    pack.write(reinterpret_cast<const char*>(&flags), 4);
    pack.write(reinterpret_cast<const char*>(&count), 4);
    pack.write(reinterpret_cast<const char*>(item.dwords.data()), std::streamsize(size_t(count) * 4));
    pack.flush();  // whole records on disk as early as possible (the harness ends the process hard)
  }
}

// What the renderer reads from a translation (the same fields for both back ends).
static void ApplyMeta(GuestShader& s, const hlsl::Translation& tr) {
  s.texture_slots |= tr.texture_slots_2d | tr.texture_slots_3d | tr.texture_slots_cube;
  s.texture_slots_2d = tr.texture_slots_2d;
  s.texture_slots_cube = tr.texture_slots_cube;
  s.texture_slots_3d = tr.texture_slots_3d;
  s.float_constants = std::max<uint32_t>(tr.float_constants, 1);
  s.float_constants_low = tr.float_constants_low == tr.float_constants_high ? s.float_constants : tr.float_constants_low;
  s.float_constants_high = tr.float_constants_low == tr.float_constants_high ? s.float_constants : tr.float_constants_high;
  for (int i = 0; i < 4; ++i) s.float_constants_read[i] = tr.float_constants_read[i];
  s.float_constants_relative = tr.float_constants_relative;
  s.memexport_stream_constants = tr.memexport_stream_constants;
}
// Patch 0099 (Vulkan, measurement): our SPIR-V written to / replaced from files named by the hash of its words
// (<hash>.spv dumped; <hash>.opt.spv read in its place), so an offline-optimised copy can be timed in game.
static std::string g_spv_dump_dir, g_spv_override_dir;
static std::atomic<uint64_t> g_spv_dumped{0}, g_spv_overridden{0}, g_spv_seen{0};
void ShaderCache::SetSpirvFiles(const std::string& dump_dir, const std::string& override_dir) {
  g_spv_dump_dir = dump_dir;
  g_spv_override_dir = override_dir;
  std::error_code ec;
  if (!dump_dir.empty()) std::filesystem::create_directories(dump_dir, ec);
  if (!dump_dir.empty() || !override_dir.empty())
    REXLOG_INFO("gow2_renderer: SPIR-V files (patch 0099): dump to '{}', replace from '{}'", dump_dir, override_dir);
}
static void SpirvFiles(std::vector<uint32_t>& words) {
  if (g_spv_dump_dir.empty() && g_spv_override_dir.empty()) return;
  char name[24];
  std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(Fnv1a64(words.data(), words.size() * 4,
                                                                                       14695981039346656037ull)));
  const uint64_t seen = ++g_spv_seen;
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!g_spv_dump_dir.empty()) {
    const fs::path p = fs::path(g_spv_dump_dir) / (std::string(name) + ".spv");
    if (!fs::exists(p, ec)) {
      const fs::path tmp = fs::path(g_spv_dump_dir) / (std::string(name) + ".tmp");
      { std::ofstream o(tmp, std::ios::binary); o.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4)); }
      fs::rename(tmp, p, ec);
      ++g_spv_dumped;
    }
  }
  if (!g_spv_override_dir.empty()) {
    const fs::path p = fs::path(g_spv_override_dir) / (std::string(name) + ".opt.spv");
    if (fs::exists(p, ec)) {
      std::ifstream i(p, std::ios::binary);
      std::vector<char> b((std::istreambuf_iterator<char>(i)), {});
      if (b.size() >= 20 && b.size() % 4 == 0 && *reinterpret_cast<const uint32_t*>(b.data()) == 0x07230203u) {
        words.resize(b.size() / 4);
        std::memcpy(words.data(), b.data(), b.size());
        ++g_spv_overridden;
      }
    }
  }
  if ((seen & (seen - 1)) == 0)
    REXLOG_INFO("gow2_renderer: SPIR-V files (patch 0099): {} converted, {} dumped, {} replaced by an optimised copy",
                seen, g_spv_dumped.load(), g_spv_overridden.load());
}
// Patch 0084: DXBC, and on Vulkan its SPIR-V (dxbc_spirv.cpp) in place of it - a pure function of the program and the
// options, so the warm-up runs it on several threads.
// 0.6.1 (Vulkan first-run cost; gn_pipebench jobs 1736 / 1737 on an RTX 3080 Ti): dxbc_spirv writes every register as
// a variable and every instruction as load / all four lanes / masked store - 357 KB of SPIR-V a pipeline against 67 KB
// of DXBC, and the driver's build took 303 ms against 158 ms on D3D12. SPIRV-Tools' performance passes (the variables
// become SSA values, dead lanes go) leave 129 KB and 193 ms. The optimiser keeps NoContraction and the float controls,
// so values are the same; a module it cannot take is used as converted.
static std::atomic<bool> g_spv_optimise{false};
static std::atomic<uint64_t> g_spv_opt_done{0}, g_spv_opt_failed{0}, g_spv_opt_in{0}, g_spv_opt_out{0};
bool ShaderCache::SetSpirvOptimise(bool on) {
#ifdef GN_SPIRV_OPT
  g_spv_optimise.store(on);
  return true;
#else
  (void)on;
  return false;
#endif
}
bool ShaderCache::OptimiseSpirvModule(std::vector<uint32_t>& words) {
#ifdef GN_SPIRV_OPT
  if (words.empty()) return false;
  spvtools::Optimizer opt(SPV_ENV_VULKAN_1_3);  // (SPIR-V 1.6, what dxbc_spirv writes)
  opt.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
  opt.RegisterPerformancePasses();
  spvtools::OptimizerOptions oo;
  oo.set_run_validator(false);  // the converter's output is valid by construction (spirv-val: tests and job 1737)
  std::vector<uint32_t> out;
  if (!opt.Run(words.data(), words.size(), &out, oo) || out.size() < 5) {
    ++g_spv_opt_failed;
    return false;
  }
  g_spv_opt_in += words.size() * 4;
  g_spv_opt_out += out.size() * 4;
  const uint64_t n = ++g_spv_opt_done;
  words.swap(out);
  if ((n & (n - 1)) == 0)
    REXLOG_INFO("gow2_renderer: SPIR-V optimiser: {} modules, {:.1f} MB -> {:.1f} MB, {} left as converted", n,
                double(g_spv_opt_in.load()) / 1048576.0, double(g_spv_opt_out.load()) / 1048576.0, g_spv_opt_failed.load());
  return true;
#else
  (void)words;
  return false;
#endif
}
static void OptimiseSpirv(std::vector<uint32_t>& words) {
  if (g_spv_optimise.load()) ShaderCache::OptimiseSpirvModule(words);
}
// 0.6.1: the SPIR-V store. One append-only file: "GNSPVST1", then records { u64 key, u32 words, u32 sum } + the words.
// Key = FNV-1a 64 of the DXBC container (so every translator option and salt is in it) mixed with the converter
// version, the precise flag and the renderer's switches. Only an index is held in memory; a record is read when asked
// for. A torn tail (crash while writing) is cut off when the file is opened. A record whose sum is wrong is not used.
namespace fs = std::filesystem;
namespace {
struct SpvStore {
  std::mutex mutex;
  fs::path path;
  uint32_t switches = 0;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> index;  // key -> (offset of the words, count)
  uint64_t end = 0;
  std::atomic<uint64_t> hits{0}, misses{0}, stored{0};
};
SpvStore g_spv_store;
constexpr char kSpvMagic[8] = {'G', 'N', 'S', 'P', 'V', 'S', 'T', '1'};
uint32_t SpvSum(const std::vector<uint32_t>& w) {
  uint32_t s = 0x9E3779B9u;
  for (uint32_t x : w) s = (s << 5 | s >> 27) ^ x;
  return s;
}
uint64_t SpvKey(const uint8_t* dxbc, size_t n, bool precise, uint32_t switches) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
  for (size_t i = 0; i < n; ++i) mix(dxbc[i]);
  // (kSpvPasses: bump when the optimiser's pass list or the vendored SPIRV-Tools version changes)
  constexpr uint32_t kSpvPasses = 1;
  const uint32_t tail[4] = {dxbc_spirv::kConverterVersion, precise ? 1u : 0u, switches, kSpvPasses};
  for (uint32_t t : tail)
    for (int i = 0; i < 4; ++i) mix(uint8_t(t >> (8 * i)));
  return h;
}
bool SpvStoreGet(uint64_t key, std::vector<uint32_t>& words) {
  std::pair<uint64_t, uint32_t> at;
  fs::path path;  // review: a copy - a failed write on another thread clears the store's own
  {
    std::lock_guard<std::mutex> lock(g_spv_store.mutex);
    if (g_spv_store.path.empty()) return false;
    path = g_spv_store.path;
    auto it = g_spv_store.index.find(key);
    if (it == g_spv_store.index.end()) {
      ++g_spv_store.misses;
      return false;
    }
    at = it->second;
  }
  std::ifstream f(path, std::ios::binary);
  std::vector<uint32_t> w(at.second);
  uint32_t sum = 0;
  if (!f || !f.seekg(std::streamoff(at.first - 4)) || !f.read(reinterpret_cast<char*>(&sum), 4) ||
      !f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size()) * 4) || SpvSum(w) != sum) {
    // review: a damaged record leaves the index, so the module converted now is stored again (a new record)
    std::lock_guard<std::mutex> lock(g_spv_store.mutex);
    auto it = g_spv_store.index.find(key);
    if (it != g_spv_store.index.end() && it->second == at) g_spv_store.index.erase(it);
    ++g_spv_store.misses;
    return false;
  }
  words.swap(w);
  ++g_spv_store.hits;
  return true;
}
void SpvStorePut(uint64_t key, const std::vector<uint32_t>& words) {
  std::lock_guard<std::mutex> lock(g_spv_store.mutex);
  if (g_spv_store.path.empty() || words.empty() || g_spv_store.index.count(key)) return;
  std::ofstream f(g_spv_store.path, std::ios::binary | std::ios::app);
  const uint32_t n = uint32_t(words.size()), sum = SpvSum(words);
  if (!f || !f.write(reinterpret_cast<const char*>(&key), 8) || !f.write(reinterpret_cast<const char*>(&n), 4) ||
      !f.write(reinterpret_cast<const char*>(&sum), 4) ||
      !f.write(reinterpret_cast<const char*>(words.data()), std::streamsize(n) * 4) || !f.flush()) {
    g_spv_store.path.clear();  // disk full or not writable: no store for the rest of this run
    return;
  }
  g_spv_store.index[key] = {g_spv_store.end + 16, n};
  g_spv_store.end += 16 + uint64_t(n) * 4;
  ++g_spv_store.stored;
}
}  // namespace
void ShaderCache::SetSpirvStore(const std::filesystem::path& dir, uint32_t switches) {
  std::lock_guard<std::mutex> lock(g_spv_store.mutex);
  g_spv_store.index.clear();
  g_spv_store.path.clear();
  g_spv_store.switches = switches;
  if (dir.empty()) return;
  const fs::path p = dir / "spirv-store.bin";
  std::error_code ec;
  uint64_t size = fs::exists(p, ec) ? uint64_t(fs::file_size(p, ec)) : 0;
  uint64_t good = 0;
  if (size >= 8) {
    std::ifstream f(p, std::ios::binary);
    char magic[8] = {};
    if (f.read(magic, 8) && std::memcmp(magic, kSpvMagic, 8) == 0) {
      good = 8;
      for (;;) {
        uint64_t key = 0;
        uint32_t n = 0, sum = 0;
        if (!f.read(reinterpret_cast<char*>(&key), 8) || !f.read(reinterpret_cast<char*>(&n), 4) ||
            !f.read(reinterpret_cast<char*>(&sum), 4))
          break;
        const uint64_t next = good + 16 + uint64_t(n) * 4;
        if (n < 5 || n > (64u << 20) || next > size) break;
        g_spv_store.index[key] = {good + 16, n};
        good = next;
        if (!f.seekg(std::streamoff(good))) break;
      }
    }
  }
  if (good == 0) {  // new, or not ours: start a fresh file (an unreadable old one is kept beside it)
    if (size) fs::rename(p, dir / "spirv-store.unreadable.bin", ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f.write(kSpvMagic, 8)) return;
    good = 8;
  } else if (good < size) {
    fs::resize_file(p, good, ec);  // a torn last record
    if (ec) return;
  }
  g_spv_store.end = good;
  g_spv_store.path = p;
  REXLOG_INFO("gow2_renderer: SPIR-V store {}: {} modules, {:.1f} MB", p.string(), g_spv_store.index.size(), double(good) / 1048576.0);
}
static std::atomic<bool> g_spv_async{false};
void ShaderCache::SetSpirvAsync(bool on) { g_spv_async.store(on); }
// (spv_pending, 0.6.2: given and async on -> a store miss is NOT converted here; the bytecode stays DXBC and
// *spv_pending is set, for AdoptDxbc to queue the conversion. Null = convert here, as always.)
static dxbc_gen::DxbcTranslation TranslateDxbcFor(const ucode::Program& program, const hlsl::HlslOptions& options,
                                                  bool spirv, bool* spv_pending = nullptr) {
  dxbc_gen::DxbcTranslation dt = dxbc_gen::TranslateToDxbc(program, options);
  if (dt.ok && spirv) {
    const uint64_t store_key = SpvKey(dt.bytecode.data(), dt.bytecode.size(), options.spirv_precise, g_spv_store.switches);
    dxbc_spirv::SpirvResult sv;
    if (spv_pending && g_spv_async.load() && g_spv_dump_dir.empty() && g_spv_override_dir.empty()) {
      if (SpvStoreGet(store_key, sv.words)) {
        dt.bytecode.resize(sv.words.size() * 4);
        std::memcpy(dt.bytecode.data(), sv.words.data(), dt.bytecode.size());
      } else {
        *spv_pending = true;  // DXBC kept; SpvWorker converts it
      }
      return dt;
    }
    // Review: with patch 0099's dump / replace folders set (a test tool) the store is left out, so those files see
    // the words as converted - their names are hashes of them - exactly as in 0.6.0; the optimiser runs after them.
    // (A store hit skips the conversion, so patch 0102's discard counts only cover shaders converted in this run.)
    const bool files_tool = !g_spv_dump_dir.empty() || !g_spv_override_dir.empty();
    const bool stored = !files_tool && SpvStoreGet(store_key, sv.words);  // 0.6.1: converted in an earlier run
    if (stored) sv.ok = true;
    else sv = dxbc_spirv::ConvertDxbcToSpirv(dt.bytecode.data(), dt.bytecode.size(), options.spirv_precise);
    if (sv.ok) {
      if (!stored) {
        SpirvFiles(sv.words);  // patch 0099
        OptimiseSpirv(sv.words);  // 0.6.1
        if (!files_tool) SpvStorePut(store_key, sv.words);
      }
      dt.bytecode.resize(sv.words.size() * 4);
      std::memcpy(dt.bytecode.data(), sv.words.data(), dt.bytecode.size());
    } else {
      dt.ok = false;
      dt.unsupported = "SPIR-V conversion: " + sv.error;
    }
  }
  return dt;
}

// 0.6.2 (first playthrough, Vulkan): the conversion off the render thread. Live jobs (a draw is waiting) first.
void ShaderCache::SpvWorker() {
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      spv_wake_.wait(lock, [this] { return stop_ || !spv_queue_.empty() || !spv_warm_queue_.empty(); });
      if (stop_) return;
      auto& q = spv_queue_.empty() ? spv_warm_queue_ : spv_queue_;
      job = std::move(q.front());
      q.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t store_key = SpvKey(job->bytecode.data(), job->bytecode.size(), job->spirv_precise, g_spv_store.switches);
    dxbc_spirv::SpirvResult sv;
    if (SpvStoreGet(store_key, sv.words)) {  // the same DXBC was converted for another shader meanwhile
      sv.ok = true;
    } else {
      sv = dxbc_spirv::ConvertDxbcToSpirv(job->bytecode.data(), job->bytecode.size(), job->spirv_precise);
      if (sv.ok) {
        OptimiseSpirv(sv.words);
        SpvStorePut(store_key, sv.words);
      }
    }
    job->ok = sv.ok && !sv.words.empty();
    if (job->ok) {
      job->bytecode.resize(sv.words.size() * 4);
      std::memcpy(job->bytecode.data(), sv.words.data(), job->bytecode.size());
    } else {
      job->bytecode.clear();
      job->messages = "DXBC -> SPIR-V conversion failed";
    }
    job->ms = MsSince(t0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_.push_back(std::move(job));
    }
    done_cv_.notify_all();
  }
}
void ShaderCache::QueueSpirv(GuestShader& s, const std::vector<uint8_t>& dxbc, bool precise, bool warm) {
  auto job = std::make_unique<Job>();
  job->shader = &s;
  char nm[64];
  std::snprintf(nm, sizeof(nm), "%s-%016llX (DXBC -> SPIR-V)", s.is_vertex ? "vs" : "ps", static_cast<unsigned long long>(s.ucode_hash));
  job->name = nm;
  job->stage = s.is_vertex ? gpu::ShaderStage::kVertex : gpu::ShaderStage::kPixel;
  job->bytecode = dxbc;
  job->spirv_job = true;
  job->spirv_precise = precise;
  ++in_flight_;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    (warm ? spv_warm_queue_ : spv_queue_).push_back(std::move(job));
    if (spv_workers_.empty()) {
      const uint32_t hw = std::thread::hardware_concurrency();
      const uint32_t n = hw > 3 ? hw - 2 : 1;
      for (uint32_t i = 0; i < n; ++i) spv_workers_.emplace_back([this] { SpvWorker(); });
      REXLOG_INFO("gow2_renderer: SPIR-V conversion on {} background thread(s)", n);
    }
  }
  spv_wake_.notify_one();
}
void ShaderCache::Promote(const GuestShader& s) {
  if (s.state != ShaderState::kPending || s.spv_promoted) return;
  s.spv_promoted = true;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = spv_warm_queue_.begin(); it != spv_warm_queue_.end(); ++it) {
    if ((*it)->shader != &s) continue;
    spv_queue_.push_front(std::move(*it));
    spv_warm_queue_.erase(it);
    break;
  }
}
size_t ShaderCache::SpirvJobsQueued() {
  std::lock_guard<std::mutex> lock(mutex_);
  return spv_queue_.size() + spv_warm_queue_.size();
}

const GuestShader& ShaderCache::AdoptDxbc(uint64_t key, std::unique_ptr<GuestShader> s,
                                          const std::vector<uint8_t>& bytecode, const hlsl::Translation& meta,
                                          bool spv_pending, uint32_t spv_flags) {
  const bool spirv = backend_.GetShaderIL() == gpu::ShaderIL::kSpirv;
  const bool is_vertex = s->is_vertex;
  ApplyMeta(*s, meta);
  s->precise_positions = is_vertex;
  s->depth_remap_ready = !is_vertex;
  s->uses_textures = s->texture_slots != 0;
  s->simplifications = uint32_t(meta.simplifications.size());
  s->dxbc = !spirv;  // DXBC bytecode (D3D12; picks the DXBC rectangle-list GS); on Vulkan the module is SPIR-V
  ++stats_.dxbc;
  GuestShader& ref = *s;
  if (spv_pending) {  // 0.6.2: everything the renderer needs to know is set; the module comes from SpvWorker (Poll)
    ref.state = ShaderState::kPending;
    shaders_[is_vertex ? 1 : 0].emplace(key, std::move(s));
    QueueSpirv(ref, bytecode, (spv_flags & 1u) != 0, (spv_flags & 2u) != 0);
    return ref;
  }
  GuestShader& ref = *s;
  ref.handle = backend_.CreateShader(is_vertex ? gpu::ShaderStage::kVertex : gpu::ShaderStage::kPixel, bytecode.data(),
                                     bytecode.size());
  ref.state = ref.handle ? ShaderState::kReady : ShaderState::kFailed;
  if (!ref.handle) {
    ++stats_.errors;
    ref.error = spirv ? "CreateShader failed for the converted SPIR-V" : "CreateShader failed for the DXBC";
    REXLOG_WARN("gow2_renderer: SHADER-FAIL {}-{:016X} ({}): {}", is_vertex ? "vs" : "ps", ref.ucode_hash,
                spirv ? "DXBC -> SPIR-V" : "DXBC", ref.error);
  }
  shaders_[is_vertex ? 1 : 0].emplace(key, std::move(s));
  return ref;
}

ShaderCache::WarmResult ShaderCache::WarmFromDisk() {
  WarmResult r;
  if (!dir_ok_) return r;
  const auto t0 = std::chrono::steady_clock::now();
  std::error_code ec;
  const std::filesystem::path dir = dir_ / "ucode";
  if (!std::filesystem::is_directory(dir, ec)) return r;
  // Patch 0083: the files are listed first, read on several threads, then made one by one on this thread. Job 385:
  // 13,581 recorded shaders took 81 s to warm on D3D12 (91 s for 12,498 on Vulkan) while their translation took 2 s
  // in all - the time went on opening and reading thousands of small files one after another. The log line now
  // splits the time (listing, reading, making).
  struct Item {
    std::filesystem::path path;
    uint64_t hash = 0;
    bool is_vertex = false;
    std::vector<uint32_t> dwords;
  };
  std::vector<Item> items;
  // Patch 0085: the pack first - one read for every shader recorded into it.
  std::unordered_map<uint64_t, bool> packed;  // key: hash ^ (vertex << 63)
  {
    std::vector<char> b;
    std::vector<UcodePackRecord> recs;
    if (ReadWholeFile(dir / "recorded.pack", b)) ParseUcodePack(b, &recs);
    for (const auto& rec : recs) {
      const uint64_t key = rec.hash ^ (uint64_t(rec.is_vertex) << 63);
      if (packed.count(key)) continue;
      packed[key] = true;
      ++r.files;
      ++r.from_pack;
      if (shaders_[rec.is_vertex ? 1 : 0].count(rec.hash)) {
        ++r.known;
        continue;
      }
      Item it;
      it.hash = rec.hash;
      it.is_vertex = rec.is_vertex;
      it.dwords.resize(rec.count);
      std::memcpy(it.dwords.data(), b.data() + rec.offset, size_t(rec.count) * 4);
      items.push_back(std::move(it));
    }
  }
  const size_t first_file_item = items.size();
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    // <vs|ps>-<16 hex>.ucode
    if (n.size() != 25 || n.compare(19, 6, ".ucode") != 0 || n[2] != '-' || (n.compare(0, 2, "vs") && n.compare(0, 2, "ps")))
      continue;
    Item it;
    it.is_vertex = n[0] == 'v';
    try {
      it.hash = std::stoull(n.substr(3, 16), nullptr, 16);
    } catch (...) {
      continue;
    }
    if (packed.count(it.hash ^ (uint64_t(it.is_vertex) << 63))) continue;  // patch 0085: read from the pack
    ++r.files;  // patch 0086: counted once (job 389's log counted pack shaders twice)
    if (shaders_[it.is_vertex ? 1 : 0].count(it.hash)) {
      ++r.known;
      continue;
    }
    it.path = e.path();
    items.push_back(std::move(it));
  }
  r.list_ms = MsSince(t0);
  const auto t1 = std::chrono::steady_clock::now();
  {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t n = uint32_t(std::min<size_t>(std::min(8u, hw), std::max<size_t>(1, (items.size() - first_file_item) / 64)));
    std::atomic<size_t> next{first_file_item};
    auto reader = [&]() {
      for (size_t i; (i = next.fetch_add(1)) < items.size();) {
        Item& it = items[i];
        std::ifstream f(it.path, std::ios::binary | std::ios::ate);
        if (!f) continue;
        const std::streamoff size = f.tellg();
        if (size <= 0 || size % 4 || size > (64 << 20)) continue;
        it.dwords.resize(size_t(size) / 4);
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(it.dwords.data()), size)) it.dwords.clear();
      }
    };
    std::vector<std::thread> threads;
    for (uint32_t i = 1; i < n; ++i) threads.emplace_back(reader);
    reader();
    for (auto& t : threads) t.join();
  }
  r.read_ms = MsSince(t1);
  // Patch 0085: shaders found only as files go into the pack too, so the next start reads them from it.
  if (record_ucode_ && items.size() > first_file_item) {
    {
      std::lock_guard<std::mutex> lock(ucode_mutex_);
      for (size_t i = first_file_item; i < items.size(); ++i) {
        if (items[i].dwords.empty()) continue;
        UcodeItem u;
        u.path = items[i].path;
        u.dwords = items[i].dwords;
        u.hash = items[i].hash;
        u.is_vertex = items[i].is_vertex;
        u.file = false;
        ucode_queue_.push_back(std::move(u));
        ++r.packed_now;
      }
      if (r.packed_now && !ucode_thread_.joinable()) ucode_thread_ = std::thread([this] { UcodeWriter(); });
    }
    ucode_cv_.notify_one();
  }
  // Patch 0084: on the DXBC path (both APIs) decode, analysis and translation run on the reading threads' pool too;
  // only taking each result into the cache (the driver's module, the maps) stays on this thread, in file order. Job
  // 387: making 13,513 shaders took 21.9 s on Vulkan (1.2 s on D3D12) - the SPIR-V conversion and the modules.
  const bool spirv = backend_.GetShaderIL() == gpu::ShaderIL::kSpirv;
  const bool dxbc_path = dxbc_ && (backend_.GetShaderIL() == gpu::ShaderIL::kDxil || spirv);
  struct Made {
    std::unique_ptr<GuestShader> s;
    dxbc_gen::DxbcTranslation dt;
    double ms = 0.0;
    uint32_t spv_flags = 0;  // 0.6.2: bit 2 = SPIR-V still to be made (async), bit 0 precise, bit 1 warm
  };
  std::vector<Made> made(dxbc_path ? items.size() : 0);
  const auto t2 = std::chrono::steady_clock::now();
  if (dxbc_path) {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t n = uint32_t(std::min<size_t>(std::max(1u, hw > 2 ? hw - 2 : 1u), std::max<size_t>(1, items.size() / 32)));
    const ucode::DecodeOptions decode_options = DecodeOptions();
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (size_t i; (i = next.fetch_add(1)) < items.size();) {
        const Item& it = items[i];
        if (it.dwords.empty()) continue;
        if (!HlslOverride(it.hash, it.is_vertex).empty()) continue;  // built on first use, from its replacement
        if (!it.is_vertex && PcssOn() && PcssShader(it.hash)) continue;  // built on first use, with the PCSS patch
        const auto tm = std::chrono::steady_clock::now();
        auto s = std::make_unique<GuestShader>();
        s->ucode_hash = it.hash;
        s->is_vertex = it.is_vertex;
        s->drawn = true;  // patch 0045: recorded in an earlier session = drawn before
        const ucode::Program program = ucode::Decode(it.dwords.data(), it.dwords.size(), it.is_vertex, decode_options);
        Analyse(program, *s);
        const hlsl::HlslOptions wo = TranslateOptions(it.is_vertex, 0, it.dwords.data(), it.dwords.size());
        bool pending = false;
        made[i].dt = TranslateDxbcFor(program, wo, spirv, &pending);
        made[i].spv_flags = (pending ? 4u : 0u) | (wo.spirv_precise ? 1u : 0u) | 2u;
        made[i].s = std::move(s);
        made[i].ms = MsSince(tm);
      }
    };
    std::vector<std::thread> threads;
    for (uint32_t i = 1; i < n; ++i) threads.emplace_back(worker);
    worker();
    for (auto& t : threads) t.join();
  }
  r.translate_ms = MsSince(t2);
  const auto t3 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < items.size(); ++i) {
    Item& it = items[i];
    if (it.dwords.empty()) continue;
    if (dxbc_path && made[i].s && made[i].dt.ok && !shaders_[it.is_vertex ? 1 : 0].count(it.hash)) {
      ++stats_.lookups;
      ++stats_.translated;
      stats_.translate_ms += made[i].ms;
      const GuestShader& s = AdoptDxbc(it.hash, std::move(made[i].s), made[i].dt.bytecode, made[i].dt.meta,
                                       (made[i].spv_flags & 4u) != 0, made[i].spv_flags & 3u);
      if (s.state == ShaderState::kFailed) ++r.failed;
      made[i] = Made{};
    } else {  // not translatable here (the HLSL path logs and queues it) or not on the DXBC path: as before
      const uint64_t hits = stats_.disk_hits, queued = stats_.queued;
      const GuestShader& s = GetImpl(it.hash, it.dwords.data(), it.dwords.size(), it.is_vertex, 0, true);
      if (stats_.disk_hits != hits) ++r.disk_hits;
      else if (stats_.queued != queued) ++r.queued;
      if (s.state == ShaderState::kFailed) ++r.failed;
    }
    std::vector<uint32_t>().swap(it.dwords);
  }
  r.make_ms = MsSince(t3);
  r.ms = MsSince(t0);
  return r;
}

void ShaderCache::Poll() {
  if (!in_flight_) return;  // job 175: nothing compiling (every steady-state frame) - no lock per draw
  // 0.6.2: at most ~2 ms of finishing a call (each one creates a back-end shader); thousands of SPIR-V jobs can land
  // together after a start. The rest stays in done_ for the next call (a few thousand a second at 100 fps).
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (done_.empty()) return;
      job = std::move(done_.front());
      done_.pop_front();
    }
    FinishJob(std::move(job));
    if (MsSince(t0) > 2.0) return;
  }
}

// Patch 0068: one place for the options, so a prebuild translation (TranslateDxbcOnly) makes the same bytes as GetImpl.
ucode::DecodeOptions ShaderCache::DecodeOptions() const {
  ucode::DecodeOptions decode_options;
  // Patch 0041: only with --gow2_renderer_canary_decode (off by default after the 0037 regression report).
  decode_options.canary_scalar_operand_z = canary_decode_;
  decode_options.canary_tfetch1d_two_components = canary_decode_;
  return decode_options;
}
hlsl::HlslOptions ShaderCache::TranslateOptions(bool is_vertex, uint32_t tess_quad_segments,
                                                const uint32_t* dwords, size_t dword_count) const {
  hlsl::HlslOptions options;
  // A simple textured shader may also draw scene effects. The UI clamp is
  // selected by GetNativeHdr only while replaying the identified post-tonemap
  // Canvas draws; never clamp all uses of a shader by its hash alone.
  if (is_vertex) options.tess_quad_segments = tess_quad_segments;
  options.compact_float_constants = compact_float_constants_;
  options.tex_sign_by_swizzle = tex_sign_by_swizzle_;  // patch 0024
  options.canary_r21 = canary_r21_;  // patch 0041
  options.canary_tfetch1d_xy = canary_decode_;          // patch 0087: the decode's two tfetch1D coordinates, used
  options.mulsc_round_toward_zero = mulsc_toward_zero_;  // patch 0087
  options.memexport_f16_nan_zero = f16_nan_zero_;        // patch 0087
  options.spirv_precise = spirv_precise_;                // patch 0088
  options.ps_refactoring_allowed = ps_refactoring_;       // patch 0095
  options.tex_num_format = tex_num_format_;  // patch 0040
  options.tex_post_constants = tex_post_constants_;  // patch 0105
  options.tex_post_affine = tex_post_affine_;        // job 634
  options.dxbc_mul_min_abs = mul_min_abs_;           // job 634
  options.dxbc_structured_jumps = lean_.structured_jumps;  // job 636
  options.dxbc_mul_movc = lean_.mul_movc;
  options.dxbc_short_r21 = lean_.short_r21;
  options.dxbc_short_swap = lean_.short_swap;
  options.dxbc_short_degamma = lean_.short_degamma;
  options.dxbc_coalesce_moves = lean_.coalesce_moves;
  options.dxbc_dead_stores = lean_.dead_stores;  // job 637
  options.dxbc_vertex_words_vector = lean_.vertex_words;  // job 638
  options.dxbc_fold_modifiers = lean_.fold_modifiers;
  options.dxbc_merge_movs = lean_.merge_movs;
  options.vertex_index_clamp = vertex_index_clamp_;    // patch 0028
  options.dxbc_direct_operands = options.dxbc_loop_only_if_needed = options.dxbc_slim_prologue = dxbc_slim_;  // 0060
  options.dxbc_canary_minmax = dxbc_canary_maths_;                                                           // 0060
  options.dxbc_salt = dxbc_salt_;  // patch 0063: test only, 0 = unchanged
  return options;
}
bool ShaderCache::TranslateDxbcOnly(const uint32_t* dwords, size_t dword_count, bool is_vertex,
                                    std::vector<uint8_t>& out) {
  if (!dxbc_ || backend_.GetShaderIL() != gpu::ShaderIL::kDxil || !dwords || !dword_count) return false;
  const ucode::Program program = ucode::Decode(dwords, dword_count, is_vertex, DecodeOptions());
  dxbc_gen::DxbcTranslation dt = dxbc_gen::TranslateToDxbc(program, TranslateOptions(is_vertex, 0, dwords, dword_count));
  if (!dt.ok) return false;
  out = std::move(dt.bytecode);
  return true;
}

gpu::ShaderHandle ShaderCache::GetNativeHdr(uint64_t hash, const uint32_t* dwords, size_t count, bool canvas_material) {
  if (!dxbc_ || !dwords ||
      (!native_hdr::IsScene(hash) && !native_hdr::FindUi(hash) && !canvas_material)) return 0;
  if (const auto it = native_hdr_shaders_.find(hash); it != native_hdr_shaders_.end()) return it->second;
  auto program = ucode::Decode(dwords, count, false, DecodeOptions());
  auto options = TranslateOptions(false, 0, dwords, count);
  const auto* ui = native_hdr::FindUi(hash);
  const bool canvas = ui != nullptr || canvas_material;
  if (ui && (count != ui->dwords || Fnv1a64(dwords, count * sizeof(uint32_t)) != ui->fingerprint)) return 0;
  if (hash == native_hdr::kGears3Scene) {
    if (count != 150 || Fnv1a64(dwords, count * sizeof(uint32_t)) != 0x8BA713A2E446A745ull) return 0;
    options.native_hdr_linear = true;
    const auto translated = hlsl::TranslateToHlsl(program, options);
    const auto source = native_hdr::Gears3SceneSource(translated.hlsl, translated.float_constants_low, translated.float_constants_high);
    const auto shader = source.empty() ? 0 : GetBuiltin("native_hdr_gears3_scene", source, gpu::ShaderStage::kPixel);
    native_hdr_shaders_[hash] = shader;
    REXLOG_INFO("gow2_native_hdr: Gears 3 filmic/LUT scene {:016X}: {}", hash, shader ? "ready" : "FAILED");
    return shader;
  }
  if (!canvas && !native_hdr::ExtendScene(program)) return 0;
  options.clamp_ui_output = canvas;
  options.native_hdr_linear = true;
  options.native_hdr_ui = canvas && (!ui || !ui->modulation);
  const auto translated = TranslateDxbcFor(program, options, backend_.GetShaderIL() == gpu::ShaderIL::kSpirv);
  const auto shader = translated.ok ? backend_.CreateShader(gpu::ShaderStage::kPixel,
      translated.bytecode.data(), translated.bytecode.size()) : 0;
  native_hdr_shaders_[hash] = shader;
  REXLOG_INFO("gow2_native_hdr: {} shader {:016X}: {}", canvas ? "bounded Canvas" : "extended scene", hash,
              shader ? "ready" : "FAILED (reference-white fallback)");
  return shader;
}

gpu::ShaderHandle ShaderCache::GetNativeSceneUi(const uint32_t* dwords, size_t count) {
  if (const auto it = native_hdr_shaders_.find(native_hdr::kSceneUi); it != native_hdr_shaders_.end()) return it->second;
  const auto* profile = native_hdr::FindUi(native_hdr::kSceneUi);
  if (!dwords || count != profile->dwords ||
      Fnv1a64(dwords, count * sizeof(uint32_t)) != profile->fingerprint) return 0;
  auto options = TranslateOptions(false, 0, dwords, count);
  options.entry = "xe_scene_ui_material";
  const auto program = ucode::Decode(dwords, count, false, DecodeOptions());
  const auto source = native_hdr::SceneUiPairSource(hlsl::TranslateToHlsl(program, options).hlsl);
  if (source.empty()) return 0;
  const auto shader = GetBuiltin("native_scene_ui_pair", source, gpu::ShaderStage::kPixel);
  native_hdr_shaders_[native_hdr::kSceneUi] = shader;
  return shader;
}

const GuestShader& ShaderCache::GetImpl(uint64_t ucode_hash, const uint32_t* dwords, size_t dword_count,
                                        bool is_vertex, uint32_t tess_quad_segments, bool warm, bool hlsl_path) {
  ++stats_.lookups;
  auto& map = shaders_[is_vertex ? 1 : 0];
  // Quad-patch tessellation variants (notes/51 section 10) live under their own key.
  uint64_t key = (is_vertex && tess_quad_segments) ? ucode_hash ^ (0x7E55ull << 48) ^ (uint64_t(tess_quad_segments) << 40)
                                                    : ucode_hash;
  if (hlsl_path) key ^= 0xD1D1ull << 32;  // GetDxilTwin: the HLSL-path twin under its own key
  auto it = map.find(key);
  if (it != map.end()) return *it->second;
  if (!warm && !hlsl_path && !(is_vertex && tess_quad_segments)) SaveUcode(ucode_hash, is_vertex, dwords, dword_count);
  auto s = std::make_unique<GuestShader>();
  s->ucode_hash = ucode_hash;
  s->is_vertex = is_vertex;
  s->drawn = warm;  // patch 0045: recorded in an earlier session = drawn before
  const auto t0 = std::chrono::steady_clock::now();
  // Patch 0037 (map group 1): the renderer decodes the Canary way - a two-component scalar operand of a co-issued
  // 3-operand vector op is W and Z (Canary 92ada8e, a fix), and tfetch1D reads two coordinates when its swizzle asks
  // (fbdb1f2). The defaults stay rexglue's for the disassembly checks against the SDK's corpus.
  const ucode::DecodeOptions decode_options = DecodeOptions();
  const ucode::Program program = ucode::Decode(dwords, dword_count, is_vertex, decode_options);
  Analyse(program, *s);
  const hlsl::HlslOptions options = TranslateOptions(is_vertex, tess_quad_segments, dwords, dword_count);
  // Overhaul option B (--gow2_renderer_shader_backend=dxbc): DXBC made here in well under a millisecond, so the shader
  // is ready at once - no compile queue, no disk cache (the driver compiles it with the pipeline, as for DXIL). The
  // DXBC carries the shader cache's text post-processing below natively (precise everywhere; the depth remap).
  // Notes/68 V5a: on Vulkan the same DXBC becomes SPIR-V instruction for instruction (dxbc_spirv.cpp), so both
  // back ends run one translation; a shader the converter doesn't handle goes through the HLSL path as before.
  {  // notes/80 developer diagnostic
    static const std::string dump_list = "," + REXCVAR_GET(gow2_renderer_hlsl_dump_hashes) + ",";
    static const std::string dump_dir = REXCVAR_GET(gow2_renderer_hlsl_dump_dir);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(ucode_hash));
    if (!dump_dir.empty() && dump_list.find("," + std::string(hex) + ",") != std::string::npos) {
      const hlsl::Translation dump = hlsl::TranslateToHlsl(program, options);
      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      std::ofstream(std::filesystem::path(dump_dir) / ((is_vertex ? "vs-" : "ps-") + std::string(hex) + ".hlsl"),
                    std::ios::binary) << dump.hlsl;
    }
  }
  // Developer: a hand-edited replacement of this shader's HLSL, if the override folder holds one.
  std::string override_hlsl = HlslOverride(ucode_hash, is_vertex);
  const bool pcss = !is_vertex && override_hlsl.empty() && PcssOn() && PcssShader(ucode_hash);
  if (!override_hlsl.empty() || pcss) {  // the preset's quality / radius unless the developer switches were given
    const bool ultra = REXCVAR_GET(gow2_pcss) == "ultra";
    const auto given = [](const char* name) { return rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault; };
    const uint32_t quality = given("gow2_pcss_quality") || !PcssOn() ? uint32_t(std::max(0, REXCVAR_GET(gow2_pcss_quality)))
                                                                    : (ultra ? 3u : 2u);
    const float radius = given("gow2_pcss_radius") || !PcssOn() ? float(REXCVAR_GET(gow2_pcss_radius)) : (ultra ? 0.25f : 0.35f);
    XenosPcssSetAvailable(true, uint32_t(std::max(0, REXCVAR_GET(gow2_pcss_mode))), quality, radius);
  }
  if (!override_hlsl.empty())
    REXLOG_INFO("gow2_renderer: {}-{:016X} from the override folder ({} bytes)", is_vertex ? "vs" : "ps", ucode_hash,
                override_hlsl.size());
  const bool spirv = backend_.GetShaderIL() == gpu::ShaderIL::kSpirv;
  if (dxbc_ && override_hlsl.empty() && !pcss && !hlsl_path && (backend_.GetShaderIL() == gpu::ShaderIL::kDxil || spirv)) {
    bool spv_pending = false;  // 0.6.2: on Vulkan a store miss is converted on a background thread, not here
    dxbc_gen::DxbcTranslation dt = TranslateDxbcFor(program, options, spirv, &spv_pending);  // patch 0084: shared with the warm-up
    stats_.translate_ms += MsSince(t0);
    ++stats_.translated;
    if (dt.ok) return AdoptDxbc(key, std::move(s), dt.bytecode, dt.meta, spv_pending, options.spirv_precise ? 1u : 0u);
    ++stats_.dxbc_fallbacks;
    REXLOG_WARN("gow2_renderer: DXBC back end: {}-{:016X} goes through the HLSL path ({})", is_vertex ? "vs" : "ps",
                ucode_hash, dt.unsupported);
  }
  hlsl::Translation tr = hlsl::TranslateToHlsl(program, options);
  if (!override_hlsl.empty()) tr.hlsl = std::move(override_hlsl);  // the translation's resource / constant layout stays
  if (pcss) {
    if (ApplyPcss(ucode_hash, tr.hlsl))
      REXLOG_INFO("gow2_pcss: ps-{:016X} has PCSS", ucode_hash);
    else
      REXLOG_WARN("gow2_pcss: ps-{:016X} is not the shape the PCSS patch expects - it keeps the game's filter", ucode_hash);
  }
  stats_.translate_ms += MsSince(t0);
  ++stats_.translated;
  s->texture_slots |= tr.texture_slots_2d | tr.texture_slots_3d | tr.texture_slots_cube;
  s->texture_slots_2d = tr.texture_slots_2d;
  s->texture_slots_cube = tr.texture_slots_cube;
  s->texture_slots_3d = tr.texture_slots_3d;
  s->float_constants = std::max<uint32_t>(tr.float_constants, 1);
  // An empty read set still uploads c0 (as before); a compacted layout keeps its own bounds.
  s->float_constants_low = tr.float_constants_low == tr.float_constants_high ? s->float_constants : tr.float_constants_low;
  s->float_constants_high = tr.float_constants_low == tr.float_constants_high ? s->float_constants : tr.float_constants_high;
  for (int i = 0; i < 4; ++i) s->float_constants_read[i] = tr.float_constants_read[i];
  s->float_constants_relative = tr.float_constants_relative;
  s->memexport_stream_constants = tr.memexport_stream_constants;
  if (is_vertex) {
    // Bit-identical clip-space positions between shaders that draw the same geometry (prepass vs base pass, notes/42
    // "Depth"): mark the register file and the position output precise, so DXC keeps every operation feeding them as
    // written (no reassociation, no fused multiply-add). hlsl.cpp is unchanged; this is a text post-process.
    const auto mark = [&](const std::string& from, const std::string& to) {
      const size_t at = tr.hlsl.find(from);
      if (at == std::string::npos) return false;
      tr.hlsl.replace(at, from.size(), to);
      return true;
    };
    const bool a = mark("\n  float4 r[", "\n  precise float4 r[");
    const bool b = mark("\n  float4 xe_opos = ", "\n  precise float4 xe_opos = ");
    s->precise_positions = a && b;
  }
  if (!is_vertex) {
    // D24FS8 targets keep guest depth * 0.5 on the host (system flag 64, the reference's full_float24_in_0_to_1
    // remap): a shader-written depth is saturated and halved, and the initial value (the host depth) doubled back.
    const std::string init = "  float4 xe_odepth = float4(xe_in.position.z, ";
    const size_t at_init = tr.hlsl.find(init);
    if (at_init != std::string::npos)
      tr.hlsl.replace(at_init, init.size(),
                      "  float4 xe_odepth = float4(((xe_sys0.x & 64u) != 0u) ? xe_in.position.z * 2.0 : "
                      "xe_in.position.z, ");
    const std::string out = "  o.depth = xe_odepth.x;\n";
    const size_t at_out = tr.hlsl.find(out);
    if (at_out != std::string::npos)
      tr.hlsl.replace(at_out, out.size(),
                      "  o.depth = ((xe_sys0.x & 64u) != 0u) ? saturate(xe_odepth.x) * 0.5 : xe_odepth.x;\n");
    s->depth_remap_ready = !s->writes_depth || at_out != std::string::npos;
  }
  s->uses_textures = s->texture_slots != 0;
  s->simplifications = uint32_t(tr.simplifications.size());
  char stem[48];
  if (is_vertex && tess_quad_segments)
    std::snprintf(stem, sizeof(stem), "vs-tess%u-%016llX", tess_quad_segments, static_cast<unsigned long long>(ucode_hash));
  else
    std::snprintf(stem, sizeof(stem), "%s-%016llX", is_vertex ? "vs" : "ps", static_cast<unsigned long long>(ucode_hash));
  const gpu::ShaderStage stage = is_vertex ? gpu::ShaderStage::kVertex : gpu::ShaderStage::kPixel;
  const std::string name = FileName(stem, tr.hlsl);
  std::vector<uint8_t> bytecode;
  GuestShader& ref = *s;
  const auto disk_t0 = std::chrono::steady_clock::now();
  bool from_pack = false;
  const bool on_disk = LoadFromDisk(name, bytecode, &from_pack);
  stats_.disk_ms += MsSince(disk_t0);  // patch 0014
  if (on_disk) {
    ++stats_.disk_hits;
    if (from_pack) ++stats_.pack_hits;
    ref.handle = backend_.CreateShader(stage, bytecode.data(), bytecode.size());
    ref.state = ref.handle ? ShaderState::kReady : ShaderState::kFailed;
  } else if (workers_.empty()) {
    ref.state = ShaderState::kFailed;
    ref.error = "no shader compiler (dxcompiler.dll not loaded)";
  } else {
    auto job = std::make_unique<Job>();
    job->shader = &ref;
    job->name = name;
    job->hlsl = std::move(tr.hlsl);
    job->stage = stage;
    {
      // A live miss goes before the start-up warm-up backlog, so what is on screen compiles first.
      std::lock_guard<std::mutex> lock(mutex_);
      (warm ? warm_queue_ : queue_).push_back(std::move(job));
    }
    wake_.notify_one();
    ++in_flight_;
    ++stats_.queued;
  }
  if (ref.state == ShaderState::kFailed) {
    ++stats_.errors;
    REXLOG_WARN("gow2_renderer: SHADER-FAIL {}: {}", name, ref.error);
  }
  map.emplace(key, std::move(s));
  return ref;
}

gpu::ShaderHandle ShaderCache::GetBuiltin(const std::string& name, const std::string& hlsl, gpu::ShaderStage stage) {
  auto it = builtins_.find(name);
  if (it != builtins_.end()) return it->second;
  const std::string file = FileName(std::string("builtin-") + StageProfile(stage) + "-" + name, hlsl);
  std::vector<uint8_t> bytecode;
  gpu::ShaderHandle h = 0;
  std::string messages;
  bool ok = LoadFromDisk(file, bytecode);
  if (ok) {
    ++stats_.disk_hits;
  } else if (sync_compiler_) {
    const auto t0 = std::chrono::steady_clock::now();
    ok = sync_compiler_->Compile(hlsl, stage, backend_.GetShaderIL(), bytecode, messages);
    stats_.compile_ms += MsSince(t0);
    SaveToDisk(file, bytecode, hlsl, messages, ok);
    if (ok) ++stats_.compiled;
  } else {
    messages = "no shader compiler (dxcompiler.dll not loaded)";
  }
  if (ok) h = backend_.CreateShader(stage, bytecode.data(), bytecode.size());
  if (!h) {
    ++stats_.errors;
    REXLOG_ERROR("gow2_renderer: SHADER-FAIL built-in {}: {}", name, messages.substr(0, messages.find('\n')));
  }
  builtins_.emplace(name, h);
  return h;
}

}  // namespace xenos::renderer
