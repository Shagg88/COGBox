// Layer 2 (Unreal Engine 3 on the 360): the front-end frame-rate cap (GitHub #101), shared by Gears 2 and Judgment.
// Gears 3 keeps its own copy in its game_patches.cpp (the same listener also drives its per-level commands and split
// layout there). See ue3/menu_fps.h.
#include "ue3/menu_fps.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/filesystem/vfs.h>  // REX_FILESYSTEM_FILE_OPEN_OBSERVER (SDK patch 32)
#include <rex/logging/macros.h>
#include "ue3/shader_prebuild.h"

namespace ue3 {
namespace {

std::atomic<bool> g_menu_front{false};
std::string g_menu_option;  // set once at start-up, before the listener is installed
int32_t g_menu_fps = 0;

bool HasNoCase(std::string_view s, std::string_view needle) {
  return std::search(s.begin(), s.end(), needle.begin(), needle.end(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
         }) != s.end();
}

// A level's persistent package: *_P.xxx (campaign), or MP_*.xxx (versus / Horde maps, not their _LOC_ files; Gears 3
// GitHub #157). GearGame_P.xxx is the game's script package, loaded with every level, so it says nothing.
bool IsLevel(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (HasNoCase(name, "geargame_p.xxx")) return false;
  if (HasNoCase(name, "_p.xxx")) return true;
  return name.size() > 3 && std::tolower(static_cast<unsigned char>(name[0])) == 'm' &&
         std::tolower(static_cast<unsigned char>(name[1])) == 'p' && name[2] == '_' && HasNoCase(name, ".xxx") &&
         !HasNoCase(name, "_loc_");
}

void OnMenuFileOpened(std::string_view path, const std::filesystem::path*) {
  if (!HasNoCase(path, ".xxx")) return;
  if (HasNoCase(path, "gearstart")) {
    if (!g_menu_front.exchange(true)) REXLOG_INFO("{}: front end ({}) - limit {} fps", g_menu_option, path, g_menu_fps);
  } else if (IsLevel(path) && g_menu_front.exchange(false)) {
    REXLOG_INFO("{}: level {} - the player's limit again", g_menu_option, path);
  }
}

}  // namespace

void InstallMenuFpsCap(std::string_view option, int32_t fps) {
  if (fps <= 0) return;
  g_menu_option = option;
  g_menu_fps = fps;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&OnMenuFileOpened);
  REXLOG_INFO("{}: on - the front end runs at {} fps at most (GitHub #101)", g_menu_option, fps);
#else
  REXLOG_WARN("{}: this SDK has no file-open observer (SDK patch 32); menus keep the player's limit", g_menu_option);
#endif
}

bool InMenuFront() { return g_menu_front.load(std::memory_order_relaxed); }

double MenuCappedLimit(int32_t player, double engine, int32_t menu) {
  const double own = player < 0 ? 0.0 : (player > 0 ? double(player) : engine);  // the limit that would apply (0 = none)
  return (own <= 0.0 || own > double(menu)) ? double(menu) : own;
}

}  // namespace ue3
