// Shared UE3 startup shader discovery. Our code (MIT).
#include <ue3/shader_prebuild.h>
#include <ue3/shader_package_scan.h>
#include <ue3/shader_package_inventory.h>
#include <xenos/dev_tools.h>
#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(ue3_shader_prebuild, true, "Gears2", "Prepare shaders from installed UE3 packages (D3D12 DXBC)");
REXCVAR_DEFINE_BOOL(ue3_shader_prebuild_startup, true, "Gears2",
                    "Scan the entire game, update, DLC and enabled mods before starting the guest; reuse cached "
                    "package indexes and driver pipelines, with a responsive progress display");
namespace {
namespace fs = std::filesystem;
using namespace ue3::shader_prebuild;
struct Prebuild {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<fs::path> queue;
  std::unordered_set<std::string> opened;
  std::vector<fs::path> roots;
  fs::path cache;
  PackageInventory inventory;
  uint64_t unchanged = 0;
  bool startup = false;
  std::atomic<bool> ready{false};
  uint64_t files = 0, indexes = 0, failures = 0, records = 0;
};
Prebuild* g_prebuild = nullptr;  // process lifetime, like the renderer
std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool Candidate(const fs::path& path) {
  const std::string name = Lower(path.filename().string());
  return name.ends_with(".xxx") || name.ends_with(".upk") || name.ends_with(".u") ||
         (name.ends_with(".bin") && name.find("shadercache") != std::string::npos);
}
void Queue(const fs::path& path) {
  if (!Candidate(path)) return;
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(path, ec);
  const std::string key = Lower((ec ? path.lexically_normal() : canonical).string());
  {
    std::lock_guard<std::mutex> lock(g_prebuild->mutex);
    // Full paths: DLC and mods can have different packages with the same basename.
    if (!g_prebuild->opened.insert(key).second) return;
    g_prebuild->queue.push_back(path);
  }
  g_prebuild->cv.notify_one();
}
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
void OnFileOpened(std::string_view, const fs::path* path) { if (g_prebuild && path) Queue(*path); }
#endif
struct Record { bool vertex; uint32_t variant; std::vector<uint8_t> bytes; };
constexpr uint64_t kIndexVersion = 0x434F475348445204ull;
// Content-keyed and checksummed: damaged or interrupted caches are re-scanned in full.
bool LoadIndex(const fs::path& path, uint64_t input_hash, std::vector<Record>& records) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  const auto size = f ? f.tellg() : std::streampos(-1);
  if (size < 32 || size > 512 * 1024 * 1024) return false;
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) return false;
  uint64_t header[4];
  std::memcpy(header, data.data(), sizeof(header));
  if (header[0] != kIndexVersion || header[1] != input_hash || header[2] > 1000000 ||
      XXH3_64bits(data.data() + 32, data.size() - 32) != header[3]) return false;
  std::vector<Record> loaded;
  size_t offset = 32;
  for (uint64_t i = 0; i < header[2]; ++i) {
    uint32_t words[2];
    if (offset + sizeof(words) > data.size()) return false;
    std::memcpy(words, data.data() + offset, sizeof(words));
    offset += sizeof(words);
    if (!words[1] || (words[1] & 3) || words[1] > 262144 || words[0] > 3 || words[1] > data.size() - offset) return false;
    loaded.push_back({bool(words[0] & 1), words[0] >> 1,
                     std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + words[1])});
    offset += words[1];
  }
  if (offset != data.size()) return false;
  records = std::move(loaded);
  return true;
}
void SaveIndex(const fs::path& path, uint64_t input_hash, const std::vector<Record>& records) {
  std::vector<uint8_t> payload;
  for (const auto& record : records) {
    const uint32_t words[] = {uint32_t(record.vertex) | record.variant << 1, uint32_t(record.bytes.size())};
    const auto* p = reinterpret_cast<const uint8_t*>(words);
    payload.insert(payload.end(), p, p + sizeof(words));
    payload.insert(payload.end(), record.bytes.begin(), record.bytes.end());
  }
  const uint64_t header[] = {kIndexVersion, input_hash, records.size(), XXH3_64bits(payload.data(), payload.size())};
  const fs::path temp = path.wstring() + L".tmp";
  std::ofstream f(temp, std::ios::binary | std::ios::trunc);
  if (!f) return;
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  f.write(reinterpret_cast<const char*>(payload.data()), payload.size());
  f.close();
  if (!f) return;
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) { fs::remove(path, ec); fs::rename(temp, path, ec); }
}
void ReadPackage(const fs::path& path) {
  const auto t0 = std::chrono::steady_clock::now();
  PackageFileStamp stamp{};
  const bool stamped = PackageStamp(path, stamp);
  uint64_t hash = 0;
  auto index_path = [&](uint64_t value) {
    std::ostringstream name; name << std::hex << kIndexVersion << '-' << value << ".index";
    return g_prebuild->cache / name.str();
  };
  std::vector<Record> records;
  const bool unchanged = stamped && g_prebuild->inventory.Find(path, stamp, hash) &&
                         LoadIndex(index_path(hash), hash, records);
  if (unchanged) { ++g_prebuild->indexes; ++g_prebuild->unchanged; }
  else {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const auto size = f ? f.tellg() : std::streampos(-1);
    if (size <= 0 || size > (std::streamoff(1) << 31)) {
      ++g_prebuild->failures;
      REXLOG_WARN("ue3_shader_prebuild: could not read {} (missing, empty or too large)", path.string());
      return;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) {
      ++g_prebuild->failures;
      REXLOG_WARN("ue3_shader_prebuild: incomplete package read: {}", path.string());
      return;
    }
    hash = XXH3_64bits(data.data(), data.size());
    const fs::path index = index_path(hash);
    const bool cached = LoadIndex(index, hash, records);
    bool complete = true;
    if (cached) ++g_prebuild->indexes;
    else {
      std::unordered_set<uint64_t> seen[2];
      PackageScanner scanner([&](bool vertex, const uint8_t* code, uint32_t length, uint32_t variant) {
        if (seen[vertex ? 1 : 0].insert(XXH3_64bits(code, length)).second)
          records.push_back({vertex, variant, std::vector<uint8_t>(code, code + length)});
      });
      const ScanResult result = scanner.Scan(data);
      if (result.failed_chunks) {
        complete = false;
        ++g_prebuild->failures;
        REXLOG_WARN("ue3_shader_prebuild: {}: {} compressed chunks failed; index not saved, live fallback enabled",
                    path.string(), result.failed_chunks);
      } else SaveIndex(index, hash, records);
    }
    PackageFileStamp after{};
    if (complete && stamped && PackageStamp(path, after) && stamp == after)
      g_prebuild->inventory.Remember(path, stamp, hash);
  }
  for (const auto& record : records)
    XenosPrebuildShader(record.vertex, record.bytes.data(), uint32_t(record.bytes.size()), record.variant,
                        XXH3_64bits(record.bytes.data(), record.bytes.size()));
  ++g_prebuild->files;
  g_prebuild->records += records.size();
  if (!records.empty())
    REXLOG_INFO("ue3_shader_prebuild: {}: {} shader records, {}, {:.0f} ms", path.string(), records.size(),
                unchanged ? "unchanged file, cached index" : "validated package",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}
void ReaderThread() {
  const auto t0 = std::chrono::steady_clock::now();
  g_prebuild->inventory.Load(g_prebuild->cache / "package-files.bin");
  if (g_prebuild->startup) {
    XenosBeginStartupPrebuild();
    XenosSetShaderScanProgress(true, 0, 0);
    for (const auto& root : g_prebuild->roots) {
      if (root.empty()) continue;
      std::error_code ec;
      if (!fs::exists(root, ec)) {
        if (ec) { ++g_prebuild->failures; REXLOG_WARN("ue3_shader_prebuild: {}: {}", root.string(), ec.message()); }
        continue;
      }
      fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
      for (; it != end; it.increment(ec)) {
        if (ec) { ++g_prebuild->failures; ec.clear(); continue; }
        const bool regular = it->is_regular_file(ec);
        if (ec) {
          ++g_prebuild->failures;
          REXLOG_WARN("ue3_shader_prebuild: {}: {}", it->path().string(), ec.message());
          ec.clear();
        } else if (regular) Queue(it->path());
      }
      if (ec) {
        ++g_prebuild->failures;
        REXLOG_WARN("ue3_shader_prebuild: could not finish enumerating {}: {}", root.string(), ec.message());
      }
    }
    std::lock_guard<std::mutex> lock(g_prebuild->mutex);
    REXLOG_INFO("ue3_shader_prebuild: startup enumerated {} package paths across {} roots",
                g_prebuild->opened.size(), g_prebuild->roots.size());
  }
  bool startup_finished = !g_prebuild->startup;
  uint64_t scanned = 0, scan_total = 0;
  { std::lock_guard<std::mutex> lock(g_prebuild->mutex); scan_total = g_prebuild->queue.size(); }
  for (;;) {
    fs::path path;
    {
      std::unique_lock<std::mutex> lock(g_prebuild->mutex);
      if (!g_prebuild->queue.empty()) {
        path = std::move(g_prebuild->queue.front());
        g_prebuild->queue.pop_front();
      } else if (startup_finished) {
        g_prebuild->cv.wait(lock, [] { return !g_prebuild->queue.empty(); });
        continue;
      }
    }
    if (!path.empty()) {
      if (!startup_finished) XenosSetShaderScanProgress(true, scanned, scan_total);
      try { ReadPackage(path); }
      catch (const std::exception& e) {
        ++g_prebuild->failures;
        REXLOG_WARN("ue3_shader_prebuild: {}: {} (live fallback enabled)", path.string(), e.what());
      }
      ++scanned;
      if ((scanned % 64) == 0) g_prebuild->inventory.Save(g_prebuild->cache / "package-files.bin");
    } else if (!startup_finished && XenosFinishStartupPrebuild()) {
      g_prebuild->inventory.Save(g_prebuild->cache / "package-files.bin");
      REXLOG_INFO("ue3_shader_prebuild: reused {} unchanged packages without rereading game data", g_prebuild->unchanged);
      startup_finished = true;
      XenosSetShaderScanProgress(false, 0, 0);
      XenosSetLoadHold(false, 0, 0);
      XenosSetShaderStartupPhase(cogbox::shader_progress::ShaderPreparationPhase::kInactive);
      REXLOG_INFO("ue3_shader_prebuild: startup ready: {} packages, {} cached indexes, {} shader records, "
                  "{} package failures, {:.1f} s; runtime-created variants retain live fallback",
                  g_prebuild->files, g_prebuild->indexes, g_prebuild->records, g_prebuild->failures,
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      g_prebuild->ready.store(true, std::memory_order_release);
    } else if (!startup_finished) {
      XenosSetShaderScanProgress(false, 0, 0);  // now show remaining driver jobs
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
}
}  // namespace

void Ue3InstallShaderPrebuildIfRequested(rex::Runtime* runtime) {
  if (g_prebuild || !runtime || !REXCVAR_GET(ue3_shader_prebuild) || !XenosPrebuildWanted()) return;
  g_prebuild = new Prebuild();
  g_prebuild->startup = REXCVAR_GET(ue3_shader_prebuild_startup);
  g_prebuild->ready.store(!g_prebuild->startup);
  g_prebuild->cache = runtime->cache_root() / "shader-package-index-v4";
  if (runtime->cache_root().empty()) g_prebuild->cache = runtime->user_data_root() / "shader-package-index-v4";
  std::error_code ec;
  fs::create_directories(g_prebuild->cache, ec);
  g_prebuild->roots = {runtime->game_data_root(), runtime->update_data_root(), runtime->user_data_root()};
  std::istringstream mods(rex::cvar::GetFlagByName("ue3_mods"));
  for (std::string mod; std::getline(mods, mod, ';');)
    if (!mod.empty()) g_prebuild->roots.push_back(fs::path(std::u8string(reinterpret_cast<const char8_t*>(mod.data()), mod.size())));
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
  rex::filesystem::SetFileOpenObserver(&OnFileOpened);
#endif
  std::thread(ReaderThread).detach();
}
void Ue3HoldForShaderPrebuild(rex::system::XThread* thread) {
  if (!g_prebuild || !g_prebuild->startup || !thread || g_prebuild->ready.load(std::memory_order_acquire)) return;
  // Add one suspension to the SDK's initial suspension. Release ours off the UI thread only
  // after discovery AND every driver job finish; the SDK releases its own after this returns.
  thread->Suspend();
  std::thread([thread] {
    while (!g_prebuild->ready.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    thread->Resume();
  }).detach();
}
