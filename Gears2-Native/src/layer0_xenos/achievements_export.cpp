// Layer 0 (game-free): writes the running game's achievement catalog and icons - see xenos/achievements_export.h.
// Our code (MIT).
#include <xenos/achievements_export.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <xenos/achievements_data.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include <filesystem>
#include <string>
#include <system_error>

// The launcher (cogbox/src/play.cpp) passes --cogbox_dir to exes that contain "COGBox achievement popups" (the popup
// style setting's description, achievement_popup.cpp): every exe with the popups has this setting too.
REXCVAR_DEFINE_STRING(cogbox_dir, "", "COGBox",
                      "The COGBox folder (set by the launcher): achievements are cached in its achievements folder");

xenos::ach::Catalog XenosCatalogFromRuntime(rex::Runtime* runtime) {
  xenos::ach::Catalog catalog;
  if (!runtime || !runtime->kernel_state()) return catalog;
  auto* kernel = runtime->kernel_state();
  catalog.title_id = kernel->title_id();
  if (const xenos::ach::GameDef* game = xenos::ach::FindGame(catalog.title_id)) catalog.game = game->name;
  catalog.language = REXCVAR_GET(user_language);
  for (const auto& info : kernel->achievements().ListAchievements()) {
    xenos::ach::CatalogEntry e;
    e.id = info.id;
    e.gamerscore = info.gamerscore;
    e.flags = info.flags;
    e.image_id = info.image_id;
    e.label = info.label;
    e.description = info.description;
    e.unachieved = info.unachieved_description;
    catalog.achievements.push_back(std::move(e));
  }
  return catalog;
}

xenos::ach::Unlocks XenosUnlocksFromRuntime(rex::Runtime* runtime) {
  xenos::ach::Unlocks unlocks;
  if (!runtime || !runtime->kernel_state()) return unlocks;
  auto& mgr = runtime->kernel_state()->achievements();
  for (const auto& info : mgr.ListAchievements())
    if (mgr.IsUnlocked(info.id)) unlocks[info.id] = mgr.GetUnlockTime(info.id);
  return unlocks;
}

void XenosExportAchievements(rex::Runtime* runtime) {
  const std::string dir = REXCVAR_GET(cogbox_dir);
  if (dir.empty() || !runtime || !runtime->kernel_state()) return;
  auto* kernel = runtime->kernel_state();
  const uint32_t title_id = kernel->title_id();
  const xenos::ach::GameDef* game = xenos::ach::FindGame(title_id);
  if (!game) {
    REXLOG_INFO("achievements: title {:08X} is not a COGBox game; no cache written", title_id);
    return;
  }
  const std::filesystem::path root = rex::to_path(dir);

  const xenos::ach::Catalog catalog = XenosCatalogFromRuntime(runtime);
  const auto list = kernel->achievements().ListAchievements();
  const std::filesystem::path catalog_path = xenos::ach::CatalogPath(root, title_id);
  if (catalog.achievements.empty()) {
    // Every COGBox game has achievements: an empty list means the game's data could not be read this time. Keep the
    // cache from an earlier start rather than replace it with nothing.
    REXLOG_WARN("achievements: the game reported no achievements; {} left as it is", rex::path_to_utf8(catalog_path));
    return;
  }
  if (!xenos::ach::WriteFileAtomic(catalog_path, xenos::ach::FormatCatalog(catalog))) {
    REXLOG_WARN("achievements: could not write {}", rex::path_to_utf8(catalog_path));
    return;
  }

  // Icons: the title's XDBF images (PNG), written once.
  int new_icons = 0;
  const auto db = kernel->title_xdbf();
  if (db.is_valid()) {
    for (const auto& info : list) {
      if (info.image_id == 0) continue;
      const std::filesystem::path icon = xenos::ach::IconPath(root, title_id, info.id);
      std::error_code ec;
      if (std::filesystem::exists(icon, ec)) continue;
      const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, info.image_id);
      if (!block || block.size == 0) continue;
      if (xenos::ach::WriteFileAtomic(icon, std::string(reinterpret_cast<const char*>(block.buffer), block.size))) {
        ++new_icons;
      }
    }
  }
  // legal2-050: the gamer pictures (large: 0x20000 + n) and the title image (0x8000), for COGBox's player icon. The
  // launcher carries no console key, so it can't read them from the encrypted default.xex; the game has them decrypted
  // in memory. <cogbox>\achievements\<TitleID>\pictures\<id in hex>.png, written once (player_icon.cpp reads them).
  int new_pictures = 0;
  if (db.is_valid()) {
    const std::filesystem::path pics = xenos::ach::TitleDir(root, title_id) / "pictures";
    std::vector<uint64_t> ids{0x8000};
    for (uint64_t n = 0; n < 256; ++n) ids.push_back(0x20000 + n);
    for (const uint64_t id : ids) {
      char name[32];
      std::snprintf(name, sizeof name, "%llX.png", static_cast<unsigned long long>(id));
      const std::filesystem::path out = pics / name;
      std::error_code ec;
      if (std::filesystem::exists(out, ec)) continue;
      const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, id);
      if (!block || block.size < 8 || std::memcmp(block.buffer, "\x89PNG", 4) != 0) continue;
      std::filesystem::create_directories(pics, ec);
      if (xenos::ach::WriteFileAtomic(out, std::string(reinterpret_cast<const char*>(block.buffer), block.size))) {
        ++new_pictures;
      }
    }
  }
  REXLOG_INFO("achievements: {} written to {} ({} new icons, {} new pictures)", list.size(), rex::path_to_utf8(catalog_path),
              new_icons, new_pictures);

  // Unlocks go to the rarity provider (NoRarity for now; a cloud tracker later).
  kernel->achievements().RegisterUnlockCallback([title_id](const rex::system::AchievementEvent& event) {
    xenos::ach::CurrentRarityProvider().ReportUnlock(title_id, event.achievement.id, event.unlocked_at_filetime);
  });
}
