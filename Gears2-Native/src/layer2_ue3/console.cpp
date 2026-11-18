// Layer 2 (Unreal Engine 3): engine console commands from the PC side (ue3/console.h, notes/25). Shared: Gears 3 builds
// this file from here (GN_LAYER0_HOME), Gears 2 for --gow2_cheats (GitHub #264), Judgment for --gowj_cheats, Gears 1 for --gow1_cheats. Our code (MIT).
#include <ue3/console.h>
#include <ue3/auto_cheats.h>   // GitHub #359: the cheat key notice reads the cheats' state back

#include <xenos/cheat_popup.h>  // GitHub #359

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// The settings group the options are saved under: Gears 3's own as before (its saved hotkeys stay where they are).
#if defined(GN_UE3_CONSOLE_GEARS2) && GN_UE3_CONSOLE_GEARS2
#define GN_UE3_CONSOLE_GROUP "Gears2"
#elif defined(GN_UE3_CONSOLE_JUDGMENT) && GN_UE3_CONSOLE_JUDGMENT
#define GN_UE3_CONSOLE_GROUP "Judgment"
#elif defined(GN_UE3_CONSOLE_GEARS1) && GN_UE3_CONSOLE_GEARS1
#define GN_UE3_CONSOLE_GROUP "Gears1"
#else
#define GN_UE3_CONSOLE_GROUP "Gears3"
#endif

REXCVAR_DEFINE_STRING(ue3_hotkeys, "", GN_UE3_CONSOLE_GROUP,
                      "Keys that run engine console commands, 'Key=command;Key=cmd A|cmd B' ('|': the key cycles), e.g. "
                      "'F5=God;F6=Slomo 0.25|Slomo 1' (ue3/console.h). Keys can be moved in the SDK settings (bind_ue3_*)");
REXCVAR_DEFINE_STRING(ue3_exec, "", GN_UE3_CONSOLE_GROUP,
                      "Engine console commands to run once a player exists, ';'-separated (also from the SDK console: "
                      "ue3_exec God)");
REXCVAR_DEFINE_STRING(ue3_exec_at, "", GN_UE3_CONSOLE_GROUP,
                      "Scripted console commands, '<game tick>:<command>,...' or '<seconds>s:<command>,...' counted from "
                      "the first engine tick (tests); '@F9' presses that cheat key (GitHub #359)");
// GitHub #342 (next update): the cheats the player picks in the launcher, simply on - no keys. Read by ue3/auto_cheats.h
// (each game's cheats.cpp), which applies them whenever a player (or a new level's player) exists, never in Versus.
REXCVAR_DEFINE_BOOL(ue3_cheat_god, false, GN_UE3_CONSOLE_GROUP,
                    "GitHub #342: god mode, on whenever a player exists (campaign and wave modes; never in Versus)");
REXCVAR_DEFINE_BOOL(ue3_cheat_weapons, false, GN_UE3_CONSOLE_GROUP,
                    "GitHub #342: all weapons and full ammo (AllWeapons, AllAmmo) each time the player gets a new body "
                    "(level start, checkpoint, respawn); infinite ammo where the game has it");
REXCVAR_DEFINE_DOUBLE(ue3_cheat_speed, 1.0, GN_UE3_CONSOLE_GROUP,
                      "GitHub #342: game speed (1 = normal, 0.5 = half speed slow motion; 0.1-1), set whenever the game "
                      "runs at its normal speed (the game's own slow-motion moments are left alone)");
REXCVAR_DEFINE_BOOL(ue3_cheat_hide_hud, false, GN_UE3_CONSOLE_GROUP,
                    "GitHub #342: the HUD hidden whenever a player exists");

namespace {
std::mutex g_mutex;
// GitHub #359: `key` is the cheat key that queued the command (empty: --ue3_exec, scripted, the launcher, COGBox's own) -
// only those get the on-screen notice.
// GitHub #353: a COGBox command (Ue3ConsoleQueueTrusted): no gate, no EnableCheats / before_each, no notice, logged at
// debug level; `after` runs on the game thread straight after it, in the same frame.
struct Queued {
  std::string command, key;
  bool trusted = false;
  std::function<void()> after;
};
std::deque<Queued> g_queue;
// GitHub #359 (tests): each cheat key's press, by key name, so --ue3_exec_at="30s:@F9" presses F9 as the player would.
std::map<std::string, std::function<void()>> g_key_press;
Ue3ConsoleTarget g_target;
std::atomic<bool> g_have_target{false};
std::thread::id g_game_thread;
uint64_t g_ticks = 0;
std::multimap<uint64_t, std::string> g_scripted;     // tick -> command
std::multimap<double, std::string> g_scripted_time;  // seconds -> command
std::chrono::steady_clock::time_point g_start;

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream in(s);
  std::string item;
  while (std::getline(in, item, sep)) {
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}

}  // namespace

// The commands the launcher itself writes into --ue3_exec / --ue3_exec_each_level for display and corpse settings (cogbox
// gears_settings.cpp; Gears 3's split-screen layout, game_patches.cpp): `set <class> <property> <value>` on exactly these
// properties. They give no advantage, so a Versus match keeps them; every other command is a cheat to the gate.
bool Ue3ConsoleIsDisplaySetting(const std::string& command) {
  static const char* const kPrefixes[] = {
      "set GameViewportClient DesiredSplitscreenType ",
      "set GearPawn CorpseMaxLifeSpan ",
      "set GearPawn DurationBeforeDestroyingDueToNotBeingSeen ",
      "set WarPawn MaxAllowedDeadRagDollPawns ",
      "set WarPawn DurationBeforeDestroyingDueToNotBeingSeen ",
  };
  for (const char* prefix : kPrefixes) {
    const size_t n = std::strlen(prefix);
    if (command.size() > n && command.compare(0, n, prefix) == 0 && command.find_first_of(";|\r\n", n) == std::string::npos)
      return true;
  }
  return false;
}

namespace {
// Runs one command through GamePlayers(index)'s FExec::Exec; false if the engine isn't ready (no such player yet).
bool ExecNow(PPCContext& ctx, uint8_t* base, const std::string& command, bool* handled, int index = 0) {
  const Ue3ConsoleTarget& t = g_target;
  const uint32_t engine = Load32(base, t.engine_global);
  if (!engine) return false;
  const uint32_t count = Load32(base, engine + t.players_num_offset), data = Load32(base, engine + t.players_data_offset);
  if (int32_t(count) <= index || index < 0 || !data) return false;
  const uint32_t player = Load32(base, data + 4u * uint32_t(index));
  if (!player) return false;
  const uint32_t self = player + t.exec_offset;
  const uint32_t vtable = Load32(base, self);
  if (!vtable) return false;
  const uint32_t fn = Load32(base, vtable + 4 * t.exec_vtable_slot);
  PPCFunc* const func = fn ? rex::runtime::ResolveIndirectFunction(fn) : nullptr;
  if (!func) return false;
  const uint32_t output = Load32(base, t.log_global);
  // A frame below the caller's: back chain, then the command as a big-endian UTF-16 string.
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 0x600) & ~0xFu;
  Store32(base, sp, ctx.r1.u32);
  const uint32_t text = sp + 0x100;
  const size_t n = std::min<size_t>(command.size(), 0x4F0 / 2 - 1);
  for (size_t i = 0; i < n; ++i) {
    base[text + 2 * i] = 0;
    base[text + 2 * i + 1] = uint8_t(command[i]);
  }
  base[text + 2 * n] = base[text + 2 * n + 1] = 0;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = self;
  ctx.r4.u64 = text;
  ctx.r5.u64 = output;
  func(ctx, base);
  *handled = ctx.r3.u32 != 0;  // UBOOL Exec(...)
  ctx = saved;
  return true;
}
}  // namespace

bool Ue3ConsoleIsGameThread() {
  return g_game_thread != std::thread::id() && std::this_thread::get_id() == g_game_thread;
}

bool Ue3ConsoleExecNow(PPCContext& ctx, uint8_t* base, int player, const std::string& command, bool* handled) {
  bool ignored = false;
  if (!g_have_target) return false;
  return ExecNow(ctx, base, command, handled ? handled : &ignored, player);
}

void Ue3ConsoleSetTarget(const Ue3ConsoleTarget& target) {
  g_target = target;
  g_have_target = target.engine_global != 0 && target.log_global != 0;
  for (const std::string& item : Split(REXCVAR_GET(ue3_exec_at), ',')) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    const std::string when = item.substr(0, colon);
    if (!when.empty() && when.back() == 's') g_scripted_time.emplace(std::strtod(when.c_str(), nullptr), item.substr(colon + 1));
    else g_scripted.emplace(std::strtoull(when.c_str(), nullptr, 10), item.substr(colon + 1));
  }
}

namespace {
void QueueFrom(const std::string& command, const std::string& key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back(Queued{command, key, false, {}});
}
}  // namespace

void Ue3ConsoleQueueTrusted(const std::string& command, std::function<void()> after) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back(Queued{command, std::string(), true, std::move(after)});
}

namespace {

// GitHub #359: the notice for a cheat key's command, from what the command did (outcome: 0 done, 1 not recognised, 2
// refused) and the cheat's state read back now (ue3/auto_cheats.h DescribeKey). One log line per notice, also with the
// notices turned off, so a test can check every key.
void NoteKey(const std::string& key, const std::string& command, int outcome) {
  const ue3::cheats::KeyNotice n = ue3::cheats::DescribeKey(command, outcome);
  const std::string shown = n.state.empty() ? n.label : n.tone == 3 ? n.label + ": " + n.state : n.label + " " + n.state;
  REXLOG_INFO("cheat_popup: {} {} {} -> {} (state read: {}){}", GN_UE3_CONSOLE_GROUP, key, command, shown, n.read,
              REXCVAR_GET(cogbox_cheat_popups) ? "" : " [popups off]");
  // review r359: a key bound to a command that is no cheat (--ue3_hotkeys can bind anything, e.g. "stat fps") is
  // logged but gets no popup - the notice is for cheats (and for a cheat key refused in Versus)
  if (n.icon == 0 && outcome != 2) return;
  static constexpr XenosCheatTone kTones[] = {XenosCheatTone::On, XenosCheatTone::Off, XenosCheatTone::Value,
                                              XenosCheatTone::Refused};
  XenosCheatPopupPush(n.label, n.state, kTones[n.tone < 0 || n.tone > 3 ? 2 : n.tone], XenosCheatIcon(n.icon));
}
}  // namespace

void Ue3ConsoleQueue(const std::string& command) { QueueFrom(command, std::string()); }

void Ue3ConsoleInstallHotkeys(const std::string& defaults) {
  std::string spec = REXCVAR_GET(ue3_hotkeys);
  if (spec.empty()) spec = defaults;
  int n = 0;
  for (const std::string& item : Split(spec, ';')) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = item.substr(0, eq);
    auto commands = std::make_shared<std::vector<std::string>>(Split(item.substr(eq + 1), '|'));
    if (commands->empty()) continue;
    if (rex::ui::ParseVirtualKey(key) == rex::ui::VirtualKey::kNone) {
      REXLOG_WARN("ue3_hotkeys: unknown key '{}'", key);
      continue;
    }
    auto next = std::make_shared<size_t>(0);
    const std::string desc = "Engine command: " + item.substr(eq + 1);
    auto press = [commands, next, key] {
      QueueFrom((*commands)[*next], key);   // GitHub #359: from a key - it gets the notice
      *next = (*next + 1) % commands->size();
    };
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_key_press[key] = press;
    }
    rex::ui::RegisterBind("bind_ue3_" + std::to_string(++n), key, desc, press);
    REXLOG_INFO("ue3_hotkeys: {} -> {}", key, item.substr(eq + 1));
  }
  // GitHub #359: the notice reads the cheats' state through the engine's classes - look them up now, so the first key
  // press already finds them (each game configures ue3/auto_cheats.h before installing its keys)
  if (n) ue3::cheats::Prepare();
}

void Ue3ConsoleRunQueued(PPCContext& ctx, uint8_t* base) {
  if (!g_have_target) return;
  // The first thread to get here is the engine loop's; others (if the hooked function is ever called elsewhere) skip.
  if (g_game_thread == std::thread::id()) {
    g_game_thread = std::this_thread::get_id();
    g_start = std::chrono::steady_clock::now();
  }
  if (std::this_thread::get_id() != g_game_thread) return;
  ++g_ticks;
  const std::string pending = REXCVAR_GET(ue3_exec);
  if (!pending.empty()) {
    rex::cvar::SetFlagByName("ue3_exec", "");
    for (const std::string& c : Split(pending, ';')) Ue3ConsoleQueue(c);
  }
  while (!g_scripted.empty() && g_scripted.begin()->first <= g_ticks) {
    Ue3ConsoleQueue(g_scripted.begin()->second);
    g_scripted.erase(g_scripted.begin());
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  while (!g_scripted_time.empty() && g_scripted_time.begin()->first <= secs) {
    Ue3ConsoleQueue(g_scripted_time.begin()->second);
    g_scripted_time.erase(g_scripted_time.begin());
  }
  for (;;) {
    std::string command, key;
    bool trusted = false;
    std::function<void()> after;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_queue.empty()) return;
      command = g_queue.front().command;
      key = g_queue.front().key;
      trusted = g_queue.front().trusted;
      after = g_queue.front().after;
    }
    if (command.size() > 1 && command[0] == '@') {   // GitHub #359 (tests): "@F9" = a press of the cheat key F9
      std::function<void()> press;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.pop_front();
        const auto it = g_key_press.find(command.substr(1));
        if (it != g_key_press.end()) press = it->second;
      }
      if (press) press();   // queues that key's next command, run below in this same loop
      else REXLOG_WARN("ue3_console: '{}': no such cheat key", command);
      continue;
    }
    bool handled = false;
    // 0.6.3: cheats are refused in Versus matches (g_target.gate = ue3/match_guard.h, asked for every command at the moment
    // it would run, so a command held over a map change meets the new map's answer). The launcher's own settings are not cheats.
    if (g_target.gate && !trusted && !Ue3ConsoleIsDisplaySetting(command)) {
      const int verdict = g_target.gate();
      if (verdict == 1) return;  // the game state is not readable yet: keep it queued
      if (verdict == 2) {
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          g_queue.pop_front();
        }
        REXLOG_DEBUG("ue3_console: tick {} ({:.1f} s): '{}' refused", g_ticks, secs, command);
        if (!key.empty()) NoteKey(key, command, 2);   // GitHub #359: "Cheats are off in Versus"
        continue;
      }
    }
    if (trusted) {
      // GitHub #353: COGBox's own commands (name tags) are no cheats - no EnableCheats (it flips NetMode), no before_each
    } else if (g_target.enable_cheats) {   // GitHub #342: Gears 2's EnableCheats (ue3/auto_cheats.h)
      if (!g_target.enable_cheats(ctx, base)) return;  // no player yet: keep it queued
    } else if (g_target.before_each && *g_target.before_each) {
      bool ignored = false;
      if (!ExecNow(ctx, base, g_target.before_each, &ignored)) return;  // no player yet: keep it queued
    }
    if (!ExecNow(ctx, base, command, &handled)) return;  // no player yet: keep it queued for a later tick
    // GitHub #353: same thread, same frame; review r366 (4): only when the engine ran the command (a refused `set` left
    // the old buffers - Gears 1 / 2's write-back must not follow it)
    if (after && handled) after();
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_queue.pop_front();
    }
    if (trusted)
      REXLOG_DEBUG("ue3_console: tick {}: COGBox '{}' {}", g_ticks, command, handled ? "done" : "not recognised");
    else
      REXLOG_INFO("ue3_console: tick {} ({:.1f} s): '{}' {}", g_ticks, secs, command,
                  handled ? "done" : "not recognised by the engine");
    if (!key.empty()) NoteKey(key, command, handled ? 0 : 1);   // GitHub #359 (a trusted command has no key)
  }
}
