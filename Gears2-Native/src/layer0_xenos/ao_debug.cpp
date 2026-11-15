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
// --gow2_ssil (prototype): Insert on / off, Shift+Insert view, Ctrl+Insert strength, Alt+Insert albedo estimate.
constexpr uint32_t kSsilViews = 3, kSsilAlbedos = 4;
const char* const kSsilViewNames[kSsilViews] = {"normal", "indirect only", "split: without | with"};
const char* const kSsilAlbedoNames[kSsilAlbedos] = {"hue 50%", "grey 50%", "scene colour", "local"};
constexpr float kSsilStrengths[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f};
constexpr uint32_t kSsilStrengthCount = sizeof(kSsilStrengths) / sizeof(kSsilStrengths[0]);
std::atomic<bool> g_ssil_available{false}, g_ssil_on{true};
std::atomic<uint32_t> g_ssil_view{0}, g_ssil_strength{3}, g_ssil_albedo{0};
// The tricks (bits): 0 back faces (Home), 1 only the difference (Shift+Home), 2 temporal (Ctrl+Home), 3 bounces
// (Alt+Home), 4 adaptive scale (End).
std::atomic<uint32_t> g_ssil_tricks{1u | 4u | 8u | 16u};
constexpr const char* kSsilTrickNames[5] = {"back faces", "only the difference", "temporal", "bounces", "adaptive scale"};
std::atomic<float> g_ssil_median{0.0f}, g_ssil_radius{0.0f}, g_ssil_t{0.0f};  // the last scale read back
std::atomic<bool> g_ssil_paused{false};  // on, but no XeGTAO depth pyramid this frame (CACAO chosen)
void SsilToggle(uint32_t bit) {
  if (!g_ssil_available.load()) return;
  const uint32_t now = g_ssil_tricks.load() ^ (1u << bit);
  g_ssil_tricks.store(now);
  REXLOG_INFO("gow2_ssil: {} -> {}", kSsilTrickNames[bit], (now >> bit) & 1u ? "on" : "off");
}

class AoLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit AoLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    char text[768], passes[32];
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
    if (g_ssil_available.load()) {
      const size_t used = std::strlen(text);
      std::snprintf(text + used, sizeof(text) - used, "   | SSIL: %s x%.2f, albedo %s, view %s (Insert on/off, Shift+Insert view, Ctrl+Insert strength, Alt+Insert albedo: scene colour / local)",
                    !g_ssil_on.load() ? "off" : g_ssil_paused.load() ? "PAUSED - needs XeGTAO (F6 / F9)" : "on",
                    kSsilStrengths[g_ssil_strength.load() % kSsilStrengthCount],
                    kSsilAlbedoNames[g_ssil_albedo.load() % kSsilAlbedos], kSsilViewNames[g_ssil_view.load() % kSsilViews]);
      const uint32_t tr = g_ssil_tricks.load();
      const size_t used2 = std::strlen(text);
      std::snprintf(text + used2, sizeof(text) - used2,
                    "\n   SSIL tricks: back faces %s (Home), difference %s (Shift+Home), temporal %s (Ctrl+Home, needs an upscaler), "
                    "bounces %s (Alt+Home), adaptive %s (End): median %.0f -> radius %.0f (%.0f%% open)",
                    tr & 1u ? "on" : "off", tr & 2u ? "on" : "off", tr & 4u ? "on" : "off", tr & 8u ? "on" : "off",
                    tr & 16u ? "on" : "off", g_ssil_median.load(), g_ssil_radius.load(), g_ssil_t.load() * 100.0f);
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
void XenosSsilSetState(bool on, uint32_t view, float strength, uint32_t albedo) {
  uint32_t nearest = 0;
  for (uint32_t i = 1; i < kSsilStrengthCount; ++i)
    if (std::fabs(kSsilStrengths[i] - strength) < std::fabs(kSsilStrengths[nearest] - strength)) nearest = i;
  g_ssil_strength.store(nearest);
  g_ssil_on.store(on);
  g_ssil_view.store(view % kSsilViews);
  g_ssil_albedo.store(albedo % kSsilAlbedos);
  g_ssil_available.store(true);
}
bool XenosSsilOn() { return g_ssil_on.load(); }
void XenosSsilSetTricks(uint32_t bits) { g_ssil_tricks.store(bits & 31u); }
void XenosSsilSetPaused(bool paused) { g_ssil_paused.store(paused); }
void XenosSsilSetScale(float median, float radius, float t) {
  g_ssil_median.store(median);
  g_ssil_radius.store(radius);
  g_ssil_t.store(t);
}
uint32_t XenosSsilTricks() { return g_ssil_tricks.load(); }
uint32_t XenosSsilView() { return g_ssil_view.load() % kSsilViews; }
float XenosSsilStrength() { return kSsilStrengths[g_ssil_strength.load() % kSsilStrengthCount]; }
uint32_t XenosSsilAlbedo() { return g_ssil_albedo.load() % kSsilAlbedos; }
uint32_t XenosPcssMode() {
  return (g_pcss_on.load() ? 1u : 0u) | ((g_pcss_view.load() % kPcssViews) << 1) | ((g_pcss_quality.load() % kPcssQualities) << 3);
}

void XenosInstallAoDebugKeys(rex::ui::ImGuiDrawer* drawer) {
  if (!REXCVAR_GET(gow2_ao_debug_keys)) return;
  g_keys.store(true);
  // --gow2_ssil (prototype): Insert on / off; Shift+Insert view, Ctrl+Insert strength, Alt+Insert albedo estimate. (The
  // F keys are all taken: the SDK has F3 debug overlay, F4 settings, F7 achievements. --bind_gow2_ssil=<key> moves it.)
  rex::ui::RegisterBind("bind_gow2_ssil", "Insert", "SSIL on / off; Shift: view, Ctrl: strength, Alt: albedo estimate (developer)", [] {
    if (!g_ssil_available.load()) return;
#ifdef _WIN32
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
               alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
#else
    const bool shift = false, ctrl = false, alt = false;
#endif
    if (alt) {  // scene colour <-> local (hue / grey only from --gow2_ssil_albedo)
      const uint32_t a = g_ssil_albedo.load() % kSsilAlbedos == 2u ? 3u : 2u;
      g_ssil_albedo.store(a);
      REXLOG_INFO("gow2_ssil: albedo -> {}", kSsilAlbedoNames[a]);
    } else if (ctrl) {
      const uint32_t s = (g_ssil_strength.load() + 1) % kSsilStrengthCount;
      g_ssil_strength.store(s);
      REXLOG_INFO("gow2_ssil: strength -> {}", kSsilStrengths[s]);
    } else if (shift) {
      const uint32_t v = (g_ssil_view.load() + 1) % kSsilViews;
      g_ssil_view.store(v);
      REXLOG_INFO("gow2_ssil: view -> {}", kSsilViewNames[v]);
    } else {
      const bool on = !g_ssil_on.load();
      g_ssil_on.store(on);
      REXLOG_INFO("gow2_ssil: {}", on ? "on" : "off");
    }
  });
  // The SSIL tricks: Home back faces, Shift+Home only the difference, Ctrl+Home temporal, Alt+Home bounces; End the
  // adaptive scale.
  rex::ui::RegisterBind("bind_gow2_ssil_tricks", "Home", "SSIL back faces; Shift: only the difference, Ctrl: temporal, Alt: bounces (developer)", [] {
#ifdef _WIN32
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
               alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
#else
    const bool shift = false, ctrl = false, alt = false;
#endif
    SsilToggle(alt ? 3u : ctrl ? 2u : shift ? 1u : 0u);
  });
  rex::ui::RegisterBind("bind_gow2_ssil_adaptive", "End", "SSIL adaptive scale (developer)", [] { SsilToggle(4u); });
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
  // social step 5 (Flippage + Shagg, 8 Nov): the F8 developer bind "XeGTAO denoise passes" is gone; F8 opens Social.
  if (drawer) g_label = std::make_unique<AoLabel>(drawer);
  REXLOG_INFO("gow2_ao: developer keys on (F9 AO, F10 view, F11 quality, F8 denoise, F6 XeGTAO / CACAO)");
}

void XenosShutdownAoDebugKeys() {
  if (!g_keys.exchange(false)) return;
  rex::ui::UnregisterBind("bind_gow2_ao_show");
  rex::ui::UnregisterBind("bind_gow2_ssil");
  rex::ui::UnregisterBind("bind_gow2_ssil_tricks");
  rex::ui::UnregisterBind("bind_gow2_ssil_adaptive");
  rex::ui::UnregisterBind("bind_gow2_ao_game");
  rex::ui::UnregisterBind("bind_gow2_mip_bias");
  rex::ui::UnregisterBind("bind_gow2_ao_view");
  rex::ui::UnregisterBind("bind_gow2_ao_quality");
  rex::ui::UnregisterBind("bind_gow2_ao_algo");
  g_label.reset();
}
