// Background file appender (PR #43, legoliamneeson): the bytes of each Append are written to the end of the file, in
// order, by a thread of its own, so the caller (the renderer thread: patch 0058's pipelines.list) never waits on the
// filesystem. Each batch is flushed after it is written, as the old per-record flush did. Errors() counts appends
// that could not be written (the file failed to open, or a write / flush failed); the caller logs it. Header-only.
// Our code (MIT).
#pragma once
#include <atomic>
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
  ~AsyncFileAppender() {  // writes everything still queued, then stops
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
  AsyncFileAppender(const AsyncFileAppender&) = delete;
  AsyncFileAppender& operator=(const AsyncFileAppender&) = delete;

  bool Good() const { return worker_.joinable(); }  // false = the file did not open (nothing will be written)
  void Append(const void* data, size_t size) {
    if (!Good()) {
      errors_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.emplace_back(bytes, bytes + size);
    }
    cv_.notify_one();
  }
  void WaitIdle() {  // until everything appended so far is written
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
  }
  uint64_t Errors() const { return errors_.load(std::memory_order_relaxed); }

 private:
  void Run() {
    for (;;) {
      std::deque<std::vector<uint8_t>> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        batch.swap(queue_);
        busy_ = true;
      }
      uint64_t failed = 0;
      for (const auto& bytes : batch) {
        file_.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file_) {  // count it and clear the state, so one failure does not silently drop every later append
          ++failed;
          file_.clear();
        }
      }
      file_.flush();
      if (!file_) {
        failed += failed ? 0 : 1;
        file_.clear();
      }
      if (failed) errors_.fetch_add(failed, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
      }
      idle_.notify_all();
    }
  }
  std::ofstream file_;
  std::mutex mutex_;
  std::condition_variable cv_, idle_;
  std::deque<std::vector<uint8_t>> queue_;
  bool stop_ = false, busy_ = false;
  std::atomic<uint64_t> errors_{0};
  std::thread worker_;  // last: started after every other member exists
};

}  // namespace cogbox::io
