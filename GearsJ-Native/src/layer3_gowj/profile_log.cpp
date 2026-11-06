// Layer 3 (Judgment): GitHub #174 groundwork - logs every profile setting the game's script reads (--gowj_profile_log,
// off by default), so one run that opens the campaign difficulty screen shows which setting / bit holds "Insane
// unlocked" (Gears 3: GearProfileSettings setting 82, bit 0 - Flippage's gears-of-war-3-recomp 26d6448).
//
// UOnlinePlayerStorage::execGetProfileSettingValueInt is sub_82538128 here: matched from Gears 3's disc 0x8251A2B0 by
// its code in our generated sources (the same 61 instructions apart from the globals' addresses, UObject's vtable
// slots 4 bytes later: 192 -> 196, the getter 336 -> 340; its five sibling exec functions map in the same order with
// the same shift). The first call resolves the C++ getter from the object's vtable (+340) and hooks that; each read
// is logged once per (object class vtable, id, value). Read-only: nothing is changed.
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

namespace {

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
  const uint32_t value = Load32(base, value_addr), vtable = Load32(base, self);
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> seen;
  const uint64_t key = (uint64_t(vtable) << 32) | uint32_t(id);
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
  if (!REXCVAR_GET(gowj_profile_log)) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  g_exec_orig = d->GetFunction(kExecGetProfileSettingValueInt);
  if (!g_exec_orig || !d->SetFunction(kExecGetProfileSettingValueInt, &ExecHook)) {
    REXLOG_WARN("gowj_profile_log: off - execGetProfileSettingValueInt not hooked");
    return;
  }
  REXLOG_INFO("gowj_profile_log: on (profile setting reads are logged)");
}
