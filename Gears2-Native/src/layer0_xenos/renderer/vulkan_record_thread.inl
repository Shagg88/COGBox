// Vulkan recording thread (--gow2_renderer_vulkan_record_thread; default off). Included by vulkan_backend.cpp right
// after `struct Vk`, inside its anonymous namespace - not a header of its own.
//
// The D3D12 backend's CommandListProxy (d3d12_backend.cpp, "Threaded command recording") on Vulkan, with one
// difference in how it is wired: instead of renaming ~80 call sites, the backend's function table `vk_` has its
// command-buffer entries replaced by the thunks below while the thread runs. A thunk has the exact PFN signature; it
// encodes its arguments (deep copies of every array and struct they point to) into a byte block, and the recording
// thread replays the blocks, in order, with the driver's real entry points (Recorder::real). So with the switch off the
// table is never touched and every call site runs exactly the code it ran before.
//
// What is encoded: every vkCmd* the backend makes (and its optional vkCmdSetDepthBias2EXT), vkResetCommandPool,
// vkBeginCommandBuffer, vkEndCommandBuffer and vkQueueSubmit2. So while the thread runs, only it touches the command
// buffers, their pools and the queue (Vulkan's external-synchronisation rules), as on D3D12 only the recording thread
// touches the list, its allocators and the queue. Everything host-side (descriptor sets, the upload ring, query
// results, timeline waits, object creation and the fence-deferred destroys) stays on the caller's thread.
//
// Direct mode: after Drain() the recording thread is idle and has replayed everything, so the caller's thread may
// record into the command buffer itself. The backend uses this for the native hand-out (BeginNativeVulkan: frame
// generation, FSR and the HUD-less copies record into our buffer with their own entry points) and the thunks use it
// for a call they cannot copy (an unknown pNext chain): the call then runs on the caller's thread after a drain.
//
// Prior art: DXVK's CS thread (D3D11/D3D9 calls are captured as commands into "CS chunks" from a DxvkCsChunkPool and
// executed in order by one worker; the context synchronises with it before Map, query reads and presents), Xenia's
// Vulkan command processor (VulkanCommandProcessor records into a DeferredCommandBuffer - a stream of encoded vkCmd*
// calls - and replays it into a real VkCommandBuffer at submission; same thread there), and vkd3d-proton (D3D12 lists
// record into VkCommandBuffers when the app calls them; queue submissions go through a per-queue submission thread so
// fence waits never block the caller). This file is DXVK's split (encode here, record + submit there) at the vkCmd*
// level, with the D3D12 proxy's block format and Drain/Kick contract.

namespace vkrec {

enum Op : uint32_t {
  kResetCommandPool = 1, kBeginCommandBuffer, kEndCommandBuffer, kQueueSubmit2, kPipelineBarrier2, kCopyImage,
  kCopyBufferToImage, kCopyImageToBuffer, kCopyBuffer, kClearColorImage, kClearDepthStencilImage, kBeginRendering,
  kEndRendering, kClearAttachments, kFillBuffer, kBindPipeline, kBindDescriptorSets, kBindIndexBuffer, kDraw,
  kDrawIndexed, kSetViewport, kSetScissor, kSetStencilReference, kSetBlendConstants, kDispatch, kResetQueryPool,
  kWriteTimestamp2, kBeginQuery, kEndQuery, kSetCullMode, kSetFrontFace, kSetDepthBiasEnable, kSetDepthBias,
  kSetDepthTestEnable, kSetDepthWriteEnable, kSetDepthCompareOp, kSetStencilTestEnable, kSetStencilOp,
  kSetStencilCompareMask, kSetStencilWriteMask, kSetDepthBias2, kBeginDebugLabel, kEndDebugLabel
};

// Why the caller had to wait for the recording thread (the stats line's "forced flushes").
enum Why : uint32_t { kWhySubmit, kWhyWait, kWhyNative, kWhyFallback, kWhyShutdown, kWhyCount };
inline const char* const kWhyNames[kWhyCount] = {"submit", "fence wait", "native", "fallback", "shutdown"};

class Recorder {
 public:
  // ---- set once by Install before the thread starts; read-only afterwards (both threads) ----
  Vk real;                                          // the driver's entry points
  PFN_vkCmdSetDepthBias2EXT real_bias2 = nullptr;  // the backend's optional vkCmdSetDepthBias2EXT
  void (*queue_lock)(void*) = nullptr;              // VulkanExternal::lock / unlock (someone else's queue)
  void (*queue_unlock)(void*) = nullptr;
  void* queue_lock_user = nullptr;

  ~Recorder() { Stop(); }

  void Start() {
    worker_ = std::thread([this] { Run(); });
  }
  void Stop() {
    if (!worker_.joinable()) return;
    Drain(kWhyShutdown);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }

  // Hands what is encoded so far to the recording thread now, without waiting (an early submit).
  void Kick() { Flush(); }
  // Waits until every encoded call has been replayed (and every encoded submit made).
  void Drain(Why why) {
    Flush();
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_.empty() && !busy_) return;
    ++forced_[why];
    const auto t0 = std::chrono::steady_clock::now();
    idle_cv_.wait(lock, [this] { return ready_.empty() && !busy_; });
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    wait_ns_ += ns;
    wait_total_ns_ += ns;
  }
  // Direct mode (see the file comment). Entering drains; leaving needs nothing (nothing was encoded meanwhile).
  void SetDirect(bool on, Why why) {
    if (on && !direct_) Drain(why);
    direct_ = on;
  }
  bool Direct() const { return direct_; }
  // A call the thunks cannot encode: drain, then the caller's thread makes it (counted as a fallback).
  const Vk& Fallback() {
    Drain(kWhyFallback);
    ++fallbacks_;
    return real;
  }

  // First failure the recording thread saw (VK_SUCCESS if none), and the highest timeline value it submitted.
  VkResult Error() const { return error_.load(std::memory_order_acquire); }
  const char* ErrorWhat() const { return error_what_.load(std::memory_order_acquire); }
  uint64_t SubmittedValue() const { return submitted_value_.load(std::memory_order_acquire); }

  // ---- stats (caller's thread; the recording thread's counters are atomics) ----
  struct Frame {
    double encode_ms = 0, record_ms = 0, wait_ms = 0;
    uint64_t blocks = 0, bytes = 0, depth_max = 0, forced[kWhyCount] = {}, fallbacks = 0, direct_calls = 0;
  };
  // What happened since the last call (the backend calls it once a frame).
  Frame TakeFrame() {
    Frame f;
    f.encode_ms = double(encode_ns_) / 1e6;
    f.record_ms = double(record_ns_.exchange(0, std::memory_order_relaxed)) / 1e6;
    f.wait_ms = double(wait_ns_) / 1e6;
    f.blocks = blocks_;
    f.bytes = bytes_;
    f.depth_max = depth_max_;
    for (uint32_t i = 0; i < kWhyCount; ++i) f.forced[i] = forced_[i];
    f.fallbacks = fallbacks_;
    f.direct_calls = direct_calls_;
    encode_ns_ = wait_ns_ = 0;
    blocks_ = bytes_ = depth_max_ = fallbacks_ = direct_calls_ = 0;
    for (auto& v : forced_) v = 0;
    return f;
  }
  double WaitMsTotal() const { return double(wait_total_ns_) / 1e6; }  // since Start (BackendStats::recording_wait_ms)
  void CountDirect() { ++direct_calls_; }

  // ---- encoding (caller's thread) ----
  // One per thunk: starts the op, and times one call in 32 (scaled up) for the caller's encode time - a clock read on
  // every call would cost more than the ~20k calls a frame it measures.
  struct Enc {
    Recorder& r;
    std::chrono::steady_clock::time_point t0;
    bool timed;
    Enc(Recorder& rec, Op op, VkCommandBuffer cb) : r(rec), timed((++rec.sample_ & 31) == 0) {
      if (timed) t0 = std::chrono::steady_clock::now();
      r.Put(op);
      r.Pod(cb);
    }
    ~Enc() {
      if (timed)
        r.encode_ns_ += 32 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
  };
  void Bytes(const void* p, size_t n) {
    if (cur_len_ + n > cur_.size()) [[unlikely]] cur_.resize(std::max(cur_len_ + n, cur_.size() * 2));
    if (n) std::memcpy(cur_.data() + cur_len_, p, n);
    cur_len_ += n;
  }
  template <typename T>
  void Pod(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    Bytes(&v, sizeof(T));
  }
  // Arrays start 8-byte aligned in the block (the block itself comes from operator new), so the driver gets properly
  // aligned structs on replay.
  template <typename T>
  void Array(const T* p, uint32_t count) {
    Pod(count);
    if (!count) return;
    Align();
    Bytes(p, sizeof(T) * count);
  }
  void Align() {
    const size_t pad = (8 - (cur_len_ & 7)) & 7;
    if (pad) {
      static const uint8_t zero[8] = {};
      Bytes(zero, pad);
    }
  }

 private:
  static constexpr size_t kBlockBytes = 32 << 10;  // handed over when full (as D3D12's proxy), and at Kick / Drain

  void Put(Op op) {
    if (cur_len_ >= kBlockBytes) Flush();
    if (cur_.size() < kBlockBytes + 4096) [[unlikely]] cur_.resize(kBlockBytes + 4096);
    Pod(uint32_t(op));
  }
  void Flush() {
    if (!cur_len_) return;
    cur_.resize(cur_len_);
    bytes_ += cur_len_;
    ++blocks_;
    cur_len_ = 0;
    std::vector<uint8_t> next;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_.push_back(std::move(cur_));
      depth_max_ = std::max<uint64_t>(depth_max_, ready_.size() + (busy_ ? 1 : 0));
      if (!free_.empty()) {
        next = std::move(free_.back());
        free_.pop_back();
      }
    }
    cv_.notify_one();
    next.clear();
    cur_ = std::move(next);
  }

  // ---- replay (recording thread) ----
  struct Reader {
    const uint8_t* base;
    const uint8_t* p;
    template <typename T>
    T Pod() {
      T v;
      std::memcpy(&v, p, sizeof(T));
      p += sizeof(T);
      return v;
    }
    template <typename T>
    const T* Array(uint32_t& count) {
      count = Pod<uint32_t>();
      if (!count) return nullptr;
      p = base + ((size_t(p - base) + 7) & ~size_t(7));
      const T* a = reinterpret_cast<const T*>(p);
      p += sizeof(T) * count;
      return a;
    }
  };
  void Fail(VkResult r, const char* what) {
    VkResult ok = VK_SUCCESS;
    if (error_.compare_exchange_strong(ok, r, std::memory_order_acq_rel)) error_what_.store(what, std::memory_order_release);
  }
  void Replay(std::vector<uint8_t>& block);
  void Run() {
#ifdef _WIN32
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
      fn(GetCurrentThread(), L"Vulkan Recording (ours)");
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stop_ || !ready_.empty(); });
      if (ready_.empty() && stop_) break;
      std::vector<uint8_t> block = std::move(ready_.front());
      ready_.pop_front();
      busy_ = true;
      lock.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      Replay(block);
      record_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                           std::memory_order_relaxed);
      lock.lock();
      busy_ = false;
      block.clear();
      if (free_.size() < 64) free_.push_back(std::move(block));
      if (ready_.empty()) idle_cv_.notify_all();
    }
  }

  // caller's thread only
  std::vector<uint8_t> cur_;
  size_t cur_len_ = 0;
  bool direct_ = false;
  uint32_t sample_ = 0;
  uint64_t encode_ns_ = 0, wait_ns_ = 0, wait_total_ns_ = 0, blocks_ = 0, bytes_ = 0, fallbacks_ = 0, direct_calls_ = 0;
  uint64_t forced_[kWhyCount] = {};
  // under mutex_ (depth_max_ is written under it by the caller, read by the caller)
  std::mutex mutex_;
  std::condition_variable cv_, idle_cv_;
  std::deque<std::vector<uint8_t>> ready_;
  std::vector<std::vector<uint8_t>> free_;
  bool busy_ = false, stop_ = false;
  uint64_t depth_max_ = 0;
  std::thread worker_;
  // written by the recording thread
  std::atomic<uint64_t> record_ns_{0};
  std::atomic<VkResult> error_{VK_SUCCESS};
  std::atomic<const char*> error_what_{nullptr};
  std::atomic<uint64_t> submitted_value_{0};
};

// The running recorder. Set by Install (caller's thread, before the thunks go into the table) and cleared by
// Uninstall; only the thunks read it, and they run on the caller's thread. One at a time: a second backend asking for
// the thread gets the immediate path (VulkanBackend::StartRecordThread).
inline Recorder* g_rec = nullptr;

// ---- the thunks (caller's thread) ----
// Each: in direct mode, the driver at once; else encode. Pointer arguments are copied whole; a pNext chain the encoder
// does not know falls back to a drained direct call.
#define VKREC_DIRECT(call)        \
  Recorder& r = *g_rec;           \
  if (r.Direct()) {               \
    r.CountDirect();              \
    return r.real.call;           \
  }

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags flags) {
  VKREC_DIRECT(vkResetCommandPool(d, pool, flags))
  Recorder::Enc e(r, kResetCommandPool, VK_NULL_HANDLE);
  r.Pod(d); r.Pod(pool); r.Pod(flags);
  return VK_SUCCESS;  // a failure shows in Recorder::Error() after the next Drain
}
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
  VKREC_DIRECT(vkBeginCommandBuffer(cb, bi))
  if (bi->pNext || bi->pInheritanceInfo) return r.Fallback().vkBeginCommandBuffer(cb, bi);
  Recorder::Enc e(r, kBeginCommandBuffer, cb);
  r.Pod(bi->flags);
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cb) {
  VKREC_DIRECT(vkEndCommandBuffer(cb))
  Recorder::Enc e(r, kEndCommandBuffer, cb);
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t count, const VkSubmitInfo2* si, VkFence fence) {
  // Direct mode: the caller holds the queue lock already (VulkanBackend::Submit takes it around this call).
  VKREC_DIRECT(vkQueueSubmit2(q, count, si, fence))
  for (uint32_t i = 0; i < count; ++i) {
    bool chained = si[i].pNext;
    for (uint32_t j = 0; j < si[i].waitSemaphoreInfoCount; ++j) chained = chained || si[i].pWaitSemaphoreInfos[j].pNext;
    for (uint32_t j = 0; j < si[i].commandBufferInfoCount; ++j) chained = chained || si[i].pCommandBufferInfos[j].pNext;
    for (uint32_t j = 0; j < si[i].signalSemaphoreInfoCount; ++j) chained = chained || si[i].pSignalSemaphoreInfos[j].pNext;
    if (chained) return r.Fallback().vkQueueSubmit2(q, count, si, fence);
  }
  Recorder::Enc e(r, kQueueSubmit2, VK_NULL_HANDLE);
  r.Pod(q); r.Pod(fence); r.Pod(count);
  for (uint32_t i = 0; i < count; ++i) {
    r.Pod(si[i].flags);
    r.Array(si[i].pWaitSemaphoreInfos, si[i].waitSemaphoreInfoCount);
    r.Array(si[i].pCommandBufferInfos, si[i].commandBufferInfoCount);
    r.Array(si[i].pSignalSemaphoreInfos, si[i].signalSemaphoreInfoCount);
  }
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
  VKREC_DIRECT(vkCmdPipelineBarrier2(cb, di))
  bool chained = di->pNext;
  for (uint32_t i = 0; i < di->memoryBarrierCount; ++i) chained = chained || di->pMemoryBarriers[i].pNext;
  for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; ++i) chained = chained || di->pBufferMemoryBarriers[i].pNext;
  for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) chained = chained || di->pImageMemoryBarriers[i].pNext;
  if (chained) return r.Fallback().vkCmdPipelineBarrier2(cb, di);
  Recorder::Enc e(r, kPipelineBarrier2, cb);
  r.Pod(di->dependencyFlags);
  r.Array(di->pMemoryBarriers, di->memoryBarrierCount);
  r.Array(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount);
  r.Array(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                        uint32_t n, const VkImageCopy* regions) {
  VKREC_DIRECT(vkCmdCopyImage(cb, src, sl, dst, dl, n, regions))
  Recorder::Enc e(r, kCopyImage, cb);
  r.Pod(src); r.Pod(sl); r.Pod(dst); r.Pod(dl);
  r.Array(regions, n);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl,
                                                uint32_t n, const VkBufferImageCopy* regions) {
  VKREC_DIRECT(vkCmdCopyBufferToImage(cb, src, dst, dl, n, regions))
  Recorder::Enc e(r, kCopyBufferToImage, cb);
  r.Pod(src); r.Pod(dst); r.Pod(dl);
  r.Array(regions, n);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst,
                                                uint32_t n, const VkBufferImageCopy* regions) {
  VKREC_DIRECT(vkCmdCopyImageToBuffer(cb, src, sl, dst, n, regions))
  Recorder::Enc e(r, kCopyImageToBuffer, cb);
  r.Pod(src); r.Pod(sl); r.Pod(dst);
  r.Array(regions, n);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* regions) {
  VKREC_DIRECT(vkCmdCopyBuffer(cb, src, dst, n, regions))
  Recorder::Enc e(r, kCopyBuffer, cb);
  r.Pod(src); r.Pod(dst);
  r.Array(regions, n);
}
VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c,
                                              uint32_t n, const VkImageSubresourceRange* ranges) {
  VKREC_DIRECT(vkCmdClearColorImage(cb, img, l, c, n, ranges))
  Recorder::Enc e(r, kClearColorImage, cb);
  r.Pod(img); r.Pod(l); r.Pod(*c);
  r.Array(ranges, n);
}
VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                     const VkClearDepthStencilValue* v, uint32_t n,
                                                     const VkImageSubresourceRange* ranges) {
  VKREC_DIRECT(vkCmdClearDepthStencilImage(cb, img, l, v, n, ranges))
  Recorder::Enc e(r, kClearDepthStencilImage, cb);
  r.Pod(img); r.Pod(l); r.Pod(*v);
  r.Array(ranges, n);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
  VKREC_DIRECT(vkCmdBeginRendering(cb, ri))
  bool chained = ri->pNext || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
                 (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
  for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) chained = chained || ri->pColorAttachments[i].pNext;
  if (chained) return r.Fallback().vkCmdBeginRendering(cb, ri);
  Recorder::Enc e(r, kBeginRendering, cb);
  r.Pod(ri->flags); r.Pod(ri->renderArea); r.Pod(ri->layerCount); r.Pod(ri->viewMask);
  r.Array(ri->pColorAttachments, ri->colorAttachmentCount);
  r.Array(ri->pDepthAttachment, ri->pDepthAttachment ? 1u : 0u);
  r.Array(ri->pStencilAttachment, ri->pStencilAttachment ? 1u : 0u);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer cb) {
  VKREC_DIRECT(vkCmdEndRendering(cb))
  Recorder::Enc e(r, kEndRendering, cb);
}
VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr,
                                               const VkClearRect* rects) {
  VKREC_DIRECT(vkCmdClearAttachments(cb, na, a, nr, rects))
  Recorder::Enc e(r, kClearAttachments, cb);
  r.Array(a, na);
  r.Array(rects, nr);
}
VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
  VKREC_DIRECT(vkCmdFillBuffer(cb, b, off, size, data))
  Recorder::Enc e(r, kFillBuffer, cb);
  r.Pod(b); r.Pod(off); r.Pod(size); r.Pod(data);
}
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
  VKREC_DIRECT(vkCmdBindPipeline(cb, bp, p))
  Recorder::Enc e(r, kBindPipeline, cb);
  r.Pod(bp); r.Pod(p);
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                                 uint32_t first, uint32_t n, const VkDescriptorSet* sets, uint32_t nd,
                                                 const uint32_t* dyn) {
  VKREC_DIRECT(vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn))
  Recorder::Enc e(r, kBindDescriptorSets, cb);
  r.Pod(bp); r.Pod(layout); r.Pod(first);
  r.Array(sets, n);
  r.Array(dyn, nd);
}
VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
  VKREC_DIRECT(vkCmdBindIndexBuffer(cb, b, off, t))
  Recorder::Enc e(r, kBindIndexBuffer, cb);
  r.Pod(b); r.Pod(off); r.Pod(t);
}
VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
  VKREC_DIRECT(vkCmdDraw(cb, vc, ic, fv, fi))
  Recorder::Enc e(r, kDraw, cb);
  r.Pod(vc); r.Pod(ic); r.Pod(fv); r.Pod(fi);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) {
  VKREC_DIRECT(vkCmdDrawIndexed(cb, ic, inst, fi, vo, finst))
  Recorder::Enc e(r, kDrawIndexed, cb);
  r.Pod(ic); r.Pod(inst); r.Pod(fi); r.Pod(vo); r.Pod(finst);
}
VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
  VKREC_DIRECT(vkCmdSetViewport(cb, first, n, v))
  Recorder::Enc e(r, kSetViewport, cb);
  r.Pod(first);
  r.Array(v, n);
}
VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* s) {
  VKREC_DIRECT(vkCmdSetScissor(cb, first, n, s))
  Recorder::Enc e(r, kSetScissor, cb);
  r.Pod(first);
  r.Array(s, n);
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t ref) {
  VKREC_DIRECT(vkCmdSetStencilReference(cb, f, ref))
  Recorder::Enc e(r, kSetStencilReference, cb);
  r.Pod(f); r.Pod(ref);
}
VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
  VKREC_DIRECT(vkCmdSetBlendConstants(cb, c))
  Recorder::Enc e(r, kSetBlendConstants, cb);
  r.Bytes(c, 16);
}
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
  VKREC_DIRECT(vkCmdDispatch(cb, x, y, z))
  Recorder::Enc e(r, kDispatch, cb);
  r.Pod(x); r.Pod(y); r.Pod(z);
}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
  VKREC_DIRECT(vkCmdResetQueryPool(cb, p, first, n))
  Recorder::Enc e(r, kResetQueryPool, cb);
  r.Pod(p); r.Pod(first); r.Pod(n);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 st, VkQueryPool p, uint32_t q) {
  VKREC_DIRECT(vkCmdWriteTimestamp2(cb, st, p, q))
  Recorder::Enc e(r, kWriteTimestamp2, cb);
  r.Pod(st); r.Pod(p); r.Pod(q);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
  VKREC_DIRECT(vkCmdBeginQuery(cb, p, q, f))
  Recorder::Enc e(r, kBeginQuery, cb);
  r.Pod(p); r.Pod(q); r.Pod(f);
}
VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
  VKREC_DIRECT(vkCmdEndQuery(cb, p, q))
  Recorder::Enc e(r, kEndQuery, cb);
  r.Pod(p); r.Pod(q);
}
VKAPI_ATTR void VKAPI_CALL CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags v) {
  VKREC_DIRECT(vkCmdSetCullMode(cb, v))
  Recorder::Enc e(r, kSetCullMode, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace v) {
  VKREC_DIRECT(vkCmdSetFrontFace(cb, v))
  Recorder::Enc e(r, kSetFrontFace, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 v) {
  VKREC_DIRECT(vkCmdSetDepthBiasEnable(cb, v))
  Recorder::Enc e(r, kSetDepthBiasEnable, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
  VKREC_DIRECT(vkCmdSetDepthBias(cb, c, clamp, slope))
  Recorder::Enc e(r, kSetDepthBias, cb);
  r.Pod(c); r.Pod(clamp); r.Pod(slope);
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 v) {
  VKREC_DIRECT(vkCmdSetDepthTestEnable(cb, v))
  Recorder::Enc e(r, kSetDepthTestEnable, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 v) {
  VKREC_DIRECT(vkCmdSetDepthWriteEnable(cb, v))
  Recorder::Enc e(r, kSetDepthWriteEnable, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp v) {
  VKREC_DIRECT(vkCmdSetDepthCompareOp(cb, v))
  Recorder::Enc e(r, kSetDepthCompareOp, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 v) {
  VKREC_DIRECT(vkCmdSetStencilTestEnable(cb, v))
  Recorder::Enc e(r, kSetStencilTestEnable, cb);
  r.Pod(v);
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass,
                                           VkStencilOp dfail, VkCompareOp cmp) {
  VKREC_DIRECT(vkCmdSetStencilOp(cb, f, fail, pass, dfail, cmp))
  Recorder::Enc e(r, kSetStencilOp, cb);
  r.Pod(f); r.Pod(fail); r.Pod(pass); r.Pod(dfail); r.Pod(cmp);
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
  VKREC_DIRECT(vkCmdSetStencilCompareMask(cb, f, m))
  Recorder::Enc e(r, kSetStencilCompareMask, cb);
  r.Pod(f); r.Pod(m);
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
  VKREC_DIRECT(vkCmdSetStencilWriteMask(cb, f, m))
  Recorder::Enc e(r, kSetStencilWriteMask, cb);
  r.Pod(f); r.Pod(m);
}
#ifdef VK_EXT_depth_bias_control
// The one chained struct the backend sends: VkDepthBiasRepresentationInfoEXT (22 Nov depth-bias fix). Anything else
// in the chain falls back.
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias2(VkCommandBuffer cb, const VkDepthBiasInfoEXT* bi) {
  Recorder& r = *g_rec;
  if (r.Direct()) {
    r.CountDirect();
    return r.real_bias2(cb, bi);
  }
  const auto* rep = static_cast<const VkDepthBiasRepresentationInfoEXT*>(bi->pNext);
  if (rep && (rep->sType != VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT || rep->pNext)) {
    r.Fallback();
    return r.real_bias2(cb, bi);
  }
  Recorder::Enc e(r, kSetDepthBias2, cb);
  r.Pod(bi->depthBiasConstantFactor); r.Pod(bi->depthBiasClamp); r.Pod(bi->depthBiasSlopeFactor);
  const uint32_t has_rep = rep ? 1u : 0u;
  r.Pod(has_rep);
  if (rep) { r.Pod(rep->depthBiasRepresentation); r.Pod(rep->depthBiasExact); }
}
#endif
VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabel(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* l) {
  VKREC_DIRECT(vkCmdBeginDebugUtilsLabelEXT(cb, l))
  if (l->pNext) return r.Fallback().vkCmdBeginDebugUtilsLabelEXT(cb, l);
  Recorder::Enc e(r, kBeginDebugLabel, cb);
  r.Bytes(l->color, 16);
  const char* name = l->pLabelName ? l->pLabelName : "";
  r.Array(name, uint32_t(std::strlen(name) + 1));  // the caller's string may be gone by the replay
}
VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabel(VkCommandBuffer cb) {
  VKREC_DIRECT(vkCmdEndDebugUtilsLabelEXT(cb))
  Recorder::Enc e(r, kEndDebugLabel, cb);
}
#undef VKREC_DIRECT

inline void Recorder::Replay(std::vector<uint8_t>& block) {
  // After a failure nothing more goes to the driver (a list whose Begin or End failed must not be submitted); the
  // caller sees Error() at its next Submit / Drain and stops the backend, as on the immediate path.
  if (Error() != VK_SUCCESS) return;
  Reader rd{block.data(), block.data()};
  const uint8_t* end = block.data() + block.size();
  const Vk& v = real;
  while (rd.p < end && Error() == VK_SUCCESS) {
    const Op op = Op(rd.Pod<uint32_t>());
    const VkCommandBuffer cb = rd.Pod<VkCommandBuffer>();
    uint32_t n = 0, m = 0;
    switch (op) {
      case kResetCommandPool: {
        const auto d = rd.Pod<VkDevice>();
        const auto pool = rd.Pod<VkCommandPool>();
        const auto flags = rd.Pod<VkCommandPoolResetFlags>();
        const VkResult res = v.vkResetCommandPool(d, pool, flags);
        if (res != VK_SUCCESS) Fail(res, "vkResetCommandPool");
        break;
      }
      case kBeginCommandBuffer: {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = rd.Pod<VkCommandBufferUsageFlags>();
        const VkResult res = v.vkBeginCommandBuffer(cb, &bi);
        if (res != VK_SUCCESS) Fail(res, "vkBeginCommandBuffer");
        break;
      }
      case kEndCommandBuffer: {
        const VkResult res = v.vkEndCommandBuffer(cb);
        if (res != VK_SUCCESS) Fail(res, "vkEndCommandBuffer");
        break;
      }
      case kQueueSubmit2: {
        const auto q = rd.Pod<VkQueue>();
        const auto fence = rd.Pod<VkFence>();
        const uint32_t count = rd.Pod<uint32_t>();
        VkSubmitInfo2 si[4];
        uint64_t top = 0;
        for (uint32_t i = 0; i < count; ++i) {
          VkSubmitInfo2 s{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
          s.flags = rd.Pod<VkSubmitFlags>();
          s.pWaitSemaphoreInfos = rd.Array<VkSemaphoreSubmitInfo>(s.waitSemaphoreInfoCount);
          s.pCommandBufferInfos = rd.Array<VkCommandBufferSubmitInfo>(s.commandBufferInfoCount);
          s.pSignalSemaphoreInfos = rd.Array<VkSemaphoreSubmitInfo>(s.signalSemaphoreInfoCount);
          for (uint32_t j = 0; j < s.signalSemaphoreInfoCount; ++j) top = std::max(top, s.pSignalSemaphoreInfos[j].value);
          if (i < 4) si[i] = s;
        }
        if (count > 4) {  // the backend submits one at a time; more would need a heap array
          Fail(VK_ERROR_UNKNOWN, "vkQueueSubmit2 (recording thread: more than 4 submits in one call)");
          break;
        }
        if (queue_lock) queue_lock(queue_lock_user);
        const VkResult res = v.vkQueueSubmit2(q, count, si, fence);
        if (queue_unlock) queue_unlock(queue_lock_user);
        if (res != VK_SUCCESS) Fail(res, "vkQueueSubmit2");
        else if (top > submitted_value_.load(std::memory_order_relaxed)) submitted_value_.store(top, std::memory_order_release);
        break;
      }
      case kPipelineBarrier2: {
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.dependencyFlags = rd.Pod<VkDependencyFlags>();
        di.pMemoryBarriers = rd.Array<VkMemoryBarrier2>(di.memoryBarrierCount);
        di.pBufferMemoryBarriers = rd.Array<VkBufferMemoryBarrier2>(di.bufferMemoryBarrierCount);
        di.pImageMemoryBarriers = rd.Array<VkImageMemoryBarrier2>(di.imageMemoryBarrierCount);
        v.vkCmdPipelineBarrier2(cb, &di);
        break;
      }
      case kCopyImage: {
        const auto src = rd.Pod<VkImage>();
        const auto sl = rd.Pod<VkImageLayout>();
        const auto dst = rd.Pod<VkImage>();
        const auto dl = rd.Pod<VkImageLayout>();
        const auto* regions = rd.Array<VkImageCopy>(n);
        v.vkCmdCopyImage(cb, src, sl, dst, dl, n, regions);
        break;
      }
      case kCopyBufferToImage: {
        const auto src = rd.Pod<VkBuffer>();
        const auto dst = rd.Pod<VkImage>();
        const auto dl = rd.Pod<VkImageLayout>();
        const auto* regions = rd.Array<VkBufferImageCopy>(n);
        v.vkCmdCopyBufferToImage(cb, src, dst, dl, n, regions);
        break;
      }
      case kCopyImageToBuffer: {
        const auto src = rd.Pod<VkImage>();
        const auto sl = rd.Pod<VkImageLayout>();
        const auto dst = rd.Pod<VkBuffer>();
        const auto* regions = rd.Array<VkBufferImageCopy>(n);
        v.vkCmdCopyImageToBuffer(cb, src, sl, dst, n, regions);
        break;
      }
      case kCopyBuffer: {
        const auto src = rd.Pod<VkBuffer>();
        const auto dst = rd.Pod<VkBuffer>();
        const auto* regions = rd.Array<VkBufferCopy>(n);
        v.vkCmdCopyBuffer(cb, src, dst, n, regions);
        break;
      }
      case kClearColorImage: {
        const auto img = rd.Pod<VkImage>();
        const auto l = rd.Pod<VkImageLayout>();
        const auto c = rd.Pod<VkClearColorValue>();
        const auto* ranges = rd.Array<VkImageSubresourceRange>(n);
        v.vkCmdClearColorImage(cb, img, l, &c, n, ranges);
        break;
      }
      case kClearDepthStencilImage: {
        const auto img = rd.Pod<VkImage>();
        const auto l = rd.Pod<VkImageLayout>();
        const auto c = rd.Pod<VkClearDepthStencilValue>();
        const auto* ranges = rd.Array<VkImageSubresourceRange>(n);
        v.vkCmdClearDepthStencilImage(cb, img, l, &c, n, ranges);
        break;
      }
      case kBeginRendering: {
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.flags = rd.Pod<VkRenderingFlags>();
        ri.renderArea = rd.Pod<VkRect2D>();
        ri.layerCount = rd.Pod<uint32_t>();
        ri.viewMask = rd.Pod<uint32_t>();
        ri.pColorAttachments = rd.Array<VkRenderingAttachmentInfo>(ri.colorAttachmentCount);
        ri.pDepthAttachment = rd.Array<VkRenderingAttachmentInfo>(n);
        ri.pStencilAttachment = rd.Array<VkRenderingAttachmentInfo>(m);
        v.vkCmdBeginRendering(cb, &ri);
        break;
      }
      case kEndRendering: v.vkCmdEndRendering(cb); break;
      case kClearAttachments: {
        const auto* a = rd.Array<VkClearAttachment>(n);
        const auto* rects = rd.Array<VkClearRect>(m);
        v.vkCmdClearAttachments(cb, n, a, m, rects);
        break;
      }
      case kFillBuffer: {
        const auto b = rd.Pod<VkBuffer>();
        const auto off = rd.Pod<VkDeviceSize>();
        const auto size = rd.Pod<VkDeviceSize>();
        const auto data = rd.Pod<uint32_t>();
        v.vkCmdFillBuffer(cb, b, off, size, data);
        break;
      }
      case kBindPipeline: {
        const auto bp = rd.Pod<VkPipelineBindPoint>();
        const auto p = rd.Pod<VkPipeline>();
        v.vkCmdBindPipeline(cb, bp, p);
        break;
      }
      case kBindDescriptorSets: {
        const auto bp = rd.Pod<VkPipelineBindPoint>();
        const auto layout = rd.Pod<VkPipelineLayout>();
        const auto first = rd.Pod<uint32_t>();
        const auto* sets = rd.Array<VkDescriptorSet>(n);
        const auto* dyn = rd.Array<uint32_t>(m);
        v.vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, m, dyn);
        break;
      }
      case kBindIndexBuffer: {
        const auto b = rd.Pod<VkBuffer>();
        const auto off = rd.Pod<VkDeviceSize>();
        const auto t = rd.Pod<VkIndexType>();
        v.vkCmdBindIndexBuffer(cb, b, off, t);
        break;
      }
      case kDraw: {
        const auto vc = rd.Pod<uint32_t>();
        const auto ic = rd.Pod<uint32_t>();
        const auto fv = rd.Pod<uint32_t>();
        const auto fi = rd.Pod<uint32_t>();
        v.vkCmdDraw(cb, vc, ic, fv, fi);
        break;
      }
      case kDrawIndexed: {
        const auto ic = rd.Pod<uint32_t>();
        const auto inst = rd.Pod<uint32_t>();
        const auto fi = rd.Pod<uint32_t>();
        const auto vo = rd.Pod<int32_t>();
        const auto finst = rd.Pod<uint32_t>();
        v.vkCmdDrawIndexed(cb, ic, inst, fi, vo, finst);
        break;
      }
      case kSetViewport: {
        const auto first = rd.Pod<uint32_t>();
        const auto* vp = rd.Array<VkViewport>(n);
        v.vkCmdSetViewport(cb, first, n, vp);
        break;
      }
      case kSetScissor: {
        const auto first = rd.Pod<uint32_t>();
        const auto* sc = rd.Array<VkRect2D>(n);
        v.vkCmdSetScissor(cb, first, n, sc);
        break;
      }
      case kSetStencilReference: {
        const auto f = rd.Pod<VkStencilFaceFlags>();
        const auto ref = rd.Pod<uint32_t>();
        v.vkCmdSetStencilReference(cb, f, ref);
        break;
      }
      case kSetBlendConstants: {
        float c[4];
        std::memcpy(c, rd.p, 16);
        rd.p += 16;
        v.vkCmdSetBlendConstants(cb, c);
        break;
      }
      case kDispatch: {
        const auto x = rd.Pod<uint32_t>();
        const auto y = rd.Pod<uint32_t>();
        const auto z = rd.Pod<uint32_t>();
        v.vkCmdDispatch(cb, x, y, z);
        break;
      }
      case kResetQueryPool: {
        const auto p = rd.Pod<VkQueryPool>();
        const auto first = rd.Pod<uint32_t>();
        const auto count = rd.Pod<uint32_t>();
        v.vkCmdResetQueryPool(cb, p, first, count);
        break;
      }
      case kWriteTimestamp2: {
        const auto st = rd.Pod<VkPipelineStageFlags2>();
        const auto p = rd.Pod<VkQueryPool>();
        const auto q = rd.Pod<uint32_t>();
        v.vkCmdWriteTimestamp2(cb, st, p, q);
        break;
      }
      case kBeginQuery: {
        const auto p = rd.Pod<VkQueryPool>();
        const auto q = rd.Pod<uint32_t>();
        const auto f = rd.Pod<VkQueryControlFlags>();
        v.vkCmdBeginQuery(cb, p, q, f);
        break;
      }
      case kEndQuery: {
        const auto p = rd.Pod<VkQueryPool>();
        const auto q = rd.Pod<uint32_t>();
        v.vkCmdEndQuery(cb, p, q);
        break;
      }
      case kSetCullMode: v.vkCmdSetCullMode(cb, rd.Pod<VkCullModeFlags>()); break;
      case kSetFrontFace: v.vkCmdSetFrontFace(cb, rd.Pod<VkFrontFace>()); break;
      case kSetDepthBiasEnable: v.vkCmdSetDepthBiasEnable(cb, rd.Pod<VkBool32>()); break;
      case kSetDepthBias: {
        const auto c = rd.Pod<float>();
        const auto clamp = rd.Pod<float>();
        const auto slope = rd.Pod<float>();
        v.vkCmdSetDepthBias(cb, c, clamp, slope);
        break;
      }
      case kSetDepthTestEnable: v.vkCmdSetDepthTestEnable(cb, rd.Pod<VkBool32>()); break;
      case kSetDepthWriteEnable: v.vkCmdSetDepthWriteEnable(cb, rd.Pod<VkBool32>()); break;
      case kSetDepthCompareOp: v.vkCmdSetDepthCompareOp(cb, rd.Pod<VkCompareOp>()); break;
      case kSetStencilTestEnable: v.vkCmdSetStencilTestEnable(cb, rd.Pod<VkBool32>()); break;
      case kSetStencilOp: {
        const auto f = rd.Pod<VkStencilFaceFlags>();
        const auto fail = rd.Pod<VkStencilOp>();
        const auto pass = rd.Pod<VkStencilOp>();
        const auto dfail = rd.Pod<VkStencilOp>();
        const auto cmp = rd.Pod<VkCompareOp>();
        v.vkCmdSetStencilOp(cb, f, fail, pass, dfail, cmp);
        break;
      }
      case kSetStencilCompareMask: {
        const auto f = rd.Pod<VkStencilFaceFlags>();
        const auto mask = rd.Pod<uint32_t>();
        v.vkCmdSetStencilCompareMask(cb, f, mask);
        break;
      }
      case kSetStencilWriteMask: {
        const auto f = rd.Pod<VkStencilFaceFlags>();
        const auto mask = rd.Pod<uint32_t>();
        v.vkCmdSetStencilWriteMask(cb, f, mask);
        break;
      }
#ifdef VK_EXT_depth_bias_control
      case kSetDepthBias2: {
        VkDepthBiasInfoEXT bi{VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT};
        bi.depthBiasConstantFactor = rd.Pod<float>();
        bi.depthBiasClamp = rd.Pod<float>();
        bi.depthBiasSlopeFactor = rd.Pod<float>();
        VkDepthBiasRepresentationInfoEXT rep{VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
        if (rd.Pod<uint32_t>()) {
          rep.depthBiasRepresentation = rd.Pod<VkDepthBiasRepresentationEXT>();
          rep.depthBiasExact = rd.Pod<VkBool32>();
          bi.pNext = &rep;
        }
        real_bias2(cb, &bi);
        break;
      }
#endif
      case kBeginDebugLabel: {
        VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        std::memcpy(l.color, rd.p, 16);
        rd.p += 16;
        l.pLabelName = rd.Array<char>(n);
        v.vkCmdBeginDebugUtilsLabelEXT(cb, &l);
        break;
      }
      case kEndDebugLabel: v.vkCmdEndDebugUtilsLabelEXT(cb); break;
      default:
        Fail(VK_ERROR_UNKNOWN, "Vulkan recording thread: unknown op");
        return;
    }
  }
}

// Every table entry the thunks replace (field, thunk).
#define VKREC_ENTRIES(X) \
  X(vkResetCommandPool, ResetCommandPool) \
  X(vkBeginCommandBuffer, BeginCommandBuffer) \
  X(vkEndCommandBuffer, EndCommandBuffer) \
  X(vkQueueSubmit2, QueueSubmit2) \
  X(vkCmdPipelineBarrier2, CmdPipelineBarrier2) \
  X(vkCmdCopyImage, CmdCopyImage) \
  X(vkCmdCopyBufferToImage, CmdCopyBufferToImage) \
  X(vkCmdCopyImageToBuffer, CmdCopyImageToBuffer) \
  X(vkCmdCopyBuffer, CmdCopyBuffer) \
  X(vkCmdClearColorImage, CmdClearColorImage) \
  X(vkCmdClearDepthStencilImage, CmdClearDepthStencilImage) \
  X(vkCmdBeginRendering, CmdBeginRendering) \
  X(vkCmdEndRendering, CmdEndRendering) \
  X(vkCmdClearAttachments, CmdClearAttachments) \
  X(vkCmdFillBuffer, CmdFillBuffer) \
  X(vkCmdBindPipeline, CmdBindPipeline) \
  X(vkCmdBindDescriptorSets, CmdBindDescriptorSets) \
  X(vkCmdBindIndexBuffer, CmdBindIndexBuffer) \
  X(vkCmdDraw, CmdDraw) \
  X(vkCmdDrawIndexed, CmdDrawIndexed) \
  X(vkCmdSetViewport, CmdSetViewport) \
  X(vkCmdSetScissor, CmdSetScissor) \
  X(vkCmdSetStencilReference, CmdSetStencilReference) \
  X(vkCmdSetBlendConstants, CmdSetBlendConstants) \
  X(vkCmdDispatch, CmdDispatch) \
  X(vkCmdResetQueryPool, CmdResetQueryPool) \
  X(vkCmdWriteTimestamp2, CmdWriteTimestamp2) \
  X(vkCmdBeginQuery, CmdBeginQuery) \
  X(vkCmdEndQuery, CmdEndQuery) \
  X(vkCmdSetCullMode, CmdSetCullMode) \
  X(vkCmdSetFrontFace, CmdSetFrontFace) \
  X(vkCmdSetDepthBiasEnable, CmdSetDepthBiasEnable) \
  X(vkCmdSetDepthBias, CmdSetDepthBias) \
  X(vkCmdSetDepthTestEnable, CmdSetDepthTestEnable) \
  X(vkCmdSetDepthWriteEnable, CmdSetDepthWriteEnable) \
  X(vkCmdSetDepthCompareOp, CmdSetDepthCompareOp) \
  X(vkCmdSetStencilTestEnable, CmdSetStencilTestEnable) \
  X(vkCmdSetStencilOp, CmdSetStencilOp) \
  X(vkCmdSetStencilCompareMask, CmdSetStencilCompareMask) \
  X(vkCmdSetStencilWriteMask, CmdSetStencilWriteMask) \
  X(vkCmdBeginDebugUtilsLabelEXT, CmdBeginDebugUtilsLabel) \
  X(vkCmdEndDebugUtilsLabelEXT, CmdEndDebugUtilsLabel)

// Puts the thunks into the backend's table (and its vkCmdSetDepthBias2EXT pointer), keeping the driver's entry
// points in `rec.real`. Called on the caller's thread before rec.Start(); entries the driver lacks stay null.
inline void Install(Recorder& rec, Vk& vk, PFN_vkCmdSetDepthBias2EXT& bias2) {
  rec.real = vk;
  rec.real_bias2 = bias2;
  g_rec = &rec;
#define VKREC_SWAP(field, thunk) \
  if (vk.field) vk.field = thunk;
  VKREC_ENTRIES(VKREC_SWAP)
#undef VKREC_SWAP
#ifdef VK_EXT_depth_bias_control
  if (bias2) bias2 = CmdSetDepthBias2;
#endif
}
// Gives the backend its driver entry points back (after rec.Stop()).
inline void Uninstall(Recorder& rec, Vk& vk, PFN_vkCmdSetDepthBias2EXT& bias2) {
#define VKREC_RESTORE(field, thunk) vk.field = rec.real.field;
  VKREC_ENTRIES(VKREC_RESTORE)
#undef VKREC_RESTORE
  bias2 = rec.real_bias2;
  if (g_rec == &rec) g_rec = nullptr;
}

}  // namespace vkrec
