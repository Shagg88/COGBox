// Append-only driver stage cache. Keeps driver PSOs out of the resident library. Our code (MIT).
#pragma once
#include <xenos/renderer/warm_source_cache.h>
#include <algorithm>
namespace cogbox::shader_cache {
class WarmStageStore {
 public:
  // Only record headers are read here; driver blobs are read and checked on demand.
  bool Open(const std::filesystem::path& path, bool compatible) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path; entries_.clear(); good_ = false;
    std::error_code ec;
    if (!compatible && std::filesystem::exists(path, ec)) {
      auto backup = path;
      backup += ".incompatible-" + std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
      std::filesystem::rename(path, backup, ec);
      if (ec) return false;
    }
    if (!std::filesystem::exists(path, ec)) {
      std::ofstream create(path, std::ios::binary);
      if (!create) return false;
    }
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() < 0) return false;
    const uint64_t size = uint64_t(f.tellg());
    uint64_t offset = 0;
    while (size - offset >= sizeof(Header)) {
      Header h{}; f.seekg(std::streamoff(offset));
      if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || !Valid(h) || h.size > size - offset - sizeof(h)) break;
      entries_[std::wstring(h.name.data(), 34)] = {offset + sizeof(h), h.size, h.checksum};
      offset += sizeof(h) + h.size;
    }
    // An interrupted final append never invalidates the complete records before it.
    f.close();
    if (offset != size) { std::filesystem::resize_file(path, offset, ec); if (ec) return false; }
    good_ = true; return true;
  }
  bool Contains(const std::wstring& name) const { std::lock_guard<std::mutex> lock(mutex_); return entries_.count(name) != 0; }
  size_t Size() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }
  bool Put(const std::wstring& name, const void* bytes, size_t size) {
    if (!bytes || !size || size > kMaxBlob || name.size() != 34) return false;
    Header h{}; h.magic = kMagic; h.size = size; h.checksum = InventoryHash(bytes, size);
    std::copy(name.begin(), name.end(), h.name.begin());
    if (!Valid(h)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!good_) return false;
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    if (!f) return false;
    const auto offset = f.tellp();
    if (offset < 0 || !f.write(reinterpret_cast<const char*>(&h), sizeof(h)) ||
        !f.write(static_cast<const char*>(bytes), std::streamsize(size))) { good_ = false; return false; }
    f.close(); if (!f) { good_ = false; return false; }
    entries_[name] = {uint64_t(offset) + sizeof(h), size, h.checksum};
    return true;
  }
  bool Get(const std::wstring& name, std::vector<uint8_t>& bytes) const {
    Entry entry{}; std::filesystem::path path;
    { std::lock_guard<std::mutex> lock(mutex_);
      const auto it = entries_.find(name); if (it == entries_.end()) return false;
      entry = it->second; path = path_;
    }
    std::ifstream f(path, std::ios::binary); f.seekg(std::streamoff(entry.offset));
    bytes.resize(size_t(entry.size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) ||
        InventoryHash(bytes.data(), bytes.size()) != entry.checksum) { bytes.clear(); return false; }
    return true;
  }
  bool Stamp(LibraryStamp& stamp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!good_) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec); if (ec) return false;
    const auto time = std::filesystem::last_write_time(path_, ec); if (ec) return false;
    const uint64_t state[] = {size, uint64_t(time.time_since_epoch().count()), entries_.size()};
    stamp.identity = InventoryHash(state, sizeof(state), stamp.identity);
    return true;
  }
 private:
  static constexpr uint64_t kMagic = 0x434f475354414731ull, kMaxBlob = 128ull << 20;
  struct Header { uint64_t magic, size, checksum; std::array<wchar_t, 40> name; };
  struct Entry { uint64_t offset, size, checksum; };
  static bool Valid(const Header& h) {
    if (h.magic != kMagic || !h.size || h.size > kMaxBlob || h.name[0] != L'g' || h.name[1] != L'p' || h.name[34]) return false;
    for (size_t i = 2; i < 34; ++i)
      if (!((h.name[i] >= L'0' && h.name[i] <= L'9') || (h.name[i] >= L'a' && h.name[i] <= L'f'))) return false;
    return true;
  }
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::unordered_map<std::wstring, Entry> entries_;
  bool good_ = false;
};
}  // namespace cogbox::shader_cache
