// Validated per-file cache keys avoid rereading unchanged multi-GB game packages. Our code (MIT).
#pragma once
#include <Windows.h>
#include <xenos/renderer/warm_source_cache.h>
namespace ue3::shader_prebuild {
struct PackageFileStamp {
  uint64_t volume = 0, file_low = 0, file_high = 0, size = 0, written = 0, changed = 0;
  bool operator==(const PackageFileStamp&) const = default;
};
inline bool PackageStamp(const std::filesystem::path& path, PackageFileStamp& out) {
  HANDLE f = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{}; FILE_ID_INFO id{};
  const bool ok = GetFileInformationByHandleEx(f, FileBasicInfo, &basic, sizeof(basic)) &&
      GetFileInformationByHandleEx(f, FileStandardInfo, &standard, sizeof(standard)) &&
      GetFileInformationByHandleEx(f, FileIdInfo, &id, sizeof(id));
  CloseHandle(f);
  if (!ok || standard.Directory || standard.EndOfFile.QuadPart <= 0) return false;
  out = {id.VolumeSerialNumber, 0, 0, uint64_t(standard.EndOfFile.QuadPart),
         uint64_t(basic.LastWriteTime.QuadPart), uint64_t(basic.ChangeTime.QuadPart)};
  std::memcpy(&out.file_low, id.FileId.Identifier, 8);
  std::memcpy(&out.file_high, id.FileId.Identifier + 8, 8);
  return true;
}
class PackageInventory {
 public:
  bool Find(const std::filesystem::path& path, const PackageFileStamp& stamp, uint64_t& hash) const {
    auto it = files_.find(path.wstring());
    if (it == files_.end() || !(it->second.stamp == stamp)) return false;
    hash = it->second.hash; return true;
  }
  void Remember(const std::filesystem::path& path, const PackageFileStamp& stamp, uint64_t hash) {
    files_[path.wstring()] = {stamp, hash};
  }
  bool Load(const std::filesystem::path& path) {
    files_.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto size = f ? f.tellg() : std::streampos(-1);
    if (size < std::streamoff(sizeof(Header)) || size > (64 << 20)) return false;
    Header h{}; f.seekg(0); f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (h.magic != kMagic || h.count > 100000) return false;
    std::vector<uint8_t> data(size_t(size) - sizeof(h));
    if (!f.read(reinterpret_cast<char*>(data.data()), data.size()) ||
        cogbox::shader_cache::InventoryHash(data.data(), data.size()) != h.checksum) return false;
    size_t offset = 0;
    decltype(files_) loaded;
    for (uint64_t i = 0; i < h.count; ++i) {
      uint32_t n = 0; Entry entry{};
      if (data.size() - offset < sizeof(n) + sizeof(entry)) return false;
      std::memcpy(&n, data.data() + offset, sizeof(n)); offset += sizeof(n);
      std::memcpy(&entry, data.data() + offset, sizeof(entry)); offset += sizeof(entry);
      if (!n || n > 32768 || n * sizeof(wchar_t) > data.size() - offset) return false;
      std::wstring name(n, 0); std::memcpy(name.data(), data.data() + offset, n * sizeof(wchar_t));
      offset += n * sizeof(wchar_t); loaded.emplace(std::move(name), entry);
    }
    if (offset != data.size()) return false;
    files_ = std::move(loaded); return true;
  }
  bool Save(const std::filesystem::path& path) const {
    std::vector<uint8_t> data;
    auto append = [&](const void* p, size_t n) {
      auto bytes = static_cast<const uint8_t*>(p); data.insert(data.end(), bytes, bytes + n);
    };
    for (const auto& [name, entry] : files_) {
      const uint32_t n = uint32_t(name.size()); append(&n, sizeof(n)); append(&entry, sizeof(entry));
      append(name.data(), name.size() * sizeof(wchar_t));
    }
    const Header h{kMagic, files_.size(), cogbox::shader_cache::InventoryHash(data.data(), data.size())};
    auto temporary = path; temporary += ".tmp";
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(&h), sizeof(h)) ||
        !f.write(reinterpret_cast<const char*>(data.data()), data.size())) return false;
    f.close(); if (!f) return false;
    std::error_code ec; std::filesystem::rename(temporary, path, ec); return !ec;
  }
 private:
  struct Entry { PackageFileStamp stamp; uint64_t hash; };
  struct Header { uint64_t magic, count, checksum; };
  static constexpr uint64_t kMagic = 0x434f47504b474931ull;
  std::unordered_map<std::wstring, Entry> files_;
};
}  // namespace ue3::shader_prebuild
