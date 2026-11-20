// GitHub #389 (and #177): a game folder whose path has a non-ASCII letter ("D:\Temporário\...") ended at start with
// "--game_data_root does not exist". The path switches hold UTF-8 text, but the SDK's application start-up assigned
// them straight to std::filesystem::path, which on Windows reads a std::string in the ANSI code page. SDK patch 62
// fixes that in the SDK, but the start-up code the games link was not rebuilt with it (job 2124b, 19 Nov: the log file
// under the accented folder was written - that part is in the runtime DLL - while the game folder was refused).
// Called from every game's OnConfigurePaths, which runs right after the SDK filled the paths in: each path given on
// the command line is taken again as UTF-8. The same result when the SDK already did it; ASCII paths are unchanged.
#pragma once

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

namespace xenos {

inline void Utf8PathArguments(rex::PathConfig& paths) {
  auto take = [](const char* name, std::filesystem::path& out) {
    const std::string value = rex::cvar::GetFlagByName(name);
    if (value.empty()) return false;
    out = rex::to_path(value);
    return true;
  };
  take("game_data_root", paths.game_data_root);
  const bool user = take("user_data_root", paths.user_data_root);
  take("update_data_root", paths.update_data_root);
  // without --cache_root the SDK uses <user data>/cache, built from the path it read wrongly
  if (!take("cache_root", paths.cache_root) && user) paths.cache_root = paths.user_data_root / "cache";
  take("metadata_root", paths.metadata_root);
}

}  // namespace xenos
