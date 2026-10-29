// Append metadata without filesystem work on the rendering thread. Our code (MIT).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
namespace cogbox::io {
class AsyncFileAppender {
 public:
  explicit AsyncFileAppender(const std::filesystem::path& path) : file_(path, std::ios::binary | std::ios::app) {
    if (file_) worker_ = std::thread([this] { Run(); });
  }
  ~AsyncFileAppender() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
  bool Good() const { return worker_.joinable(); }
  void Append(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    { std::lock_guard<std::mutex> lock(mutex_); queue_.emplace_back(bytes, bytes + size); }
    cv_.notify_one();
  }
  void WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
  }
 private:
  void Run() {
    for (;;) {
      std::deque<std::vector<uint8_t>> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        batch.swap(queue_); busy_ = true;
      }
      for (const auto& bytes : batch) file_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      file_.flush();
      { std::lock_guard<std::mutex> lock(mutex_); busy_ = false; }
      idle_.notify_all();
    }
  }
  std::ofstream file_;
  std::mutex mutex_;
  std::condition_variable cv_, idle_;
  std::deque<std::vector<uint8_t>> queue_;
  bool stop_ = false, busy_ = false;
  std::thread worker_;
};
}  // namespace cogbox::io
