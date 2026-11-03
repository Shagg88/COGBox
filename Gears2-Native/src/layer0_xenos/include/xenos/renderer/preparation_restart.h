// A fresh per-game cache without deleting the previous cache or releasing its lock.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <xenos/renderer/preparation_progress.h>

namespace xenos::preparation {
inline std::string LowerCacheName(std::string name) {
  for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  return name;
}

inline bool IsCacheArtifact(const std::filesystem::path& path) {
  const auto name = LowerCacheName(path.filename().string());
  if (std::filesystem::is_directory(path)) return name == "ucode";
  const auto ext = LowerCacheName(path.extension().string());
  return name == "pipelines.list" || name == "prebuild-files.txt" || name.rfind("pipelines-", 0) == 0 ||
         name.rfind("shaders.pack", 0) == 0 || name.rfind("prepare-progress", 0) == 0 ||
         name.rfind("prepare-cancel", 0) == 0 || ext == ".dxil" || ext == ".spv" ||
         ext == ".hlsl" || ext == ".errors" || ext == ".err";
}

// Caller holds prepare.lock. Reject broad/unrecognized folders before moving any files.
// The lock and this session's cancellation channel stay in place throughout the transaction.
inline std::filesystem::path RestartCache(const std::filesystem::path& directory, const std::string& session) {
  namespace fs = std::filesystem;
  if (session.empty() || session.size() > 64 ||
      session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
    throw std::runtime_error("Invalid cache restart session.");
  const auto cache = fs::canonical(directory);
  const auto name = LowerCacheName(cache.filename().string());
  if (!fs::is_directory(cache) || (name != "shadercache" && name != "shader-cache"))
    throw std::runtime_error("A fresh restart requires a dedicated shadercache folder.");

  std::vector<fs::path> artifacts;
  for (const auto& item : fs::directory_iterator(cache)) {
    if (item.path().filename() == "prepare.lock" || item.path() == CancelPath(cache, session) ||
        item.path() == ProgressPath(cache, session)) continue;
    if (fs::weakly_canonical(item.path()).parent_path() != cache || !IsCacheArtifact(item.path()))
      throw std::runtime_error("The shader cache contains an unrecognized entry; restart left it unchanged.");
    artifacts.push_back(item.path());
  }
  if (artifacts.empty()) return {};
  std::sort(artifacts.begin(), artifacts.end());
  const auto parent = cache.parent_path() / (cache.filename().string() + "-backups");
  if (fs::weakly_canonical(parent) != parent)
    throw std::runtime_error("The shader cache backup folder points outside its expected location.");
  fs::create_directories(parent);
  const auto backup = parent / ("restart-" + session);
  if (!fs::create_directory(backup)) throw std::runtime_error("This cache backup already exists; restart was cancelled.");

  size_t moved = 0;
  try {
    for (const auto& source : artifacts) {
      fs::rename(source, backup / source.filename());
      ++moved;
    }
  } catch (...) {
    bool restored = true;
    while (moved) {
      const auto& source = artifacts[--moved];
      std::error_code ec;
      fs::rename(backup / source.filename(), source, ec);
      if (ec) restored = false;
    }
    throw std::runtime_error(restored ? "Cache restart could not finish; the previous cache was restored."
                                     : "Cache restart could not finish; retained files are in the shadercache-backups folder.");
  }
  return backup;
}
}  // namespace xenos::preparation
