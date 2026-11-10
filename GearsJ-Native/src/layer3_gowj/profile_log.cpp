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
//
// GitHub #174 / gj100: --gowj_profile_100 (test only, off by default) writes the values listed in
// --gowj_profile_100_file ("id=value" lines, decimal or 0x hex, '#' comments; ids from the gj100-profile-ids job's
// GearGame ProfileMappings dump) into the profile settings array of the object the getter reads, so the game's next
// profile save writes a real 100% profile itself (its own encrypted title-settings format). The array is found on the
// first read that hits a listed id: a TArray {ptr, count, max} in the object whose elements (stride 12..32) hold that
// id and the value the getter returned; every write is logged. Values are re-applied when the array moves.
#include "profile_log.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(gowj_profile_log, false, "Judgment",
                    "GitHub #174: log each profile setting the game's script reads (id, value, object vtable), once per "
                    "change - to find the setting that unlocks Insane. Read-only");

// The launcher looks for this description's text in the exe before it passes the option.
REXCVAR_DEFINE_BOOL(gowj_unlock_insane, false, "Judgment",
                    "insane050: Unlock Insane difficulty from the start (the profile is not changed)");

REXCVAR_DEFINE_BOOL(gowj_unlock_aftermath, false, "Judgment",
                    "GitHub #236 (test only): unlock the Aftermath campaign in memory (the profile is not changed); "
                    "needs gowj_unlock_aftermath_setting");
REXCVAR_DEFINE_INT32(gowj_unlock_aftermath_setting, -1, "Judgment",
                     "GitHub #236: profile setting id whose value gowj_unlock_aftermath ORs the mask into (-1 = none)");
REXCVAR_DEFINE_INT32(gowj_unlock_aftermath_mask, 0x7FFF, "Judgment",
                     "GitHub #236: bits gowj_unlock_aftermath sets (default: unlockables 0..14)");

REXCVAR_DEFINE_BOOL(gowj_profile_100, false, "Judgment",
                    "gj100 (test only): write the profile setting values in gowj_profile_100_file into the in-memory "
                    "profile, so the game saves them (100% campaign test profile). Changes the profile when it saves");
REXCVAR_DEFINE_STRING(gowj_profile_100_file, "data/profile_100.txt", "Judgment",
                      "gj100: text file of id=value lines for gowj_profile_100");

namespace {

void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
bool g_log = false, g_unlock = false, g_insane = false;
// insane050: Judgment's Coalesced: [InsaneDifficulty GearUnlockableResourceProvider] UnlockableId=0 ("Complete the game
// on any difficulty to unlock it"); GearProfileSettings ProfileMappings Id=82 Name=UnlockedUnlockables (job gj100), the
// same as Gears 3 (unlocks.cpp). Only what the menus read is changed; nothing is written to the profile.
constexpr int32_t kUnlockablesSetting = 82;
constexpr uint32_t kInsaneUnlockBit = 1u << 0;
int32_t g_unlock_id = -1;
uint32_t g_unlock_mask = 0;

constexpr uint32_t kExecGetProfileSettingValueInt = 0x82538128;
constexpr uint32_t kGetValueIntVtableOffset = 340;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool g_p100 = false;
std::vector<std::pair<int32_t, uint32_t>> g_p100_values;
struct P100Layout { uint32_t field = 0, stride = 0, id_off = 0, val_off = 0; bool ok = false, failed = false; };
P100Layout g_lay;
uint32_t g_p100_last_ptr = 0, g_p100_last_count = 0;

bool GuestPtr(uint32_t p) { return p >= 0x40000000u && p < 0xA0000000u; }

// Finds the settings TArray from a read of `id` that returned `value`; then writes every listed value.
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
              if (Load32(base, ptr + i * st + vo) == value) g_lay = {f, st, io, vo, true, false};
          }
        }
    }
    if (!g_lay.ok) {
      g_lay.failed = true;
      REXLOG_WARN("gowj_profile_100: settings array not found in object {:08X} (id {} value 0x{:X})", self, id, value);
      return;
    }
    REXLOG_INFO("gowj_profile_100: settings array at object+0x{:X}, stride {}, id +{}, value +{}", g_lay.field,
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
      REXLOG_INFO("gowj_profile_100: id {} 0x{:08X} -> 0x{:08X}", want_id, old, want);
      hit = true, ++written;
      break;
    }
    if (!hit) {
      ++missing;
      REXLOG_WARN("gowj_profile_100: id {} not in the settings array", want_id);
    }
  }
  REXLOG_INFO("gowj_profile_100: {} written, {} missing (array {:08X} x{}); saved with the next profile save", written,
              missing, ptr, cnt);
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
  if (g_p100 && (found & 1)) {
    static std::mutex p100_mutex;
    std::lock_guard<std::mutex> lock(p100_mutex);
    for (auto& [want_id, want] : g_p100_values)
      if (want_id == id) { Profile100Apply(base, self, id, Load32(base, value_addr)); break; }
    for (auto& [want_id, want] : g_p100_values)
      if (want_id == id) { Store32(base, value_addr, want); break; }
  }
  if (g_insane && id == kUnlockablesSetting) {
    const uint32_t v = Load32(base, value_addr);
    if (!(v & kInsaneUnlockBit)) {
      Store32(base, value_addr, v | kInsaneUnlockBit);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4)
        REXLOG_INFO("gowj_unlock_insane: Insane unlocked for the menus (unlockables 0x{:08X} -> 0x{:08X}, object vtable "
                    "{:08X}; in memory only)", v, v | kInsaneUnlockBit, Load32(base, self));
    }
  }
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
  g_insane = REXCVAR_GET(gowj_unlock_insane);
  g_unlock_id = REXCVAR_GET(gowj_unlock_aftermath_setting);
  g_unlock_mask = uint32_t(REXCVAR_GET(gowj_unlock_aftermath_mask));
  if (g_unlock && g_unlock_id < 0) {
    REXLOG_WARN("gowj_unlock_aftermath: off - set gowj_unlock_aftermath_setting (find it with gowj_profile_log)");
    g_unlock = false;
  }
  g_p100 = REXCVAR_GET(gowj_profile_100);
  if (g_p100) {
    const std::string path = REXCVAR_GET(gowj_profile_100_file);
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
      line = line.substr(0, line.find('#'));
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      try {
        g_p100_values.emplace_back(int32_t(std::stol(line.substr(0, eq), nullptr, 0)),
                                   uint32_t(std::stoul(line.substr(eq + 1), nullptr, 0)));
      } catch (...) {
      }
    }
    if (g_p100_values.empty()) {
      REXLOG_WARN("gowj_profile_100: off - no id=value lines in gowj_profile_100_file");
      g_p100 = false;
    }
  }
  if (!g_log && !g_unlock && !g_p100 && !g_insane) return;
  rex::Runtime* rt = rex::Runtime::instance();
  auto* d = rt ? rt->function_dispatcher() : nullptr;
  if (!d) return;
  g_exec_orig = d->GetFunction(kExecGetProfileSettingValueInt);
  if (!g_exec_orig || !d->SetFunction(kExecGetProfileSettingValueInt, &ExecHook)) {
    REXLOG_WARN("gowj_profile_log: off - execGetProfileSettingValueInt not hooked");
    return;
  }
  REXLOG_INFO("gowj_profile_log: on (log={}, unlock_insane={}, unlock_aftermath={} setting={} mask=0x{:X}, profile_100={} "
              "values)", g_log, g_insane, g_unlock, g_unlock_id, g_unlock_mask, g_p100 ? g_p100_values.size() : 0);
}
