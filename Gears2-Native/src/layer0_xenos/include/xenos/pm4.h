// Layer 0 (game-free): our own Xenos PM4 command processor (Stage 5B, notes/49). Parses the command ring the game's D3D
// writes - type 0/1/2/3 packets, indirect buffers, constants, shader loads, draws, events, swaps - into our own register
// file, and reports draws, swaps and events through callbacks. Phase 1 runs it next to the SDK's command processor on
// the same ring ranges and compares (pm4_shadow.cpp); the SDK's processing goes away once it matches everywhere.
//
// Our code (MIT). The packet semantics follow the rexglue SDK's src/graphics/command_processor.cpp (BSD-3-Clause,
// Copyright (c) Ben Vanik and Xenia project contributors / rexglue contributors), re-expressed here; packet numbers are
// the R6xx/Xenos PM4 opcodes (SDK include/rex/graphics/xenos.h).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xenos::pm4 {

constexpr uint32_t kRegisterCount = 0x5003;  // SDK RegisterFile::kRegisterCount

// Type-3 opcodes Gears 2 uses (SDK xenos.h PM4_*).
enum Opcode : uint32_t {
  kMeInit = 0x48,
  kNop = 0x10,
  kIndirectBuffer = 0x3F,
  kIndirectBufferPfd = 0x37,
  kWaitForIdle = 0x26,
  kWaitRegMem = 0x3C,
  kRegRmw = 0x21,
  kRegToMem = 0x3E,
  kMemWrite = 0x3D,
  kCondWrite = 0x45,
  kEventWrite = 0x46,
  kEventWriteShd = 0x58,
  kEventWriteExt = 0x5A,
  kEventWriteZpd = 0x5B,
  kDrawIndx = 0x22,
  kDrawIndx2 = 0x36,
  kVizQuery = 0x23,
  kSetConstant = 0x2D,
  kSetConstant2 = 0x55,
  kLoadAluConstant = 0x2F,
  kSetShaderConstants = 0x56,
  kImLoad = 0x27,
  kImLoadImmediate = 0x2B,
  kInvalidateState = 0x3B,
  kSetBinMaskLo = 0x60,
  kSetBinMaskHi = 0x61,
  kSetBinSelectLo = 0x62,
  kSetBinSelectHi = 0x63,
  kSetBinMask = 0x50,
  kSetBinSelect = 0x51,
  kContextUpdate = 0x5E,
  kInterrupt = 0x54,
  kXeSwap = 0x64,
};

struct ShaderRef {
  uint32_t address = 0;  // guest physical address of the ucode (IM_LOAD), 0 for IM_LOAD_IMMEDIATE
  uint64_t hash = 0;     // Environment::hash over the guest's big-endian bytes (the SDK's ucode_data_hash), 0 if none
  std::vector<uint32_t> dwords;  // ucode, host order (byte-swapped from the guest's big-endian)
  bool valid = false;
};

struct Draw {
  uint32_t initiator = 0;    // VGT_DRAW_INITIATOR
  uint32_t viz_query = 0;    // DRAW_INDX's first dword (0 for DRAW_INDX_2)
  bool indexed = false;
  uint32_t index_base = 0;   // VGT_DMA_BASE (aligned to the index size)
  uint32_t index_size = 0;   // VGT_DMA_SIZE
  // False when the draw is dropped before reaching any backend: immediate indices (unsupported) or a viz-query draw
  // with kill_pix_post_hi_z (SDK ExecutePacketType3Draw; the SDK's draw observer is not called for these).
  bool executed = true;
};

// What the processor needs from the outside world. Memory reads go through `read32` (guest physical address, returns
// the raw 32-bit word as stored, i.e. big-endian for D3D's data). Writes are reported, not done, unless `apply_writes`.
struct Environment {
  std::function<uint8_t*(uint32_t physical)> translate;  // guest physical -> host pointer (nullptr if invalid)
  bool apply_writes = false;  // phase 1 (shadow): false - the SDK does the writes
  // Phase 2 (external, SDK patch 21): we run the ring. Register writes/reads go to the host's register file (its side
  // effects stay); draws, shader loads, ZPD, XE_SWAP and INTERRUPT are handed back as single packets; memory writes,
  // waits, conditional writes and predication are ours. Empty functions = phase 1.
  std::function<void(uint32_t, uint32_t)> write_register;
  std::function<uint32_t(uint32_t)> read_register;
  std::function<void(const uint32_t* big_endian_dwords, uint32_t count)> execute_packet;
  std::function<uint32_t()> counter;
  std::function<bool()> running;
  std::function<void()> make_coherent;
  std::function<bool()> vsync;
  // Phase 3: draws and shader loads are ours too. `hash` hashes the ucode as the guest stores it; `own_draw` gets each
  // executed draw/copy after its registers are written (the SDK's dropped draws are dropped here the same way).
  std::function<uint64_t(const uint8_t* big_endian_bytes, size_t size)> hash;
  std::function<void(const Draw&, const ShaderRef& vs, const ShaderRef& ps)> own_draw;
  // ZPD is ours too: `own_zpd` gets each EVENT_WRITE_ZPD (after the initiator write-back) with RB_SAMPLE_COUNT_ADDR
  // and whether the report held D3D's end marker (0xFFFFFEED, big-endian, in a ZPass or ZFail lane) - the caller
  // writes the report and tells the observers.
  std::function<void(uint32_t report_address, bool is_end)> own_zpd;
  // Stage 5C-2: XE_SWAP is ours too: `own_swap` gets the front buffer address and size (the caller tells the
  // observers, presents, and does the host's swap bookkeeping).
  std::function<void(uint32_t frontbuffer, uint32_t width, uint32_t height)> own_swap;
  // Job 168: the host register file itself. When set, writes to the constant files (ALU / fetch / bool / loop,
  // 0x4000-0x4927: no side effects on the host) are stored there directly instead of through write_register (two
  // std::function hops per dword were ~10% of the GPU thread). track_changes = false also skips the change list,
  // which only the shadow comparison reads.
  uint32_t* host_registers = nullptr;
  bool track_changes = true;
  // Job 171 (notes/52 section 13): called before anything the title's CPU can see that says "the GPU got this far" -
  // memory writes, scratch-register write-backs, packets handed back (interrupts), the end of each ring kick. Our
  // renderer finishes its deferred guest-memory copies there, so the title never reuses a buffer we still read.
  std::function<void()> release;
  // Job 173 (notes/52 section 15, the PM4 / renderer split): when set and it returns true, a memory write the ring
  // makes (MEM_WRITE, EVENT_WRITE_SHD fences, EVENT_WRITE_EXT) is done later by the renderer thread, in order after
  // the draws before it - as a real GPU writes a fence only when it gets there. While writes are pending, a read of
  // their addresses (WAIT_REG_MEM, constant loads) and any wait that fails first call `release` (= wait for the
  // renderer thread), and the end of a ring kick is no longer a release point (the read pointer write-back is
  // deferred the same way by our graphics system).
  std::function<bool(uint32_t physical, const void* bytes, uint32_t size)> defer_write;
  bool External() const { return bool(execute_packet); }
  bool OwnDraws() const { return bool(own_draw); }
};

struct Callbacks {
  std::function<void(const std::array<uint32_t, kRegisterCount>&, const Draw&, const ShaderRef& vs,
                     const ShaderRef& ps)>
      on_draw;  // after the draw's registers are written (copies/resolves too: RB_MODECONTROL edram_mode)
  std::function<void(const std::array<uint32_t, kRegisterCount>&, uint32_t frontbuffer, uint32_t width,
                     uint32_t height)>
      on_swap;
  std::function<void(uint32_t opcode, uint32_t initiator)> on_event;  // EVENT_WRITE(_SHD/_EXT/_ZPD)
};

struct Stats {
  uint64_t packets = 0, type0 = 0, type3 = 0, draws = 0, swaps = 0, indirect = 0, predicated_skipped = 0,
           unknown_opcodes = 0, errors = 0, shader_loads = 0, waits = 0, memory_writes_seen = 0;
  uint64_t shader_loads_cached = 0;  // job 172: IM_LOADs whose bytes matched the cache
  // Phase 2: packets handed back to the host, memory words we wrote, wait loops and the iterations they spun.
  uint64_t handed_back = 0, memory_writes_done = 0, wait_loops = 0, wait_spins = 0;
  uint64_t zpds = 0, zpd_ends = 0;
  uint64_t releases = 0;  // Environment::release calls
  uint64_t deferred_writes = 0, release_reads = 0, release_waits = 0;  // job 173  // EVENT_WRITE_ZPD we ran ourselves (own_zpd), and how many were ends
  uint32_t last_unknown_opcode = 0;
};

class Processor {
 public:
  Processor(Environment env, Callbacks cb) : env_(std::move(env)), cb_(std::move(cb)) {}
  // Executes the primary ring's dwords [read_index, write_index) (wrapping at ring_dwords).
  void ExecutePrimary(uint32_t ring_physical, uint32_t ring_dwords, uint32_t read_index, uint32_t write_index);
  const std::array<uint32_t, kRegisterCount>& Registers() const { return regs_; }
  const Stats& GetStats() const { return stats_; }
  // Registers whose value changed since the last call, with their new values (for compact per-draw comparison).
  std::vector<std::pair<uint32_t, uint32_t>> TakeChanges();
  // Job 173: the changed register indices since the last call, appended to `out` (no allocation once warm).
  void TakeChangedIndices(std::vector<uint32_t>& out);
  // Job 173: the shaders the next draw uses (kept alive by whoever copies the pointer).
  const std::shared_ptr<const ShaderRef>& CurrentVs() const { return vs_; }
  const std::shared_ptr<const ShaderRef>& CurrentPs() const { return ps_; }

 private:
  struct Reader {
    const uint8_t* base;
    uint32_t dwords;   // capacity
    uint32_t pos;      // dword index
    uint32_t remaining;
    uint32_t Next();
    void Skip(uint32_t n) {
      pos = (pos + n) % dwords;
      remaining -= n;
    }
  };
  bool Packet(Reader& r, uint32_t depth);
  bool Type3(Reader& r, uint32_t packet, uint32_t depth);
  void ExecuteIndirect(uint32_t physical, uint32_t dwords, uint32_t depth);
  void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadMemory(uint32_t physical);  // raw word (no swap); job 173: waits for pending deferred writes there
  void WriteMemory(uint32_t physical, uint32_t raw);  // phase 2 only
  void Release() {
    if (env_.release) {
      ++stats_.releases;
      env_.release();
    }
    pending_lo_ = ~0u;  // job 173: every deferred write is done after a release
    pending_hi_ = 0;
  }
  bool Defer(uint32_t physical, const void* bytes, uint32_t size) {
    if (!env_.defer_write || !env_.defer_write(physical, bytes, size)) return false;
    ++stats_.deferred_writes;
    pending_lo_ = std::min(pending_lo_, physical);
    pending_hi_ = std::max(pending_hi_, physical + size);
    return true;
  }
  uint32_t pending_lo_ = ~0u, pending_hi_ = 0;  // job 173: address range of deferred writes not yet known done
  uint32_t ReadReg(uint32_t index) const;
  void HandBack(const Reader& r, uint32_t start_pos, uint32_t dwords, uint32_t packet);  // phase 2: one packet
  bool Compare(uint32_t func, uint32_t value, uint32_t ref, uint32_t mask) const;
  ShaderRef LoadShader(uint32_t physical, const uint8_t* src, uint32_t dwords);
  // Job 172: IM_LOAD through a cache keyed by (address, size) holding the guest's bytes - the title reloads the same
  // few shaders thousands of times a frame; an unchanged reload (memcmp) reuses the decoded ShaderRef (no byte-swap
  // copy, allocation or hash). Changed bytes make a new entry, so a slot still holding the old one keeps it.
  std::shared_ptr<const ShaderRef> CachedShader(uint32_t physical, const uint8_t* src, uint32_t dwords);
  struct ShaderCacheEntry {
    std::vector<uint8_t> raw;  // the guest bytes as stored
    std::shared_ptr<const ShaderRef> ref;
  };
  std::unordered_map<uint64_t, ShaderCacheEntry> shader_cache_;

  Environment env_;
  Callbacks cb_;
  std::array<uint32_t, kRegisterCount> regs_{};
  std::array<bool, kRegisterCount> dirty_flag_{};
  std::vector<uint32_t> dirty_;
  uint64_t bin_mask_ = 0xFFFFFFFFull, bin_select_ = 0xFFFFFFFFull;  // SDK command_processor.h defaults
  std::shared_ptr<const ShaderRef> vs_ = std::make_shared<const ShaderRef>(),
                                   ps_ = std::make_shared<const ShaderRef>();
  Stats stats_;
};

}  // namespace xenos::pm4
