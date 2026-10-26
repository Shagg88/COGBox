// Layer 2 (Unreal Engine 3): engine console commands from the PC side (ue3/console.h, notes/25). Our code (MIT).
#include <ue3/console.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(ue3_hotkeys, "", "Gears3",
                      "Keys that run engine console commands, 'Key=command;Key=cmd A|cmd B' ('|': the key cycles), e.g. "
                      "'F5=God;F6=Slomo 0.25|Slomo 1' (ue3/console.h). Keys can be moved in the SDK settings (bind_ue3_*)");
REXCVAR_DEFINE_STRING(ue3_exec, "", "Gears3",
                      "Engine console commands to run once a player exists, ';'-separated (also from the SDK console: "
                      "ue3_exec God)");
REXCVAR_DEFINE_STRING(ue3_exec_at, "", "Gears3",
                      "Scripted console commands, '<game tick>:<command>,...' or '<seconds>s:<command>,...' counted from "
                      "the first engine tick (tests)");

namespace {
std::mutex g_mutex;
std::deque<std::string> g_queue;
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

// Runs one command through GamePlayers(0)'s FExec::Exec; false if the engine isn't ready (no player yet).
bool ExecNow(PPCContext& ctx, uint8_t* base, const std::string& command, bool* handled) {
  const Ue3ConsoleTarget& t = g_target;
  const uint32_t engine = Load32(base, t.engine_global);
  if (!engine) return false;
  const uint32_t count = Load32(base, engine + t.players_num_offset), data = Load32(base, engine + t.players_data_offset);
  if (int32_t(count) <= 0 || !data) return false;
  const uint32_t player = Load32(base, data);
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

void Ue3ConsoleQueue(const std::string& command) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back(command);
}

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
    rex::ui::RegisterBind("bind_ue3_" + std::to_string(++n), key, desc, [commands, next] {
      Ue3ConsoleQueue((*commands)[*next]);
      *next = (*next + 1) % commands->size();
    });
    REXLOG_INFO("ue3_hotkeys: {} -> {}", key, item.substr(eq + 1));
  }
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
    std::string command;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_queue.empty()) return;
      command = g_queue.front();
    }
    bool handled = false;
    if (!ExecNow(ctx, base, command, &handled)) return;  // no player yet: keep it queued for a later tick
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_queue.pop_front();
    }
    REXLOG_INFO("ue3_console: tick {} ({:.1f} s): '{}' {}", g_ticks, secs, command,
                handled ? "done" : "not recognised by the engine");
  }
}
