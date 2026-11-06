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
#include <cstdint>
#include <mutex>

// The launcher looks for this description's text in the exe before it passes the option.
REXCVAR_DEFINE_BOOL(gow3_unlock_insane, false, "Gears3",
                    "GitHub #174: Unlock Insane difficulty from the start (the profile is not changed)");

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

PPCFunc* g_exec_orig = nullptr;
PPCFunc* g_get_int_orig = nullptr;
std::atomic<bool> g_inner_resolved{false};

void GetValueIntHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32, value_addr = ctx.r5.u32;
  const int32_t id = ctx.r4.s32;
  g_get_int_orig(ctx, base);
  if (id != kUnlockablesSetting || !value_addr) return;
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
  if (!REXCVAR_GET(gow3_unlock_insane)) return;
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
