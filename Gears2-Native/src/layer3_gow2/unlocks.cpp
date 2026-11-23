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
#include <ue3/audio_menu.h>
#include <ue3/profile_tools.h>

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
// 23 Nov parity pass (Shagg 10:42: the profile debugging tools in all four games): Gears 1 / 3 / Judgment's
UE3_AUDIO_MENU_CVARS(gow2, "Gears2");

// --*_profile_log - each int setting read (id, value, object, caller) once per change. Read-only.
REXCVAR_DEFINE_BOOL(gow2_profile_log, false, "Gears2",
                    "Developers: log each profile setting the game reads (id, value, object, caller) once per change. "
                    "Read-only");

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
bool g_insane = false, g_log = false;
ue3::Profile100 g_p100;        // --gow2_profile_100 (layer 2 ue3/profile_tools.h)
ue3::ProfileLogSeen g_seen;    // --gow2_profile_log
// The audio menu's sliders (ue3/audio_menu.h). Ids: not known yet for this build - the test job reads them from the
// game's ProfileMappings (as GearsJ-Native/tools/gj100_profile_ids.ps1 does) or --gow2_profile_log while each slider is
// moved; until then --gow2_audio_menu_ids=<music>,<effects>,<dialogue> sets them and the launcher's values are skipped.
constexpr ue3::AudioMenuSlider kAudioSliders[ue3::kAudioMenuSliders] = {{"music", -1}, {"effects", -1}, {"dialogue", -1}};
ue3::AudioMenu g_audio;   // --gow2_audio_music / _effects / _dialogue

void GetValueIntHook(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ctx.r4.s32;
  const uint32_t value_addr = ctx.r5.u32, self = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  g_orig(ctx, base);
  g_p100.OnRead(base, self, id, value_addr, ctx.r3.u32, false);   // g2-100 never overrode the read itself
  g_audio.OnRead(base, self, id, value_addr, ctx.r3.u32);
  if (g_log && value_addr) {
    const uint32_t value = Load32(base, value_addr);
    if (g_seen.First(lr, id, value))
      REXLOG_INFO("gow2_profile_log: get int id={} value={} (0x{:08X}) found={} object={:08X} vtable={:08X} from {:08X}",
                  id, int32_t(value), value, ctx.r3.u32 & 1, self, Load32(base, self), lr);
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
  g_log = REXCVAR_GET(gow2_profile_log);
  if (REXCVAR_GET(gow2_profile_100)) g_p100.Load("gow2_profile_100", REXCVAR_GET(gow2_profile_100_file));
  UE3_AUDIO_MENU_SETUP(g_audio, gow2, kAudioSliders);
  if (!g_insane && !g_p100.On() && !g_log && !g_audio.On()) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  g_orig = d->GetFunction(kGetProfileSettingValueInt);
  if (!g_orig || !d->SetFunction(kGetProfileSettingValueInt, &GetValueIntHook)) {
    REXLOG_WARN("gow2_unlock_insane: off - the profile getter {:08X} isn't there ({})", kGetProfileSettingValueInt, kBuild);
    return;
  }
  REXLOG_INFO("gow2_unlock_insane / profile_100 / profile_log: hooked ({}, getter {:08X}; insane {}, profile_100 {}, "
              "profile_log {})", kBuild, kGetProfileSettingValueInt, g_insane, g_p100.Count(), g_log);
  if (g_insane) REXLOG_INFO("gow2_unlock_insane: on ({}, getter {:08X})", kBuild, kGetProfileSettingValueInt);
}
