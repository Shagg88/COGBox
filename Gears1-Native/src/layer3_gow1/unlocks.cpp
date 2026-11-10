// Layer 3 (Gears of War, 2006): insane050 - Insane without finishing the campaign first (--gow1_unlock_insane), and the
// research switch --gow1_profile_log (read-only).
// Job 1512: opening Gears 1's difficulty screen reads no profile setting; job 1515 (the native table in the image dump):
// the screen's kismet action is UIAction_IsInsaneUnlocked and WarProfileSettings has the native
// execHasCompletedGame (function 0x821D9918, table entry 0x82F9DB50 {name 0x820A1E90, function}). The unlock hooks that
// exec: after the original ran, its UBOOL result (r5) reads true. In memory only - the profile isn't changed, and the
// game's own achievement code (UpdateAchievementStat*, SeqAct_ChapterComplete) doesn't go through this query.
// Gears 1's profile-setting getters (sub_82376F10 / sub_82376FC0: the same walk as Gears 2's sub_825E8540 and Gears 3's
// getter - per-owner arrays of 20-byte settings, type 1 = int; the two differ in the owner flag they accept) were matched
// by code from Gears 3 TU6 / Judgment (jobs 1509 / 1510). Unlike Gears 2 / 3 / Judgment, Gears 1's Coalesced has no
// unlockable list (its three UIDataProvider_WarfareDifficulty rows are all Filter=-1), so the setting that holds the
// unlock isn't known yet: --gow1_profile_log logs every int read (id, value, which getter, caller) once per change -
// one visit to the campaign difficulty screen names it. --gow1_unlock_insane then ORs --gow1_unlock_insane_mask into
// setting --gow1_unlock_insane_setting as well (research; -1 = none).
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

// The launcher looks for this description's text in the exe before it passes the option.
REXCVAR_DEFINE_BOOL(gow1_unlock_insane, false, "Gears1",
                    "insane050: Unlock Insane difficulty from the start (in memory only - COGBox writes nothing to the profile)");
REXCVAR_DEFINE_INT32(gow1_unlock_insane_setting, -1, "Gears1",
                     "insane050: the profile setting id that holds Gears 1's Insane unlock (-1 = not known yet)");
REXCVAR_DEFINE_INT32(gow1_unlock_insane_mask, 1, "Gears1", "insane050: the bits gow1_unlock_insane sets in that setting");
REXCVAR_DEFINE_BOOL(gow1_profile_log, false, "Gears1",
                    "insane050 research: log each profile setting the game reads (id, value, getter, caller) once per "
                    "change - open the campaign difficulty screen to find the Insane unlock. Read-only");

namespace {
constexpr uint32_t kGetters[2] = {0x82376F10u, 0x82376FC0u};
// 0.5.0 (the fix): UIAction_IsInsaneUnlocked::Activated (vtable 0x82092508 slot 67). It asks the profile's
// HasCompletedAct(act, Hardcore) / (act, Casual) for acts 1-5 and fires OutputLinks[1] ("locked") unless all five
// are done, else OutputLinks[0] ("unlocked") - execHasCompletedGame is never called, which is why hooking that did
// nothing (job 1519). The hook runs the parent's Activated (0x8234DDC8) as the original does, then fires output 0.
constexpr uint32_t kIsInsaneUnlockedActivated = 0x82197860u;
constexpr uint32_t kSeqActActivated = 0x8234DDC8u;
constexpr uint32_t kIsInsaneUnlockedVtable = 0x82092508u;
PPCFunc* g_parent_activated = nullptr;
PPCFunc* g_orig_activated = nullptr;   // 0.5.0 review: the game's own IsInsaneUnlocked::Activated
PPCFunc* g_orig[2] = {nullptr, nullptr};
bool g_log = false, g_unlock = false;
int32_t g_id = -1;
uint32_t g_mask = 1;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

template <int N>
void Hook(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ctx.r4.s32;
  const uint32_t value_addr = ctx.r5.u32, self = ctx.r3.u32, lr = uint32_t(ctx.lr);
  g_orig[N](ctx, base);
  const bool found = (ctx.r3.u32 & 1) != 0;
  if (!value_addr) return;
  if (g_unlock && id == g_id && found) {
    const uint32_t v = Load32(base, value_addr);
    if ((v & g_mask) != g_mask) {
      Store32(base, value_addr, v | g_mask);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4)
        REXLOG_INFO("gow1_unlock_insane: setting {} 0x{:08X} -> 0x{:08X} for the menus (in memory only)", id, v, v | g_mask);
    }
  }
  if (!g_log) return;
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> seen;
  const uint32_t value = Load32(base, value_addr);
  const uint64_t key = (uint64_t(lr ^ (uint32_t(N) << 31)) << 32) | uint32_t(id);
  std::lock_guard<std::mutex> lock(mutex);
  auto it = seen.find(key);
  if (it != seen.end() && it->second == value) return;
  if (seen.size() >= 4096) return;
  seen[key] = value;
  REXLOG_INFO("gow1_profile_log: getter {} id={} value={} (0x{:08X}) found={} object={:08X} from {:08X}", N, id,
              int32_t(value), value, found, self, lr);
}
void IsInsaneUnlockedHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  // 0.5.0 review: anything that isn't exactly the case we handle (another class on this function, no output link,
  // the "unlocked" link disabled) runs the game's own function untouched, so the screen never gets no answer
  const uint32_t links = self ? Load32(base, self + 0x94) : 0;   // OutputLinks data; [0] = "unlocked"
  if (!self || Load32(base, self) != kIsInsaneUnlockedVtable || !links || int32_t(Load32(base, self + 0x98)) < 1 ||
      (Load32(base, links + 0x18) & 0x40000000u)) {
    g_orig_activated(ctx, base);
    return;
  }
  g_parent_activated(ctx, base);   // what the original does first (r3 = this)
  Store32(base, links + 0x18, Load32(base, links + 0x18) | 0x80000000u);   // bHasImpulse on "unlocked"
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("gow1_unlock_insane: the difficulty screen asked - Insane unlocked (in memory only)");
}
}  // namespace

void Gow1InstallUnlocks() {
  g_log = REXCVAR_GET(gow1_profile_log);
  const bool insane = REXCVAR_GET(gow1_unlock_insane);
  g_id = REXCVAR_GET(gow1_unlock_insane_setting);
  g_mask = uint32_t(REXCVAR_GET(gow1_unlock_insane_mask));
  g_unlock = insane && g_id >= 0;   // research only: an extra profile setting to OR in
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  if (insane) {   // 0.5.0: the difficulty screen's own check (execHasCompletedGame isn't on its path - job 1519)
    if (!(g_parent_activated = d->GetFunction(kSeqActActivated)) || !(g_orig_activated = d->GetFunction(kIsInsaneUnlockedActivated)) ||
        !d->SetFunction(kIsInsaneUnlockedActivated, &IsInsaneUnlockedHook)) {
      REXLOG_WARN("gow1_unlock_insane: off - UIAction_IsInsaneUnlocked not hooked");
    } else {
      REXLOG_INFO("gow1_unlock_insane: on (UIAction_IsInsaneUnlocked {:08X})", kIsInsaneUnlockedActivated);
    }
  }
  if (!g_log && !g_unlock) return;
  g_orig[0] = d->GetFunction(kGetters[0]);
  g_orig[1] = d->GetFunction(kGetters[1]);
  const bool ok = g_orig[0] && g_orig[1] && d->SetFunction(kGetters[0], &Hook<0>) && d->SetFunction(kGetters[1], &Hook<1>);
  if (!ok) { REXLOG_WARN("gow1_unlock_insane: off - the profile getters aren't there"); return; }
  REXLOG_INFO("gow1_profile_log: getters hooked (log={}, extra setting {} mask=0x{:X})", g_log, g_unlock ? g_id : -1, g_mask);
}
