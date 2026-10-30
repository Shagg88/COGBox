// Read-only backing for large D3D12 pipeline libraries. Our code (MIT).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace cogbox::io {
class MappedFile {
 public:
  MappedFile() = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile() { Reset(); }
  bool Open(const std::filesystem::path& path) {
    Reset();
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER length{};
    if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &length) || length.QuadPart <= 0 ||
        uint64_t(length.QuadPart) > std::numeric_limits<size_t>::max()) { Reset(); return false; }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) { Reset(); return false; }
    data_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (!data_) { Reset(); return false; }
    size_ = size_t(length.QuadPart);
    return true;
  }
  void Reset() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    data_ = nullptr; mapping_ = nullptr; file_ = INVALID_HANDLE_VALUE; size_ = 0;
  }
  const void* Data() const { return data_; }
  size_t Size() const { return size_; }
 private:
  HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
  void* data_ = nullptr;
  size_t size_ = 0;
};
}  // namespace cogbox::io
