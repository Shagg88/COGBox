// Unit tests for xenos/pm4.h (Stage 5B, notes/49): our PM4 processor on hand-built rings. Game-free: guest memory is a
// local array; packets are written big-endian as the game's D3D writes them. Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xenos/pm4.h"

namespace pm = xenos::pm4;

namespace {
std::vector<uint8_t> g_mem(1 << 20);
void Put(uint32_t addr, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(g_mem.data() + addr, &v, 4);
}
uint32_t Type0(uint32_t reg, uint32_t count, bool one = false) {
  return (0u << 30) | ((count - 1) << 16) | (one ? 0x8000u : 0u) | reg;
}
uint32_t Type3(uint32_t op, uint32_t count, bool predicate = false) {
  return (3u << 30) | ((count - 1) << 16) | (op << 8) | (predicate ? 1u : 0u);
}
struct Seen {
  std::vector<pm::Draw> draws;
  std::vector<std::vector<uint32_t>> vs, ps;
  std::vector<uint32_t> swaps;
};
pm::Processor Make(Seen& seen) {
  return pm::Processor(pm::Environment{[](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; }, false},
                       pm::Callbacks{[&seen](const auto&, const pm::Draw& d, const pm::ShaderRef& vs,
                                             const pm::ShaderRef& ps) {
                                       seen.draws.push_back(d);
                                       seen.vs.push_back(vs.dwords);
                                       seen.ps.push_back(ps.dwords);
                                     },
                                     [&seen](const auto&, uint32_t fb, uint32_t, uint32_t) { seen.swaps.push_back(fb); },
                                     nullptr});
}
}  // namespace

TEST_CASE("pm4: register writes, constants, shaders, draws, indirect buffers, swap", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  // Indirect buffer at 0x8000: a type-0 write and a DRAW_INDX_2 (auto-index).
  uint32_t ib = 0x8000;
  auto ibput = [&](uint32_t v) { Put(ib, v); ib += 4; };
  ibput(Type0(0x2205, 1));  // PA_SU_SC_MODE_CNTL
  ibput(0x12345678);
  ibput(Type3(pm::kDrawIndx2, 1));
  ibput((2u << 6) | 4);  // source_select = auto-index, prim 4
  const uint32_t ib_dwords = (ib - 0x8000) / 4;
  // Shader ucode at 0x9000 (3 dwords).
  Put(0x9000, 0xAAAA0001);
  Put(0x9004, 0xAAAA0002);
  Put(0x9008, 0xAAAA0003);
  // Constants in memory at 0xA000 for LOAD_ALU_CONSTANT (2 dwords).
  Put(0xA000, 0x3F800000);
  Put(0xA004, 0x40000000);
  // The ring at 0x1000, 64 dwords.
  std::vector<uint32_t> ring = {
      Type0(0x2000, 2), 0x11, 0x22,                        // RB_SURFACE_INFO, RB_COLOR_INFO
      Type0(0x2104, 3, true), 1, 2, 0xF,                   // write-one-register: last value wins
      Type3(pm::kSetConstant, 3), (4u << 16) | 0x200, 7, 8,  // REGISTERS type: 0x2200, 0x2201
      Type3(pm::kSetConstant, 2), (0u << 16) | 4, 0xC0,      // ALU constant 4 -> 0x4004
      Type3(pm::kLoadAluConstant, 3), 0xA000, (1u << 16) | 2, 2,  // FETCH 2..3 from memory -> 0x4802, 0x4803
      Type3(pm::kImLoad, 2), 0x9000 | 0, 3,                // vertex shader
      Type3(pm::kImLoadImmediate, 4), 1, 2, 0xBBBB0001, 0xBBBB0002,  // pixel shader, embedded
      Type3(pm::kIndirectBuffer, 2), 0x8000, ib_dwords,
      Type3(pm::kDrawIndx, 4), 0, (0u << 6) | 4, 0x7000, 6,  // viz token, initiator (DMA), base, size
      Type3(pm::kSetBinSelect, 2), 0, 0,                   // select 0 -> predicated packets are skipped
      Type3(pm::kDrawIndx2, 1, true), (2u << 6) | 4,       // predicated: skipped
      Type3(pm::kXeSwap, 4), 0x53574150, 0x1F5F8000, 1280, 720,
  };
  REQUIRE(ring.size() <= 64);
  for (size_t i = 0; i < ring.size(); ++i) Put(0x1000 + uint32_t(i) * 4, ring[i]);
  Seen seen;
  pm::Processor p = Make(seen);
  p.ExecutePrimary(0x1000, 64, 0, uint32_t(ring.size()));
  const auto& r = p.Registers();
  CHECK(r[0x2000] == 0x11);
  CHECK(r[0x2001] == 0x22);
  CHECK(r[0x2104] == 0xF);
  CHECK(r[0x2105] == 0);
  CHECK(r[0x2200] == 7);
  CHECK(r[0x2201] == 8);
  CHECK(r[0x4004] == 0xC0);
  CHECK(r[0x4802] == 0x3F800000);
  CHECK(r[0x4803] == 0x40000000);
  CHECK(r[0x2205] == 0x12345678);
  REQUIRE(seen.draws.size() == 2);  // the indirect buffer's, then DRAW_INDX; the predicated one is skipped
  CHECK(!seen.draws[0].indexed);
  CHECK(seen.draws[1].indexed);
  CHECK(seen.draws[1].index_base == 0x7000);
  CHECK(seen.draws[1].index_size == 6);
  CHECK(seen.vs[0] == std::vector<uint32_t>{0xAAAA0001, 0xAAAA0002, 0xAAAA0003});
  CHECK(seen.ps[0] == std::vector<uint32_t>{0xBBBB0001, 0xBBBB0002});
  REQUIRE(seen.swaps.size() == 1);
  CHECK(seen.swaps[0] == 0x1F5F8000);
  CHECK(p.GetStats().predicated_skipped == 1);
  CHECK(p.GetStats().errors == 0);
  CHECK(p.GetStats().unknown_opcodes == 0);
}

TEST_CASE("pm4: the ring wraps and changes are reported once", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  // 8-dword ring at 0x2000; the range starts at dword 6 and wraps.
  const uint32_t words[] = {Type0(0x2180, 1), 0x55, Type0(0x2181, 1), 0x66};
  const uint32_t at[] = {6, 7, 0, 1};
  for (int i = 0; i < 4; ++i) Put(0x2000 + at[i] * 4, words[i]);
  Seen seen;
  pm::Processor p = Make(seen);
  p.ExecutePrimary(0x2000, 8, 6, 2);
  CHECK(p.Registers()[0x2180] == 0x55);
  CHECK(p.Registers()[0x2181] == 0x66);
  auto changes = p.TakeChanges();
  CHECK(changes.size() == 2);
  CHECK(p.TakeChanges().empty());
}

TEST_CASE("pm4: REG_RMW and COND_WRITE to a register", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  Put(0x3000, 5);  // memory value polled by COND_WRITE, stored big-endian: polled with k8in32 (address | 2)
  const uint32_t ring[] = {
      Type0(0x0300, 1), 0xF0F0,                              // REG_RMW addresses registers & 0x1FFF (as the SDK)
      Type3(pm::kRegRmw, 3), 0x0300, 0xFF00, 0x000F,         // (0xF0F0 & 0xFF00) | 0xF = 0xF00F
      Type3(pm::kCondWrite, 6), 0x13, 0x3002, 5, 0xFFFFFFFF, 0x2301, 0x77,  // memory == 5 -> write register
      Type3(pm::kCondWrite, 6), 0x13, 0x3002, 6, 0xFFFFFFFF, 0x2302, 0x88,  // memory != 6 -> no write
  };
  for (size_t i = 0; i < std::size(ring); ++i) Put(0x4000 + uint32_t(i) * 4, ring[i]);
  Seen seen;
  pm::Processor p = Make(seen);
  p.ExecutePrimary(0x4000, 32, 0, uint32_t(std::size(ring)));
  CHECK(p.Registers()[0x0300] == 0xF00F);
  CHECK(p.Registers()[0x2301] == 0x77);
  CHECK(p.Registers()[0x2302] == 0);
}

TEST_CASE("pm4: external mode (phase 2) - registers to the host, action packets handed back, memory written", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  std::vector<std::pair<uint32_t, uint32_t>> host_writes;
  std::vector<std::vector<uint32_t>> handed;
  uint32_t host_regs[0x5003] = {};
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) {
    host_writes.emplace_back(i, v);
    if (i < 0x5003) host_regs[i] = v;
  };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t* d, uint32_t n) { handed.emplace_back(d, d + n); };
  env.counter = [] { return 42u; };
  env.running = [] { return true; };
  env.vsync = [] { return true; };
  pm::Processor p(std::move(env), pm::Callbacks{});
  Put(0x6000, 7);  // polled by WAIT_REG_MEM (k8in32), already equal
  const uint32_t ring[] = {
      Type0(0x2200, 1), 0xAB,
      Type3(pm::kSetBinSelect, 2), 0, 1,                 // select 1 & mask 0xFFFFFFFF -> predicated packets run
      Type3(pm::kDrawIndx2, 1, true), (2u << 6) | 4,     // predicated draw: handed back with the predicate bit cleared
      Type3(pm::kEventWriteShd, 3), 0x80000000u | 5, 0x5002, 0,  // counter (42) to 0x5000, k8in32
      Type3(pm::kMemWrite, 3), 0x5012, 0x11111111, 0x22222222,   // two words at 0x5010, k8in32
      Type3(pm::kWaitRegMem, 5), 0x13, 0x6002, 7, 0xFFFFFFFF, 0x100,  // memory == 7: no wait
      Type3(pm::kXeSwap, 4), 0x53574150, 0x1F5F8000, 1280, 720,
  };
  for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
  CHECK(host_regs[0x2200] == 0xAB);
  REQUIRE(handed.size() == 2);  // the draw and the swap
  CHECK(std::byteswap(handed[0][0]) == Type3(pm::kDrawIndx2, 1, false));
  CHECK(handed[0].size() == 2);
  CHECK(handed[1].size() == 5);
  auto be = [](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, g_mem.data() + a, 4);
    return std::byteswap(v);
  };
  CHECK(be(0x5000) == 42);
  CHECK(be(0x5010) == 0x11111111);
  CHECK(be(0x5014) == 0x22222222);
  CHECK(p.GetStats().wait_loops == 1);
  CHECK(p.GetStats().wait_spins == 0);
  CHECK(p.GetStats().errors == 0);
}

TEST_CASE("pm4: own draws (phase 3) - draws and shader loads stay with us", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  uint32_t host_regs[0x5003] = {};
  size_t handed = 0;
  std::vector<pm::Draw> draws;
  std::vector<uint64_t> vs_hashes;
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) { if (i < 0x5003) host_regs[i] = v; };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t*, uint32_t) { ++handed; };
  env.hash = [](const uint8_t* p, size_t n) {
    uint64_t h = 0;
    for (size_t i = 0; i < n; ++i) h = h * 31 + p[i];
    return h;
  };
  env.own_draw = [&](const pm::Draw& d, const pm::ShaderRef& vs, const pm::ShaderRef&) {
    draws.push_back(d);
    vs_hashes.push_back(vs.hash);
  };
  pm::Processor p(std::move(env), pm::Callbacks{});
  Put(0x9000, 0x01020304);
  const uint32_t ring[] = {
      Type3(pm::kImLoad, 2), 0x9000, 1,
      Type0(0x2293, 1), 0x81,                          // viz query on + kill: the next draw is dropped
      Type3(pm::kDrawIndx2, 1), (2u << 6) | 4,
      Type0(0x2293, 1), 0,
      Type3(pm::kDrawIndx, 4), 0, (10u << 16) | (1u << 11) | 4, 0x7002, 0x40000006,  // 10 32-bit indices, 8in32
  };
  for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
  CHECK(handed == 0);
  REQUIRE(draws.size() == 1);
  CHECK(draws[0].indexed);
  CHECK(draws[0].index_base == 0x7002);
  CHECK(draws[0].index_size == 0x40000006);
  CHECK(host_regs[0x21FC] == ((10u << 16) | (1u << 11) | 4));
  CHECK(host_regs[0x21FA] == 0x7002);
  CHECK(vs_hashes[0] == ((((uint64_t(1) * 31 + 2) * 31) + 3) * 31 + 4));  // over the big-endian bytes 01 02 03 04
}

TEST_CASE("pm4: shader load cache (job 172) - same bytes reuse, changed bytes reload", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  uint32_t host_regs[0x5003] = {};
  std::vector<std::pair<uint64_t, uint32_t>> seen;  // (vs hash, first vs dword)
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) { if (i < 0x5003) host_regs[i] = v; };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t*, uint32_t) {};
  env.hash = [](const uint8_t* p, size_t n) {
    uint64_t h = 0;
    for (size_t i = 0; i < n; ++i) h = h * 31 + p[i];
    return h;
  };
  env.own_draw = [&](const pm::Draw&, const pm::ShaderRef& vs, const pm::ShaderRef&) {
    seen.push_back({vs.hash, vs.dwords.empty() ? 0u : vs.dwords[0]});
  };
  pm::Processor p(std::move(env), pm::Callbacks{});
  const uint32_t ring[] = {Type3(pm::kImLoad, 2), 0x9000, 2, Type3(pm::kDrawIndx2, 1), (2u << 6) | 4};
  for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
  Put(0x9000, 0x01020304);
  Put(0x9004, 0x05060708);
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));  // same bytes: from the cache
  Put(0x9000, 0x0A0B0C0D);                                      // the title rewrote the shader in place
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == seen[1]);
  CHECK(seen[0].second == 0x01020304u);
  CHECK(seen[2].second == 0x0A0B0C0Du);
  CHECK(seen[2].first != seen[0].first);
  CHECK(p.GetStats().shader_loads == 3);
  CHECK(p.GetStats().shader_loads_cached == 1);
}

TEST_CASE("pm4: deferred writes (job 173) - fences wait for the renderer, reads and waits release first", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  uint32_t host_regs[0x5003] = {};
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> queued;  // what the renderer thread would write
  size_t releases = 0;
  auto run_queued = [&] {
    for (auto& [a, b] : queued) std::memcpy(g_mem.data() + a, b.data(), b.size());
    queued.clear();
  };
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) { if (i < 0x5003) host_regs[i] = v; };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t*, uint32_t) {};
  env.counter = [] { return 42u; };
  env.running = [] { return true; };
  env.vsync = [] { return true; };
  env.release = [&] { ++releases; run_queued(); };
  env.defer_write = [&](uint32_t a, const void* b, uint32_t n) {
    auto* p = static_cast<const uint8_t*>(b);
    queued.push_back({a, std::vector<uint8_t>(p, p + n)});
    return true;
  };
  pm::Processor p(std::move(env), pm::Callbacks{});
  auto be = [](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, g_mem.data() + a, 4);
    return std::byteswap(v);
  };
  {  // a fence write is queued, not written; the kick's end is no release point any more
    const uint32_t ring[] = {Type3(pm::kMemWrite, 2), 0x5002, 0x11111111};
    for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
    p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
    CHECK(be(0x5000) == 0);
    CHECK(queued.size() == 1);
    CHECK(releases == 0);
  }
  {  // a wait on that address: the read finds the pending write, releases (the renderer writes it), and passes
    const uint32_t ring[] = {Type3(pm::kWaitRegMem, 5), 0x13, 0x5002, 0x11111111, 0xFFFFFFFF, 0x100};
    for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
    p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
    CHECK(be(0x5000) == 0x11111111);
    CHECK(releases == 1);
    CHECK(p.GetStats().release_reads == 1);
    CHECK(p.GetStats().wait_spins == 0);
  }
  {  // a failing wait elsewhere releases once before it spins (the renderer may owe the value)
    queued.push_back({0x6000, {0, 0, 0, 7}});  // stands for work the renderer thread has queued
    const uint32_t ring[] = {Type3(pm::kWaitRegMem, 5), 0x13, 0x6002, 7, 0xFFFFFFFF, 0x100};
    for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
    p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
    CHECK(releases == 2);
    CHECK(p.GetStats().release_waits == 1);
    CHECK(p.GetStats().wait_spins == 0);
  }
  CHECK(p.GetStats().deferred_writes == 1);
}

TEST_CASE("pm4: own ZPD - end marker detected, nothing handed back", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  uint32_t host_regs[0x5003] = {};
  size_t handed = 0;
  std::vector<std::pair<uint32_t, bool>> zpds;
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) { if (i < 0x5003) host_regs[i] = v; };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t*, uint32_t) { ++handed; };
  env.own_zpd = [&](uint32_t a, bool end) { zpds.push_back({a, end}); };
  pm::Processor p(std::move(env), pm::Callbacks{});
  // report at 0x9100: a begin (no marker), then D3D's end marker FF FF FE ED in ZPass_B (lane 5)
  const uint8_t marker[4] = {0xFF, 0xFF, 0xFE, 0xED};
  const uint32_t ring[] = {
      Type0(0x2325, 1), 0x9100, Type3(pm::kEventWriteZpd, 1), 0x15,
      Type3(pm::kMemWrite, 2), 0x9100 + 20, 0xEDFEFFFF,  // no swap: stored as host bytes FF FF FE ED
      Type3(pm::kEventWriteZpd, 1), 0x15,
  };
  for (size_t i = 0; i < std::size(ring); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
  p.ExecutePrimary(0x7000, 64, 0, uint32_t(std::size(ring)));
  CHECK(handed == 0);
  CHECK(host_regs[0x21F9] == 0x15);
  REQUIRE(zpds.size() == 2);
  CHECK(zpds[0] == std::make_pair(0x9100u, false));
  CHECK(zpds[1].first == 0x9100u);
  CHECK(std::memcmp(g_mem.data() + 0x9100 + 20, marker, 4) == 0);  // the marker is where D3D puts it
  CHECK(zpds[1].second);
  CHECK(p.GetStats().zpds == 2);
}

TEST_CASE("pm4: own swap (5C-2) - XE_SWAP not handed back, front buffer and size passed on", "[pm4]") {
  std::fill(g_mem.begin(), g_mem.end(), 0);
  uint32_t host_regs[0x5003] = {};
  size_t handed = 0;
  std::vector<std::array<uint32_t, 3>> swaps;
  pm::Environment env;
  env.translate = [](uint32_t a) -> uint8_t* { return a < g_mem.size() ? g_mem.data() + a : nullptr; };
  env.write_register = [&](uint32_t i, uint32_t v) { if (i < 0x5003) host_regs[i] = v; };
  env.read_register = [&](uint32_t i) { return i < 0x5003 ? host_regs[i] : 0u; };
  env.execute_packet = [&](const uint32_t*, uint32_t) { ++handed; };
  env.own_swap = [&](uint32_t fb, uint32_t w, uint32_t h) { swaps.push_back({fb, w, h}); };
  pm::Processor p(std::move(env), pm::Callbacks{});
  std::vector<uint32_t> ring = {Type3(pm::kXeSwap, 63), 0x53574150, 0x1F00A000, 1280, 720};
  ring.resize(1 + 63, 0);
  ring.push_back(Type0(0x2000, 1));
  ring.push_back(0x1234);  // the packet after the swap still runs
  for (size_t i = 0; i < ring.size(); ++i) Put(0x7000 + uint32_t(i) * 4, ring[i]);
  p.ExecutePrimary(0x7000, 128, 0, uint32_t(ring.size()));
  CHECK(handed == 0);
  REQUIRE(swaps.size() == 1);
  CHECK(swaps[0] == std::array<uint32_t, 3>{0x1F00A000u, 1280u, 720u});
  CHECK(host_regs[0x2000] == 0x1234u);
  CHECK(p.GetStats().swaps == 1);
}
