// Layer 2 (Unreal Engine 3, both games): patch 0068 - shaders built while the game loads an area (Shagg: "if it's able
// to build behind the actual loading screen"). Our code (MIT).
//
// The renderer meets an area's shaders at its first draws: on a first visit ~350 new pipelines at 15-35 ms each in
// the driver (job 358), which the 0063 hold covers with a 2-3 s pause. But the shaders are in the game's own files,
// and the game opens those files seconds before the first draw. So when the game opens a package (SDK patch 32's
// file-open observer), a background thread here reads the same file from disk, unpacks it and finds the shader
// containers, and hands each pixel shader's microcode to the renderer, which translates it exactly as a draw would
// and has the driver compile it in a warm-only pipeline (layer 0, xenos/dev_tools.h XenosPrebuildPixelShader). The
// driver keeps each compiled stage (job 362c [V]), so the real pipelines later cost ~0.1 ms. Nothing is written to
// disk and nothing ships: every player's own game files are the source.
//
// Package layout (UE3 on the 360; found with the Gears 3 chat, jobs 365 / 366 and its jobs 004 / 006):
// - Compressed chunks: tag 0x9E2A83C1, block size (a power of two, 0x20000 here), compressed and uncompressed totals,
//   then per block [compressed size][uncompressed size], then the blocks. Each block is XMemCompress-framed LZX
//   (frames "FF uu uu cc cc" or "cc cc"; window 128 KB). Found by scanning for the tag with its totals checked, so the
//   per-game package summary layout does not matter.
// - Shader containers: magic 0x102A1100 (pixel) / 0x102A1101 (vertex), [1] offset to the microcode, [2] its size.
//   Gears 2: each container follows a TArray count (offset + size). Gears 3: inside SeekFreeShaderCache, in inner
//   XMemCompress blocks ([u32 uncompressed][u32 compressed] then frames), packed back to back.
// - Hash key: XXH3-64 of the big-endian microcode, the same key as the renderer's.
// Pixel shaders only: the XDK patches vertex shaders at bind, so 0 of 218 drawn vertex shaders match the files (job 367
// [V]); ~70% of drawn pixel shaders do.
// LZX: decoder from PR #43 (legoliamneeson), ue3/prebuild_lzx.h, in place of the SDK's lzx_decompress (libmspack),
// which lacks the "no pad after an odd uncompressed block" fix, so the blocks that needed it were skipped. It tries the
// XMemCompress form first and the CAB-style pad only as a second attempt; the log line counts what each fix added.
// Also from PR #43: the bytes between and around compressed chunks are scanned for containers too, and a fully
// compressed file of which no chunk decoded is logged as unreadable instead of passing as a package without shaders.
#include <ue3/shader_prebuild.h>

#include <ue3/prebuild_lzx.h>
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <utility>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(ue3_shader_prebuild, false, "Gears2",
                    "Patch 0068: when the game opens a package, read its pixel shaders from the file and have our "
                    "renderer build them before their first draw (needs SDK patch 32 and the DXBC shader path). "
                    "Patch 0069: off by default until it measurably shortens first-visit holds (job 368b: 97% of a "
                    "cold area's new pixel shaders were in the opened packages, but ~10,000 were offered per load, so "
                    "most were not built in time, and building them doubled a cold ch10's frame time)");

REXCVAR_DEFINE_STRING(ue3_shader_prebuild_at_start, "", "Gears2",
                      "Job 630 (Shagg 19:05: build the shipped recipes during the opening videos): folders (';' between "
                      "them - the game's data and update folders) whose packages are all read at start-up, shader caches "
                      "first, instead of waiting for the game to open each one; with a recipe pack only its shaders are "
                      "built. Same low-priority reader thread. Empty = only the packages the game opens");

REXCVAR_DEFINE_INT32(ue3_shader_prebuild_threads, 0, "Gears2",
                     "Job 632: reader threads with --ue3_shader_prebuild_at_start (0 = a quarter of the CPU threads, 2 to 4): "
                     "the videos and menus leave the CPU idle; all at the lowest priority, so the game still comes first");

namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// One XMemCompress-framed LZX block into out (out_size bytes). True = decoded; *padded = only with the CAB-style pad.
bool Lzx(const uint8_t* in, size_t n, uint8_t* out, size_t out_size, std::vector<uint8_t>& scratch, bool* padded) {
  *padded = false;
  scratch.clear();
  size_t p = 0;
  while (p + 2 <= n) {
    size_t csz;
    if (in[p] == 0xFF) {
      if (p + 5 > n) break;
      csz = Be16(in + p + 3);
      p += 5;
    } else {
      csz = Be16(in + p);
      p += 2;
    }
    if (!csz || p + csz > n) break;
    scratch.insert(scratch.end(), in + p, in + p + csz);
    p += csz;
  }
  if (scratch.empty()) return false;
  return ue3::prebuild::PrebuildLzxDecode(scratch.data(), scratch.size(), out, out_size, 17, padded);
}

// The frames of an XMemCompress stream of `c` bytes add up exactly to `c` input bytes and `u` output bytes (a frame
// "FF uu uu cc cc" gives its output size, a plain "cc cc" frame is 32 KB). A cheap test before trying to decode.
bool FramesFit(const uint8_t* in, size_t c, size_t u) {
  size_t p = 0, out = 0;
  while (p < c && out < u) {
    size_t csz, usz;
    if (in[p] == 0xFF) {
      if (p + 5 > c) return false;
      usz = Be16(in + p + 1);
      csz = Be16(in + p + 3);
      p += 5;
    } else {
      if (p + 2 > c) return false;
      usz = 0x8000;
      csz = Be16(in + p);
      p += 2;
    }
    if (!csz || !usz || p + csz > c) return false;
    p += csz;
    out += std::min(usz, u - out);
  }
  return out == u && c - p <= 16;  // a few bytes may follow the last frame (9 seen in Gears 3)
}

struct ScanResult {
  uint32_t chunks = 0, failed_chunks = 0, nested = 0, pixel = 0, vertex = 0, new_pixel = 0;
  uint32_t offered[2] = {};  // pixel shaders handed over per variant (see Containers)
  // What PR #43's parts added: blocks / whole chunks / inner blocks decoded only by the padded second attempt, and
  // shader records (pixel + vertex) found in the bytes outside the compressed chunks of a compressed package.
  uint32_t padded_blocks = 0, padded_chunks = 0, padded_nested = 0, gap_records = 0;
  bool unreadable = false;  // starts like a fully compressed file, but no chunk decoded
};

class PackageScanner {
 public:
  explicit PackageScanner(std::unordered_set<uint64_t>& seen) : seen_(seen) {}
  // Which variants to hand over (Containers); both by default.
  void SetVariants(bool whole, bool code_only) {
    offer_[0] = whole;
    offer_[1] = code_only;
  }
  ScanResult Scan(const std::vector<uint8_t>& b) {
    r_ = {};
    std::vector<uint8_t> all;
    std::vector<std::pair<size_t, size_t>> regions;  // [begin, end) of each compressed chunk in b
    uint32_t decoded_chunks = 0;
    const size_t n = b.size();
    for (size_t i = 0; i + 24 <= n; ++i) {
      if (b[i] != 0x9E || b[i + 1] != 0x2A || b[i + 2] != 0x83 || b[i + 3] != 0xC1) continue;
      const uint32_t bsz = Be32(&b[i + 4]), tc = Be32(&b[i + 8]), tu = Be32(&b[i + 12]);
      if (bsz < 0x8000 || bsz > 0x100000 || (bsz & (bsz - 1)) || !tu || tu > 0x10000000u || !tc || tc > n) continue;
      const uint32_t nb = (tu + bsz - 1) / bsz;
      if (i + 16 + size_t(nb) * 8 > n) continue;
      const uint8_t* blk = &b[i + 16];
      const uint8_t* data = blk + size_t(nb) * 8;
      uint64_t sumc = 0, sumu = 0;
      for (uint32_t k = 0; k < nb; ++k) {
        sumc += Be32(blk + k * 8);
        sumu += Be32(blk + k * 8 + 4);
      }
      if (sumc != tc || sumu != tu || size_t(data - b.data()) + tc > n) continue;
      const size_t base = all.size();
      all.resize(base + tu);
      size_t w = 0;
      bool ok = true, any = false, padded = false;
      for (uint32_t k = 0; k < nb; ++k) {
        const uint32_t cs = Be32(blk + k * 8), us = Be32(blk + k * 8 + 4);
        // Each block is compressed on its own, so a block that fails to decode is zeros and the next ones still count.
        // A block never unpacks to more than the chunk's block size (the decoder's output-size check).
        if (w + us > tu || us > bsz) {
          ok = false;
          break;
        }
        bool pad = false;
        if (Lzx(data, cs, &all[base + w], us, scratch_, &pad)) {
          any = true;
          if (pad) {
            padded = true;
            ++r_.padded_blocks;
          }
        } else {
          ok = false;
          std::memset(&all[base + w], 0, us);
        }
        data += cs;
        w += us;
      }
      ++r_.chunks;
      if (!ok) ++r_.failed_chunks;
      if (ok && padded) ++r_.padded_chunks;  // fully decoded only thanks to the second attempt
      if (any) ++decoded_chunks;
      regions.emplace_back(i, size_t(data - b.data()));
      i = size_t(data - b.data()) - 1;
    }
    if (r_.chunks) {
      Containers(all.data(), all.size(), false);
      Nested(all.data(), all.size());
    }
    // The bytes outside the compressed chunks: the whole file when it has none (the global shader cache), else the
    // package summary and anything stored between or after the chunks (PR #43).
    const uint32_t before_gaps = r_.pixel + r_.vertex;
    size_t raw = 0;
    regions.emplace_back(n, n);
    for (const auto& [begin, end] : regions) {
      if (begin > raw) Containers(b.data() + raw, begin - raw, false);
      raw = end;
    }
    if (r_.chunks) r_.gap_records = r_.pixel + r_.vertex - before_gaps;
    // A fully compressed file (the chunk tag at 0 followed by a power-of-two block size; an uncompressed package has
    // its version there) of which no chunk decoded is unreadable, not a package without shaders (PR #43).
    if (n >= 8 && Be32(b.data()) == 0x9E2A83C1u) {
      const uint32_t block = Be32(b.data() + 4);
      if (block >= 0x8000 && block <= 0x100000 && !(block & (block - 1)) && !decoded_chunks) r_.unreadable = true;
    }
    return r_;
  }

 private:
  // One pixel shader record. Its microcode region is [literal constants][code]: the header word before the first
  // 0x1000xx00 word is the code size and the one before that the constants' size (they add up to the region's size).
  // Which part a draw shows the GPU differs by game [V, 2026-10-23 against the drawn records]: Gears 2 mostly the
  // whole region (Hospital_03: 187 whole vs 16 code-only), Gears 3 the code only (5 vs 120). Both are handed over
  // until the renderer has seen which one this game draws (Ue3: XenosPrebuildVariantHits).
  void Add(bool vertex, const uint8_t* rec, uint32_t off, uint32_t size) {
    if (vertex) {
      ++r_.vertex;
      // notes/78 C3: the renderer rebuilds recipe vertex shaders from these (a no-op without a recipe pack).
      if (Fresh(XXH3_64bits(rec + off, size) ^ 0x5653ull)) XenosPrebuildVertexShader(rec + off, size);
      return;
    }
    ++r_.pixel;
    const uint8_t* region = rec + off;
    Offer(0, region, size);
    for (uint32_t w = 3; w + 1 < off / 4; ++w) {
      if ((Be32(rec + w * 4) & 0xFFFF00FFu) != 0x10000000u) continue;
      const uint32_t consts = Be32(rec + (w - 2) * 4), code = Be32(rec + (w - 1) * 4);
      if (consts && !(consts & 3) && code && consts + code == size) Offer(1, region + consts, code);
      break;
    }
  }
  void Offer(uint32_t variant, const uint8_t* code, uint32_t size) {
    if (!offer_[variant] || !Fresh(XXH3_64bits(code, size))) return;
    ++r_.new_pixel;
    ++r_.offered[variant];
    XenosPrebuildPixelShader(code, size, variant);
  }
  // Shader containers in a buffer. loose = packed back to back (no TArray count before each).
  uint32_t Containers(const uint8_t* a, size_t n, bool loose) {
    uint32_t count = 0;
    for (size_t i = loose ? 0 : 4; i + 12 <= n; ++i) {
      if (a[i] != 0x10 || a[i + 1] != 0x2A || a[i + 2] != 0x11 || a[i + 3] > 1) continue;
      const uint32_t off = Be32(a + i + 4), sz = Be32(a + i + 8);
      if (!sz || sz > 262144 || (sz & 3) || off < 12 || off > 65536 || i + off + sz > n) continue;
      bool ok = false;
      if (i >= 4) {
        const uint32_t cbe = Be32(a + i - 4);
        const uint32_t cle = uint32_t(a[i - 4]) | uint32_t(a[i - 3]) << 8 | uint32_t(a[i - 2]) << 16 |
                             uint32_t(a[i - 1]) << 24;
        ok = off + sz == cbe || off + sz == cle;
      }
      if (!ok && loose) {
        const size_t e = i + off + sz;
        ok = e + 4 > n || (a[e] == 0x10 && a[e + 1] == 0x2A && a[e + 2] == 0x11);
      }
      if (!ok) continue;
      Add(a[i + 3] != 0, a + i, off, sz);
      ++count;
    }
    return count;
  }
  // Gears 3: LZX blocks inside the decompressed package ([u32 U][u32 C] then frames).
  void Nested(const uint8_t* b, size_t n) {
    std::vector<uint8_t> out;
    for (size_t i = 12; i + 5 <= n; ++i) {
      if (b[i] != 0xFF && (b[i - 12] | b[i - 11])) continue;
      const uint32_t U = Be32(b + i - 8), C = Be32(b + i - 4);
      if (U < 64 || U > 0x1000000u || C < 8 || C > U + 1024 || i + C > n) continue;
      const uint32_t first = U < 0x8000 ? U : 0x8000;
      if (b[i] == 0xFF) {
        if (Be16(b + i + 1) != first || Be16(b + i + 3) + 5 > C) continue;
      } else {
        const uint32_t cs = Be16(b + i);
        if (U < 0x8000 || !cs || cs + 2 > C) continue;
      }
      if (!FramesFit(b + i, C, U)) continue;
      out.resize(U);
      bool pad = false;
      if (Lzx(b + i, C, out.data(), U, scratch_, &pad) && Containers(out.data(), U, true)) {
        ++r_.nested;
        if (pad) ++r_.padded_nested;
        i += C - 1;
      }
    }
  }
  std::unordered_set<uint64_t>& seen_;
  bool offer_[2] = {true, true};
  // Job 632: several reader threads at start-up share one set of shaders already handed over.
  static std::mutex& SeenMutex() { static std::mutex m; return m; }
  bool Fresh(uint64_t h) { std::lock_guard<std::mutex> lock(SeenMutex()); return seen_.insert(h).second; }
  std::vector<uint8_t> scratch_;
  ScanResult r_;
};

#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
struct Prebuild {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::filesystem::path> queue;
  std::unordered_set<std::string> opened;  // lower-case file names already queued (each package once per session)
  std::unordered_set<std::string> waiting;  // job 666: ... of those, the ones still in `queue` (not taken by a reader)
  std::thread thread;
  std::filesystem::path shader_files;      // job 632: <shader cache>\prebuild-files.txt - packages that held shaders
  std::unordered_set<std::string> known;   // ... their lower-case names (read at start, added to as found)
  std::unordered_set<uint64_t> seen;  // reader thread: pixel shader hashes already handed over
  // Job 641 (under mutex): readers busy with a file, files read, the start-up list queued, the start-up reading done.
  int busy = 0;
  uint64_t files_read = 0;
  bool at_start_listed = false, at_start_done = false;
};
Prebuild* g_prebuild = nullptr;  // never destroyed (the thread runs until the process exits)

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The opening thread (the game's loader): copy the path and return.
// COGBox job 659: a second listener (Judgment's loading-screen frame limit, GitHub #38). The SDK keeps one observer,
// and this reader installs after the game's own code, so the game hands its listener here and it is told first.
std::atomic<void (*)(std::string_view, const std::filesystem::path*)> g_chain{nullptr};

void OnFileOpened(std::string_view guest_path, const std::filesystem::path* host_path) {
  if (const auto chain = g_chain.load()) chain(guest_path, host_path);
  if (!host_path || !g_prebuild) return;
  const std::string name = Lower(host_path->filename().string());
  const bool package = name.size() > 4 && name.ends_with(".xxx");
  const bool global_cache = name.ends_with(".bin") && name.find("shadercache") != std::string::npos;
  if (!package && !global_cache) return;
  {
    std::lock_guard<std::mutex> lock(g_prebuild->mutex);
    if (!g_prebuild->opened.insert(name).second) {
      // Job 630: queued at start-up and not read yet - the game wants it now, so it goes to the front.
      // Job 666: only looked for when it is still waiting (a package read already costs nothing here).
      if (!g_prebuild->waiting.count(name)) return;
      auto& q = g_prebuild->queue;
      for (auto it = q.begin(); it != q.end(); ++it)
        if (Lower(it->filename().string()) == name) {
          std::filesystem::path moved = std::move(*it);
          q.erase(it);
          q.push_front(std::move(moved));
          break;
        }
      return;
    }
    g_prebuild->waiting.insert(name);
    g_prebuild->queue.push_front(*host_path);   // job 630: what the game opens now comes before the start-up list
  }
  g_prebuild->cv.notify_one();
}

void ReaderThread() {
#ifdef _WIN32
  // Job 615: the reader decompresses every package the game opens while the game loads it too - at the lowest
  // priority it only takes CPU time the game, the renderer and the pipeline workers leave free (Gears 3 ch11's
  // opening was ~5,000 skipped draws worse with it on).
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
  PackageScanner scanner(g_prebuild->seen);
  uint64_t files = 0, pixel = 0, fresh = 0;
  for (;;) {
    std::filesystem::path path;
    {
      std::unique_lock<std::mutex> lock(g_prebuild->mutex);
      g_prebuild->cv.wait(lock, [] { return !g_prebuild->queue.empty(); });
      path = std::move(g_prebuild->queue.front());
      g_prebuild->queue.pop_front();
      g_prebuild->waiting.erase(Lower(path.filename().string()));   // job 666
      ++g_prebuild->busy;  // job 641: counted with the pop, so no other reader sees an empty queue and nobody busy
    }
    const auto t0 = std::chrono::steady_clock::now();
    // Job 641: when this was the last queued file and no other reader is busy, the start-up reading is done.
    struct BusyGuard {
      ~BusyGuard() {
        bool done = false;
        uint64_t files = 0;
        {
          std::lock_guard<std::mutex> l(g_prebuild->mutex);
          --g_prebuild->busy;
          ++g_prebuild->files_read;
          if (g_prebuild->at_start_listed && !g_prebuild->at_start_done && g_prebuild->queue.empty() && !g_prebuild->busy) {
            g_prebuild->at_start_done = true;
            done = true;
            files = g_prebuild->files_read;
          }
        }
        if (done) XenosPrebuildAtStartDone(files);
      }
    } busy_guard;
    std::vector<uint8_t> data;
    {
      std::ifstream f(path, std::ios::binary);
      if (!f) continue;
      f.seekg(0, std::ios::end);
      const std::streamoff size = f.tellg();
      if (size <= 0 || size > (std::streamoff(1) << 31)) continue;
      data.resize(size_t(size));
      f.seekg(0, std::ios::beg);
      if (!f.read(reinterpret_cast<char*>(data.data()), size)) continue;
    }
    // Once enough new draws have used prebuilt shaders, stop offering a variant this game never draws (under 5%).
    const uint64_t h0 = XenosPrebuildVariantHits(0), h1 = XenosPrebuildVariantHits(1), hn = h0 + h1;
    scanner.SetVariants(hn < 32 || h0 * 20 >= hn, hn < 32 || h1 * 20 >= hn);
    const ScanResult r = scanner.Scan(data);
    if ((r.pixel || r.vertex) && !g_prebuild->shader_files.empty()) {  // job 632: remembered for the next start-up
      const std::string name = Lower(path.filename().string());
      std::lock_guard<std::mutex> lock(g_prebuild->mutex);
      if (g_prebuild->known.insert(name).second) {
        std::ofstream f(g_prebuild->shader_files, std::ios::app);
        f << name << "\n";
      }
    }
    ++files;
    pixel += r.pixel;
    fresh += r.new_pixel;
    if (r.unreadable)
      REXLOG_WARN("ue3_shader_prebuild: {}: {:.1f} MB, a fully compressed file of which none of its {} chunk(s) "
                  "decoded - unreadable, no shaders taken from it",
                  path.filename().string(), double(data.size()) / 1048576.0, r.chunks);
    if (r.pixel || r.failed_chunks)
      REXLOG_INFO("ue3_shader_prebuild: {}: {:.1f} MB, {} chunk(s) ({} with an unreadable block, {} decoded only "
                  "with the padding retry: {} block(s)), {} inner block(s) ({} with the padding retry), {} pixel / "
                  "{} vertex shader(s) ({} of them outside the compressed chunks), {} new pixel shader(s) handed to "
                  "the renderer (whole {} / code only {}), {:.0f} ms | {} file(s) read, {} handed over in all | "
                  "draws so far used prebuilt whole {} / code only {}",
                  path.filename().string(), double(data.size()) / 1048576.0, r.chunks, r.failed_chunks,
                  r.padded_chunks, r.padded_blocks, r.nested, r.padded_nested, r.pixel, r.vertex, r.gap_records,
                  r.new_pixel, r.offered[0], r.offered[1],
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), files,
                  fresh, h0, h1);
  }
}
#endif

}  // namespace

// Job 659: a game's own file-open listener, called before this reader's (see g_chain).
void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*)) {
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
  g_chain.store(listener);
  // Job 666: either order works - while this reader isn't on, its observer still passes every open to the game's
  // listener (OnFileOpened tells the chain first, then returns without g_prebuild); when it goes on it sets the same one.
  rex::filesystem::SetFileOpenObserver(&OnFileOpened);
#else
  (void)listener;
#endif
}

void Ue3InstallShaderPrebuildIfRequested() {
  if (!REXCVAR_GET(ue3_shader_prebuild)) return;
  if (!XenosPrebuildWanted()) {
    REXLOG_INFO("ue3_shader_prebuild: off - our renderer is not running the DXBC shader path");
    return;
  }
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
  g_prebuild = new Prebuild();
  const std::string at_start_dirs = REXCVAR_GET(ue3_shader_prebuild_at_start);
  int readers = 1;
  if (!at_start_dirs.empty()) {   // job 632: more readers at start-up (the videos and menus leave the CPU idle)
    readers = REXCVAR_GET(ue3_shader_prebuild_threads);
    if (readers <= 0) readers = std::clamp(int(std::thread::hardware_concurrency() / 4), 2, 4);   // each holds a whole package in memory
    readers = std::min(readers, 16);
    const std::string cache = rex::cvar::GetFlagByName("gow2_shader_cache_dir");
    if (!cache.empty()) {
      g_prebuild->shader_files = std::filesystem::u8path(cache) / "prebuild-files.txt";
      std::ifstream f(g_prebuild->shader_files);
      for (std::string l; std::getline(f, l);) if (!l.empty()) g_prebuild->known.insert(Lower(l));
    }
    REXLOG_INFO("ue3_shader_prebuild_at_start: {} reader thread(s), {} package(s) known to hold shaders", readers,
                g_prebuild->known.size());
  }
  g_prebuild->thread = std::thread(ReaderThread);   // after the list above is read (the readers add to it)
  for (int i = 1; i < readers; ++i) std::thread(ReaderThread).detach();
  rex::filesystem::SetFileOpenObserver(&OnFileOpened);
  REXLOG_INFO("ue3_shader_prebuild: on - packages the game opens are read for their pixel shaders (patch 0068)");
  // Job 630: every package of the game's folders, queued at start-up (shader caches first, then the largest - the
  // chapter packages); a package the game opens later is not read twice (the `opened` set).
  const std::string at_start = REXCVAR_GET(ue3_shader_prebuild_at_start);
  if (!at_start.empty()) {
    std::unordered_set<std::string> known;
    {
      std::lock_guard<std::mutex> lock(g_prebuild->mutex);
      known = g_prebuild->known;
    }
    std::thread([at_start, known = std::move(known)] {
      std::vector<std::pair<std::filesystem::path, uintmax_t>> caches, packages;
      size_t p = 0;
      while (p <= at_start.size()) {
        size_t q = at_start.find(';', p);
        if (q == std::string::npos) q = at_start.size();
        const std::filesystem::path dir = std::filesystem::u8path(at_start.substr(p, q - p));
        p = q + 1;
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
          if (!it->is_regular_file(ec)) continue;
          const std::string name = Lower(it->path().filename().string());
          const uintmax_t size = it->file_size(ec);
          if (name.ends_with(".bin") && name.find("shadercache") != std::string::npos) caches.emplace_back(it->path(), size);
          else if (name.size() > 4 && name.ends_with(".xxx")) {
            if (known.count(name)) caches.emplace_back(it->path(), size);   // job 632: held shaders last time
            else packages.emplace_back(it->path(), size);
          }
        }
      }
      std::sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      uintmax_t bytes = 0;
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(g_prebuild->mutex);
        for (const auto* list : {&caches, &packages})
          for (const auto& [path, size] : *list)
            if (g_prebuild->opened.insert(Lower(path.filename().string())).second) {
              g_prebuild->waiting.insert(Lower(path.filename().string()));   // job 666
              g_prebuild->queue.push_back(path);
              bytes += size;
              ++queued;
            }
      }
      {
        std::lock_guard<std::mutex> lock(g_prebuild->mutex);
        g_prebuild->at_start_listed = true;  // job 641
        if (g_prebuild->queue.empty() && !g_prebuild->busy && !g_prebuild->at_start_done) {
          g_prebuild->at_start_done = true;
          XenosPrebuildAtStartDone(g_prebuild->files_read);
        }
      }
      g_prebuild->cv.notify_all();
      REXLOG_INFO("ue3_shader_prebuild_at_start: {} file(s) queued ({} shader cache(s) / known shader packages first, {:.0f} MB in all) from {}", queued,
                  caches.size(), double(bytes) / 1048576.0, at_start);
    }).detach();
  }
#else
  REXLOG_WARN("ue3_shader_prebuild: this SDK has no file-open observer (SDK patch 32); nothing is prebuilt");
#endif
}
