// Layer 0 (game-free): Stage 3 step 1 - collect the game's shader corpus. With --gow2_shader_dump_dir=<dir>, every
// distinct vertex/pixel shader that reaches a draw is written once: <dir>/<vs|ps>-<ucode hash>.ucode (microcode dwords,
// host byte order) and .sdk.txt (the SDK's own microcode disassembly, the reference our disassembler/translator is
// checked against). <dir>/index.csv (rewritten every 600 frames) lists type, hash, dword count, draws seen, first frame.
// The files are derived from the game, so they live under out\ and are never committed.
// Files that already exist are kept (a shared folder across runs keeps the first route's copy), and each newly written
// shader is appended to <dir>/first_seen.csv with --gow2_shader_dump_route (job 074 lesson: rewriting every run lost the
// route a shader first appeared in). The microcode is written at first sight even before the SDK analyses the shader
// (job 074 missed vs-AB1E86137A0240E8, never analysed); the .sdk.txt follows once it is.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging/macros.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>

REXCVAR_DEFINE_STRING(gow2_shader_dump_dir, "", "Gears2", "Write every distinct shader (microcode + SDK disassembly) to this folder");
REXCVAR_DEFINE_STRING(gow2_shader_dump_route, "", "Gears2", "Route label recorded in first_seen.csv for shaders this run adds");

namespace {

using namespace rex::graphics;

struct Seen {
  const Shader* shader = nullptr;
  bool vertex = false;
  bool written = false;       // .sdk.txt done (or already present)
  bool ucode_written = false;  // .ucode done (or already present)
  uint64_t draws = 0;
  uint64_t first_frame = 0;
};

class ShaderDump final : public DrawObserver {
 public:
  ShaderDump(std::filesystem::path dir, std::string route) : dir_(std::move(dir)), route_(std::move(route)) {}
  void OnDrawShaders(const Shader* vs, const Shader* ps) override {
    Note(vs, true);
    Note(ps, false);
  }
  void OnDraw(const RegisterFile&, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {}
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frame_;
    // Shaders are analysed (and disassembled) at their first draw; write any that were not ready when first seen.
    for (auto& [hash, s] : seen_) if (!s.written) Write(hash, s);
    if (frame_ % 600 == 0) Index();
  }

 private:
  void Note(const Shader* sh, bool vertex) {
    if (!sh) return;
    Seen& s = seen_[sh->ucode_data_hash() ^ (vertex ? 0 : 0x8000000000000000ull)];
    if (!s.shader) { s.shader = sh; s.vertex = vertex; s.first_frame = frame_ + 1; }
    ++s.draws;
  }
  void Write(uint64_t key, Seen& s) {
    const Shader* sh = s.shader;
    char base[64];
    std::snprintf(base, sizeof(base), "%s-%016llX", s.vertex ? "vs" : "ps", (unsigned long long)sh->ucode_data_hash());
    const auto stem = dir_ / base;
    std::error_code ec;
    if (!s.ucode_written) {
      s.ucode_written = true;
      const auto ucode_path = stem.string() + ".ucode";
      if (!std::filesystem::exists(ucode_path, ec)) {
        if (FILE* f = std::fopen(ucode_path.c_str(), "wb")) {
          std::fwrite(sh->ucode_dwords(), 4, sh->ucode_dword_count(), f);
          std::fclose(f);
        }
        if (FILE* f = std::fopen((dir_ / "first_seen.csv").string().c_str(), "ab")) {
          std::fprintf(f, "%s,%s,%llu\n", base, route_.c_str(), (unsigned long long)s.first_frame);
          std::fclose(f);
        }
        ++added_;
      }
    }
    if (!sh->is_ucode_analyzed()) return;
    if (std::filesystem::exists(stem.string() + ".sdk.txt", ec)) {
      s.written = true;
      return;
    }
    if (FILE* f = std::fopen((stem.string() + ".sdk.txt").c_str(), "wb")) {
      const std::string& d = sh->ucode_disassembly();
      std::fwrite(d.data(), 1, d.size(), f);
      std::fclose(f);
    }
    s.written = true;
    ++written_;
  }
  void Index() {
    if (FILE* f = std::fopen((dir_ / "index.csv").string().c_str(), "wb")) {
      std::fprintf(f, "type,hash,dwords,draws,first_frame\n");
      for (const auto& [key, s] : seen_)
        std::fprintf(f, "%s,%016llX,%zu,%llu,%llu\n", s.vertex ? "vs" : "ps",
                     (unsigned long long)s.shader->ucode_data_hash(), s.shader->ucode_dword_count(),
                     (unsigned long long)s.draws, (unsigned long long)s.first_frame);
      std::fclose(f);
    }
    uint64_t unanalysed = 0;
    for (const auto& [key, s] : seen_) if (!s.written) ++unanalysed;
    REXLOG_INFO("gow2_shader_dump: frame {}: {} distinct shaders, {} new to the folder, {} disassemblies written, {} never analysed",
                frame_, seen_.size(), added_, written_, unanalysed);
  }

  std::filesystem::path dir_;
  std::string route_;
  uint64_t added_ = 0;
  std::unordered_map<uint64_t, Seen> seen_;
  uint64_t frame_ = 0, written_ = 0;
};

}  // namespace

void XenosInstallShaderDumpIfRequested() {
  const std::string dir = REXCVAR_GET(gow2_shader_dump_dir);
  if (dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  static ShaderDump dump{std::filesystem::path(dir), std::string(REXCVAR_GET(gow2_shader_dump_route))};
  XenosRegisterDrawObserver(&dump);
  REXLOG_INFO("gow2_shader_dump: writing shaders to {}", dir);
}
