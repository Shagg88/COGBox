// gowj - ReXGlue Recompiled Project. Layer 3: Gears of War: Judgment only. Copied from Gears 3's gow3_app.h (D5);
// Gears 3's own game patches and its per-game renderer settings are left out until Judgment's are measured.
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

REXCVAR_DECLARE(bool, gowj_mute_unfocused);  // game_patches.cpp

void GowjAddBuiltInIniEdits();  // game_patches.cpp: --gowj_trilinear, --gowj_fast_mip_fade
void GowjStartMemoryGauge();    // game_patches.cpp: --gowj_memory_log_seconds (physical memory every N s)
void GowjStopMemoryGauge();     // stops it before the kernel goes away
void GowjApplyUltrawideIfRequested(rex::Runtime* runtime);  // game_patches.cpp: --gowj_ultrawide (0.2.0)


class GowjApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GowjApp>(new GowjApp(ctx, "gowj", PPCImageConfig));
  }

  // Our own graphics system in place of the SDK's GPU plugin, when asked (layer 0 option).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (auto ours = XenosCreateOurGpuSystemIfRequested()) config.graphics = std::move(ours);
  }
  // XenosLogSystemReport (Gears 2 launcher patch 0131, layer 0 system_report.cpp, picked up by our layer 0 glob): one
  // read-only report of Windows / CPU / memory / graphics cards + drivers / screens / controllers / build, for problem
  // reports.
  void OnPostInitLogging() override {
    XenosInstallCrashHandler();
    XenosLogSystemReport("Gears of War: Judgment");
    XenosLatencyInstall();  // Gears 2 patch 0135: Reflex / Anti-Lag 2 (only with --gow2_reflex and when the build has it)
  }
  void OnPostSetup() override {
    CogMouseLookInstall(window());  // 0.2.1: native mouse look (mouse_look.cpp)
    XenosAddScriptPadIfRequested(runtime());
    GowjAddBuiltInIniEdits();  // before the bundle is edited
    Ue3InstallConfigEditsIfRequested(runtime());  // --ue3_ini_edits (job 050): before the game reads its config
    GowjApplyUltrawideIfRequested(runtime());     // --gowj_ultrawide: the two aspect floats, the present shape
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
    // Judgment's settings for the shared renderer (Gears 2's layer 0), each set only while it still has its compiled-in
    // default, so a command line, config file or environment variable always wins:
    //  - memexport: Judgment's vertex shaders write guest memory like Gears 3's (job 009 gameplay: "skipped 90: vertex
    //    shader memexport", rainbow skin; Gears 3 job 042 fixed the same look). Job 010 (10:57) confirmed: the menus route is clean with it.
    //  - DLSS / FSR / XeSS (Shagg 20:53 "Just make them work please"): Gears 3's values, which job 019 C verified on
    //    Judgment (every frame upscaled, camera found every frame, the object vote agreed, clean shots). An object's
    //    object-to-world rows are in c7-c10, the camera's world position is in c5, and the lit scene is 10:10:10:2 UNORM.
    //    Gears 2 job 487 log: "camera 0 msaa 0 format 1 (scene needs 2)". Without these the upscaler never started.
    //    Scene formats 3 (16-bit float and 10:10:10:2): exactly what job 019 C ran and verified. gow3.exe uses 2, and
    //    66c5d5b briefly used 2 as well, but that value was never verified on Judgment (Gears 2 mail 21:04).
    const auto game_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    };
    game_default("gow2_upscale_object_constants", "7");
    game_default("gow2_upscale_camera_constant", "5");
    game_default("gow2_upscale_scene_formats", "3");
    game_default("gow2_renderer_memexport", "true");
    game_default("gow2_renderer_resolution_scale_threshold", "1120");
    //  - first-visit freezes: the DXBC shader path (shaders ready at once; HLSL spent 221 s of compiler time on University's
    //    first visit, job 017). Holding / waiting for pipelines is layer 0's call: since Gears 2 patch 0132 (Shagg 21:46-22:07,
    //    "brief pop-in", stutter rules lifted) layer 0 defaults to never waiting on the driver - a draw whose pipeline is
    //    still building is skipped and pops in. Judgment no longer sets pipeline_wait_ms / play_hold_draws (it set 0 / 8,
    //    and 8 would have brought the hold back - Shagg's 22:00 session froze up to 1 s per backlog).
    game_default("gow2_renderer_shader_backend", "dxbc");
    XenosInstallRendererIfRequested(runtime(), {0x1E6883FCCDE1F688ull, 0xA4A965C189287B99ull, 0xB6C9863F710683ECull});
    Ue3InstallShaderPrebuildIfRequested(runtime());  // prebuild v2: after the renderer (it asks whether the renderer wants them)
    XenosInstallQueryLogIfRequested(runtime(), {});
    XenosInstallStateCoverageIfRequested();
    XenosInstallPm4ShadowIfRequested(runtime());
  }
  void OnPostLoadXexImage() override {
    XenosDumpImageIfRequested(runtime());
    GowjStartMemoryGauge();
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
  // --gowj_mute_unfocused (launcher item 8, Gears 2 0130): the SDK's SDL driver reads audio_mute for every buffer, so
  // this takes effect at once. A mute the player set themselves is left alone.
  void OnWindowFocusChanged(bool focused) override {
    if (!REXCVAR_GET(gowj_mute_unfocused)) return;
    if (!focused) {
      if (REXCVAR_GET(audio_mute)) return;
      we_muted_ = rex::cvar::SetFlagByName("audio_mute", "true");
    } else if (we_muted_) {
      rex::cvar::SetFlagByName("audio_mute", "false");
      we_muted_ = false;
    }
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    xenos::display::SetSurfaceSize(pixel_width, pixel_height);
  }
  void OnShutdown() override {
    GowjStopMemoryGauge();
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
  bool we_muted_ = false;  // audio_mute was set by OnWindowFocusChanged, not by the player
};
