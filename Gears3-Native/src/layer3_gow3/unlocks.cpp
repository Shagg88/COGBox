// Layer 3 (Gears 3): GitHub #174 - Insane difficulty without finishing the campaign first (--gow3_unlock_insane).
// Ported from Flippage's gears-of-war-3-recomp (commit 26d6448, "Unlock Insane difficulty from the start"), whose
// addresses are the base disc's (0.0.0.11) - the same default.xex as COGBox's Disc version (notes/01). Title Update 6:
// the exec function matched by code (gow3_addresses.h), the class checked by name.
//
// The campaign difficulty screen reads the unlockables bitmask (GearProfileSettings setting 82; Insane is unlockable 0,
// Coalesced [InsaneDifficulty GearUnlockableResourceProvider] UnlockableId=0) through
// UOnlinePlayerStorage::GetProfileSettingValueInt. The native exec function (0x8251A2B0, native table entry at
// 0x83396738) is hooked; its first call resolves the C++ getter from the object's vtable (+336) and hooks that, which
// sets the Insane bit on reads of setting 82 by GearProfileSettings (vtable 0x82247140). Nothing is written to the
// profile or the saves: only what the screen reads is changed.
//
// --gow3_unlock_all_chapters (test): every act/chapter on every difficulty for Chapter Select. The repo does not know
// which GearProfileSettings ids hold campaign chapter progress (notes/24 names GearCampaignActData:InitializeChapters /
// CompleteChapterByDifficulty only), so the ids are not hard-coded: --gow3_unlock_chapter_settings="id:mask,..." lists
// them (decimal or 0x, mask OR-ed into what the screen reads); --gow3_unlock_all_chapters also turns on Insane. Find
// the ids with --gow3_profile_log: it logs each int setting read (id, value, class, caller) once per change, and
// marks reads made while the Select Chapter / Campaign menus are being read (one menu visit shows them).
#include "unlocks.h"
#include "gow3_addresses.h"

#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The launcher looks for this description's text in the exe before it passes the option.
REXCVAR_DEFINE_BOOL(gow3_unlock_insane, false, "Gears3",
                    "GitHub #174: Unlock Insane difficulty from the start (the profile is not changed)");

REXCVAR_DEFINE_BOOL(gow3_unlock_all_chapters, false, "Gears3",
                    "Test: report all campaign chapters on all difficulties (and Insane) as unlocked to the menus, using "
                    "the setting ids in gow3_unlock_chapter_settings (the profile is not changed)");
REXCVAR_DEFINE_STRING(gow3_unlock_chapter_settings, "", "Gears3",
                      "Test: GearProfileSettings ids holding chapter progress, as id:mask,... (found with "
                      "gow3_profile_log); mask is OR-ed into what the game reads when gow3_unlock_all_chapters is on");
REXCVAR_DEFINE_BOOL(gow3_profile_log, false, "Gears3",
                    "Developers: log each profile setting the game reads (id, value, class, caller) once per change - "
                    "open Select Chapter to see the chapter progress ids. Read-only");

namespace {

constexpr uint32_t kExecGetProfileSettingValueInt = gow3::addr::kExecGetProfileSettingValueInt;  // disc / TU6
constexpr uint32_t kNativeTableEntry = gow3::addr::kExecNativeTableEntry;                       // 0 = TU6: unknown
constexpr uint32_t kGetValueIntVtableOffset = 336;
constexpr uint32_t kGearProfileSettingsVtable = 0x82247140;  // disc only; TU6 checks the class by name
constexpr int32_t kUnlockablesSetting = 82;
constexpr uint32_t kInsaneUnlockBit = 1u << 0;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// The object is a GearProfileSettings: on the disc by its vtable (Flippage's check); on TU6, whose vtable address isn't
// mapped, by its class's name through the engine's own objects (ue3/objects.h, the globals in gow3_addresses.h).
bool IsGearProfileSettings(const uint8_t* base, uint32_t self) {
#if defined(GOW3_TU6) && GOW3_TU6
  (void)base;
  namespace obj = ue3::obj;
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return false;
  static std::atomic<uint32_t> cls{0};
  const uint32_t c = obj::ClassOf(self);
  if (c && c == cls.load(std::memory_order_relaxed)) return true;
  if (!c || obj::ObjectName(c) != "GearProfileSettings") return false;
  cls.store(c, std::memory_order_relaxed);
  return true;
#else
  return Load32(base, self) == kGearProfileSettingsVtable;
#endif
}

bool g_insane = false, g_log = false;
std::vector<std::pair<int32_t, uint32_t>> g_chapter_masks;  // id -> mask (gow3_unlock_chapter_settings)

void ParseChapterSettings(const std::string& text) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    char* e1 = nullptr; char* e2 = nullptr;
    const long id = std::strtol(item.c_str(), &e1, 0);
    const unsigned long mask = std::strtoul(item.c_str() + colon + 1, &e2, 0);
    if (e1 == item.c_str() || e2 == item.c_str() + colon + 1 || !mask) continue;
    g_chapter_masks.emplace_back(int32_t(id), uint32_t(mask));
  }
}

// Logs a read once per (class, id, value). "menu" marks reads while a GearProfileSettings read happens with the
// UI's chapter / campaign screen being built: we can't see the screen, so the caller (lr) is logged and every read
// within the burst that follows the first read of the unlockables (82, which the campaign menus read) is tagged.
void LogRead(uint8_t* base, uint32_t self, int32_t id, uint32_t value, uint32_t found, uint32_t lr) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> seen;
  static int burst = 0;
  const uint32_t vtable = Load32(base, self);
  const bool gear = IsGearProfileSettings(base, self);
  std::lock_guard<std::mutex> lock(mutex);
  if (gear && id == kUnlockablesSetting) burst = 64;  // campaign / chapter menus read 82 first
  const bool menu = burst > 0 && gear;
  if (menu) --burst;
  const uint64_t key = (uint64_t(vtable) << 32) | uint32_t(id);
  auto it = seen.find(key);
  if (it != seen.end() && it->second == value && !menu) return;
  if (seen.size() >= 4096) return;
  seen[key] = value;
  REXLOG_INFO("gow3_profile_log: get int id={} value={} (0x{:08X}) found={} gear={} menu={} object={:08X} vtable={:08X} "
              "from {:08X}", id, int32_t(value), value, found & 1, gear, menu, self, vtable, lr);
}

PPCFunc* g_exec_orig = nullptr;
PPCFunc* g_get_int_orig = nullptr;
std::atomic<bool> g_inner_resolved{false};

void GetValueIntHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32, value_addr = ctx.r5.u32;
  const int32_t id = ctx.r4.s32;
  const uint32_t lr = uint32_t(ctx.lr);
  g_get_int_orig(ctx, base);
  if (!value_addr) return;
  if (g_log) LogRead(base, self, id, Load32(base, value_addr), ctx.r3.u32, lr);
  for (const auto& [cid, mask] : g_chapter_masks) {
    if (cid != id || !IsGearProfileSettings(base, self)) continue;
    const uint32_t v = Load32(base, value_addr);
    if ((v & mask) == mask) break;
    Store32(base, value_addr, v | mask);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8)
      REXLOG_INFO("gow3_unlock_all_chapters: setting {} 0x{:08X} -> 0x{:08X}", id, v, v | mask);
    break;
  }
  if (!g_insane || id != kUnlockablesSetting) return;
  if (!IsGearProfileSettings(base, self)) return;
  const uint32_t unlocks = Load32(base, value_addr);
  if (unlocks & kInsaneUnlockBit) return;
  Store32(base, value_addr, unlocks | kInsaneUnlockBit);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4)
    REXLOG_INFO("gow3_unlock_insane: Insane unlocked for the difficulty screen (unlockables 0x{:08X} -> 0x{:08X})",
                unlocks, unlocks | kInsaneUnlockBit);
}

void ExecHook(PPCContext& ctx, uint8_t* base) {
  if (!g_inner_resolved.load(std::memory_order_acquire)) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!g_inner_resolved.load(std::memory_order_relaxed)) {
      const uint32_t vtable = Load32(base, ctx.r3.u32);
      const uint32_t fn = vtable ? Load32(base, vtable + kGetValueIntVtableOffset) : 0;
      auto* d = rex::Runtime::instance()->function_dispatcher();
      g_get_int_orig = fn ? d->GetFunction(fn) : nullptr;
      if (g_get_int_orig && d->SetFunction(fn, &GetValueIntHook))
        REXLOG_INFO("gow3_unlock_insane: GetProfileSettingValueInt at {:08X} hooked (vtable {:08X})", fn, vtable);
      else
        REXLOG_WARN("gow3_unlock_insane: GetProfileSettingValueInt not hooked (vtable {:08X}, entry {:08X})", vtable, fn);
      g_inner_resolved.store(true, std::memory_order_release);
    }
  }
  g_exec_orig(ctx, base);
}

}  // namespace

void Gow3InstallUnlocks() {
  const bool chapters = REXCVAR_GET(gow3_unlock_all_chapters);
  g_insane = REXCVAR_GET(gow3_unlock_insane) || chapters;
  g_log = REXCVAR_GET(gow3_profile_log);
  if (chapters) {
    ParseChapterSettings(REXCVAR_GET(gow3_unlock_chapter_settings));
    if (g_chapter_masks.empty())
      REXLOG_WARN("gow3_unlock_all_chapters: no chapter setting ids (gow3_unlock_chapter_settings) - Insane only; run "
                  "with --gow3_profile_log=true and open Select Chapter to find them");
  }
  if (!g_insane && !g_log && g_chapter_masks.empty()) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
  if (!d || !base) return;
  // The native table entry {name, function} must name our function: another build (Title Update 6) keeps it off
  // Disc: the native table entry {name, function} must name our function (Flippage's check). TU6: the address is the
  // one matched from the disc's code for this build (GOW3_TU6 is a separate exe).
  if (kNativeTableEntry && Load32(base, kNativeTableEntry) != kExecGetProfileSettingValueInt &&
      Load32(base, kNativeTableEntry + 4) != kExecGetProfileSettingValueInt) {
    REXLOG_WARN("gow3_unlock_insane: off - this build's native table differs");
    return;
  }
  g_exec_orig = d->GetFunction(kExecGetProfileSettingValueInt);
  if (!g_exec_orig || !d->SetFunction(kExecGetProfileSettingValueInt, &ExecHook)) {
    REXLOG_WARN("gow3_unlock_insane: off - execGetProfileSettingValueInt not hooked");
    return;
  }
  REXLOG_INFO("gow3_unlock_insane: on ({})", gow3::addr::kVersion);
}
