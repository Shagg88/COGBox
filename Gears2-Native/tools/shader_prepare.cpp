// COGBox's compiler worker: no ReXApp, guest execution, window, presenter, or render loop.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/filesystem/vfs.h>
#include <ue3/shader_prebuild.h>
#include <xenos/dev_tools.h>
#include <xenos/renderer/preparation_cpu.h>
#include <xenos/renderer/preparation_progress.h>
#include <xenos/renderer/preparation_restart.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <stdexcept>
#include <windows.h>

REXCVAR_DEFINE_BOOL(gow2_renderer_prepare_reset, false, "Gears2",
                    "Start Deep preparation with a fresh per-game COGBox cache; keep the previous cache as a backup.");
REXCVAR_DEFINE_INT32(gow2_renderer_prepare_priority, 1, "Gears2",
                     "Standalone compiler process priority: -1 below normal, 0 normal, 1 above normal (default), 2 high.");

static void SetPreparationDefault(const char* name, int value) {
  // Keep explicit advanced settings, including command-line overrides used for profiling.
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagFromCommandLine(name, std::to_string(value));
}

static void ConfigureCpu() {
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  const uint64_t available = GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
  const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
  const auto plan = xenos::preparation::SelectCpuPlan(logical, available);
  SetPreparationDefault("gow2_renderer_pipeline_threads", int(plan.pipeline_workers));
  SetPreparationDefault("gow2_renderer_pipeline_thread_priority", 0);
  SetPreparationDefault("gow2_renderer_shader_prebuild_workers", int(plan.pipeline_workers));
  SetPreparationDefault("gow2_renderer_shader_prebuild_priority", 0);
  SetPreparationDefault("ue3_shader_prebuild_threads", int(plan.package_readers));
  SetPreparationDefault("ue3_shader_prebuild_thread_priority", 0);

  const int priority = std::stoi(rex::cvar::GetFlagByName("gow2_renderer_prepare_priority"));
  if (priority < -1 || priority > 2) throw std::runtime_error("Compiler priority must be between -1 and 2.");
  const DWORD classes[] = {BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS,
                            ABOVE_NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS};
  const char* names[] = {"below normal", "normal", "above normal", "high"};
  if (!SetPriorityClass(GetCurrentProcess(), classes[priority + 1]))
    REXLOG_WARN("shader preparation: could not change process priority (Windows error {})", GetLastError());
  REXLOG_INFO("shader preparation CPU: {} logical processors; priority {} (actual class {}); "
              "pipeline workers {}, driver warm workers {}, package readers {}; {} MiB available",
              logical, names[priority + 1], GetPriorityClass(GetCurrentProcess()),
              rex::cvar::GetFlagByName("gow2_renderer_pipeline_threads"),
              rex::cvar::GetFlagByName("gow2_renderer_shader_prebuild_workers"),
              rex::cvar::GetFlagByName("ue3_shader_prebuild_threads"), available / (1024 * 1024));
}

static std::string Utf8(const wchar_t* s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string result(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), n, nullptr, nullptr);
  result.pop_back();
  return result;
}

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path cache, log;
  bool owns_cache = false;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = Utf8(argv[i]);
      if (arg == "--help") {
        std::puts("COGBox shader compiler: --ue3_shader_prebuild_at_start=<game;update> "
                  "--gow2_renderer_recipe_pack=<pack> --gow2_shader_cache_dir=<cache> "
                  "[--gow2_renderer_prepare_deep=true] [--gow2_renderer_prepare_reset=true] "
                  "[--gow2_renderer_prepare_priority=1] [renderer settings]");
        return 0;
      }
      const auto eq = arg.find('=');
      if (arg.rfind("--", 0) != 0 || eq == std::string::npos) continue;
      const auto name = arg.substr(2, eq - 2), value = arg.substr(eq + 1);
      if (name == "log_file") { log = std::filesystem::u8path(value); continue; }
      // The launcher supplies a game's normal arguments so translation/cache settings match play.
      // Game, input, save, and network options have no role in a compiler process.
      if (name.rfind("gow2_renderer_", 0) == 0 || name.rfind("gow2_shader_", 0) == 0 ||
          name.rfind("ue3_shader_", 0) == 0 || name == "gow2_dxc_dir") {
        if (rex::cvar::GetFlagInfo(name) && !rex::cvar::SetFlagFromCommandLine(name, value))
          throw std::runtime_error("Invalid shader setting: " + name);
      }
    }
    cache = std::filesystem::u8path(rex::cvar::GetFlagByName("gow2_shader_cache_dir"));
    if (cache.empty()) throw std::runtime_error("A shader cache folder is required.");
    auto session = rex::cvar::GetFlagByName("gow2_renderer_prepare_session");
    if (session.empty()) {
      session = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
      rex::cvar::SetFlagFromCommandLine("gow2_renderer_prepare_session", session);
    }
    if (session.size() > 64 || session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
      throw std::runtime_error("Invalid preparation session ID.");
    std::filesystem::create_directories(cache);
    rex::InitLogging(log);
    const auto roots = rex::cvar::GetFlagByName("ue3_shader_prebuild_at_start");
    if (roots.empty()) throw std::runtime_error("No game folders were supplied.");
    for (size_t p = 0; p < roots.size();) {
      auto q = roots.find(';', p);
      if (q == std::string::npos) q = roots.size();
      const auto dir = std::filesystem::u8path(roots.substr(p, q - p));
      if (dir.empty() || !std::filesystem::is_directory(dir)) throw std::runtime_error("A game folder is unavailable.");
      p = q + 1;
    }
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(rex::cvar::GetFlagByName("gow2_renderer_recipe_pack"))))
      throw std::runtime_error("The game's recipe pack is missing.");
    rex::cvar::SetFlagFromCommandLine("gow2_renderer_prepare_all", "true");
    rex::cvar::SetFlagFromCommandLine("ue3_shader_prebuild", "true");
    rex::cvar::SetFlagFromCommandLine("gow2_renderer_shader_backend", "dxbc");
    const bool restart = rex::cvar::GetFlagByName("gow2_renderer_prepare_reset") == "true";
    if (restart) rex::cvar::SetFlagFromCommandLine("gow2_renderer_prepare_deep", "true");
    if (rex::cvar::GetFlagByName("gow2_renderer_prepare_deep") == "true" &&
        rex::cvar::GetFlagByName("gow2_renderer_api") != "d3d12")
      throw std::runtime_error("Deep driver preparation currently requires DirectX 12. Use Quick for Vulkan.");
    ConfigureCpu();
    // Serialize compiler instances; the launcher also excludes Play while preparation runs.
    const auto lock_path = cache / "prepare.lock";
    HANDLE cache_lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (cache_lock == INVALID_HANDLE_VALUE) throw std::runtime_error("Shader preparation is already using this cache.");
    owns_cache = true;
    if (restart) {
      const auto backup = xenos::preparation::RestartCache(cache, session);
      REXLOG_INFO("shader preparation: fresh Deep restart; previous cache backup: {}",
                  backup.empty() ? std::string("cache was already empty") : backup.string());
    }
    xenos::preparation::WriteProgress(xenos::preparation::ProgressPath(cache, session), {});
    if (!XenosInitStandalonePreparation()) throw std::runtime_error("The shader compiler device or recipe pack could not be initialized.");
    Ue3InstallShaderPrebuildIfRequested();
    for (;;) {
      XenosStepStandalonePreparation();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Shader preparation failed: %s\n", e.what());
    if (owns_cache) {
      xenos::preparation::Progress p;
      p.state = "failed";
      xenos::preparation::WriteProgress(xenos::preparation::ProgressPath(cache, rex::cvar::GetFlagByName("gow2_renderer_prepare_session")), p);
    }
    rex::FlushLogging();
    return 2;
  }
}
