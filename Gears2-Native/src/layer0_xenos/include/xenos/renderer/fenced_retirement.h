#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace xenos::renderer {
// Own resources until their final submission completes. A full queue leaves the
// caller's resource untouched so it can use its synchronous fallback instead.
template<class T, size_t Capacity>
class FencedRetirement {
 public:
  bool Push(uint64_t fence, T&& value) {
    if (entries_.size() >= Capacity) return false;
    entries_.push_back({fence, std::move(value)});
    return true;
  }
  template<class Release>
  void Collect(uint64_t completed, Release&& release) {
    // D3D12 returns UINT64_MAX on device removal, not successful completion.
    if (completed == std::numeric_limits<uint64_t>::max()) return;
    for (size_t i = 0; i < entries_.size();) {
      if (entries_[i].fence > completed) { ++i; continue; }
      release(entries_[i].value);
      if (i + 1 != entries_.size()) entries_[i] = std::move(entries_.back());
      entries_.pop_back();
    }
  }
  template<class Release>
  void Clear(Release&& release) {
    for (auto& entry : entries_) release(entry.value);
    entries_.clear();
  }
  size_t Pending() const { return entries_.size(); }
 private:
  struct Entry { uint64_t fence; T value; };
  std::vector<Entry> entries_;
};
}  // namespace xenos::renderer
