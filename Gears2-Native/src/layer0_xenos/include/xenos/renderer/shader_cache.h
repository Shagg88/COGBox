// Layer 0 (game-free): guest shader microcode -> our translated HLSL (xenos/hlsl.h, S3.3) -> host bytecode (DXIL now,
// SPIR-V for the Vulkan backend later), cached in memory by the microcode hash and on disk (default out\shader-cache,
// never committed: game-derived, R3). S3.5 steps 1-2 (notes/42 section 4).
//
// Disk file name: <vs|ps>-<microcode hash>-<key>.<dxil|spv> where key hashes the generated HLSL, the compiler
// arguments and the compiler's version, so a translator or compiler update simply misses the old files (they are not
// deleted - R11).
//
// Threads (jobs 095/096: ~72 ms per DXC compile stalled the emulated game when done on the GPU worker thread):
//  - Get() runs on the caller's thread (the GPU worker thread): decode + translate (~0.1 ms) and a disk-cache lookup.
//    A disk hit is ready at once. A miss is queued and Get() returns the shader in state kPending.
//  - Worker threads (hardware_concurrency() - 2, at least 1), each with its own compiler instance, run DXC and write
//    the disk file.
//  - Poll() (caller's thread) picks up finished compiles and creates the backend shader objects (the backend is not
//    thread-safe, so only the caller's thread touches it).
// Built-in shaders (GetBuiltin) are compiled synchronously: there are a few, at start-up. Our code (MIT).
//
// Shader pop-in (Gears 3 2026-10-23, research in Gears-Mailbox\to-gears2\20261023-001500-research-shader-popin.md):
//  - SetUcodeRecord(true): every guest shader seen is also saved as <dir>\ucode\<vs|ps>-<hash>.ucode (game-derived,
//    never committed). That list does not depend on the translator or the compiler, so it survives their updates.
//  - WarmFromDisk(): translates every recorded shader at start-up and queues the ones without a DXIL file, behind the
//    live misses (which go to the front of the queue), so areas seen in any earlier session have their shaders ready.
//  - WaitReady(): lets a draw wait (within a budget) for its shader instead of being skipped; a still-queued job is
//    compiled on the caller's thread, one already on a worker is waited for.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <xenos/hlsl.h>
#include <xenos/renderer/gpu.h>
#include <xenos/ucode.h>

namespace xenos::renderer {

// HLSL -> bytecode. The DXC implementation loads dxcompiler.dll (and DXC loads dxil.dll for signing) at run time.
// One instance is used by one thread at a time.
class ShaderCompiler {
 public:
  virtual ~ShaderCompiler() = default;
  virtual std::string Describe() const = 0;
  // Compiler identity for cache keys (version + arguments).
  virtual std::string CacheTag(gpu::ShaderIL il) const = 0;
  virtual bool Compile(const std::string& hlsl, gpu::ShaderStage stage, gpu::ShaderIL il, std::vector<uint8_t>& out,
                       std::string& messages) = 0;
};
// `dll_dir` empty = the folder of the running executable. Null + error text when the DLL can't be loaded.
std::unique_ptr<ShaderCompiler> CreateDxcCompiler(const std::filesystem::path& dll_dir, std::string& error);
using ShaderCompilerFactory = std::function<std::unique_ptr<ShaderCompiler>()>;

enum class ShaderState : uint8_t { kPending, kReady, kFailed };

// What the renderer needs to know about a guest shader, from the decoded microcode.
struct GuestShader {
  uint64_t ucode_hash = 0;
  bool is_vertex = true;
  ShaderState state = ShaderState::kPending;
  gpu::ShaderHandle handle = 0;  // valid in kReady
  bool ready() const { return state == ShaderState::kReady; }
  // Analysis (independent of compile success).
  bool kills = false;            // any kill instruction (pixel shaders)
  bool writes_depth = false;     // oDepth
  uint32_t color_targets_written = 0;  // bit per oC0-3
  bool memexport = false;        // eA / eM writes (notes/11: run with --gow2_renderer_memexport)
  std::vector<uint32_t> memexport_stream_constants;  // float constants read by eA writes (notes/11)
  bool uses_textures = false;
  uint32_t texture_slots = 0;    // bit per texture fetch constant used (any dimension)
  uint32_t texture_slots_2d = 0, texture_slots_cube = 0, texture_slots_3d = 0;  // as the translation declares them
  uint32_t float_constants = 256;  // c0..c(n-1) the translation can read (only those need uploading)
  // Compacted layout (hlsl::Translation): the shader's constant buffer is c[0, low) then c[high, float_constants).
  // low == high == float_constants when not compacted.
  uint32_t float_constants_low = 256, float_constants_high = 256;
  bool compacted() const { return float_constants_low != float_constants_high; }
  uint64_t float_constants_read[4] = {};  // bit per constant read absolutely (hlsl::Translation)
  bool float_constants_relative = false;
  bool Reads(uint32_t c) const { return c < 256 && ((float_constants_read[c >> 6] >> (c & 63)) & 1); }
  // Pixel shaders whose only colour output is oC0 = c[k] (a constant colour, as clear shaders are): k, else -1.
  int32_t constant_color0 = -1;
  // Vertex fetch data formats read (xenos VertexFormat values, bit per format 0-63), for diagnostics.
  uint64_t vfetch_formats = 0;
  bool precise_positions = false;  // vertex shader: precise added to the register file and position (see .cpp)
  // Pixel shader: its depth output follows the D24FS8 host remap (system flag 64, see .cpp).
  bool depth_remap_ready = false;
  uint32_t vfetch_constants[3] = {};  // bit per vertex fetch constant 0-95 the shader reads
  // Registers: highest absolute register index used + 1, and whether any operand is a0/aL-relative (Xenia's
  // register_static_address_bound / uses_register_dynamic_addressing, for the pixel interpolator mask).
  uint32_t register_bound = 0;
  bool dynamic_addressing = false;
  uint32_t simplifications = 0;  // count of per-shader translator simplifications (hlsl.h)
  // Patch 0045 (Edge): a vertex shader some draw has used (or that the start-up warm-up brought from an earlier
  // session). A new one drawing to a render target not drawn recently makes the draw wait for its shaders.
  mutable bool drawn = false;
  // Overhaul option B: the bytecode is DXBC from xenos::dxbc_gen (made on the calling thread, no DXC), not DXIL. A
  // pipeline takes one bytecode kind, so the renderer pairs it with the DXBC rectangle-list geometry shader.
  bool dxbc = false;
  std::string error;             // translation/compile failure, first line
};

struct ShaderCacheStats {
  uint64_t lookups = 0, translated = 0, disk_hits = 0, compiled = 0, errors = 0, queued = 0;
  double compile_ms = 0.0, translate_ms = 0.0;  // compile_ms summed over the worker threads
  double disk_ms = 0.0;  // patch 0014: caller-thread time reading compiled shaders from the disk cache
  uint64_t pack_hits = 0;  // patch 0015: of disk_hits, read from shaders.pack (the rest from loose .dxil files)
  uint64_t dxbc = 0, dxbc_fallbacks = 0;  // DXBC back end: shaders made as DXBC, and ones it left to the HLSL path
  uint32_t threads = 0;
};

class ShaderCache {
 public:
  // `factory` makes one compiler per worker thread plus one for built-ins; it may return null (then nothing compiles
  // and only disk hits work). threads = 0: hardware_concurrency() - 2, at least 1.
  ShaderCache(gpu::Backend& backend, const ShaderCompilerFactory& factory, std::filesystem::path dir,
              uint32_t threads = 0);
  ~ShaderCache();
  // `dwords` in host byte order (rex::graphics::Shader::ucode_dwords()). Returns a stable reference; its state moves
  // from kPending to kReady/kFailed in a later Poll().
  // tess_quad_segments > 0 (vertex shaders): the quad-patch tessellation variant with N segments per patch edge.
  const GuestShader& Get(uint64_t ucode_hash, const uint32_t* dwords, size_t dword_count, bool is_vertex,
                         uint32_t tess_quad_segments = 0) {
    return GetImpl(ucode_hash, dwords, dword_count, is_vertex, tess_quad_segments, false);
  }
  // Patch 0058: a shader already known to this cache (Get or the warm-up), or nullptr - no translation, no queueing.
  const GuestShader* Find(uint64_t ucode_hash, bool is_vertex, uint32_t tess_quad_segments = 0) const {
    const auto& map = shaders_[is_vertex ? 1 : 0];
    const uint64_t key = (is_vertex && tess_quad_segments)
                             ? ucode_hash ^ (0x7E55ull << 48) ^ (uint64_t(tess_quad_segments) << 40)
                             : ucode_hash;  // the same key as GetImpl
    const auto it = map.find(key);
    return it == map.end() ? nullptr : it->second.get();
  }
  // Finishes compiles that are done (creates the backend shaders). Call often on the caller's thread.
  void Poll();
  size_t PendingCount() const { return in_flight_; }
  // Shader pop-in (see the header comment).
  void SetUcodeRecord(bool on) { record_ucode_ = on && dir_ok_; }
  // Patch 0015 (stutter): keep compiled shaders in one file, <dir>\shaders.pack, opened once and kept open. Each loose
  // .dxil file cost ~8 ms to open mid-game (job 296: 118 reads = 920 ms in one frame, ch05) - the per-file open, not
  // the 16 KB read. Loose files found on disk are copied into the pack the first time they are read, so an existing
  // cache moves over by itself; new compiles go to the pack only. Call before the first Get / GetBuiltin.
  void SetPack(bool on);
  struct PackInfo {
    bool open = false;
    uint64_t shaders = 0, bytes = 0, added = 0, bad = 0;
  };
  PackInfo GetPackInfo();
  struct WarmResult {
    size_t files = 0, known = 0, disk_hits = 0, queued = 0, failed = 0;
    double ms = 0.0;
    double list_ms = 0.0, read_ms = 0.0, translate_ms = 0.0, make_ms = 0.0;
    size_t from_pack = 0, packed_now = 0;  // patch 0085: shaders read from recorded.pack / files queued into it  // patch 0083: where the time went
  };
  WarmResult WarmFromDisk();
  // Patch 0068 (first-start shader prebuild): the DXBC this cache would make for these dwords - same decode and
  // translation options, so the driver later sees identical bytes - without keeping, recording or creating anything.
  // False when the DXBC back end is off or the shader is not translatable to DXBC.
  bool TranslateDxbcOnly(const uint32_t* dwords, size_t dword_count, bool is_vertex, std::vector<uint8_t>& out);
  // True when `s` is no longer pending (ready or failed) on return; waits at most budget_ms.
  bool WaitReady(const GuestShader& s, double budget_ms);
  // Our own fixed shaders (rectangle-list geometry shader, resolve compute shaders), compiled synchronously.
  gpu::ShaderHandle GetBuiltin(const std::string& name, const std::string& hlsl, gpu::ShaderStage stage);
  const ShaderCacheStats& Stats() const { return stats_; }
  const std::filesystem::path& Dir() const { return dir_; }
  // Float-constant compaction for shaders translated from now on (hlsl::HlslOptions::compact_float_constants).
  // Set once, before the first Get: a shader's layout must not change while the renderer uploads for it.
  void SetCompactFloatConstants(bool on) { compact_float_constants_ = on; }
  void SetTexSignBySwizzle(bool on) { tex_sign_by_swizzle_ = on; }  // patch 0024
  void SetCanaryR21(bool on) { canary_r21_ = on; }  // patch 0041
  void SetCanaryDecode(bool on) { canary_decode_ = on; }  // patch 0041
  void SetMulscTowardZero(bool on) { mulsc_toward_zero_ = on; }   // patch 0087
  void SetMemexportF16NanZero(bool on) { f16_nan_zero_ = on; }    // patch 0087
  void SetSpirvPrecise(bool on) { spirv_precise_ = on; }          // patch 0088 (measurement)
  void SetPsRefactoring(bool on) { ps_refactoring_ = on; }        // patch 0095
  static void SetSpirvFiles(const std::string& dump_dir, const std::string& override_dir);  // patch 0099
  void SetTexNumFormat(bool on) { tex_num_format_ = on; }  // patch 0040
  void SetTexPostConstants(bool on) { tex_post_constants_ = on; }  // patch 0105
  void SetTexPostAffine(bool on) { tex_post_affine_ = on; }        // job 634
  void SetMulMinAbs(bool on) { mul_min_abs_ = on; }                // job 634
  // Job 636: the leaner DXBC forms (xenos::hlsl::HlslOptions dxbc_structured_jumps ... dxbc_coalesce_moves).
  struct Lean {
    bool structured_jumps = false, mul_movc = false, short_r21 = false, short_swap = false, short_degamma = false,
         coalesce_moves = false, dead_stores = false;  // dead_stores: job 637
    bool vertex_words = false, fold_modifiers = false, merge_movs = false;  // job 638
  };
  void SetLean(const Lean& l) { lean_ = l; }
  void SetVertexIndexClamp(bool on) { vertex_index_clamp_ = on; }    // patch 0028
  // --gow2_renderer_shader_backend=dxbc: guest shaders are translated straight to DXBC (D3D12 only). Set once,
  // before the first Get.
  void SetDxbc(bool on) { dxbc_ = on; }
  // Patch 0060 (DXBC back end): slim = the exact size cuts (operands in place, no needless one-pass loop, branch-free
  // pixel prologue - the same values); canary_maths = Canary's max / min / clamp rules. Set once, before the first Get.
  void SetDxbcSlim(bool slim, bool canary_maths) {
    dxbc_slim_ = slim;
    dxbc_canary_maths_ = canary_maths;
  }
  void SetDxbcSalt(uint32_t salt) { dxbc_salt_ = salt; }  // patch 0063: test only (--gow2_renderer_dxbc_salt)
  bool Dxbc() const { return dxbc_; }
  std::string CompilerDescription() const;

 private:
  // Patch 0068: the decode / translation options of every translation (GetImpl and TranslateDxbcOnly alike).
  ucode::DecodeOptions DecodeOptions() const;
  hlsl::HlslOptions TranslateOptions(bool is_vertex, uint32_t tess_quad_segments) const;
  struct Job {
    GuestShader* shader = nullptr;  // only dereferenced on the caller's thread (in Poll)
    std::string name;               // disk file name
    std::string hlsl;
    gpu::ShaderStage stage = gpu::ShaderStage::kVertex;
    // Results (written by the worker).
    bool ok = false;
    std::vector<uint8_t> bytecode;
    std::string messages;
    double ms = 0.0;
  };
  const GuestShader& GetImpl(uint64_t ucode_hash, const uint32_t* dwords, size_t dword_count, bool is_vertex,
                             uint32_t tess_quad_segments, bool warm);
  void SaveUcode(uint64_t ucode_hash, bool is_vertex, const uint32_t* dwords, size_t dword_count);
  // Patch 0084: a translated DXBC / SPIR-V shader taken into the cache (its module made, stats counted) - one place for
  // GetImpl and the warm-up, which translates on several threads first.
  const GuestShader& AdoptDxbc(uint64_t key, std::unique_ptr<GuestShader> s, const std::vector<uint8_t>& bytecode,
                               const hlsl::Translation& meta);
  void UcodeWriter();  // patch 0014: writes queued ucode records off the caller's thread
  void FinishJob(std::unique_ptr<Job> job);  // the caller's half of a finished compile (Poll / WaitReady)
  std::string FileName(const std::string& stem, const std::string& hlsl) const;
  bool LoadFromDisk(const std::string& name, std::vector<uint8_t>& bytecode, bool* from_pack = nullptr);
  void SaveToDisk(const std::string& name, const std::vector<uint8_t>& bytecode, const std::string& hlsl,
                  const std::string& messages, bool ok);
  bool PackFind(const std::string& name, std::vector<uint8_t>& bytecode);  // patch 0015
  void PackAppend(const std::string& name, const std::vector<uint8_t>& bytecode);
  void Worker(ShaderCompiler* compiler);

  gpu::Backend& backend_;
  std::unique_ptr<ShaderCompiler> sync_compiler_;  // built-ins, and the cache tag
  std::vector<std::unique_ptr<ShaderCompiler>> worker_compilers_;
  std::filesystem::path dir_;
  bool dir_ok_ = false;
  std::string tag_;
  std::unordered_map<uint64_t, std::unique_ptr<GuestShader>> shaders_[2];  // [vertex]
  std::unordered_map<std::string, gpu::ShaderHandle> builtins_;
  ShaderCacheStats stats_;
  bool compact_float_constants_ = false;
  bool tex_sign_by_swizzle_ = false;  // patch 0024
  bool canary_r21_ = false;  // patch 0041
  bool canary_decode_ = false;  // patch 0041
  bool mulsc_toward_zero_ = false, f16_nan_zero_ = false;  // patch 0087
  bool spirv_precise_ = true;  // patch 0088
  bool ps_refactoring_ = false;  // patch 0095
  bool tex_num_format_ = false;  // patch 0040
  bool tex_post_constants_ = false;  // patch 0105
  bool tex_post_affine_ = false, mul_min_abs_ = false;  // job 634
  Lean lean_;                                            // job 636
  bool vertex_index_clamp_ = false;   // patch 0028
  bool dxbc_ = false;
  bool dxbc_slim_ = false, dxbc_canary_maths_ = false;  // patch 0060
  uint32_t dxbc_salt_ = 0;                              // patch 0063 (test only)
  bool record_ucode_ = false;
  size_t in_flight_ = 0;
  // Worker pool.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_cv_;  // a worker finished a job (WaitReady)
  std::deque<std::unique_ptr<Job>> queue_, done_;
  std::deque<std::unique_ptr<Job>> warm_queue_;  // WarmFromDisk's compiles: taken only when queue_ is empty
  bool stop_ = false;
  std::vector<std::thread> workers_;
  // Patch 0014 (stutter): ucode records are written by one background thread - creating a file on Windows (with the
  // virus scanner looking at it) cost ~10 ms each on the recording thread, a 700-880 ms frame when a new area brought
  // ~60 new shaders (job 292, ch05 first run).
  std::mutex ucode_mutex_;
  std::condition_variable ucode_cv_;
  // Patch 0085: each item also goes into ucode/recorded.pack (one file the warm-up reads in one go); `file` false =
  // the pack only (a recorded shader the warm-up found as a file but not yet in the pack).
  struct UcodeItem {
    std::filesystem::path path;
    std::vector<uint32_t> dwords;
    uint64_t hash = 0;
    bool is_vertex = false;
    bool file = true;
  };
  std::deque<UcodeItem> ucode_queue_;
  bool ucode_stop_ = false;
  std::thread ucode_thread_;
  // Patch 0015: the pack (any thread; guarded by pack_mutex_).
  struct PackEntry {
    uint64_t offset = 0;  // of the bytecode
    uint32_t size = 0;
    uint64_t checksum = 0;  // Fnv1a64 of the name, then the bytecode
  };
  std::mutex pack_mutex_;
  std::atomic<bool> pack_on_{false};
  std::fstream pack_;
  uint64_t pack_end_ = 0;  // where the next record goes (the end of the last good record)
  uint64_t pack_added_ = 0, pack_bad_ = 0;
  std::unordered_map<std::string, PackEntry> pack_index_;
};

uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ull);

}  // namespace xenos::renderer
