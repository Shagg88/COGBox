// gow2 - ReXGlue Recompiled Project (from the rexglue init template). Layer 3: Gears of War 2 only.
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>

#include <ue3/config_bundle.h>
#include <ue3/launch.h>
#include <ue3/shader_prebuild.h>
#include <xenos/dev_tools.h>
#include <xenos/display_size.h>
#include <xenos/gpu_system.h>
#include <xenos/lazy_achievement_toast.h>

#include <rex/system/interfaces/graphics.h>
#include <xdk7645/stage1.h>

void Gow2ApplyDataPatchesIfRequested(rex::Runtime* runtime);
REXCVAR_DECLARE(bool, gow2_mute_unfocused);  // game_patches.cpp (patch 0130)

class Gow2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Gow2App>(new Gow2App(ctx, "gow2",
        PPCImageConfig));
  }

  // Stage 6 (notes/51): --gow2_gpu=ours puts our own graphics system in place of the SDK's GPU plugin.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (auto ours = XenosCreateOurGpuSystemIfRequested()) config.graphics = std::move(ours);
  }
  // Logging and options are ready: install the crash report early.
  void OnPostInitLogging() override {
    XenosInstallCrashHandler();
    XenosLogSystemReport("Gears of War 2");  // patch 0131: the PC description at the top of the log
    XenosLatencyInstall();  // patch 0135: NVIDIA Reflex (when --gow2_reflex and the build has it)
  }
  // The runtime (and its input system) exists; the game hasn't started.
  void OnPostSetup() override {
    XenosAddScriptPadIfRequested(runtime());
    Ue3InstallConfigEditsIfRequested(runtime());  // --ue3_ini_edits (notes/64): before the game reads its config
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
    XenosInstallRendererIfRequested(runtime(), {0x1E6883FCCDE1F688ull, 0xA4A965C189287B99ull, 0xB6C9863F710683ECull});
    Ue3InstallShaderPrebuildIfRequested();  // patch 0068: after the renderer (it asks whether the renderer wants them)
    XenosInstallQueryLogIfRequested(runtime(), {-32});
    XenosInstallStateCoverageIfRequested();
    XenosInstallPm4ShadowIfRequested(runtime());
    Gow2InstallResourceCheckIfRequested(runtime());
  }
  // The image is mapped into guest memory; the game hasn't started.
  void OnPostLoadXexImage() override {
    XenosDumpImageIfRequested(runtime());
    Gow2ApplyDataPatchesIfRequested(runtime());
  }
  // Everything is set up and the game is about to start.
  void OnPreLaunchModule() override { Ue3SetLaunchCommandLineIfRequested(); }
  // The game's main thread exists but hasn't run yet.
  void OnPostLaunchModule(rex::system::XThread* thread) override { XenosHoldGuestIfRequested(thread); }

  // Patch 0054: the achievement toast attaches the overlay drawer only while a toast shows (the SDK's kept the
  // presenter repainting at the monitor's rate all the time - the NVIDIA overlay counted those as frames).
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    if (!imgui_drawer() || !immediate_drawer() || !runtime()) return nullptr;
    return std::make_unique<xenos::LazyAchievementToast>(imgui_drawer(), immediate_drawer(), runtime(), &app_context());
  }

  // Other hooks available from rex::ReXApp:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // The window and the SDK's ImGui overlay layer exist.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // "Match my monitor": layer 0 learns the window and its size (xenos/display_size.h).
    if (rex::ui::Window* w = window()) {
      xenos::display::SetWindow(w->GetNativeWindowHandle());
      xenos::display::SetSurfaceSize(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
    }
    XenosCreateFpsBarIfRequested(drawer);
    XenosInstallInGameMenu(drawer, window(), runtime());  // patch 0163: COGBox's in-game menu (Back + Start or F1)
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    xenos::display::SetSurfaceSize(pixel_width, pixel_height);
  }
  void OnShutdown() override {
    XenosShutdownInGameMenu();  // patch 0163
    XenosDestroyFpsBar();
  }
  // Patch 0130: --gow2_mute_unfocused. The SDK checks audio_mute for each audio buffer, so this takes effect at once.
  // Only undoes a mute we set ourselves: a player's own --audio_mute=true stays on.
  void OnWindowFocusChanged(bool focused) override {
    if (!REXCVAR_GET(gow2_mute_unfocused)) return;
    if (!focused) {
      if (REXCVAR_GET(audio_mute)) return;
      we_muted_ = rex::cvar::SetFlagByName("audio_mute", "true");
    } else if (we_muted_) {
      rex::cvar::SetFlagByName("audio_mute", "false");
      we_muted_ = false;
    }
  }
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  bool we_muted_ = false;  // patch 0130
};
