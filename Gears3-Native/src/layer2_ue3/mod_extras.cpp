// Layer 2 (Unreal Engine 3): --ue3_mods extras (ue3/mod_extras.h) on modcore (modcore/, MIT, from KiltedKernel's
// Gears 3 port, where the same rules run its mods folder). Our code (MIT).
#include <ue3/mod_extras.h>

#include <modcore/modcore.h>

#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/string/utf8.h>

#include <fstream>
#include <iterator>
#include <mutex>

namespace ue3 {
namespace {
namespace fs = std::filesystem;

std::mutex g_mutex;
std::vector<modcore::Manifest> g_mods;        // the --ue3_mods folders' manifests, load order, problems set
std::vector<std::string> g_content;
uint64_t g_fingerprint = 0;

std::vector<fs::path> SplitSpec(const std::string& spec) {
  std::vector<fs::path> out;
  size_t at = 0;
  while (at <= spec.size()) {
    size_t end = spec.find(';', at);
    if (end == std::string::npos) end = spec.size();
    std::string one = spec.substr(at, end - at);
    at = end + 1;
    while (!one.empty() && (one.back() == ' ' || one.back() == '"')) one.pop_back();
    while (!one.empty() && (one.front() == ' ' || one.front() == '"')) one.erase(0, 1);
    if (!one.empty()) out.push_back(rex::to_path(one));
  }
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = c == '/' ? '\\' : char(std::tolower(uint8_t(c)));
  return s;
}
}  // namespace

std::vector<fs::path> ActiveModFolders(const std::string& spec) {
  std::vector<fs::path> dirs;
  std::error_code ec;
  for (const fs::path& d : SplitSpec(spec)) {
    if (!fs::is_directory(d, ec)) {
      REXLOG_WARN("ue3_mods: {} is not a folder - skipped", rex::path_to_utf8(d));
      continue;
    }
    dirs.push_back(d);
  }
  std::lock_guard lock(g_mutex);
  g_mods = modcore::FromFolders(dirs, "gears3");
  g_content.clear();
  std::vector<fs::path> active;
  for (const modcore::Manifest& m : g_mods) {
    if (!m.Active()) {
      REXLOG_WARN("ue3_mods: {} ({}) left out: {}", m.name, rex::path_to_utf8(m.dir), m.enabled ? m.problem : "enabled = false in its mod.ini");
      continue;
    }
    active.push_back(m.dir);
    for (const std::string& c : m.content)
      if (std::find(g_content.begin(), g_content.end(), c) == g_content.end()) g_content.push_back(c);
  }
  const modcore::NetSet net = modcore::ForMultiplayer(g_mods);
  g_fingerprint = net.fingerprint;
  if (net.fingerprint) REXLOG_INFO("ue3_mods: co-op / System Link mod fingerprint {:016X} ({})", net.fingerprint, net.summary);
  return active;
}

std::string ModGameKey(const std::string& rel_in_mod, const std::string& bundle_rel) {
  const std::string key = Lower(rel_in_mod);
  if (key == "config" || key.rfind("config\\", 0) == 0) return {};       // fragments (MergeModConfigFragments)
  if (key.find('\\') != std::string::npos) return key;                   // laid out like the game's folder
  // a bare file at the mod's top: Xbox 360 mods are shared that way
  const std::string bundle = Lower(bundle_rel);
  const size_t cut = bundle.find_last_of('\\');
  std::string mapped = modcore::GamePath(key, cut == std::string::npos ? bundle : bundle.substr(cut + 1));
  return mapped;
}

int MergeModConfigFragments(const fs::path& game_root, const std::string& bundle_rel, const fs::path& out_dir,
                            std::map<std::string, fs::path>& files) {
  std::vector<std::pair<std::string, fs::path>> fragments;
  {
    std::lock_guard lock(g_mutex);
    for (const modcore::Manifest& m : g_mods)
      if (m.Active())
        for (const fs::path& f : modcore::Fragments(m)) fragments.emplace_back(m.name, f);
  }
  if (fragments.empty()) return 0;
  std::error_code ec;
  const fs::path src_dir = game_root / rex::to_path(bundle_rel);
  fs::create_directories(out_dir, ec);
  int written = 0;
  for (const auto& e : fs::directory_iterator(src_dir, ec)) {
    const std::string name = rex::path_to_utf8(e.path().filename());
    const std::string low = Lower(name);
    if (!e.is_regular_file() || low.rfind("coalesced_", 0) != 0 || low.size() < 5 || low.substr(low.size() - 4) != ".bin") continue;
    const std::string key = Lower(bundle_rel + "\\" + name);
    const auto modded = files.find(key);
    std::ifstream in(modded != files.end() ? modded->second : e.path(), std::ios::binary);
    const std::vector<uint8_t> base((std::istreambuf_iterator<char>(in)), {});
    const bool log_this = low == "coalesced_int.bin";
    const std::vector<uint8_t> merged = modcore::MergeConfig(base, fragments, [&](const std::string& line) {
      if (log_this) REXLOG_INFO("ue3_mods: {}: {}", name, line);
    });
    if (merged.empty()) {
      REXLOG_WARN("ue3_mods: {} is not a config bundle - mods' config fragments not applied to it", name);
      continue;
    }
    const fs::path dst = out_dir / name;
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(merged.data()), std::streamsize(merged.size()));
    if (!o) {
      REXLOG_WARN("ue3_mods: could not write {}", rex::path_to_utf8(dst));
      continue;
    }
    files[key] = dst;
    ++written;
  }
  REXLOG_INFO("ue3_mods: {} config fragment(s) merged into {} bundle(s)", fragments.size(), written);
  return written;
}

const std::vector<std::string>& ModContentPackages() { return g_content; }

uint64_t ModFingerprint() { return g_fingerprint; }

}  // namespace ue3
