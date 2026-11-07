// Layer 3 (Judgment): GitHub #174 groundwork - logs every profile setting the game's script reads (--gowj_profile_log,
// off by default), so one run that opens the campaign difficulty screen shows which setting / bit holds "Insane
// unlocked" (Gears 3: GearProfileSettings setting 82, bit 0 - Flippage's gears-of-war-3-recomp 26d6448).
//
// UOnlinePlayerStorage::execGetProfileSettingValueInt is sub_82538128 here: matched from Gears 3's disc 0x8251A2B0 by
// its code in our generated sources (the same 61 instructions apart from the globals' addresses, UObject's vtable
// slots 4 bytes later: 192 -> 196, the getter 336 -> 340; its five sibling exec functions map in the same order with
// the same shift). The first call resolves the C++ getter from the object's vtable (+340) and hooks that; each read
// is logged once per (object class vtable, id, caller, value), so one visit to a menu names every setting it reads.
//
// GitHub #236: --gowj_unlock_aftermath (test only, off by default) ORs --gowj_unlock_aftermath_mask into the value the
// getter returns for setting --gowj_unlock_aftermath_setting, like Gears 3's Insane unlock (unlocks.cpp). In memory
// only: the profile and saves are never written differently. The setting id is not known yet (jaft-01: Coalesced has
// only [InsaneDifficulty GearUnlockableResourceProvider] UnlockableId=0 and GearProfileSettings
// LastCampaignUnlockable=14, no Aftermath/Epilogue unlockable), so it must be given; the default mask sets
// unlockables 0..14 (the campaign ones up to LastCampaignUnlockable). Find the id with --gowj_profile_log on the
// campaign menu first.
#include "profile_log.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(gowj_profile_log, false, "Judgment",
                    "GitHub #174: log each profile setting the game's script reads (id, value, object vtable), once per "
                    "change - to find the setting that unlocks Insane. Read-only");

REXCVAR_DEFINE_BOOL(gowj_unlock_aftermath, false, "Judgment",
                    "GitHub #236 (test only): unlock the Aftermath campaign in memory (the profile is not changed); "
                    "needs gowj_unlock_aftermath_setting");
REXCVAR_DEFINE_INT32(gowj_unlock_aftermath_setting, -1, "Judgment",
                     "GitHub #236: profile setting id whose value gowj_unlock_aftermath ORs the mask into (-1 = none)");
REXCVAR_DEFINE_INT32(gowj_unlock_aftermath_mask, 0x7FFF, "Judgment",
                     "GitHub #236: bits gowj_unlock_aftermath sets (default: unlockables 0..14)");

namespace {

void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
bool g_log = false, g_unlock = false;
int32_t g_unlock_id = -1;
uint32_t g_unlock_mask = 0;

constexpr uint32_t kExecGetProfileSettingValueInt = 0x82538128;
constexpr uint32_t kGetValueIntVtableOffset = 340;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

PPCFunc* g_exec_orig = nullptr;
PPCFunc* g_get_int_orig = nullptr;
std::atomic<bool> g_inner_resolved{false};

void GetValueIntHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32, value_addr = ctx.r5.u32, lr = uint32_t(ctx.lr);
  const int32_t id = ctx.r4.s32;
  g_get_int_orig(ctx, base);
  const uint32_t found = ctx.r3.u32;
  if (!value_addr) return;
  if (g_unlock && id == g_unlock_id) {
    const uint32_t v = Load32(base, value_addr);
    if ((v & g_unlock_mask) != g_unlock_mask) {
      Store32(base, value_addr, v | g_unlock_mask);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4)
        REXLOG_INFO("gowj_unlock_aftermath: setting {} 0x{:08X} -> 0x{:08X} (in memory)", id, v, v | g_unlock_mask);
    }
  }
  if (!g_log) return;
  const uint32_t value = Load32(base, value_addr), vtable = Load32(base, self);
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> seen;
  const uint64_t key = (uint64_t(vtable ^ (lr << 8)) << 32) | uint32_t(id);
  std::lock_guard<std::mutex> lock(mutex);
  auto it = seen.find(key);
  if (it != seen.end() && it->second == value) return;
  if (seen.size() >= 4096) return;
  seen[key] = value;
  REXLOG_INFO("gowj_profile_log: get int id={} value={} (0x{:08X}) found={} object={:08X} vtable={:08X} from {:08X}", id,
              int32_t(value), value, found & 1, self, vtable, lr);
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
        REXLOG_INFO("gowj_profile_log: GetProfileSettingValueInt at {:08X} hooked (vtable {:08X})", fn, vtable);
      else
        REXLOG_WARN("gowj_profile_log: GetProfileSettingValueInt not hooked (vtable {:08X}, entry {:08X})", vtable, fn);
      g_inner_resolved.store(true, std::memory_order_release);
    }
  }
  g_exec_orig(ctx, base);
}

}  // namespace

void GowjInstallProfileLog() {
  g_log = REXCVAR_GET(gowj_profile_log);
  g_unlock = REXCVAR_GET(gowj_unlock_aftermath);
  g_unlock_id = REXCVAR_GET(gowj_unlock_aftermath_setting);
  g_unlock_mask = uint32_t(REXCVAR_GET(gowj_unlock_aftermath_mask));
  if (g_unlock && g_unlock_id < 0) {
    REXLOG_WARN("gowj_unlock_aftermath: off - set gowj_unlock_aftermath_setting (find it with gowj_profile_log)");
    g_unlock = false;
  }
  if (!g_log && !g_unlock) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  g_exec_orig = d->GetFunction(kExecGetProfileSettingValueInt);
  if (!g_exec_orig || !d->SetFunction(kExecGetProfileSettingValueInt, &ExecHook)) {
    REXLOG_WARN("gowj_profile_log: off - execGetProfileSettingValueInt not hooked");
    return;
  }
  REXLOG_INFO("gowj_profile_log: on (log={}, unlock_aftermath={} setting={} mask=0x{:X})", g_log, g_unlock, g_unlock_id, g_unlock_mask);
}
