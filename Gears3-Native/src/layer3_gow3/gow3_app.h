// gow3 - ReXGlue Recompiled Project (from the rexglue init template). Layer 3: Gears of War 3 only.
// Same hook order as Gears 2's Gow2App (layer 0 developer tools and our renderer are installed only when their options
// are set). Not carried over: layer 1 (Gears 2's D3D resource-call watch and resource check - developer diagnostics
// tied to XDK 7645 addresses). The renderer's D3D built-in shader hints ARE the same as Gears 2's (see below).
#pragma once

#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>
#include "mouse_look.h"

namespace rex::ui {
class WindowedAppContext;
}
void XenosSetUiContext(rex::ui::WindowedAppContext* ui);  // layer 0 fps_bar.cpp (0.2.1)
#include <rex/ui/window.h>

#include <ue3/config_bundle.h>
#include <ue3/launch.h>
#include <xenos/dev_tools.h>
#include <xenos/display_size.h>
#include <xenos/gpu_system.h>
#include <xenos/lazy_achievement_toast.h>

#include <rex/system/interfaces/graphics.h>

// Prebuild v2: Gears 2's layer 2 shader prebuild (Gears2-Native/src/layer2_ue3/shader_prebuild.cpp, built into this
// game's layer 2 from GN_LAYER0_HOME): reads each package the game opens for its pixel shaders. After the renderer.
void Ue3InstallShaderPrebuildIfRequested(rex::Runtime* runtime);
void Ue3HoldForShaderPrebuild(rex::system::XThread* thread);

void Gow3ApplyDataPatchesIfRequested(rex::Runtime* runtime);
void Gow3AddBuiltInIniEdits();  // game_patches.cpp: --gow3_trilinear, --gow3_fast_mip_fade
void Gow3StopMemoryGauge();     // game_patches.cpp: --gow3_memory_log_seconds stops before the kernel goes away
void Gow3InstallExtras();       // extras.cpp: --gow3_cheats, --gow3_unlock_mutators, engine console hotkeys
REXCVAR_DECLARE(bool, gow3_mute_unfocused);  // extras.cpp

class Gow3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Gow3App>(new Gow3App(ctx, "gow3", PPCImageConfig));
  }

  // Our own graphics system in place of the SDK's GPU plugin, when asked (layer 0 option).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (auto ours = XenosCreateOurGpuSystemIfRequested()) config.graphics = std::move(ours);
  }
  void OnPostInitLogging() override {
    XenosInstallCrashHandler();
    XenosLogSystemReport("Gears of War 3");  // PC description for problem reports (Gears 2 launcher patch 0131)
    XenosLatencyInstall();  // NVIDIA Reflex / AMD Anti-Lag 2 when --gow2_reflex / --gow2_antilag2 (Gears 2 patches 0135/0136)
  }
  void OnPostSetup() override {
    CogMouseLookInstall(window());  // 0.2.1: native mouse look (mouse_look.cpp)
    XenosAddScriptPadIfRequested(runtime());
    Gow3AddBuiltInIniEdits();  // before the bundle is edited
    Gow3InstallExtras();       // its package patches too
    Ue3InstallConfigEditsIfRequested(runtime());  // --ue3_ini_edits (job 050): before the game reads its config
    XenosInstallDrawStatsIfRequested();
    XenosInstallPrimDetailIfRequested();
    XenosInstallFrameCaptureIfRequested();
    XenosInstallFrameTimesIfRequested();
    XenosInstallProfilerIfRequested();
    XenosInstallDrawLogIfRequested();
    XenosInstallResolveLogIfRequested();
    XenosInstallBufferDumpIfRequested(runtime());
    XenosInstallShaderDumpIfRequested();
    XenosInstallShaderWatchIfRequested();
    // D3D built-in shaders (fast-clear pair, no-op single-point VS): the same microcode hashes as Gears 2's XDK 7645 -
    // all three appear in Gears 3's shader cache after job 014 run B (vs-1E6883FCCDE1F688, ps-A4A965C189287B99,
    // vs-B6C9863F710683EC), so XDK 20500's D3D kept them byte-identical. Without them, clears weren't recognised
    // (job 014 B: "clears 0").
    // Gears 3's settings for the shared renderer (Gears 2's layer 0), each set only while it still has its compiled-in
    // default, so a command line, config file or environment variable always wins:
    //  - DLSS / FSR camera move (job 055 probe): an object's object-to-world rows in c7-c10 (Gears 2: c5-c8) and the
    //    camera's world position in c5 on every scene draw;
    //  - memexport (notes/11): Gears 3's vertex shaders write guest memory (particles, DXT copies, job 041);
    //  - resolution scale threshold 1120 (notes/53b): targets with a pitch at or below it stay 1x when scaled.
    const auto game_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    };
    game_default("gow2_upscale_object_constants", "7");
    game_default("gow2_upscale_camera_constant", "5");
    game_default("gow2_renderer_memexport", "true");
    game_default("gow2_renderer_resolution_scale_threshold", "1120");
    // Real per-issuance answers cull hidden geometry and lens flares. Explicit
    // renderer-query settings remain available for comparisons.
    if (XenosOurGpuSystem() && rex::cvar::GetFlagByName("gow2_renderer_api") == "d3d12" &&
        rex::cvar::GetFlagByName("gow2_pm4_split") == "true")
      game_default("gow2_renderer_queries", "strict");
    //  - the upscalers' scene colour: Gears 3's lit scene is 10:10:10:2 UNORM (job 055), not 16-bit float as Gears 2's
    //    (the shared renderer's default, 1, would never find it).
    game_default("gow2_upscale_scene_formats", "2");
    //  - the upscalers' late draws (Gears 3 jobs 117-120b, Gears 2 patch 0106): ch8 cutscenes draw ~170 camera draws
    //    per frame into the scene depth AFTER the scene colour resolve that runs the upscaler (later lighting passes);
    //    unjittered, whole character triangles lost their lighting under DLSS / FSR. Jittered, the frames are clean.
    game_default("gow2_upscale_jitter_late", "true");
    XenosInstallRendererIfRequested(runtime(), {0x1E6883FCCDE1F688ull, 0xA4A965C189287B99ull, 0xB6C9863F710683ECull});
    Ue3InstallShaderPrebuildIfRequested(runtime());  // prebuild v2: after the renderer (it asks whether the renderer wants them)
    XenosInstallQueryLogIfRequested(runtime(), {});
    XenosInstallStateCoverageIfRequested();
    XenosInstallPm4ShadowIfRequested(runtime());
  }
  void OnPostLoadXexImage() override {
    XenosDumpImageIfRequested(runtime());
    Gow3ApplyDataPatchesIfRequested(runtime());
  }
  void OnPreLaunchModule() override { Ue3SetLaunchCommandLineIfRequested(); }
  void OnPostLaunchModule(rex::system::XThread* thread) override { Ue3HoldForShaderPrebuild(thread); XenosHoldGuestIfRequested(thread); }
  // --gow2_fps_bar (job 052, Gears 2 b3a16da): our frame-rate bar on the SDK's overlay layer.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // "Match my monitor" (Gears 2 0086, same lines as Gow2App): layer 0 learns the window and its size, for the
    // renderer's auto scale and exact-size present, and for --gow3_aspect=auto.
    if (rex::ui::Window* w = window()) {
      xenos::display::SetWindow(w->GetNativeWindowHandle());
      xenos::display::SetSurfaceSize(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
    }
    if (rex::ui::Window* uw = window()) XenosSetUiContext(&uw->app_context());  // 0.2.1: the shader note's UI thread
    XenosCreateFpsBarIfRequested(drawer);
    XenosInstallInGameMenu(drawer, window(), runtime());  // patch 0163: COGBox's in-game menu (Back + Start or F1)
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    xenos::display::SetSurfaceSize(pixel_width, pixel_height);
  }
  // --gow3_mute_unfocused (launcher "Mute when the game isn't the active window"; same code as Gears 2's launcher patch
  // 0130): the SDK's audio driver reads audio_mute for every buffer, so flipping it here takes effect at once. A
  // player's own --audio_mute=true is left alone.
  void OnWindowFocusChanged(bool focused) override {
    if (!REXCVAR_GET(gow3_mute_unfocused)) return;
    if (!focused) {
      if (REXCVAR_GET(audio_mute)) return;
      we_muted_ = rex::cvar::SetFlagByName("audio_mute", "true");
    } else if (we_muted_) {
      rex::cvar::SetFlagByName("audio_mute", "false");
      we_muted_ = false;
    }
  }
  void OnShutdown() override {
    Gow3StopMemoryGauge();
    XenosShutdownInGameMenu();  // patch 0163
    XenosDestroyFpsBar();  // before the SDK drops its drawer
  }
  // 0054 (Gears 2): the achievement toast is built only when one is shown, so the SDK's always-on toast window no longer
  // counts as a second presented surface (the NVIDIA overlay showed 2x fps). Same lines as Gow2App.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    if (!imgui_drawer() || !immediate_drawer() || !runtime()) return nullptr;
    return std::make_unique<xenos::LazyAchievementToast>(imgui_drawer(), immediate_drawer(), runtime(), &app_context());
  }

 private:
  bool we_muted_ = false;  // audio_mute was set by us (focus lost), not by the player
};
