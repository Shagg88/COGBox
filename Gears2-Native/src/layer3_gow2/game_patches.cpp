// Layer 3 (Gears of War 2): the few xenia-canary game patches worth keeping (D28, notes/40), as switches. Source:
// xenia-canary/game-patches "4D53082D - Gears of War 2" (authors credited there: boma; Sowa_95, boma). Each site was
// disassembled from our image dump first; the manifest hooks reproduce the patched instruction's effect:
//  - --gow2_patch_aniso16: 0x8280E7FC `lwz r5,0x198(r30)` (the anisotropy setting) -> after it, r5 = 16.
//  - --gow2_patch_skip_coalesced_check: 0x82AA8904 `li r3,1` -> after it, r3 = 0; and 0x827F9148 (function entry,
//    `mflr r12`) returns at once - lets edited Coalesced (engine .ini) files load without the dirty-disc error.
//  - --gow2_patch_show_fps: sets the two engine flags at 0x83123468 / 0x83123470 (FPS and frame time) to 1 at load.
//  - --gow2_patch_texture_pool_mb (Canary "Texture Streaming Fix" part 1, boma; Canary uses 196): 0x8280D534
//    `lwz r4,80(r1)` in the pool setup 8280D4C8 reads the ini PoolSize in MB (140 on the disc: the pool is 140 MB,
//    0x8C00000) -> after it, r4 = the switch's value. 0 = the game's own value.
//  - --gow2_patch_texture_stats_off (part 2): RHIGetTextureMemoryStats 8280DDC8 (a leaf: writes the pool's allocated and
//    available bytes, returns 1) returns 0 at its first instruction, so the streamer never scales its budget down
//    ("FudgeFactor" stays 1.0).
//  - --gow2_ultrawide (Stage 7.5, notes/57; Canary "21:9 Ultrawide Aspect Ratio Support", authors boma and
//    TudorHorse): the game draws a 21:9 picture. Five sites, as the patch:
//      0x8252D73C `lfs f0,1032(r7)` (the field-of-view factor) -> after it, f0 = 0.0106355;
//      0x8252D608 `beq cr6,0x8252D69C` -> always taken (ConstrainAspectRatio off);
//      0x82806B10 `li r11,720` (the scene buffer height after the video-mode query) -> after it, r11 = 550;
//      0x826E3728 `bctrl` (the UI scene viewport's height getter) -> after it, r3 = 564 (the patch replaces the call);
//      the .rdata float at 0x821D0B28 (the widescreen aspect used at 0x82806B04) -> 2.33333, written at load.
//    The game's front buffer becomes 1280x550 itself (job 200), so the switch also has the window show it at that shape
//    (--gow2_present_frame_aspect): it fills a 21:9 window and is letterboxed in others.
//  Why (notes/51, jobs 150-152): when a texture allocation fails, the pool defragmenter 8280D848 (from the allocator
//  8280DB18) protects the whole pool writable and moves textures; in ch18 it runs ~1 s into the chapter title and the
//  title's font comes out as garbage. --gow2_log_texture_defrag logs every defrag (entry of 8280D848) with the swap.
// All off by default except the two texture streaming switches (on since job 154: job 153 passed - notes/51 section 12). The Black Shading Fix lives in untiled.cpp (on by default). The other patches in that file are
// emulator workarounds or cheap hacks and are deliberately not ported (D28).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>

#include "gow2_addresses.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <immintrin.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(gow2_patch_aniso16, false, "Gears2", "Force 16x anisotropic filtering (community patch, boma)");
REXCVAR_DEFINE_INT32(gow2_patch_query_poll_yield, 0, "Gears2",
                     "The render thread's GPU query poll: 0 as the title (spins), 1 pause per poll, 2 yield per poll "
                     "(job 178), 3 sleep until our GPU side writes guest memory after 64 quick polls (as gow2_patch_gpu_wait)");
REXCVAR_DEFINE_INT32(gow2_patch_gpu_wait, 1, "Gears2",
                     "Jobs 225/227 (notes/60), default on since job 227: the XDK's wait-for-the-GPU step (sub_82977688) - 0 as the title (its delay "
                     "loop is empty here, so it spins), 1 sleep until our GPU side writes a fence / the read pointer "
                     "(after a short spin; 1 ms safety timeout)");
REXCVAR_DEFINE_INT32(gow2_patch_query_wait, 2, "Gears2",
                     "Jobs 231/232 (notes/60): sub_82807588 (UE3's block-until-a-query-has-its-answer) polls ONE query in "
                     "a loop; 1 = after 32 quick polls each retry sleeps until our GPU side writes guest memory (1 ms max); "
                     "2 (default) = as 1 when the frame rate is capped (vsync on or --gow2_max_fps not -1), else off; 0 = off");
REXCVAR_DEFINE_BOOL(gow2_patch_skip_coalesced_check, false, "Gears2",
                    "Skip the Coalesced (engine .ini) hash check so edited files load (community patch, Sowa_95/boma)");
REXCVAR_DEFINE_BOOL(gow2_patch_show_fps, false, "Gears2", "Turn on the engine's FPS and frame-time display (community patch, Sowa_95)");
REXCVAR_DEFINE_INT32(gow2_patch_texture_pool_mb, 196, "Gears2",
                     "Texture pool size in MB (0 = the game's 140; default 196 as Canary's Texture Streaming Fix, boma)");
REXCVAR_DEFINE_BOOL(gow2_patch_texture_stats_off, true, "Gears2",
                    "Texture streaming never scales its budget down for a full pool (Canary Texture Streaming Fix, boma)");
REXCVAR_DEFINE_BOOL(gow2_mute_unfocused, false, "Gears2",
                    "Patch 0130: mute the sound while another window is in front (sets audio_mute; a player's own "
                    "--audio_mute=true is left alone)");
REXCVAR_DEFINE_BOOL(gow2_log_texture_defrag, false, "Gears2", "Log each texture pool defragmentation (8280D848) with the swap");
REXCVAR_DEFINE_BOOL(gow2_ultrawide, false, "Gears2",
                    "Stage 7.5: 21:9 ultrawide (Canary patch by boma and TudorHorse): a 1280x550 picture (x the "
                    "resolution scale) with a wider view; fills a 21:9 window or monitor, letterboxed in others");
REXCVAR_DEFINE_INT32(gow2_max_fps, 0, "Gears2",
                     "Stage 7.4: the engine's frame-rate limit (UE3 MaxSmoothedFrameRate, 62 in the game's GearEngine.ini): "
                     "0 = the game's, N = N fps, -1 = none. Above 30 also needs --gow2_gpu_vsync=false (the game asks for "
                     "every second vertical blank)");

// --gow2_memory_log_seconds (patch 0087; Gears 3's job 113 gauge, Gow3StartMemoryGauge, game-free - Gears 3 mail
// 23 Oct 21:24): the SDK gives the title 512 MB of "physical" memory (everything the GPU reads: textures, the texture
// pool, vertex buffers); "virtual" allocations are separate host memory and do not count against it. Logs both and the
// lowest free physical memory seen, so the texture pool (196 MB) can be sized from a measurement. Starts with the pool.
REXCVAR_DEFINE_INT32(gow2_memory_log_seconds, 10, "Gears2",
                     "Log the game's memory use every N seconds (physical = the 512 MB the GPU sees; virtual = the "
                     "game's other memory; lowest free physical so far). 0 = off");
static void Gow2StartMemoryGauge() {
  const int32_t secs = REXCVAR_GET(gow2_memory_log_seconds);
  if (secs <= 0) return;
  std::thread([secs] {
    uint32_t lowest_free_mb = 0xFFFFFFFFu;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(secs));
      auto* ks = REX_KERNEL_STATE();
      auto* mem = ks ? ks->memory() : nullptr;
      if (!mem) continue;
      const rex::memory::BaseHeap* phys = mem->GetPhysicalHeap();
      const rex::memory::BaseHeap* v4k = mem->LookupHeap(0x10000000u);
      const rex::memory::BaseHeap* v64k = mem->LookupHeap(0x50000000u);
      if (!phys || !v4k || !v64k) continue;
      auto used_mb = [](const rex::memory::BaseHeap* h) {
        return uint32_t((uint64_t(h->reserved_page_count()) * h->page_size()) >> 20);
      };
      const uint32_t total_mb = uint32_t((uint64_t(phys->total_page_count()) * phys->page_size()) >> 20);
      const uint32_t phys_used = used_mb(phys);
      const uint32_t free_mb = total_mb > phys_used ? total_mb - phys_used : 0;
      if (free_mb < lowest_free_mb) lowest_free_mb = free_mb;
      REXLOG_INFO("gow2_memory: physical {} of {} MB used, {} MB free (lowest {} MB) | virtual 4K {} MB, 64K {} MB",
                  phys_used, total_mb, free_mb, lowest_free_mb, used_mb(v4k), used_mb(v64k));
    }
  }).detach();
}

void Gow2PatchTexturePoolSize(PPCRegister& r4) {
  static std::atomic<bool> gauge{false};
  if (!gauge.exchange(true)) Gow2StartMemoryGauge();  // patch 0087
  const int32_t mb = REXCVAR_GET(gow2_patch_texture_pool_mb);
  REXLOG_INFO("gow2_patch_texture_pool_mb: texture pool {} MB (game's value {} MB)", mb > 0 ? uint32_t(mb) : r4.u32, r4.u32);
  if (mb > 0 && mb <= 400) r4.u64 = uint32_t(mb);
}

bool Gow2PatchTextureStatsOff(PPCRegister& r3) {
  if (!REXCVAR_GET(gow2_patch_texture_stats_off)) return false;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_patch_texture_stats_off: RHIGetTextureMemoryStats returns FALSE"); }
  r3.u64 = 0;
  return true;  // return_on_true: return before the function's first instruction
}

void Gow2NoteTextureDefrag() {
  if (!REXCVAR_GET(gow2_log_texture_defrag)) return;
  static std::atomic<uint32_t> n{0};
  const uint32_t i = n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (i <= 200) REXLOG_INFO("gow2_texture_defrag: defrag #{} at swap {}", i, XenosSwapCount());
}

// Job 225: one step of a wait for the GPU. Steps less than 200 us apart belong to the same wait; its first 64 steps
// only pause (short waits stay fast), later ones sleep until our GPU side writes guest memory (a fence, the read
// pointer, a query report - XenosNotifyGuestWrite) or 1 ms.
struct Gow2WaitStreak {
  uint64_t last_qpc = 0;
  uint32_t count = 0;
};
static void Gow2SleepStep(Gow2WaitStreak& w, uint32_t quick_steps = 64) {
  static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (uint64_t(now.QuadPart) - w.last_qpc > freq / 5000) w.count = 0;
  if (++w.count <= quick_steps) {
    for (int i = 0; i < 32; ++i) _mm_pause();
  } else {
    XenosWaitGuestWrite(XenosGuestWriteSeq(), 1);
  }
  QueryPerformanceCounter(&now);
  w.last_qpc = uint64_t(now.QuadPart);
}

// Job 178: the title's render thread polls the GPU (a D3D query's GetData, sub_829783A8, called in a loop by
// sub_82807588) flat out - ~45% of a core in the call alone. 1: a short pause per poll (frees the core's other
// hardware thread - our renderer may share the physical core); 2: give up the time slice every poll.
void Gow2QueryPollYield() {
  const int mode = REXCVAR_GET(gow2_patch_query_poll_yield);
  if (mode == 1) {
    for (int i = 0; i < 32; ++i) _mm_pause();
  } else if (mode == 2) {
    SwitchToThread();
  } else if (mode == 3) {
    thread_local Gow2WaitStreak streak;
    Gow2SleepStep(streak);
  }
}

// Job 231 (notes/60): sub_82807588 is UE3's wait for one query's answer: GetData(query, out, 4, 0) once, then
// GetData(..., FLUSH) in a loop (0x828075BC-0x828075D8) until it returns 0, timing the stall into a global counter.
// Unlike every GetData call (job 227's mode 3, which caught the many queries a frame that answer at once), each pass
// of this loop is a real wait on the same query, whose answer (a sample-count report or a fence / read pointer) our
// GPU side writes - so the retry can sleep on XenosNotifyGuestWrite. The hook sits on the loop head (0x828075BC).
void Gow2QueryWaitStep() {
  const int32_t mode = REXCVAR_GET(gow2_patch_query_wait);
  if (mode == 0) return;
  // Job 232: uncapped (vsync off and no frame limit) the sleep cost ~2% in job 231; capped it saves ~0.3 of a core.
  // Job 233: Query<bool> looks the cvar up by name (GetFlagByName) - 4.3% of the game's render thread when this loop
  // spins uncapped. Read it once per 1024 calls instead (the F4 overlay can still change it while playing).
  thread_local uint32_t vsync_calls = 0;
  thread_local bool vsync_on = true;
  if ((vsync_calls++ & 1023) == 0) vsync_on = rex::cvar::Query<bool>("gow2_gpu_vsync");
  if (mode == 2 && !vsync_on && REXCVAR_GET(gow2_max_fps) == -1) return;
  if (mode != 1 && mode != 2) return;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow2_patch_query_wait: on - the query wait loop sleeps between polls");
  thread_local Gow2WaitStreak streak;
  Gow2SleepStep(streak, 32);
}

// Job 225 (notes/60): sub_82977688 is one step of the XDK's wait for the GPU (sub_8298C2A0 calls it until the GPU's read
// pointer passes a point in the ring). On the console the step lowers the hardware thread's priority (cctpl), burns
// 32 x db16cyc and raises it again (cctpm); recompiled, those are empty, so the game's render thread spins at 100%,
// also when the frame rate is capped. The hook sits on the cctpl at 0x829776A0 and, when on, replaces the delay loop
// (jump to the cctpm at 0x829776CC): the first 64 steps of a wait just pause (short waits stay fast), after that each
// step sleeps until our GPU side writes guest memory (fences, the read pointer - XenosNotifyGuestWrite) or 1 ms. The
// rest of the step (the GPU-hang check, the read-pointer test in the caller) runs as before.
bool Gow2GpuWaitStep() {
  if (REXCVAR_GET(gow2_patch_gpu_wait) != 1) return false;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow2_patch_gpu_wait: on - the GPU wait sleeps instead of spinning");
  thread_local Gow2WaitStreak streak;
  Gow2SleepStep(streak);
  return true;
}

// Stage 7.4 (notes/56): sub_824A2E78 is the engine's GetMaxTickRate: with frame-rate smoothing on it loads
// MaxSmoothedFrameRate (`lfs f1,1284(r3)` at 0x824A2E94) and returns it; this hook runs after that load. The same site is
// the first half of the community "Unlock FPS" patch (illusion, boma), which nops the load (the default, no limit, is
// returned then); the second half (swap interval 2 -> 1) is our --gow2_gpu_vsync=false.
void Gow2MaxTickRate(PPCRegister& f1) {
  const int32_t v = REXCVAR_GET(gow2_max_fps);
  if (!v) return;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("gow2_max_fps: engine frame-rate limit {} -> {}", f1.f64, v < 0 ? std::string("none") : std::to_string(v));
  }
  f1.f64 = v < 0 ? 0.0 : double(v);
}

// Stage 7.5 ultrawide hooks (see the top of the file).
void Gow2UltrawideFov(PPCRegister& f0) {
  if (!REXCVAR_GET(gow2_ultrawide)) return;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_ultrawide: field-of-view factor {} -> 0.0106355", f0.f64); }
  f0.f64 = double(0.0106355f);
}

bool Gow2UltrawideNoConstrain() { return REXCVAR_GET(gow2_ultrawide); }  // jump_address_on_true: the beq is taken

void Gow2UltrawideSceneHeight(PPCRegister& r11, PPCRegister& f13) {
  if (!REXCVAR_GET(gow2_ultrawide)) return;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_ultrawide: scene buffer height {} -> 550 (aspect factor {})", r11.u32, f13.f64); }
  r11.u64 = 550;
}

void Gow2UltrawideUiHeight(PPCRegister& r3) {
  if (!REXCVAR_GET(gow2_ultrawide)) return;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_ultrawide: UI viewport height {} -> 564", r3.u32); }
  r3.u64 = 564;
}

// Stage 6 Horde crash (notes/58, jobs 206-208): about 2 s into Horde's first wave countdown the main thread reads through
// a null pointer in sub_828307A0 (`lwz r11,4(r4)` then `lwz r9,4(r11)`), called from sub_82826EC8 with r4 = the
// caller's field at +1004. Same on the SDK reference, so not a drawing problem. This hook (the function's first
// instruction) logs the arguments when that pointer is null; --gow2_patch_horde_guard makes the function return at
// once then (a test of what follows, not a fix).
REXCVAR_DEFINE_BOOL(gow2_patch_horde_guard, false, "Gears2",
                    "Test only: sub_828307A0 returns at once when its second argument's +4 pointer is null (Horde crash, "
                    "notes/58)");
static uint8_t* g_gow2_membase = nullptr;
static uint32_t Gow2Load32(uint32_t a) {
  const uint8_t* p = g_gow2_membase + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static bool Gow2Readable(uint32_t a) { return a >= 0x10000u && a < 0xFFFF0000u; }

bool Gow2HordeGuard(PPCRegister& r3, PPCRegister& r4) {
  if (!g_gow2_membase) return false;
  const uint32_t a = r4.u32;
  if (Gow2Readable(a) && Gow2Readable(Gow2Load32(a + 4))) return false;
  static std::atomic<uint32_t> n{0};
  const uint32_t i = n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (i <= 20) {
    std::string words;
    if (Gow2Readable(a))
      for (uint32_t k = 0; k < 8; ++k) {
        char w[12];
        std::snprintf(w, sizeof(w), " %08X", Gow2Load32(a + 4 * k));
        words += w;
      }
    const uint32_t self = r3.u32;
    REXLOG_INFO("gow2_horde_guard: #{} at swap {}: sub_828307A0(r3 {:08X} vtable {:08X}, r4 {:08X}) - r4+4 is not a "
                "pointer; r4 words:{}{}",
                i, XenosSwapCount(), self, Gow2Readable(self) ? Gow2Load32(self) : 0u, a, words.empty() ? " (unreadable)" : words,
                REXCVAR_GET(gow2_patch_horde_guard) ? " -> returning" : "");
  }
  return REXCVAR_GET(gow2_patch_horde_guard);
}

void Gow2PatchAniso16(PPCRegister& r5) {
  if (!REXCVAR_GET(gow2_patch_aniso16)) return;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_patch_aniso16: anisotropy {} -> 16", r5.u32); }
  r5.u64 = 16;
}

void Gow2PatchCoalescedResult(PPCRegister& r3) {
  if (REXCVAR_GET(gow2_patch_skip_coalesced_check)) r3.u64 = 0;
}

bool Gow2PatchCoalescedSkip() {
  if (!REXCVAR_GET(gow2_patch_skip_coalesced_check)) return false;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow2_patch_skip_coalesced_check: hash check skipped"); }
  return true;  // return_on_true: the function returns before its prologue
}

// Writes a big-endian float into the loaded image (.rdata may be mapped read-only on the host: lift that for the write).
static void Gow2WriteGuestFloat(rex::Runtime* runtime, uint32_t addr, float value, const char* what) {
  uint8_t* p = runtime->memory()->TranslateVirtual<uint8_t*>(addr);
  uint32_t old_bits = 0, bits = 0;
  std::memcpy(&bits, &value, 4);
  for (int i = 0; i < 4; ++i) old_bits = (old_bits << 8) | p[i];
  float old_value;
  std::memcpy(&old_value, &old_bits, 4);
#ifdef _WIN32
  DWORD old_protect = 0;
  const bool lifted = VirtualProtect(p, 4, PAGE_READWRITE, &old_protect) != 0;
#endif
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(bits >> (24 - 8 * i));
#ifdef _WIN32
  if (lifted) VirtualProtect(p, 4, old_protect, &old_protect);
#endif
  REXLOG_INFO("{}: {:08X} {} -> {}", what, addr, old_value, value);
}

void Gow2ApplyDataPatchesIfRequested(rex::Runtime* runtime) {
  if (!runtime || !runtime->memory()) return;
  g_gow2_membase = runtime->virtual_membase();
  if (REXCVAR_GET(gow2_patch_show_fps)) {
    for (uint32_t addr : {gow2::addr::kShowFpsFlag, gow2::addr::kShowFrameTimeFlag}) {
      *runtime->memory()->TranslateVirtual<uint8_t*>(addr) = 1;
    }
    REXLOG_INFO("gow2_patch_show_fps: FPS and frame-time display flags set");
  }
  if (REXCVAR_GET(gow2_ultrawide)) {
    Gow2WriteGuestFloat(runtime, gow2::addr::kWidescreenAspect, 2.33333f, "gow2_ultrawide: widescreen aspect");
    // Job 200: the front buffer itself becomes 1280x550 (x the resolution scale). Show it at that shape (letterboxed in
    // a window of another shape) rather than Canary's way (letterbox off, which is right only in a 21:9 window).
    const bool a = rex::cvar::SetFlagByName("gow2_present_frame_aspect", "true");
    REXLOG_INFO("gow2_ultrawide: present at the front buffer's own shape {}", a);
  }
}

// Patch 0066 (Shagg 19:09-19:23: build new chapters' shaders behind the loading screen): UE3's shader constructors,
// after each has copied the shader's microcode into its own allocation (disassembled from the image dumps;
// gow2_manifest.toml / gow2_manifest_tu6.toml):
//  - pixel shader 82809A38 (TU6 82819608): at 82809BBC (TU6 82819790) `stw r30,8(r31)`, r6 = the microcode it just
//    stored at header +24 (stw r6,0x18(r30)), r27 = its size in bytes (lwz r27,8(blob));
//  - vertex shader 828098D0 (TU6 82819440): at 82809A20 (TU6 82819594) `bl 829831F0` (the XDK's vertex-shader
//    header init, notes/29), r4 = the microcode, r28 = its size in bytes.
// The renderer translates each one and has the driver build it before its first draw (XenosGuestShaderCreated).
void Gow2ShaderCreatedPixel(PPCRegister& r6, PPCRegister& r27) { XenosGuestShaderCreated(false, r6.u32, r27.u32); }
void Gow2ShaderCreatedVertex(PPCRegister& r4, PPCRegister& r28) { XenosGuestShaderCreated(true, r4.u32, r28.u32); }
