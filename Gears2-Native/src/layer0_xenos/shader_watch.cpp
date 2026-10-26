// Layer 0 (game-free): watch chosen shaders at runtime. --gow2_watch_shaders="ps-8DCC9EF73F7C1B78,vs-..." logs, for each
// draw using one of them, every distinct value of the loop constants and the bool constants it sees (S3.3 input: a loop
// constant's start and step are runtime values, and XenosRecomp only handles start 0 step 1). Loop constant layout
// (Xenos): bits 0-7 count, 8-15 start (initial aL), 16-23 step (signed). Logged once per distinct value, plus a count.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/logging/macros.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

REXCVAR_DEFINE_STRING(gow2_watch_shaders, "", "Gears2", "Comma list of vs-/ps-<hash> shaders whose loop/bool constants to log");

namespace {

using namespace rex::graphics;

class ShaderWatch final : public DrawObserver {
 public:
  explicit ShaderWatch(std::set<std::string> names) : names_(std::move(names)) {}
  void OnDrawShaders(const Shader* vs, const Shader* ps) override {
    hit_.clear();
    for (auto [s, prefix] : {std::pair{vs, "vs-"}, std::pair{ps, "ps-"}}) {
      if (!s) continue;
      char name[32];
      std::snprintf(name, sizeof(name), "%s%016llX", prefix, (unsigned long long)s->ucode_data_hash());
      if (names_.count(name)) hit_ = name;
    }
  }
  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {
    if (hit_.empty()) return;
    ++draws_;
    // Loop constants 0x4908-0x4927, bool constants 0x4900-0x4907 (256 bits, vertex and pixel halves).
    for (uint32_t i = 0; i < 32; ++i) {
      const uint32_t v = regs[0x4908 + i];
      if (!seen_.insert({hit_, i, v}).second) continue;
      if (v == 0 && i != 31) continue;  // unset constants are noise; i31 is the one the census found
      REXLOG_INFO("gow2_watch_shaders: frame {} {} loop i{} = {:08X} (count {} start {} step {})", frame_ + 1, hit_, i, v,
                  v & 0xFF, (v >> 8) & 0xFF, int8_t((v >> 16) & 0xFF));
    }
  }
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frame_;
    if (frame_ % 600 == 0 && draws_)
      REXLOG_INFO("gow2_watch_shaders: frame {}: {} draws used a watched shader so far", frame_, draws_);
  }

 private:
  std::set<std::string> names_;
  std::string hit_;
  std::set<std::tuple<std::string, uint32_t, uint32_t>> seen_;
  uint64_t frame_ = 0, draws_ = 0;
};

}  // namespace

void XenosInstallShaderWatchIfRequested() {
  const std::string list = REXCVAR_GET(gow2_watch_shaders);
  if (list.empty()) return;
  std::set<std::string> names;
  std::stringstream ss(list);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) names.insert(item);
  static ShaderWatch watch(std::move(names));
  XenosRegisterDrawObserver(&watch);
  REXLOG_INFO("gow2_watch_shaders: watching {}", list);
}
