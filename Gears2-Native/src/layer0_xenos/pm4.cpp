// Layer 0 (game-free): see xenos/pm4.h. Our code (MIT); packet semantics re-expressed from the rexglue SDK's
// src/graphics/command_processor.cpp (BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project contributors / rexglue
// contributors), function by function (named in the comments).
#include <xenos/pm4.h>

#include <bit>
#include <chrono>
#include <cstring>
#include <thread>

namespace xenos::pm4 {
namespace {
constexpr uint32_t kRegVgtEventInitiator = 0x21F9, kRegVgtDrawInitiator = 0x21FC, kRegVgtDmaBase = 0x21FA,
                   kRegVgtDmaSize = 0x21FB, kRegScratchReg0 = 0x0578, kRegScratchUmsk = 0x01DC,
                   kRegScratchAddr = 0x01DD, kRegCoherStatusHost = 0x0A31,
                   kRegRbSampleCountAddr = 0x2325;
constexpr uint32_t kSwapSignature = 0x53574150;  // 'SWAP'
uint32_t Load(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint32_t GpuSwap(uint32_t v, uint32_t endian) {  // xenos::GpuSwap for 32-bit values
  switch (endian & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // k8in16
    case 2: return std::byteswap(v);                                     // k8in32
    case 3: return (v << 16) | (v >> 16);                                // k16in32
    default: return v;
  }
}
}  // namespace

uint32_t Processor::Reader::Next() {
  const uint32_t v = std::byteswap(Load(base + size_t(pos) * 4));  // ReadAndSwap: the ring is big-endian
  if (++pos >= dwords) pos = 0;  // was % dwords: a division per dword (job 169)
  --remaining;
  return v;
}

std::vector<std::pair<uint32_t, uint32_t>> Processor::TakeChanges() {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  out.reserve(dirty_.size());
  for (uint32_t i : dirty_) {
    out.emplace_back(i, regs_[i]);
    dirty_flag_[i] = false;
  }
  dirty_.clear();
  return out;
}

void Processor::TakeChangedIndices(std::vector<uint32_t>& out) {
  for (uint32_t i : dirty_) {
    out.push_back(i);
    dirty_flag_[i] = false;
  }
  dirty_.clear();
}

uint32_t Processor::ReadMemory(uint32_t physical) {
  if (physical < pending_hi_ && physical + 4 > pending_lo_) {  // job 173: a deferred write may land here
    ++stats_.release_reads;
    Release();
  }
  const uint8_t* p = env_.translate ? env_.translate(physical & 0x1FFFFFFCu) : nullptr;
  return p ? Load(p) : 0u;
}

void Processor::WriteMemory(uint32_t physical, uint32_t raw) {  // memory::store of an already GpuSwap'ped value
  ++stats_.memory_writes_seen;
  if (!env_.External() && !env_.apply_writes) return;
  uint8_t* p = env_.translate ? env_.translate(physical & 0x1FFFFFFCu) : nullptr;
  if (!p) return;
  if (!Defer(physical & 0x1FFFFFFCu, &raw, 4)) {
    Release();
    std::memcpy(p, &raw, 4);
  }
  ++stats_.memory_writes_done;
}

uint32_t Processor::ReadReg(uint32_t index) const {
  if (env_.read_register) return env_.read_register(index);
  return index < kRegisterCount ? regs_[index] : 0u;
}

void Processor::HandBack(const Reader& r, uint32_t start_pos, uint32_t dwords, uint32_t packet) {
  std::vector<uint32_t> raw(dwords);
  for (uint32_t i = 0; i < dwords; ++i) raw[i] = Load(r.base + size_t((start_pos + i) % r.dwords) * 4);
  raw[0] = std::byteswap(packet & ~1u);  // predication was decided here (our bin mask/select); the host must not re-check
  ++stats_.handed_back;
  Release();
  env_.execute_packet(raw.data(), dwords);
}

void Processor::WriteRegister(uint32_t index, uint32_t value) {  // CommandProcessor::WriteRegister (state part)
  if (env_.host_registers && index >= 0x4000 && index < 0x4928) {  // constant files: no host side effects (job 168)
    env_.host_registers[index] = value;
    if (!env_.track_changes) {
      regs_[index] = value;
      return;
    }
  } else if (env_.write_register) {
    if (index >= kRegScratchReg0 && index <= kRegScratchReg0 + 7) Release();  // may be written back to memory
    env_.write_register(index, value);  // phase 2: the host's register file and side effects
  }
  if (index >= kRegisterCount) return;  // the SDK keeps these in an "extended" map; nothing in our compare range
  const uint32_t old = regs_[index];
  regs_[index] = value;
  if (index == kRegCoherStatusHost) regs_[index] |= 0x80000000u;
  if (regs_[index] != old && !dirty_flag_[index]) {
    dirty_flag_[index] = true;
    dirty_.push_back(index);
  }
  // Scratch register write-back to memory and the gamma-ramp LUT side effects are not state we compare; the
  // write-back is reported only.
  if (index >= kRegScratchReg0 && index <= kRegScratchReg0 + 7 &&
      ((1u << (index - kRegScratchReg0)) & regs_[kRegScratchUmsk]))
    ++stats_.memory_writes_seen;
  (void)kRegScratchAddr;
}

bool Processor::Compare(uint32_t func, uint32_t value, uint32_t ref, uint32_t mask) const {
  value &= mask;
  switch (func & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

ShaderRef Processor::LoadShader(uint32_t physical, const uint8_t* src, uint32_t dwords) {
  ShaderRef s;
  s.address = physical;
  s.valid = src != nullptr;
  if (src) {
    s.dwords.resize(dwords);
    for (uint32_t i = 0; i < dwords; ++i) s.dwords[i] = std::byteswap(Load(src + size_t(i) * 4));
    if (env_.hash) s.hash = env_.hash(src, size_t(dwords) * 4);
  }
  ++stats_.shader_loads;
  return s;
}

std::shared_ptr<const ShaderRef> Processor::CachedShader(uint32_t physical, const uint8_t* src, uint32_t dwords) {
  if (!src) return std::make_shared<const ShaderRef>(LoadShader(physical, src, dwords));
  const size_t bytes = size_t(dwords) * 4;
  ShaderCacheEntry& e = shader_cache_[(uint64_t(physical) << 32) | dwords];
  if (e.ref && e.raw.size() == bytes && std::memcmp(e.raw.data(), src, bytes) == 0) {
    ++stats_.shader_loads;
    ++stats_.shader_loads_cached;
    return e.ref;
  }
  if (shader_cache_.size() > 8192) {  // bound it (addresses of freed shaders); keep the entry being filled
    ShaderCacheEntry keep = std::move(e);
    shader_cache_.clear();
    shader_cache_[(uint64_t(physical) << 32) | dwords] = std::move(keep);
    return CachedShader(physical, src, dwords);
  }
  e.raw.assign(src, src + bytes);
  e.ref = std::make_shared<const ShaderRef>(LoadShader(physical, src, dwords));
  return e.ref;
}

void Processor::ExecutePrimary(uint32_t ring_physical, uint32_t ring_dwords, uint32_t read_index,
                               uint32_t write_index) {  // CommandProcessor::ExecutePrimaryBuffer
  const uint8_t* base = env_.translate ? env_.translate(ring_physical) : nullptr;
  if (!base || !ring_dwords) {
    ++stats_.errors;
    return;
  }
  Reader r{base, ring_dwords, read_index % ring_dwords,
           (write_index + ring_dwords - read_index % ring_dwords) % ring_dwords};
  while (r.remaining)
    if (!Packet(r, 0)) {
      ++stats_.errors;
      break;
    }
  if (!env_.defer_write) Release();  // the host updates the read pointer the title polls (split: deferred too)
}

void Processor::ExecuteIndirect(uint32_t physical, uint32_t dwords, uint32_t depth) {  // ExecuteIndirectBuffer
  if (!dwords) return;
  const uint8_t* base = env_.translate ? env_.translate(physical) : nullptr;
  if (!base || depth > 8) {
    ++stats_.errors;
    return;
  }
  ++stats_.indirect;
  Reader r{base, dwords, 0, dwords};
  while (r.remaining)
    if (!Packet(r, depth + 1)) {
      ++stats_.errors;
      break;
    }
}

bool Processor::Packet(Reader& r, uint32_t depth) {  // CommandProcessor::ExecutePacket
  const uint32_t packet = r.Next();
  ++stats_.packets;
  if (packet == 0) return true;
  switch (packet >> 30) {
    case 0: {  // ExecutePacketType0
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (r.remaining < count) return false;
      const uint32_t base = packet & 0x7FFF;
      const bool one = (packet >> 15) & 1;
      for (uint32_t m = 0; m < count; ++m) WriteRegister(one ? base : base + m, r.Next());
      ++stats_.type0;
      return true;
    }
    case 1: {  // ExecutePacketType1
      if (r.remaining < 2) return false;
      const uint32_t a = r.Next(), b = r.Next();
      WriteRegister(packet & 0x7FF, a);
      WriteRegister((packet >> 11) & 0x7FF, b);
      return true;
    }
    case 2:
      return true;
    default:
      return Type3(r, packet, depth);
  }
}

bool Processor::Type3(Reader& r, uint32_t packet, uint32_t depth) {  // ExecutePacketType3 + its handlers
  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (r.remaining < count) return false;
  ++stats_.type3;
  const uint32_t start_pos = (r.pos + r.dwords - 1) % r.dwords;  // the header's position (phase 2 hand-back)
  const uint32_t end_remaining = r.remaining - count;
  auto finish = [&]() {  // whatever a handler read, continue at the next packet
    if (r.remaining > end_remaining) r.Skip(r.remaining - end_remaining);
    return true;
  };
  const bool external = env_.External();
  auto hand_back = [&]() {  // phase 2: the host runs this packet as it always did
    HandBack(r, start_pos, count + 1, packet);
    return finish();
  };
  if (packet & 1) {  // predicated: bin check; predicated swaps never run
    if ((bin_select_ & bin_mask_) == 0 || opcode == kXeSwap) {
      ++stats_.predicated_skipped;
      return finish();
    }
  }
  auto write_range = [&](uint32_t base, uint32_t n) {
    for (uint32_t i = 0; i < n && r.remaining > end_remaining; ++i) WriteRegister(base + i, r.Next());
  };
  auto write_range_mem = [&](uint32_t base, uint32_t physical, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) WriteRegister(base + i, std::byteswap(ReadMemory(physical + 4 * i)));
  };
  constexpr uint32_t kTypeBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};  // ALU, FETCH, BOOL, LOOP, REGISTERS
  switch (opcode) {
    case kMeInit:
    case kNop:
    case kInvalidateState:
    case kContextUpdate:
    case kWaitForIdle:
    case kVizQuery:
      return finish();
    case kInterrupt:
      return external ? hand_back() : finish();  // the title's interrupt callback is the host's
    case kXeSwap: {  // ExecutePacketType3_XE_SWAP
      if (external && !env_.own_swap) {
        ++stats_.swaps;
        return hand_back();  // observer OnSwap + present
      }
      if (external) {  // ours (5C-2): 63 words, the first four used (magic, front buffer, width, height)
        r.Next();
        const uint32_t fb = r.Next(), w = r.Next(), h = r.Next();
        ++stats_.swaps;
        env_.own_swap(fb, w, h);
        return finish();
      }
      const uint32_t magic = r.Next();
      const uint32_t fb = r.Next(), w = r.Next(), h = r.Next();
      if (magic == kSwapSignature && cb_.on_swap) cb_.on_swap(regs_, fb, w, h);
      ++stats_.swaps;
      return finish();
    }
    case kIndirectBuffer:
    case kIndirectBufferPfd: {  // ExecutePacketType3_INDIRECT_BUFFER
      const uint32_t ptr = r.Next() & 0x1FFFFFFF, len = r.Next() & 0xFFFFF;
      finish();
      ExecuteIndirect(ptr, len, depth);
      return true;
    }
    case kWaitRegMem: {  // ExecutePacketType3_WAIT_REG_MEM
      ++stats_.waits;
      if (!external) return finish();  // phase 1: the SDK waits
      const uint32_t info = r.Next(), poll = r.Next(), ref = r.Next(), mask = r.Next(), wait = r.Next();
      ++stats_.wait_loops;
      bool released = false;
      for (;;) {
        uint32_t value;
        if (info & 0x10) {
          value = GpuSwap(ReadMemory(poll & ~3u), poll & 3);
        } else {
          if (poll == kRegCoherStatusHost && env_.make_coherent) env_.make_coherent();
          value = ReadReg(poll);
        }
        if (Compare(info, value, ref, mask)) break;
        if (env_.defer_write && !released) {  // job 173: what it waits for may be queued for the renderer thread
          released = true;
          ++stats_.release_waits;
          Release();
          continue;
        }
        ++stats_.wait_spins;
        if (env_.running && !env_.running()) break;
        if (wait >= 0x100 && (!env_.vsync || env_.vsync()))
          std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
        else
          std::this_thread::yield();
      }
      return finish();
    }
    case kRegRmw: {  // ExecutePacketType3_REG_RMW
      const uint32_t info = r.Next(), and_mask = r.Next(), or_mask = r.Next();
      uint32_t v = ReadReg(info & 0x1FFF);
      v &= (info >> 31) & 1 ? ReadReg(and_mask & 0x1FFF) : and_mask;
      v |= (info >> 30) & 1 ? ReadReg(or_mask & 0x1FFF) : or_mask;
      WriteRegister(info & 0x1FFF, v);
      return finish();
    }
    case kRegToMem: {  // ExecutePacketType3_REG_TO_MEM
      const uint32_t reg = r.Next(), mem = r.Next();
      WriteMemory(mem & ~3u, GpuSwap(ReadReg(reg), mem & 3));
      return finish();
    }
    case kMemWrite: {  // ExecutePacketType3_MEM_WRITE
      uint32_t addr = r.Next();
      for (uint32_t i = 0; i + 1 < count; ++i, addr += 4) WriteMemory(addr & ~3u, GpuSwap(r.Next(), addr & 3));
      return finish();
    }
    case kCondWrite: {  // ExecutePacketType3_COND_WRITE (register writes only; memory writes are the SDK's)
      const uint32_t info = r.Next(), poll = r.Next(), ref = r.Next(), mask = r.Next(), dst = r.Next(),
                     data = r.Next();
      const uint32_t value = info & 0x10 ? GpuSwap(ReadMemory(poll & ~3u), poll & 3) : ReadReg(poll);
      if (Compare(info, value, ref, mask)) {
        if (info & 0x100) WriteMemory(dst & ~3u, GpuSwap(data, dst & 3));
        else WriteRegister(dst, data);
      }
      return finish();
    }
    case kEventWriteZpd:
      if (external && !env_.own_zpd) return hand_back();  // query reports: the host's fake/host counts + OnZpd
      [[fallthrough]];
    case kEventWrite:
    case kEventWriteShd:
    case kEventWriteExt: {  // the EVENT_WRITE family: initiator write-back, then each one's memory write
      const uint32_t initiator = r.Next();
      WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
      if (opcode == kEventWriteShd && count >= 3) {  // ExecutePacketType3_EVENT_WRITE_SHD
        const uint32_t address = r.Next(), value = r.Next();
        const uint32_t data = (initiator >> 31) & 1 ? (env_.counter ? env_.counter() : 0u) : value;
        WriteMemory(address & ~3u, GpuSwap(data, address & 3));
      } else if (opcode == kEventWriteExt && count >= 2) {  // ExecutePacketType3_EVENT_WRITE_EXT: faked extents
        const uint32_t address = r.Next() & ~3u;
        // 6 x uint16 {0, 8192 >> 3, 0, 8192 >> 3, 0, 1}, copied and swapped 16-bit (k8in16)
        const uint16_t ext[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
        uint8_t* p = (external || env_.apply_writes) && env_.translate ? env_.translate(address) : nullptr;
        ++stats_.memory_writes_seen;
        if (p) {
          uint8_t bytes[12];
          for (int i = 0; i < 6; ++i) {
            const uint16_t s = uint16_t((ext[i] << 8) | (ext[i] >> 8));
            std::memcpy(bytes + 2 * i, &s, 2);
          }
          if (!Defer(address & 0x1FFFFFFFu, bytes, 12)) {
            Release();
            std::memcpy(p, bytes, 12);
          }
          ++stats_.memory_writes_done;
        }
      } else if (opcode == kEventWriteZpd && env_.own_zpd) {  // ExecutePacketType3_EVENT_WRITE_ZPD's end test
        const uint32_t report = ReadReg(kRegRbSampleCountAddr);
        const uint8_t* p = env_.translate ? env_.translate(report) : nullptr;
        bool is_end = false;
        for (uint32_t lane = 2; p && lane < 6; ++lane) {  // ZFail_A, ZFail_B, ZPass_A, ZPass_B
          is_end |= p[lane * 4] == 0xFF && p[lane * 4 + 1] == 0xFF && p[lane * 4 + 2] == 0xFE && p[lane * 4 + 3] == 0xED;
        }
        ++stats_.zpds;
        if (is_end) ++stats_.zpd_ends;
        env_.own_zpd(report, is_end);
      } else if (opcode == kEventWriteZpd) {
        ++stats_.memory_writes_seen;  // phase 1: the SDK writes the report
      }
      if (cb_.on_event) cb_.on_event(opcode, initiator);
      return finish();
    }
    case kDrawIndx:
    case kDrawIndx2: {  // ExecutePacketType3_DRAW_INDX(_2) -> ExecutePacketType3Draw
      if (external && !env_.OwnDraws()) {
        ++stats_.draws;
        return hand_back();  // the host writes the draw registers, tells the observer, and (patch 19) draws nothing
      }
      Draw d;
      if (opcode == kDrawIndx) {
        if (!r.remaining || r.remaining == end_remaining) return finish();
        d.viz_query = r.Next();
      }
      if (r.remaining == end_remaining) return finish();
      d.initiator = r.Next();
      WriteRegister(kRegVgtDrawInitiator, d.initiator);
      const uint32_t source_select = (d.initiator >> 6) & 3;  // kDMA = 0, kImmediate = 1, kAutoIndex = 2
      // Patch 0029 (Canary; Gears 3 bug hunt E8): a DMA-index draw too short to hold its index address and size draws
      // nothing (it used to run as a non-indexed draw).
      const bool short_dma = source_select == 0 && r.remaining < end_remaining + 2;
      if (source_select == 0 && r.remaining >= end_remaining + 2) {
        d.indexed = true;
        d.index_base = r.Next();
        WriteRegister(kRegVgtDmaBase, d.index_base);
        d.index_size = r.Next();
        WriteRegister(kRegVgtDmaSize, d.index_size);
      }
      const uint32_t viz = ReadReg(0x2293);  // PA_SC_VIZ_QUERY: viz_query_ena bit 0, kill_pix_post_hi_z bit 7
      d.executed = source_select != 1 && !short_dma && !((viz & 1) && (viz & 0x80));
      ++stats_.draws;
      if (env_.OwnDraws()) {
        if (d.executed) env_.own_draw(d, *vs_, *ps_);
        return finish();
      }
      if (cb_.on_draw) cb_.on_draw(regs_, d, *vs_, *ps_);
      return finish();
    }
    case kSetConstant: {  // ExecutePacketType3_SET_CONSTANT
      const uint32_t ot = r.Next(), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
      if (type <= 4) write_range(kTypeBase[type] + index, count - 1);
      return finish();
    }
    case kSetConstant2:
    case kSetShaderConstants: {  // SET_CONSTANT2 / SET_SHADER_CONSTANTS: absolute register index
      const uint32_t index = r.Next() & 0xFFFF;
      write_range(index, count - 1);
      return finish();
    }
    case kLoadAluConstant: {  // ExecutePacketType3_LOAD_ALU_CONSTANT
      const uint32_t address = r.Next() & 0x3FFFFFFF, ot = r.Next(), size = r.Next() & 0xFFF;
      const uint32_t index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
      if (type <= 4) write_range_mem(kTypeBase[type] + index, address & 0x1FFFFFFF, size);
      return finish();
    }
    case kImLoad: {  // ExecutePacketType3_IM_LOAD
      if (external && !env_.OwnDraws()) {
        ++stats_.shader_loads;
        return hand_back();  // the host's Shader objects (the renderer receives them)
      }
      const uint32_t at = r.Next(), ss = r.Next();
      const uint32_t addr = (at & ~3u) & 0x1FFFFFFF, dwords = ss & 0xFFFF;
      ((at & 3) == 0 ? vs_ : ps_) = CachedShader(addr, env_.translate ? env_.translate(addr) : nullptr, dwords);
      // xenos::ShaderType: 0 vertex, 1 pixel
      return finish();
    }
    case kImLoadImmediate: {  // ExecutePacketType3_IM_LOAD_IMMEDIATE
      if (external && !env_.OwnDraws()) {
        ++stats_.shader_loads;
        return hand_back();
      }
      const uint32_t type = r.Next(), ss = r.Next(), dwords = ss & 0xFFFF;
      ShaderRef s;
      s.valid = dwords <= r.remaining - end_remaining;
      std::vector<uint32_t> raw;
      for (uint32_t i = 0; i < dwords && r.remaining > end_remaining; ++i) {
        raw.push_back(Load(r.base + size_t(r.pos) * 4));  // as stored (big-endian), for the hash
        s.dwords.push_back(r.Next());
      }
      if (env_.hash && !raw.empty()) s.hash = env_.hash(reinterpret_cast<const uint8_t*>(raw.data()), raw.size() * 4);
      ++stats_.shader_loads;
      (type == 0 ? vs_ : ps_) = std::make_shared<const ShaderRef>(std::move(s));
      return finish();
    }
    case kSetBinMaskLo: bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | r.Next(); return finish();
    case kSetBinMaskHi: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(r.Next()) << 32); return finish();
    case kSetBinSelectLo: bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | r.Next(); return finish();
    case kSetBinSelectHi: bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(r.Next()) << 32); return finish();
    case kSetBinMask: {
      const uint64_t hi = r.Next(), lo = r.Next();
      bin_mask_ = (hi << 32) | lo;
      return finish();
    }
    case kSetBinSelect: {
      const uint64_t hi = r.Next(), lo = r.Next();
      bin_select_ = (hi << 32) | lo;
      return finish();
    }
    default:
      ++stats_.unknown_opcodes;
      stats_.last_unknown_opcode = opcode;
      return finish();
  }
}

}  // namespace xenos::pm4
