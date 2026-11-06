// gow2 - ReXGlue Recompiled Project (from the rexglue init template). Layer 3: Gears of War 2 only.
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include "mouse_look.h"
#include "main_menu_exit.h"

namespace rex::ui {
class WindowedAppContext;
}
void XenosSetUiContext(rex::ui::WindowedAppContext* ui);  // layer 0 fps_bar.cpp (0.2.1)
#include <rex/audio/flags.h>
#include <rex/cvar.h>

#include <ue3/config_bundle.h>
#include <ue3/launch.h>
#include <ue3/shader_prebuild.h>
#include <xenos/dev_tools.h>
#include <xenos/display_size.h>
#include <xenos/gpu_system.h>
#include <xenos/achievements_export.h>
#include <xenos/achievement_popup_assets.h>
#include <xenos/achievement_popup.h>

#include <rex/system/interfaces/graphics.h>
#include <xdk7645/stage1.h>

void Gow2ApplyDataPatchesIfRequested(rex::Runtime* runtime);
void Gow2InstallLocLogIfRequested();  // game_patches.cpp (GitHub #90)
void Gow2AddTestIniEdits();  // game_patches.cpp: --cogbox_test_skip_movies
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
    CogMouseLookInstall(window());  // 0.2.1: native mouse look (mouse_look.cpp)
    XenosAddScriptPadIfRequested(runtime());
    Gow2AddTestIniEdits();  // before the bundle is edited
    Ue3InstallConfigEditsIfRequested(runtime());  // --ue3_ini_edits (notes/64): before the game reads its config
    XenosInstallDrawStatsIfRequested();
    XenosInstallPrimDetailIfRequested();
    XenosInstallFrameCaptureIfRequested();
    XenosInstallFrameTimesIfRequested();
    XenosInstallProfilerIfRequested();
    XenosInstallDrawLogIfRequested();
    XenosInstallResolveLogIfRequested();
    XenosInstallSmallResolveLogIfRequested(runtime());  // GitHub #185
    XenosInstallBufferDumpIfRequested(runtime());
    XenosInstallShaderDumpIfRequested();
    XenosInstallShaderWatchIfRequested();
    // Notes/80 s.6 / s.11: Gears 2's settings for the shared renderer, each set only while it still has its compiled-in
    // default (a command line, config file or environment variable always wins), as Gow3App does:
    //  - the upscalers' depth copied at the scene depth resolve (Gears 2 clears it before the scene colour resolve);
    //  - the characters' own motion from Gears 2's velocity pass (its c5-c12 layout).
    const auto game_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    };
    game_default("gow2_upscale_depth_at_resolve", "true");
    game_default("gow2_upscale_character_motion", "true");
    XenosInstallRendererIfRequested(runtime(), {0x1E6883FCCDE1F688ull, 0xA4A965C189287B99ull, 0xB6C9863F710683ECull});
    Gow2InstallLocLogIfRequested();  // GitHub #90: before the game opens its localized packages
    Gow2InstallMainMenuExitIfRequested();
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
  // Achievements part 2: the popups' fonts (Segoe UI from the PC; CJK from the cached achievement lists).
  void OnConfigureFonts(ImFontAtlas* atlas) override { XenosConfigureAchievementFonts(atlas); }
  void OnPreLaunchModule() override {
    XenosExportAchievements(runtime());  // achievements part 1: the shared catalog + icons
    Ue3SetLaunchCommandLineIfRequested();
  }
  // The game's main thread exists but hasn't run yet.
  void OnPostLaunchModule(rex::system::XThread* thread) override { XenosHoldGuestIfRequested(thread); }

  // Patch 0054: the achievement toast attaches the overlay drawer only while a toast shows (the SDK's kept the
  // presenter repainting at the monitor's rate all the time - the NVIDIA overlay counted those as frames).
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    // Achievements part 2: COGBox's own popups (Xbox 360 / Xbox / PS5); "Hide achievement popups" is checked per
    // unlock, so "Hide on-screen notes" no longer hides them.
    if (!imgui_drawer() || !immediate_drawer() || !runtime()) return nullptr;
    return std::make_unique<xenos::AchievementPopup>(imgui_drawer(), immediate_drawer(), runtime(), &app_context());
  }

  // Other hooks available from rex::ReXApp:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // The window and the SDK's ImGui overlay layer exist.
  // Achievements part 3: no SDK achievements window; F7 opens COGBox's in-game menu on its Achievements page.
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override { return nullptr; }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // "Match my monitor": layer 0 learns the window and its size (xenos/display_size.h).
    if (rex::ui::Window* w = window()) {
      xenos::display::SetWindow(w->GetNativeWindowHandle());
      xenos::display::SetSurfaceSize(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
    }
    if (rex::ui::Window* uw = window()) XenosSetUiContext(&uw->app_context());  // 0.2.1: the shader note's UI thread
    XenosCreateFpsBarIfRequested(drawer);
    XenosInstallAoDebugKeys(drawer);  // notes/80 s.12 (--gow2_ao_debug_keys)
    XenosInstallInGameMenu(drawer, immediate_drawer(), window(), runtime());  // patch 0163: COGBox's in-game menu (Back + Start or F1)
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    xenos::display::SetSurfaceSize(pixel_width, pixel_height);
  }
  void OnShutdown() override {
    XenosShutdownInGameMenu();  // patch 0163
    XenosDestroyFpsBar();
    XenosShutdownAoDebugKeys();  // notes/80
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
