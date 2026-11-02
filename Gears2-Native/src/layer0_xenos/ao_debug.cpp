// Layer 0 (game-free): notes/80 s.12 - live A/B switches for the screen-space AO, for developers only
// (--gow2_ao_debug_keys; developer tools never ship on). F9: which AO the game's composite gets (XeGTAO / none / the
// game's own); F10: the debug view (normal, XeGTAO only, split, view depth, raw term, edges); F11: XeGTAO's quality
// preset. A small label (top left, like the FPS bar) says what is on. The keys run on the UI thread and only set
// atomics; the renderer reads them once per frame (ao_debug.h-style accessors in dev_tools.h). Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

REXCVAR_DEFINE_BOOL(gow2_ao_debug_keys, false, "Gears2",
                    "Notes/80 developer switches (with --gow2_ao=gtao): F9 cycles the AO the game gets (XeGTAO / none / "
                    "the game's own), F10 the debug view, F11 XeGTAO's quality; a label shows the state. Off in releases");

namespace {

constexpr uint32_t kShowCount = 3, kViewCount = 6, kQualityCount = 4;
const char* const kShowNames[kShowCount] = {"XeGTAO", "no AO", "game's own"};
const char* const kViewNames[kViewCount] = {"normal", "XeGTAO only", "split", "view depth", "raw term", "edges"};
const char* const kQualityNames[kQualityCount] = {"low", "medium", "high", "ultra"};

std::atomic<uint32_t> g_show{0}, g_view{0}, g_quality{2};
std::atomic<bool> g_keys{false}, g_active{false};

class AoLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit AoLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    char text[160];
    if (g_active.load())
      std::snprintf(text, sizeof(text), "AO: %s | %s | view: %s   (F9 AO, F10 view, F11 quality)",
                    kShowNames[g_show.load() % kShowCount], kQualityNames[g_quality.load() % kQualityCount],
                    kViewNames[g_view.load() % kViewCount]);
    else
      std::snprintf(text, sizeof(text), "AO keys: start with --gow2_ao=gtao");
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

void XenosAoDebugSetState(uint32_t show, uint32_t view, uint32_t quality) {
  g_show.store(show % kShowCount);
  g_view.store(view % kViewCount);
  g_quality.store(quality % kQualityCount);
  g_active.store(true);
}
uint32_t XenosAoDebugShow() { return g_show.load(); }
uint32_t XenosAoDebugView() { return g_view.load(); }
uint32_t XenosAoDebugQuality() { return g_quality.load(); }

void XenosInstallAoDebugKeys(rex::ui::ImGuiDrawer* drawer) {
  if (!REXCVAR_GET(gow2_ao_debug_keys)) return;
  g_keys.store(true);
  rex::ui::RegisterBind("bind_gow2_ao_show", "F9", "AO: XeGTAO / none / the game's own (developer)",
                        [] { Cycle(g_show, kShowCount, "shown AO", kShowNames); });
  rex::ui::RegisterBind("bind_gow2_ao_view", "F10", "AO debug view (developer)",
                        [] { Cycle(g_view, kViewCount, "debug view", kViewNames); });
  rex::ui::RegisterBind("bind_gow2_ao_quality", "F11", "XeGTAO quality (developer)",
                        [] { Cycle(g_quality, kQualityCount, "quality", kQualityNames); });
  if (drawer) g_label = std::make_unique<AoLabel>(drawer);
  REXLOG_INFO("gow2_ao: developer keys on (F9 AO, F10 view, F11 quality)");
}

void XenosShutdownAoDebugKeys() {
  if (!g_keys.exchange(false)) return;
  rex::ui::UnregisterBind("bind_gow2_ao_show");
  rex::ui::UnregisterBind("bind_gow2_ao_view");
  rex::ui::UnregisterBind("bind_gow2_ao_quality");
  g_label.reset();
}
