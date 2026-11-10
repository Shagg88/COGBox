// Layer 0 (game-free): notes/80 s.12 - live A/B switches for the screen-space AO, for developers only
// XeGTAO ambient occlusion, upscaler motion-vector / depth fixes and SMAA T2x: PR #139 by gmihaic (https://github.com/gmihaic).
// (--gow2_ao_debug_keys; developer tools never ship on). F9: which AO the game's composite gets (XeGTAO / CACAO / the
// game's own / none); F10: the debug view (normal, XeGTAO only, split, view depth, raw term, edges); F11: XeGTAO's quality
// preset; F8: the denoise passes (1-3); F6: XeGTAO / CACAO; F5: what is shown <-> the game's own. A small label (top left, like the FPS bar) says what is on. The keys run on the UI thread and only set
// atomics; the renderer reads them once per frame (ao_debug.h-style accessors in dev_tools.h). Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <memory>

REXCVAR_DEFINE_BOOL(gow2_ao_debug_keys, false, "Gears2",
                    "Notes/80 developer switches (with --gow2_ao=gtao): F9 cycles the AO the game gets (XeGTAO / CACAO / "
                    "the game's own / none), F10 the debug view, F11 XeGTAO's quality, F8 its denoise passes (1-3), F6 XeGTAO / CACAO; a "
                    "label shows the state. Off in releases");

namespace {

constexpr uint32_t kShowCount = 3, kViewCount = 6, kQualityCount = 4;
const char* const kShowNames[kShowCount] = {"XeGTAO", "no AO", "game's own"};
const char* const kViewNames[kViewCount] = {"normal", "XeGTAO only", "split", "view depth", "raw term", "edges"};
const char* const kQualityNames[kQualityCount] = {"low", "medium", "high", "ultra"};
constexpr uint32_t kDenoiseCount = 3;
const char* const kDenoiseNames[kDenoiseCount] = {"1 denoise pass", "2 denoise passes", "3 denoise passes"};
constexpr uint32_t kBlurSteps[] = {1, 2, 3, 4, 6, 8};  // F8; AMD allows up to 8

std::atomic<uint32_t> g_show{0}, g_view{0}, g_quality{2}, g_denoise{2};  // g_denoise: passes - 1
std::atomic<bool> g_keys{false}, g_active{false}, g_cacao{false}, g_cacao_available{false};
std::atomic<uint32_t> g_cacao_blur{3};  // the passes themselves
std::atomic<uint32_t> g_show_before_game{0};  // F5: what it returns to
std::atomic<bool> g_mip_bias{false};  // notes/82: F2
// Notes/80 s.16 (PCSS): the patched shadow shaders read xe_sys1.z = filter (bit 0: PCSS) | view << 1 | quality << 3.
// F12: the game's filter <-> PCSS; Shift+F12: normal -> split (left the game's, right PCSS) -> coverage (right shadowed);
// Ctrl+F12: PCSS quality; Alt+F12: PCSS radius.
constexpr uint32_t kPcssViews = 3;
const char* const kPcssViewNames[kPcssViews] = {"normal", "split: game | PCSS", "coverage"};
std::atomic<bool> g_pcss_on{true};
std::atomic<uint32_t> g_pcss_view{0};
// Ctrl+F12: quality - blocker-search / filter samples 8/16, 16/24, 16/32, 32/64 (bits 3-4 of xe_sys1.z).
constexpr uint32_t kPcssQualities = 4;
const char* const kPcssQualityNames[kPcssQualities] = {"low", "medium", "high", "ultra"};
std::atomic<uint32_t> g_pcss_quality{2};
// Alt+F12: the radius - scales the penumbra, its widest and the blocker search together (xe_sys1.w, a float).
constexpr float kPcssRadii[] = {1.0f, 0.75f, 0.5f, 0.35f, 0.25f};
constexpr uint32_t kPcssRadiusCount = sizeof(kPcssRadii) / sizeof(kPcssRadii[0]);
std::atomic<uint32_t> g_pcss_radius{2};
std::atomic<bool> g_pcss_available{false};
std::atomic<bool> g_pcss_trace{false};  // Ctrl+Shift+F12: log the next frame's depth-texture draws (pcss_trace)

class AoLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit AoLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    char text[384], passes[32];
    if (g_cacao.load()) std::snprintf(passes, sizeof(passes), "%u blur passes", g_cacao_blur.load());
    else std::snprintf(passes, sizeof(passes), "%s", kDenoiseNames[g_denoise.load() % kDenoiseCount]);
    if (g_active.load())
      std::snprintf(text, sizeof(text), "AO: %s%s | %s, %s | view: %s | mip bias %s   (F2 mip bias, F5 game AO on/off, F9 AO, F10 view, F11 quality, F8 denoise, F6 XeGTAO/CACAO)",
                    g_show.load() % kShowCount == 0 && g_cacao.load() ? "CACAO" : kShowNames[g_show.load() % kShowCount], "", kQualityNames[g_quality.load() % kQualityCount],
                    passes,
                    kViewNames[g_view.load() % kViewCount], g_mip_bias.load() ? "on" : "off");
    else
      std::snprintf(text, sizeof(text), "AO keys: start with --gow2_ao=gtao");
    if (g_pcss_available.load()) {
      const size_t used = std::strlen(text);
      std::snprintf(text + used, sizeof(text) - used,
                    "   | shadows: %s %s, radius %.0f%%, view %s (F12 filter, Shift+F12 view, Ctrl+F12 quality, Alt+F12 radius)",
                    g_pcss_on.load() ? "PCSS" : "game's filter", kPcssQualityNames[g_pcss_quality.load() % kPcssQualities],
                    kPcssRadii[g_pcss_radius.load() % kPcssRadiusCount] * 100.0f, kPcssViewNames[g_pcss_view.load() % kPcssViews]);
    }
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(size.x * scale + 20.0f * scale, size.y * scale + 8.0f * scale));
    if (ImGui::Begin("##gow2_ao_label", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, at,
                                          IM_COL32(255, 210, 90, 255), text);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }
};

std::unique_ptr<AoLabel> g_label;

void Cycle(std::atomic<uint32_t>& v, uint32_t count, const char* what, const char* const* names) {
  if (!g_active.load()) return;
  const uint32_t n = (v.load() + 1) % count;
  v.store(n);
  REXLOG_INFO("gow2_ao: {} -> {}", what, names[n]);
}

}  // namespace

void XenosAoDebugSetState(uint32_t show, uint32_t view, uint32_t quality, uint32_t denoise_passes) {
  g_show.store(show % kShowCount);
  g_view.store(view % kViewCount);
  g_quality.store(quality % kQualityCount);
  g_denoise.store((std::clamp(denoise_passes, 1u, kDenoiseCount) - 1) % kDenoiseCount);
  g_active.store(true);
}
uint32_t XenosAoDebugShow() { return g_show.load(); }
uint32_t XenosAoDebugView() { return g_view.load(); }
uint32_t XenosAoDebugQuality() { return g_quality.load(); }
void XenosAoDebugSetCacao(bool on, bool available, uint32_t blur_passes) {
  g_cacao.store(on);
  g_cacao_available.store(available);
  g_cacao_blur.store(std::min(blur_passes, 8u));
}
bool XenosAoDebugCacao() { return g_cacao.load(); }
void XenosAoDebugSetMipBias(bool on) { g_mip_bias.store(on); }
bool XenosAoDebugMipBias() { return g_mip_bias.load(); }
uint32_t XenosAoDebugCacaoBlur() { return g_cacao_blur.load(); }
uint32_t XenosAoDebugDenoisePasses() { return g_denoise.load() % kDenoiseCount + 1; }
void XenosAoDebugSetDenoisePasses(uint32_t passes) { g_denoise.store(std::clamp(passes, 1u, kDenoiseCount) - 1); }
void XenosPcssSetAvailable(bool on, uint32_t mode, uint32_t quality, float radius) {  // mode: 0 game, 1 PCSS, 2 split, 3 coverage
  if (!on || g_pcss_available.exchange(true)) return;
  uint32_t nearest = 0;
  for (uint32_t i = 1; i < kPcssRadiusCount; ++i)
    if (std::fabs(kPcssRadii[i] - radius) < std::fabs(kPcssRadii[nearest] - radius)) nearest = i;
  g_pcss_radius.store(nearest);
  g_pcss_on.store(mode != 0);
  g_pcss_view.store(mode >= 2 ? mode - 1 : 0);
  g_pcss_quality.store(quality % kPcssQualities);
}
bool XenosPcssTakeTrace() { return g_pcss_trace.exchange(false); }
float XenosPcssRadius() { return kPcssRadii[g_pcss_radius.load() % kPcssRadiusCount]; }
uint32_t XenosPcssMode() {
  return (g_pcss_on.load() ? 1u : 0u) | ((g_pcss_view.load() % kPcssViews) << 1) | ((g_pcss_quality.load() % kPcssQualities) << 3);
}

void XenosInstallAoDebugKeys(rex::ui::ImGuiDrawer* drawer) {
  if (!REXCVAR_GET(gow2_ao_debug_keys)) return;
  g_keys.store(true);
  rex::ui::RegisterBind("bind_gow2_ao_show", "F9", "AO: XeGTAO / CACAO / the game's own / none (developer)", [] {
    if (!g_active.load()) return;
    // g_show: 0 ours, 1 none, 2 the game's own.
    const uint32_t show = g_show.load() % kShowCount;
    const bool cacao = g_cacao.load();
    const char* name = "XeGTAO";
    if (show == 0 && !cacao && g_cacao_available.load()) {
      g_cacao.store(true);
      name = "CACAO";
    } else if (show == 0) {
      g_cacao.store(false);
      g_show.store(2);
      name = "game's own";
    } else if (show == 2) {
      g_show.store(1);
      name = "no AO";
    } else {
      g_show.store(0);
    }
    REXLOG_INFO("gow2_ao: shown AO -> {}", name);
  });
  rex::ui::RegisterBind("bind_gow2_pcss", "F12", "Shadows (Ctrl+Shift: trace the next frame): game's filter / PCSS; Shift: normal / split / coverage; Ctrl: PCSS quality; Alt: PCSS radius (developer)", [] {
    if (!g_pcss_available.load()) return;
#ifdef _WIN32
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
               alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
#else
    const bool shift = false, ctrl = false, alt = false;
#endif
    if (ctrl && shift) {
      g_pcss_trace.store(true);
      REXLOG_INFO("gow2_pcss: Ctrl+Shift+F12 - the next frame's shadow passes go to the log (pcss_trace)");
    } else if (alt) {
      const uint32_t r = (g_pcss_radius.load() + 1) % kPcssRadiusCount;
      g_pcss_radius.store(r);
      REXLOG_INFO("gow2_pcss: radius -> {:.0f}%", kPcssRadii[r] * 100.0f);
    } else if (ctrl) {
      const uint32_t q = (g_pcss_quality.load() + 1) % kPcssQualities;
      g_pcss_quality.store(q);
      REXLOG_INFO("gow2_pcss: quality -> {}", kPcssQualityNames[q]);
    } else if (shift) {
      const uint32_t v = (g_pcss_view.load() + 1) % kPcssViews;
      g_pcss_view.store(v);
      REXLOG_INFO("gow2_pcss: view -> {}", kPcssViewNames[v]);
    } else {
      const bool on = !g_pcss_on.load();
      g_pcss_on.store(on);
      REXLOG_INFO("gow2_pcss: filter -> {}", on ? "PCSS" : "the game's");
    }
  });
  rex::ui::RegisterBind("bind_gow2_mip_bias", "F2", "Upscaler mip bias on / off (developer)", [] {
    const bool on = !g_mip_bias.load();
    g_mip_bias.store(on);
    REXLOG_INFO("gow2_upscaler: mip bias -> {}", on ? "on" : "off");
  });
  rex::ui::RegisterBind("bind_gow2_ao_game", "F5", "AO: what is shown <-> the game's own (developer)", [] {
    if (!g_active.load()) return;
    const uint32_t show = g_show.load() % kShowCount;
    if (show == 2) {
      g_show.store(g_show_before_game.load());
    } else {
      g_show_before_game.store(show);
      g_show.store(2);
    }
    const uint32_t now = g_show.load() % kShowCount;
    REXLOG_INFO("gow2_ao: F5 -> {}", now == 0 && g_cacao.load() ? "CACAO" : kShowNames[now]);
  });
  rex::ui::RegisterBind("bind_gow2_ao_view", "F10", "AO debug view (developer)",
                        [] { Cycle(g_view, kViewCount, "debug view", kViewNames); });
  rex::ui::RegisterBind("bind_gow2_ao_quality", "F11", "XeGTAO quality (developer)",
                        [] { Cycle(g_quality, kQualityCount, "quality", kQualityNames); });
  rex::ui::RegisterBind("bind_gow2_ao_algo", "F6", "AO: XeGTAO / CACAO (developer)", [] {
    if (!g_active.load() || !g_cacao_available.load()) return;
    const bool on = !g_cacao.load();
    g_cacao.store(on);
    REXLOG_INFO("gow2_ao: algorithm -> {}", on ? "CACAO" : "XeGTAO");
  });
  rex::ui::RegisterBind("bind_gow2_ao_denoise", "F8", "XeGTAO denoise passes (developer)",
                        [] {
                          if (g_cacao.load()) {
                            if (!g_active.load()) return;
                            uint32_t next = kBlurSteps[0];
                            for (const uint32_t s : kBlurSteps)
                              if (s > g_cacao_blur.load()) { next = s; break; }
                            g_cacao_blur.store(next);
                            REXLOG_INFO("gow2_ao: CACAO blur -> {} passes", next);
                          } else {
                            Cycle(g_denoise, kDenoiseCount, "denoise", kDenoiseNames);
                          }
                        });
  if (drawer) g_label = std::make_unique<AoLabel>(drawer);
  REXLOG_INFO("gow2_ao: developer keys on (F9 AO, F10 view, F11 quality, F8 denoise, F6 XeGTAO / CACAO)");
}

void XenosShutdownAoDebugKeys() {
  if (!g_keys.exchange(false)) return;
  rex::ui::UnregisterBind("bind_gow2_ao_show");
  rex::ui::UnregisterBind("bind_gow2_ao_game");
  rex::ui::UnregisterBind("bind_gow2_mip_bias");
  rex::ui::UnregisterBind("bind_gow2_ao_view");
  rex::ui::UnregisterBind("bind_gow2_ao_quality");
  rex::ui::UnregisterBind("bind_gow2_ao_denoise");
  rex::ui::UnregisterBind("bind_gow2_ao_algo");
  g_label.reset();
}
