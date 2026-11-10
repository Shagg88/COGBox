// gn_pipebench (patch 0059, MIT): how long the graphics driver takes to build a pipeline from our DXBC shaders, for
// several translator variants, on this PC's GPU. No game runs: it reads recorded guest shaders (<shader cache>\ucode,
// game-derived, local only), translates each one per variant with a unique salt (one dead instruction, so the driver's
// own shader cache cannot answer), builds one pipeline per VS/PS pair on one thread and times CreateGraphicsPipelineState.
// Usage: gn_pipebench <ucode dir> <out.csv> [pairs = 150] [reuse]
#include <xenos/dxbc_gen.h>
#include <xenos/hlsl.h>
#include <xenos/renderer/gpu.h>
#include <xenos/ucode.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xenos;
namespace gpu = xenos::renderer::gpu;

struct Variant {
  const char* name;
  bool slim, canary_minmax;  // patch 0060: slim = direct operands + loop only if needed + slim prologue
};

static std::vector<uint32_t> ReadUcode(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<uint32_t> d(b.size() / 4);
  if (!d.empty()) std::memcpy(d.data(), b.data(), d.size() * 4);
  return d;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: gn_pipebench <ucode dir> <out.csv> [pairs]\n");
    return 2;
  }
  const fs::path dir = argv[1];
  const uint32_t want = argc > 3 ? uint32_t(std::stoul(argv[3])) : 150u;
  std::vector<fs::path> vss, pss;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    if (n.size() != 25 || n.substr(19) != ".ucode") continue;
    (n[0] == 'v' ? vss : pss).push_back(e.path());
  }
  std::sort(vss.begin(), vss.end());
  std::sort(pss.begin(), pss.end());
  std::printf("pipebench: %zu vertex and %zu pixel shaders in %s\n", vss.size(), pss.size(), dir.string().c_str());
  if (vss.empty() || pss.empty()) return 3;

  auto backend = gpu::CreateD3D12Backend();
  gpu::BackendOptions bo;
  bo.upload_ring_mb = 16;
  bo.guest_memory_bytes = 16ull << 20;
  std::string err;
  if (!backend || !backend->Init(bo, err)) {
    std::printf("pipebench: D3D12 init failed: %s\n", err.c_str());
    return 4;
  }
  std::printf("pipebench: %s\n", backend->Describe().c_str());

  // Patch 0065: "reuse" mode - does the driver reuse its shader work across pipelines that share the same VS / PS?
  // Per pair (unique salts; two fresh VS/PS pairs a, b): A = (VSa, PSa) opaque, cold; B = (VSa, PSa) blended and
  // C = (VSa, PSa) two targets / other depth state - the same shaders; D = (VSb, PSa): PSa already through the driver,
  // VSb new; E = (VSa, PSb): VSa known, PSb new; F = (VSb, PSb): both stages known, only as parts of other pipelines.
  // B, C ~ 0 = the cost is per shader pair, not per state. F ~ 0 = the driver keeps each STAGE (so a pixel shader built
  // once with any vertex shader is cheap with every other one - what building at shader-load time needs).
  if (argc > 4 && std::string(argv[4]) == "reuse") {
    ucode::DecodeOptions rdop;
    rdop.canary_scalar_operand_z = true;
    rdop.canary_tfetch1d_two_components = true;
    uint32_t salt = uint32_t(std::chrono::system_clock::now().time_since_epoch().count()) | 1u;
    constexpr int kR = 6;
    const char* labels[kR] = {"A cold opaque", "B same shaders, blended", "C same shaders, 2 targets",
                              "D PS known, VS new", "E VS known, PS new", "F both known separately"};
    std::vector<double> ms[kR];
    std::ofstream rcsv(argv[2]);
    rcsv << "pair,vs,ps,bytes,a_ms,b_ms,c_ms,d_ms,e_ms,f_ms\n";
    const uint32_t rpairs = std::min<uint32_t>(want, uint32_t(pss.size()));
    for (uint32_t i = 0; i < rpairs; ++i) {
      const fs::path& pp = pss[size_t(i) * pss.size() / rpairs];
      const fs::path& vp = vss[i % vss.size()];
      const std::vector<uint32_t> vd = ReadUcode(vp), pd = ReadUcode(pp);
      const ucode::Program vprog = ucode::Decode(vd.data(), vd.size(), true, rdop);
      const ucode::Program pprog = ucode::Decode(pd.data(), pd.size(), false, rdop);
      auto make = [&](gpu::ShaderHandle& vs, gpu::ShaderHandle& ps, size_t& bytes) {
        hlsl::HlslOptions o;
        o.tex_sign_by_swizzle = o.canary_r21 = o.compact_float_constants = o.vertex_index_clamp = true;
        o.dxbc_loop_only_if_needed = o.dxbc_direct_operands = o.dxbc_slim_prologue = true;  // the default (0060)
        o.dxbc_salt = salt++;
        const auto vt = dxbc_gen::TranslateToDxbc(vprog, o);
        o.dxbc_salt = salt++;
        const auto pt = dxbc_gen::TranslateToDxbc(pprog, o);
        if (!vt.ok || !pt.ok) return false;
        vs = backend->CreateShader(gpu::ShaderStage::kVertex, vt.bytecode.data(), vt.bytecode.size());
        ps = backend->CreateShader(gpu::ShaderStage::kPixel, pt.bytecode.data(), pt.bytecode.size());
        bytes = vt.bytecode.size() + pt.bytecode.size();
        return true;
      };
      auto state = [](int which, gpu::ShaderHandle vs, gpu::ShaderHandle ps) {
        gpu::GraphicsPipelineDesc d;
        d.vs = vs;
        d.ps = ps;
        d.topology = gpu::Topology::kTriangleList;
        d.depth_format = gpu::Format::kD24UnormS8;
        if (which == 0) {  // opaque scene geometry
          d.cull = gpu::CullMode::kBack;
          d.depth_enable = d.depth_write = true;
          d.depth_func = gpu::CompareFunc::kLess;
          d.color_count = 1;
          d.color_formats[0] = gpu::Format::kR8G8B8A8Unorm;
          d.color_write_masks[0] = 0xF;
        } else if (which == 1) {  // alpha blended, depth test only, float target
          d.depth_enable = true;
          d.depth_func = gpu::CompareFunc::kLessEqual;
          d.color_count = 1;
          d.color_formats[0] = gpu::Format::kR16G16B16A16Float;
          d.color_write_masks[0] = 0xF;
          d.color_blend[0].enable = true;
          d.color_blend[0].src = gpu::BlendFactor::kSrcAlpha;
          d.color_blend[0].dst = gpu::BlendFactor::kInvSrcAlpha;
        } else {  // two targets, other depth state and write masks
          d.cull = gpu::CullMode::kFront;
          d.depth_enable = true;
          d.depth_func = gpu::CompareFunc::kEqual;
          d.color_count = 2;
          d.color_formats[0] = gpu::Format::kR8G8B8A8Unorm;
          d.color_formats[1] = gpu::Format::kR16G16B16A16Float;
          d.color_write_masks[0] = 0x7;
          d.color_write_masks[1] = 0xF;
        }
        return d;
      };
      auto timed = [&](const gpu::GraphicsPipelineDesc& d) {
        const auto t0 = std::chrono::steady_clock::now();
        const gpu::PipelineHandle h = backend->GetGraphicsPipeline(d, false, nullptr);
        const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return h ? t : -1.0;
      };
      gpu::ShaderHandle vs = 0, ps = 0, vs2 = 0, ps2 = 0;
      size_t bytes = 0, bytes2 = 0;
      if (!make(vs, ps, bytes) || !make(vs2, ps2, bytes2)) continue;
      const double t[kR] = {timed(state(0, vs, ps)),  timed(state(1, vs, ps)),   timed(state(2, vs, ps)),
                            timed(state(0, vs2, ps)), timed(state(0, vs, ps2)),  timed(state(0, vs2, ps2))};
      rcsv << i << ',' << vp.filename().string() << ',' << pp.filename().string() << ',' << bytes;
      bool all = true;
      for (int k = 0; k < kR; ++k) {
        rcsv << ',' << t[k];
        all = all && t[k] >= 0;
      }
      rcsv << '\n';
      if (all)
        for (int k = 0; k < kR; ++k) ms[k].push_back(t[k]);
      if ((i + 1) % 25 == 0) std::printf("pipebench reuse: %u / %u pairs\n", i + 1, rpairs);
    }
    for (int k = 0; k < kR; ++k) {
      std::vector<double>& v = ms[k];
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (double x : v) sum += x;
      const size_t n = v.size();
      std::printf("pipebench REUSE %-26s pairs %zu | ms each: mean %.1f median %.1f p90 %.1f max %.1f\n", labels[k], n,
                  n ? sum / double(n) : 0.0, n ? v[n / 2] : 0.0, n ? v[std::min(n - 1, n * 9 / 10)] : 0.0,
                  n ? v.back() : 0.0);
    }
    return 0;
  }

  const Variant variants[] = {{"base", false, false}, {"slim", true, false}, {"canary", false, true}, {"slim+canary", true, true}};
  constexpr size_t kV = sizeof(variants) / sizeof(variants[0]);
  ucode::DecodeOptions dop;
  dop.canary_scalar_operand_z = true;
  dop.canary_tfetch1d_two_components = true;
  const uint32_t salt0 = uint32_t(std::chrono::system_clock::now().time_since_epoch().count()) | 1u;
  struct Row {
    uint32_t pair, variant;
    std::string vs, ps;
    size_t bytes, instr;
    double ms;
    bool ok;
  };
  std::vector<Row> rows;
  const uint32_t pairs = std::min<uint32_t>(want, uint32_t(pss.size()));
  for (uint32_t i = 0; i < pairs; ++i) {
    const fs::path& pp = pss[size_t(i) * pss.size() / pairs];  // spread over the whole set
    const fs::path& vp = vss[i % vss.size()];
    const std::vector<uint32_t> vd = ReadUcode(vp), pd = ReadUcode(pp);
    const ucode::Program vprog = ucode::Decode(vd.data(), vd.size(), true, dop);
    const ucode::Program pprog = ucode::Decode(pd.data(), pd.size(), false, dop);
    for (uint32_t v = 0; v < kV; ++v) {  // interleaved: drift over the run hits every variant alike
      hlsl::HlslOptions o;
      o.tex_sign_by_swizzle = true;
      o.canary_r21 = true;
      o.compact_float_constants = true;
      o.vertex_index_clamp = true;
      o.dxbc_loop_only_if_needed = o.dxbc_direct_operands = o.dxbc_slim_prologue = variants[v].slim;
      o.dxbc_canary_minmax = variants[v].canary_minmax;
      o.dxbc_salt = salt0 + (i * kV + v) * 2u;
      const auto vt = dxbc_gen::TranslateToDxbc(vprog, o);
      o.dxbc_salt += 1u;
      const auto pt = dxbc_gen::TranslateToDxbc(pprog, o);
      Row r{i, v, vp.filename().string(), pp.filename().string(), vt.bytecode.size() + pt.bytecode.size(), 0, 0.0, false};
      if (!vt.ok || !pt.ok) {
        rows.push_back(r);
        continue;
      }
      gpu::GraphicsPipelineDesc d;
      d.vs = backend->CreateShader(gpu::ShaderStage::kVertex, vt.bytecode.data(), vt.bytecode.size());
      d.ps = backend->CreateShader(gpu::ShaderStage::kPixel, pt.bytecode.data(), pt.bytecode.size());
      d.topology = gpu::Topology::kTriangleList;
      d.depth_enable = d.depth_write = true;
      d.depth_func = gpu::CompareFunc::kLess;
      d.depth_format = gpu::Format::kD24UnormS8;
      d.color_count = 1;
      d.color_formats[0] = gpu::Format::kR8G8B8A8Unorm;
      d.color_write_masks[0] = 0xF;
      const auto t0 = std::chrono::steady_clock::now();
      const gpu::PipelineHandle h = backend->GetGraphicsPipeline(d, false, nullptr);
      r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      r.ok = h != 0;
      rows.push_back(r);
    }
    if ((i + 1) % 25 == 0) std::printf("pipebench: %u / %u pairs\n", i + 1, pairs);
  }
  std::ofstream csv(argv[2]);
  csv << "pair,variant,vs,ps,bytes,ms,ok\n";
  for (const Row& r : rows)
    csv << r.pair << ',' << variants[r.variant].name << ',' << r.vs << ',' << r.ps << ',' << r.bytes << ',' << r.ms << ','
        << (r.ok ? 1 : 0) << '\n';
  for (uint32_t v = 0; v < kV; ++v) {
    std::vector<double> ms;
    double kb = 0;
    uint32_t failed = 0;
    for (const Row& r : rows)
      if (r.variant == v) {
        if (r.ok) {
          ms.push_back(r.ms);
          kb += double(r.bytes) / 1024.0;
        } else {
          ++failed;
        }
      }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double x : ms) sum += x;
    const size_t n = ms.size();
    std::printf("pipebench RESULT %-11s pipelines %zu failed %u | ms each: mean %.1f median %.1f p90 %.1f max %.1f | "
                "%.1f KB bytecode each | %.1f s in all\n",
                variants[v].name, n, failed, n ? sum / double(n) : 0.0, n ? ms[n / 2] : 0.0,
                n ? ms[std::min(n - 1, n * 9 / 10)] : 0.0, n ? ms.back() : 0.0, n ? kb / double(n) : 0.0, sum / 1000.0);
  }
  return 0;
}
