// Layer 1 (XDK 7645 D3D library profile): Stage 1 groundwork - watch the game's resource calls without changing them.
// Mid-asm hooks at the entry of each function (gow2_manifest.toml) call these with the argument registers. Addresses and
// roles come from the earlier research pass (roadmap section 3, notes/29): UE3 builds resources with XGraphics header
// calls over memory it allocates itself, binds them with XGOffset*Address, locks through one core function, and unlocks
// through 829768C0 plus two inlined copies; texture-pool defrag and mip reallocation move contents without any lock.
// The same hooks feed the Stage 1 comparison mode (resource_check.cpp, --gow2_res_check) with "this memory is about
// to change" events.
// --gow2_d3d_watch=N logs, every N frames, how often each was called and the arguments of its first call in that window.
#include <xenos/dev_tools.h>
#include <xdk7645/stage1.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

REXCVAR_DEFINE_INT32(gow2_d3d_watch, 0, "Gears2", "Log resource-call counts (XG headers, lock/unlock, defrag) every N frames");

namespace {

enum Call : int {
  kXGSetTextureHeaderEx, kXGSetCubeTextureHeaderEx, kXGSetVertexBufferHeader, kXGSetIndexBufferHeader,
  kXGOffsetResourceAddress, kXGOffsetBaseTextureAddress, kLockCore, kUnlock, kInlineUnlockA, kInlineUnlockB,
  kPoolDefrag, kMipRealloc, kDeferredFree, kCount
};
constexpr const char* kNames[kCount] = {
    "XGSetTextureHeaderEx 829A0C40", "XGSetCubeTextureHeaderEx 829A0D78", "XGSetVertexBufferHeader 829A00D8",
    "XGSetIndexBufferHeader 829A0170", "XGOffsetResourceAddress 829A0E58", "XGOffsetBaseTextureAddress 829A0DF0",
    "Lock core 82976600", "Unlock 829768C0", "inline unlock fn 8259CE00", "inline unlock fn 827BF6F0",
    "texture pool defrag 8280D150", "mip realloc 8280B0C0", "deferred free 828093B8"};

struct Window {
  std::array<std::atomic<uint64_t>, kCount> calls{};
  std::array<std::array<uint32_t, 4>, kCount> first_args{};
  std::array<std::atomic<bool>, kCount> have_args{};
  std::array<uint64_t, kCount> totals{};
  uint64_t logged_at = 0;
  std::mutex mutex;
};

Window& W() {
  static Window w;
  return w;
}

void Record(Call c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  const int every = REXCVAR_GET(gow2_d3d_watch);
  if (every <= 0) return;
  Window& w = W();
  w.calls[c].fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (w.have_args[c].compare_exchange_strong(expected, true)) w.first_args[c] = {a0, a1, a2, a3};
  const uint64_t frame = XenosSwapCount();
  if (frame >= w.logged_at + uint64_t(every)) {
    std::lock_guard<std::mutex> lock(w.mutex);
    if (frame < w.logged_at + uint64_t(every)) return;
    w.logged_at = frame;
    for (int i = 0; i < kCount; ++i) {
      const uint64_t n = w.calls[i].exchange(0);
      w.totals[i] += n;
      const auto& a = w.first_args[i];
      if (n) {
        REXLOG_INFO("gow2_d3d_watch: frame {} {:<34} {:>6} calls (total {:>8}) first args {:08X} {:08X} {:08X} {:08X}",
                    frame, kNames[i], n, w.totals[i], a[0], a[1], a[2], a[3]);
      }
      w.have_args[i].store(false);
    }
  }
}

}  // namespace

// Called by the generated code (gow2_manifest.toml [[entrypoint.midasm_hook]]) at each function's first instruction.
void Gow2WatchXGSetTextureHeaderEx(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGSetTextureHeaderEx, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchXGSetCubeTextureHeaderEx(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGSetCubeTextureHeaderEx, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchXGSetVertexBufferHeader(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGSetVertexBufferHeader, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchXGSetIndexBufferHeader(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGSetIndexBufferHeader, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchXGOffsetResourceAddress(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGOffsetResourceAddress, r3.u32, r4.u32, r5.u32, r6.u32); Gow2ResourceExpect(r4.u32, "XGOffsetResourceAddress"); }
void Gow2WatchXGOffsetBaseTextureAddress(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kXGOffsetBaseTextureAddress, r3.u32, r4.u32, r5.u32, r6.u32); Gow2ResourceExpect(r4.u32, "XGOffsetBaseTextureAddress"); if (r5.u32) Gow2ResourceExpect(r5.u32, "XGOffsetBaseTextureAddress mip"); }
void Gow2WatchLockCore(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kLockCore, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchUnlock(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kUnlock, r3.u32, r4.u32, r5.u32, r6.u32); Gow2ResourceExpect(r4.u32, "Unlock"); }
void Gow2WatchInlineUnlockA(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kInlineUnlockA, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchInlineUnlockB(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kInlineUnlockB, r3.u32, r4.u32, r5.u32, r6.u32); }
void Gow2WatchPoolDefrag(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kPoolDefrag, r3.u32, r4.u32, r5.u32, r6.u32); Gow2ResourceExpect(0, "texture pool defrag", true); }
void Gow2WatchMipRealloc(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kMipRealloc, r3.u32, r4.u32, r5.u32, r6.u32); Gow2ResourceExpect(0, "mip realloc", true); }
void Gow2WatchDeferredFree(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) { Record(kDeferredFree, r3.u32, r4.u32, r5.u32, r6.u32); }
