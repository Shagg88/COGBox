// Unit tests for xenos/renderer/queries.h (Stage 4, notes/46): inferred mode and exact ZPD mode (SDK patch 15). Game-free: guest memory is a local array, and the
// report layout (end = begin - 32) is the XDK 7645 one passed in the same way the app passes it.
// Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include "xenos/renderer/queries.h"

namespace q = xenos::renderer::queries;

namespace {
uint8_t g_mem[4096];
const uint8_t* Read(void*, uint32_t a) { return a < sizeof(g_mem) ? g_mem + a : nullptr; }
void Answer(uint32_t a, uint32_t n) { q::SampleCounts::WriteAnswer(g_mem + a, n, false); }
}  // namespace

TEST_CASE("queries: tracker pairs begin/end reports and reads the emulator's answer", "[queries]") {
  std::memset(g_mem, 0, sizeof(g_mem));
  q::QueryTracker t(q::ReportLayout{-32}, &Read, nullptr);
  uint32_t d = 0;
  REQUIRE(!t.OnDraw(0, false, d++).opened);         // no report programmed yet
  REQUIRE(t.OnDraw(0x120, false, d++).opened);      // q0 begin report 0x120 (end 0x100)
  t.OnDraw(0x120, true, d);                         // a copy inside the query: not counted
  t.OnDraw(0x120, false, d++);
  Answer(0x100, 77);                                // the emulator answers q0's end ZPD
  auto e = t.OnDraw(0x160, false, d++);             // q1 begin: q0 closes
  REQUIRE(e.closed);
  REQUIRE(e.opened);
  e = t.OnDraw(0x140, false, d++);                  // q1's end report programmed, draw outside any query
  REQUIRE(e.closed);
  REQUIRE(!e.opened);
  auto& r = t.EndFrame();
  REQUIRE(r.size() == 2);
  REQUIRE(r[0].EmulatorSamples() == 77);
  REQUIRE(r[0].draws == 2);
  REQUIRE(r[1].draws == 1);
  REQUIRE(r[1].end_seen);
  REQUIRE(r[1].EmulatorSamples() == 0);
  r[0].has_ours = true;
  r[0].our_samples = 0;
  REQUIRE(q::Compare(r[0]) == q::Verdict::kVisibilityDiffers);
  r[0].our_samples = 75;
  REQUIRE(q::Compare(r[0]) == q::Verdict::kClose);
  r[0].our_samples = 40;
  REQUIRE(q::Compare(r[0]) == q::Verdict::kCountDiffers);
  REQUIRE(q::Compare(r[1]) == q::Verdict::kNoOurs);
  t.NextFrame();
  REQUIRE(!t.OnDraw(0x140, false, d++).opened);     // stale register across the swap: no phantom query
}

TEST_CASE("queries: pending marker and answer layout", "[queries]") {
  uint8_t rep[32] = {};
  const uint32_t m[2] = {q::kPendingMarkerLE, q::kPendingMarkerLE};
  std::memcpy(rep + 16, m, 8);
  REQUIRE(q::SampleCounts::Read(rep).Pending());
  // D3D stores 0xFFFFFEED big-endian: bytes FF FF FE ED.
  REQUIRE(rep[16] == 0xFF);
  REQUIRE(rep[19] == 0xED);
  q::SampleCounts::WriteAnswer(rep, 7, true);
  const auto s = q::SampleCounts::Read(rep);
  REQUIRE(!s.Pending());
  REQUIRE(s.zpass_a == 4);
  REQUIRE(s.zpass_b == 3);
  REQUIRE(s.ZPass() == 7);
  REQUIRE(s.total_a + s.total_b == 7);
  q::SampleCounts::WriteAnswer(rep, 7, false);  // the SDK's current layout
  REQUIRE(q::SampleCounts::Read(rep).zpass_a == 7);
  REQUIRE(q::SampleCounts::Read(rep).zpass_b == 0);
}

TEST_CASE("queries: exact ZPD events (SDK patch 15)", "[queries]") {
  std::memset(g_mem, 0, sizeof(g_mem));
  q::QueryTracker t(q::ReportLayout{-32}, &Read, nullptr);
  t.UseExactZpds();
  uint32_t d = 0;
  REQUIRE(!t.OnDraw(0x400, false, d++).opened);  // stale register before any ZPD: ignored in exact mode
  auto e = t.OnZpd(0x120, false, q::EmuAnswer::kUnknown, 0, 0, d);
  REQUIRE(e.opened);
  REQUIRE(t.exact());
  t.OnDraw(0x120, false, d++);
  t.OnDraw(0x120, true, d);  // copy: not counted
  t.OnDraw(0x120, false, d++);
  Answer(0x100, 500);
  e = t.OnZpd(0x100, true, q::EmuAnswer::kHost, 500, 2, d);
  REQUIRE(e.closed);
  t.OnDraw(0x100, false, d++);  // outside any query
  // A begin whose end never comes, then a begin of another query.
  t.OnZpd(0x160, false, q::EmuAnswer::kUnknown, 0, 0, d);
  t.OnDraw(0x160, false, d++);
  e = t.OnZpd(0x1A0, false, q::EmuAnswer::kUnknown, 0, 0, d);
  REQUIRE(e.closed);
  REQUIRE(e.opened);
  e = t.OnZpd(0x900, true, q::EmuAnswer::kFake, 1000, 0, d);  // not this query's end report
  REQUIRE(!e.closed);
  REQUIRE(t.unmatched_ends() == 1);
  Answer(0x180, 1000);
  t.OnZpd(0x180, true, q::EmuAnswer::kFake, 1000, 0, d);
  auto& r = t.EndFrame();
  REQUIRE(r.size() == 3);
  REQUIRE(r[0].draws == 2);
  REQUIRE(r[0].answer == q::EmuAnswer::kHost);
  REQUIRE(r[0].host_segments == 2);
  REQUIRE(r[0].EmulatorSamples() == 500);
  REQUIRE(r[1].unended);
  REQUIRE(r[1].draws == 1);
  REQUIRE(r[2].answer == q::EmuAnswer::kFake);
  r[2].has_ours = true;
  r[2].our_samples = 1000;
  REQUIRE(q::Compare(r[2]) == q::Verdict::kNoEmulator);  // a fake answer is never a reference
  r[0].has_ours = true;
  r[0].our_samples = 500;
  REQUIRE(q::Compare(r[0]) == q::Verdict::kSame);
  REQUIRE(t.zpd_begins() == 3);
  REQUIRE(t.zpd_ends() == 3);
}

TEST_CASE("queries: answer cache (our renderer's immediate answers, upl-s4c)", "[queries]") {
  q::AnswerCache c;
  bool hit = true;
  REQUIRE(c.Estimate(0x100, 1, 0, &hit) == 1);  // nothing known yet: visible
  REQUIRE(!hit);
  c.Learn(0x100, 0);
  REQUIRE(c.Estimate(0x100, 1, 0, &hit) == 0);  // a real zero is kept ...
  REQUIRE(hit);
  REQUIRE(c.Estimate(0x100, 1, 1) == 1);        // ... unless a minimum is asked for
  c.Learn(0x100, 345);
  REQUIRE(c.Estimate(0x100, 1, 0) == 345);
  // Guarded correction: only while the report still holds our write for that ZPD.
  uint8_t rep[32] = {};
  const uint64_t seq = c.NoteWritten(0x100, 345);
  q::SampleCounts::WriteAnswer(rep, 345, false);
  REQUIRE(c.StillOurs(0x100, seq, q::SampleCounts::Read(rep)));
  const uint32_t marker[2] = {q::kPendingMarkerLE, q::kPendingMarkerLE};
  std::memcpy(rep + 16, marker, 8);  // the game re-issued the query: D3D put its marker back
  REQUIRE(!c.StillOurs(0x100, seq, q::SampleCounts::Read(rep)));
  q::SampleCounts::WriteAnswer(rep, 345, false);
  c.NoteWritten(0x100, 345);  // a later end ZPD on the same report
  REQUIRE(!c.StillOurs(0x100, seq, q::SampleCounts::Read(rep)));
  REQUIRE(q::AnswerCache::ToGuestSamples(10, 4) == 3);
  REQUIRE(q::AnswerCache::ToGuestSamples(1, 4) == 1);  // something passed: never rounded to 0
  REQUIRE(q::AnswerCache::ToGuestSamples(0, 4) == 0);
  REQUIRE(c.size() == 1);
}

TEST_CASE("queries: 'hidden' only after repeated zeros, keyed by report and draw count (job 139 flicker)", "[queries]") {
  q::AnswerCache c;
  c.SetHiddenAfter(3);
  const uint64_t k = q::AnswerCache::Key(0x200, 4);
  c.Learn(k, 0);
  REQUIRE(c.Estimate(k, 1, 0) == 1);  // one zero: not trusted, visible
  REQUIRE(!c.TrustZero(k));
  c.Learn(k, 0);
  REQUIRE(c.Estimate(k, 1, 0) == 1);
  c.Learn(k, 0);
  REQUIRE(c.Estimate(k, 1, 0) == 0);  // three in a row: hidden
  REQUIRE(c.TrustZero(k));
  c.Learn(k, 12);                     // seen again: visible at once, streak reset
  REQUIRE(c.Estimate(k, 1, 0) == 12);
  c.Learn(k, 0);
  REQUIRE(c.Estimate(k, 1, 0) == 1);
  // The same report with a different draw count (another primitive reusing the query) does not inherit the zeros.
  const uint64_t other = q::AnswerCache::Key(0x200, 9);
  for (int i = 0; i < 3; ++i) c.Learn(k, 0);
  REQUIRE(c.Estimate(k, 1, 0) == 0);
  bool hit = true;
  REQUIRE(c.Estimate(other, 1, 0, &hit) == 1);
  REQUIRE(!hit);
}
