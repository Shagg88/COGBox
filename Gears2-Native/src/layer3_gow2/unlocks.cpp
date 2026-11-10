// Layer 3 (Gears of War 2): insane050 - Insane difficulty without finishing the campaign first (--gow2_unlock_insane).
// Gears 2 keeps its unlocks in profile setting 74 (GearProfileSettings ProfileMappings Id=74 Name=UnlockedUnlockables;
// jobs 1503 / 1504: the decompressed GearGame package), Insane being EGearUnlockable 0 (eUNLOCK_InsaneDifficulty; the
// enum's order, job 1504), so bit 0 - as Gears 3 / Judgment use setting 82 bit 0 (Gears3-Native unlocks.cpp).
// The C++ getter that answers GetProfileSettingValueInt (walks the per-owner settings arrays; 64-byte owner entries,
// 24-byte settings, type 1 = int) was matched from Gears 3 TU6's sub_82888290 / Judgment's sub_828A3310 - the getters
// those games' hooks resolve at run time (job 1507) - by code (jobs 1509 / 1510): two siblings share the shape; the one
// reading the settings array at +76 / +80, like Gears 3's at +72 / +76, is this one (the other reads +88 / +92).
// Nothing is written to the profile or the saves: only the value the game reads is changed, after the original ran.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The launcher looks for this description's text in the exe before it passes the option.
REXCVAR_DEFINE_BOOL(gow2_unlock_insane, false, "Gears2",
                    "insane050: Unlock Insane difficulty from the start (COGBox writes nothing to the profile; the game may keep it when it next saves your unlocks)");

// g2-100 (Shagg, test only, off by default): --gow2_profile_100 writes the id=value lines of --gow2_profile_100_file
// into the profile settings array the getter reads (found on the first read of a listed id, as Judgment's gj100), so
// the game's next profile save writes a 100% test profile itself. Ids: GearGame ProfileMappings (66-73 chapters per
// difficulty, 74 unlockables, 61/62 collectables "DiscoverFound", 79/80 co-op chapters, 95/96 chapter access).
REXCVAR_DEFINE_BOOL(gow2_profile_100, false, "Gears2",
                    "g2-100 (test only): write the profile values in gow2_profile_100_file into the in-memory profile, so "
                    "the game saves them (100% campaign test profile). Changes the profile when it saves");
REXCVAR_DEFINE_STRING(gow2_profile_100_file, "", "Gears2", "g2-100: text file of id=value lines for gow2_profile_100");

namespace {
#if defined(GOW2_TU6) && GOW2_TU6
constexpr uint32_t kGetProfileSettingValueInt = 0x825EC9E8u;  // TU6 (0.0.6.5)
constexpr const char* kBuild = "TU6";
#else
constexpr uint32_t kGetProfileSettingValueInt = 0x825E8540u;  // disc (0.0.0.5)
constexpr const char* kBuild = "disc";
#endif
constexpr int32_t kUnlockablesSetting = 74;
constexpr uint32_t kInsaneUnlockBit = 1u << 0;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

PPCFunc* g_orig = nullptr;
bool g_insane = false, g_p100 = false;
std::vector<std::pair<int32_t, uint32_t>> g_p100_values;
struct P100Layout { uint32_t field = 0, stride = 0, id_off = 0, val_off = 0; bool ok = false, failed = false; };
P100Layout g_lay;
uint32_t g_p100_last_ptr = 0, g_p100_last_count = 0;
bool GuestPtr(uint32_t p) { return p >= 0x40000000u && p < 0xA0000000u; }

void Profile100Apply(uint8_t* base, uint32_t self, int32_t id, uint32_t value) {
  if (!g_lay.ok && !g_lay.failed) {
    for (uint32_t f = 0x20; f < 0x400 && !g_lay.ok; f += 4) {
      const uint32_t ptr = Load32(base, self + f), cnt = Load32(base, self + f + 4), max = Load32(base, self + f + 8);
      if (!GuestPtr(ptr) || cnt == 0 || cnt > 4096 || max < cnt) continue;
      for (uint32_t st = 12; st <= 32 && !g_lay.ok; st += 4)
        for (uint32_t io = 0; io <= 8 && !g_lay.ok; io += 4) {
          bool sane = true;
          for (uint32_t i = 0; i < cnt && sane; ++i) sane = Load32(base, ptr + i * st + io) < 0x10000u;
          if (!sane) continue;
          for (uint32_t i = 0; i < cnt && !g_lay.ok; ++i) {
            if (Load32(base, ptr + i * st + io) != uint32_t(id)) continue;
            for (uint32_t vo = io + 4; vo + 4 <= st && !g_lay.ok; vo += 4)
              if (Load32(base, ptr + i * st + vo) == value) {
                // 0.5.0 review: a zero-padded struct can match one id + value by chance - trust the layout only when at
                // least 3 of the listed ids (or all, if fewer are listed) are in this array with the same stride
                size_t found_ids = 0;
                for (auto& [want_id, want] : g_p100_values) {
                  (void)want;
                  for (uint32_t k = 0; k < cnt; ++k)
                    if (Load32(base, ptr + k * st + io) == uint32_t(want_id)) { ++found_ids; break; }
                }
                if (found_ids >= std::min<size_t>(3, g_p100_values.size())) g_lay = {f, st, io, vo, true, false};
              }
          }
        }
    }
    if (!g_lay.ok) {
      g_lay.failed = true;
      REXLOG_WARN("gow2_profile_100: settings array not found in object {:08X} (id {} value 0x{:X})", self, id, value);
      return;
    }
    REXLOG_INFO("gow2_profile_100: settings array at object+0x{:X}, stride {}, id +{}, value +{}", g_lay.field,
                g_lay.stride, g_lay.id_off, g_lay.val_off);
  }
  if (!g_lay.ok) return;
  const uint32_t ptr = Load32(base, self + g_lay.field), cnt = Load32(base, self + g_lay.field + 4);
  if (!GuestPtr(ptr) || cnt > 4096 || (ptr == g_p100_last_ptr && cnt == g_p100_last_count)) return;
  g_p100_last_ptr = ptr, g_p100_last_count = cnt;
  int written = 0, missing = 0;
  for (auto& [want_id, want] : g_p100_values) {
    bool hit = false;
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint32_t e = ptr + i * g_lay.stride;
      if (Load32(base, e + g_lay.id_off) != uint32_t(want_id)) continue;
      const uint32_t old = Load32(base, e + g_lay.val_off);
      Store32(base, e + g_lay.val_off, want);
      REXLOG_INFO("gow2_profile_100: id {} 0x{:08X} -> 0x{:08X}", want_id, old, want);
      hit = true, ++written;
      break;
    }
    if (!hit) { ++missing; REXLOG_WARN("gow2_profile_100: id {} not in the settings array", want_id); }
  }
  REXLOG_INFO("gow2_profile_100: {} written, {} missing (array {:08X} x{}); saved with the next profile save", written,
              missing, ptr, cnt);
}

void GetValueIntHook(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ctx.r4.s32;
  const uint32_t value_addr = ctx.r5.u32, self = ctx.r3.u32;
  g_orig(ctx, base);
  if (g_p100 && value_addr && (ctx.r3.u32 & 1)) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    for (auto& [want_id, want] : g_p100_values)
      if (want_id == id) { Profile100Apply(base, self, id, Load32(base, value_addr)); break; }
  }
  if (!g_insane || id != kUnlockablesSetting || !value_addr || !(ctx.r3.u32 & 1)) return;
  const uint32_t v = Load32(base, value_addr);
  if (v & kInsaneUnlockBit) return;
  Store32(base, value_addr, v | kInsaneUnlockBit);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4)
    REXLOG_INFO("gow2_unlock_insane: Insane unlocked for the menus (unlockables 0x{:08X} -> 0x{:08X}, object {:08X}; in "
                "memory only)", v, v | kInsaneUnlockBit, self);
}
}  // namespace

void Gow2InstallUnlocks() {
  g_insane = REXCVAR_GET(gow2_unlock_insane);
  g_p100 = REXCVAR_GET(gow2_profile_100);
  if (g_p100) {
    std::ifstream in(REXCVAR_GET(gow2_profile_100_file));
    std::string line;
    while (std::getline(in, line)) {
      const size_t h = line.find('#');
      if (h != std::string::npos) line.erase(h);
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      try {
        g_p100_values.emplace_back(std::stoi(line.substr(0, eq)), uint32_t(std::stoul(line.substr(eq + 1), nullptr, 0)));
      } catch (...) {}
    }
    REXLOG_INFO("gow2_profile_100: {} value(s) from '{}'", g_p100_values.size(), REXCVAR_GET(gow2_profile_100_file));
    if (g_p100_values.empty()) g_p100 = false;
  }
  if (!g_insane && !g_p100) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  g_orig = d->GetFunction(kGetProfileSettingValueInt);
  if (!g_orig || !d->SetFunction(kGetProfileSettingValueInt, &GetValueIntHook)) {
    REXLOG_WARN("gow2_unlock_insane: off - the profile getter {:08X} isn't there ({})", kGetProfileSettingValueInt, kBuild);
    return;
  }
  REXLOG_INFO("gow2_unlock_insane / profile_100: hooked ({}, getter {:08X}; insane {}, profile_100 {})", kBuild,
              kGetProfileSettingValueInt, g_insane, g_p100);
  if (g_insane) REXLOG_INFO("gow2_unlock_insane: on ({}, getter {:08X})", kBuild, kGetProfileSettingValueInt);
}
