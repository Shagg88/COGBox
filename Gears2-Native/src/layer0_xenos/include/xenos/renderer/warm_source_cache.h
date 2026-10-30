// Inventory of source shaders already compiled into a validated native library. Our code (MIT).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace cogbox::shader_cache {
inline uint64_t InventoryHash(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (n--) { h ^= *p++; h *= 1099511628211ull; }
  return h;
}
struct SourceKey {
  uint64_t hash = 0;
  bool vertex = false;
  bool operator==(const SourceKey&) const = default;
};
struct SourceKeyHash {
  size_t operator()(const SourceKey& k) const { return size_t(k.hash ^ (k.vertex ? 0x9e3779b97f4a7c15ull : 0)); }
};
struct LibraryStamp {
  uint64_t identity = 0, size = 0, write_time = 0;
  bool operator==(const LibraryStamp&) const = default;
};
class WarmSourceCache {
 public:
  static bool Stamp(const std::filesystem::path& library, const std::string& identity, LibraryStamp& out) {
    std::error_code ec;
    const uint64_t n = std::filesystem::file_size(library, ec);
    if (ec || !n) return false;
    const auto written = std::filesystem::last_write_time(library, ec);
    if (ec) return false;
    out = {InventoryHash(identity.data(), identity.size()), n, uint64_t(written.time_since_epoch().count())};
    return true;
  }
  bool Contains(SourceKey key) const { std::lock_guard<std::mutex> lock(mutex_); return sources_.count(key) != 0; }
  bool HasPipeline(const std::wstring& name) const { std::lock_guard<std::mutex> lock(mutex_); return pipelines_.count(name) != 0; }
  void Remember(SourceKey key, const std::wstring& pipeline) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[key] = pipeline; pipelines_.insert(pipeline);
  }
  size_t Size() const { std::lock_guard<std::mutex> lock(mutex_); return sources_.size(); }
  void Clear() { std::lock_guard<std::mutex> lock(mutex_); sources_.clear(); pipelines_.clear(); }
  // A stage store may grow or lose an interrupted final append. Validate the
  // individual referenced records rather than invalidating every configuration
  // merely because the append-only file's size or timestamp changed.
  template<class Exists> size_t RetainPipelines(Exists&& exists) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pipelines_.begin(); it != pipelines_.end();) {
      if (!exists(*it)) it = pipelines_.erase(it); else ++it;
    }
    size_t removed = 0;
    for (auto it = sources_.begin(); it != sources_.end();) {
      if (!pipelines_.contains(it->second)) { it = sources_.erase(it); ++removed; }
      else ++it;
    }
    return removed;
  }
  bool Load(const std::filesystem::path& path, const LibraryStamp& expected) {
    Clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const auto n = f ? f.tellg() : std::streampos(-1);
    if (n < std::streamoff(sizeof(Header))) return false;
    Header h{}; f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != kMagic || !(h.stamp == expected) ||
        h.count > 2000000 || uint64_t(n) != sizeof(h) + h.count * sizeof(Record)) return false;
    std::vector<Record> records(size_t(h.count));
    if (!f.read(reinterpret_cast<char*>(records.data()), records.size() * sizeof(Record)) ||
        InventoryHash(records.data(), records.size() * sizeof(Record)) != h.checksum) return false;
    for (const auto& r : records) {
      if (r.vertex > 1 || r.name[0] != L'g' || r.name[1] != L'p' || r.name[34] != 0) return false;
      for (size_t i = 2; i < 34; ++i)
        if (!((r.name[i] >= L'0' && r.name[i] <= L'9') || (r.name[i] >= L'a' && r.name[i] <= L'f'))) return false;
    }
    for (const auto& r : records) Remember({r.hash, r.vertex != 0}, std::wstring(r.name.data(), 34));
    return true;
  }
  bool Save(const std::filesystem::path& path, const LibraryStamp& stamp) const {
    std::vector<Record> records;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      records.reserve(sources_.size());
      for (const auto& [key, name] : sources_) {
        if (name.size() != 34) return false;
        Record r{}; r.hash = key.hash; r.vertex = key.vertex;
        std::memcpy(r.name.data(), name.data(), name.size() * sizeof(wchar_t));
        records.push_back(r);
      }
    }
    Header h{}; h.magic = kMagic; h.stamp = stamp; h.count = records.size();
    h.checksum = InventoryHash(records.data(), records.size() * sizeof(Record));
    auto temporary = path; temporary += ".tmp";
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(&h), sizeof(h)) ||
        !f.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(Record))) return false;
    f.close(); if (!f) return false;
    std::error_code ec; std::filesystem::rename(temporary, path, ec);
    return !ec;
  }
 private:
  static constexpr uint64_t kMagic = 0x434f475352434831ull;
  struct Header { uint64_t magic; LibraryStamp stamp; uint64_t count, checksum; };
  struct Record { uint64_t hash; uint32_t vertex, reserved; std::array<wchar_t, 40> name; };
  mutable std::mutex mutex_;
  std::unordered_map<SourceKey, std::wstring, SourceKeyHash> sources_;
  std::unordered_set<std::wstring> pipelines_;
};
}  // namespace cogbox::shader_cache
