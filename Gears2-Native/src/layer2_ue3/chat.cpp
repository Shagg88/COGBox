// Layer 2 (Unreal Engine 3): the in-match chat's engine side (ue3/chat.h). Shared: Gears 1, 3 and Judgment build it
// from GN_LAYER0_HOME with their own ue3/objects.h, as name_tags.cpp. Our code (MIT).
#include <ue3/chat.h>
#include <ue3/console.h>
#include <ue3/objects.h>
#include <xenos/chat_logic.h>
#include <xenos/chat_panel.h>   // cogbox_match_chat

#include <rex/logging/macros.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace obj = ue3::obj;

constexpr uint32_t kEntrySize = 24;      // FString Text (12), FColor (4), float MessageLife (4), PRI* (4)
constexpr uint32_t kEntryPri = 20;
constexpr uint32_t kMaxEntries = 64;     // more than the engine ever keeps: anything bigger is not the array
constexpr uint32_t kMaxTextChars = 600;

struct Offsets {
  int actor = -1, my_hud = -1, world = -1, gri = -1, messages = -1, pri = -1, player_name = -1;
  int match_begun = -1, match_over = -1;
  uint32_t begun_mask = 0, over_mask = 0;
};

Ue3ChatConfig g_config;
std::atomic<bool> g_configured{false}, g_ready{false}, g_match{false};
Offsets g_off;   // written once before g_ready
std::atomic<void (*)(const Ue3ChatReceived&)> g_sink{nullptr};

// The messages waiting to be sent:
// the engine drops a message sent within 2 s of the last one (PlayerController.AllowTextMessage - Gears 1 even when
// playing solo, the others online), so they go out one at a time, kSendGap apart, from Ue3ChatPump.
std::mutex g_sent_mutex;
std::deque<std::string> g_waiting;   // console commands, oldest first
constexpr size_t kMaxWaiting = 5;
constexpr double kSendGap = 2.1;     // seconds
double g_last_send = -1e9;           // game thread

double Seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Resolve() {
  for (int attempt = 0; attempt < 60; attempt++) {
    if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    if (!obj::Ready() && !(g_config.open_objects && g_config.open_objects())) continue;
    const uint32_t local_player = obj::FindClass("LocalPlayer"), pc = obj::FindClass("PlayerController"),
                   actor = obj::FindClass("Actor"), world = obj::FindClass("WorldInfo"), hud = obj::FindClass("HUD"),
                   gri = obj::FindClass("GameReplicationInfo"), pri = obj::FindClass("PlayerReplicationInfo");
    if (!local_player || !pc || !actor || !world || !hud || !gri || !pri) continue;
    Offsets o;
    o.actor = obj::PropertyOffset(local_player, "Actor");
    o.my_hud = obj::PropertyOffset(pc, "myHUD");
    o.pri = obj::PropertyOffset(pc, "PlayerReplicationInfo");
    o.world = obj::PropertyOffset(actor, "WorldInfo");
    o.gri = obj::PropertyOffset(world, "GRI");
    o.messages = obj::PropertyOffset(hud, "ConsoleMessages");
    o.player_name = obj::PropertyOffset(pri, "PlayerName");
    o.match_begun = obj::PropertyOffset(gri, "bMatchHasBegun");
    o.match_over = obj::PropertyOffset(gri, "bMatchIsOver");
    o.begun_mask = obj::BoolMask(gri, "bMatchHasBegun");
    o.over_mask = obj::BoolMask(gri, "bMatchIsOver");
    REXLOG_INFO("chat ({}): LocalPlayer.Actor +{}, PlayerController.myHUD +{} .PlayerReplicationInfo +{}, Actor.WorldInfo "
                "+{}, WorldInfo.GRI +{}, HUD.ConsoleMessages +{}, GRI.bMatchHasBegun +{}/{:X} .bMatchIsOver +{}/{:X}",
                g_config.game, o.actor, o.my_hud, o.pri, o.world, o.gri, o.messages, o.match_begun, o.begun_mask,
                o.match_over, o.over_mask);
    if (o.actor < 0 || o.my_hud < 0 || o.messages < 0 || o.pri < 0) {
      REXLOG_WARN("chat ({}): the HUD's message list was not found - in-match chat is off", g_config.game);
      return;
    }
    g_off = o;
    g_ready.store(true, std::memory_order_release);
    return;
  }
  REXLOG_WARN("chat ({}): the engine's classes were not found - in-match chat is off", g_config.game);
}

// The FString at `at` ({data, count, max}; UTF-16, count includes the terminator) as ASCII; false if it isn't one.
bool ReadFString(uint32_t at, std::string* out) {
  const uint32_t data = obj::Read32(at), count = obj::Read32(at + 4);
  if (!data || count < 2 || count > kMaxTextChars || (data & 1) || !obj::Readable(data, count * 2)) return false;
  out->clear();
  for (uint32_t i = 0; i + 1 < count; ++i) {
    const uint32_t a = data + 2 * i;
    const uint32_t w = obj::Read32(a & ~3u);
    const uint16_t c = (a & 2) ? uint16_t(w & 0xFFFF) : uint16_t(w >> 16);
    out->push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
  }
  return true;
}

}  // namespace

void Ue3ChatConfigure(const Ue3ChatConfig& config) {
  g_config = config;
  if (!g_configured.exchange(true)) std::thread(Resolve).detach();
}

bool Ue3ChatSend(const std::string& typed, bool team) {
  if (!g_ready.load(std::memory_order_acquire)) return false;
  std::string text = xenos::chat::CleanText(typed);
  if (text.empty()) return false;
  std::lock_guard<std::mutex> lock(g_sent_mutex);
  if (g_waiting.size() >= kMaxWaiting) {
    REXLOG_INFO("chat: {} messages already waiting to be sent - this one was not sent", kMaxWaiting);
    return false;
  }
  if (team) text = xenos::chat::MarkTeam(text);   // every player's game draws it as a team line
  // Gears 1 takes one word unless quoted (CleanText has already turned any '"' into '\'')
  if (g_config.quote_text) text = "\"" + text + "\"";
  g_waiting.push_back(std::string(team ? "TeamSay " : "Say ") + text);
  return true;
}

void Ue3ChatPump(PPCContext&, uint8_t*) {
  // the HUD's array is the game thread's (it is read and emptied here): never from any other thread
  if (!g_ready.load(std::memory_order_acquire) || !Ue3ConsoleIsGameThread()) return;
  {
    const double now = Seconds();
    std::lock_guard<std::mutex> lock(g_sent_mutex);
    if (!g_waiting.empty() && now - g_last_send >= kSendGap) {
      Ue3ConsoleQueueTrusted(g_waiting.front());   // runs on the next Ue3ConsoleRunQueued
      g_waiting.pop_front();
      g_last_send = now;
    }
  }
  const Offsets& o = g_off;
  bool match = false;
  const uint32_t engine = obj::Read32(g_config.engine_global);
  const uint32_t count = engine ? obj::Read32(engine + g_config.players_num_offset) : 0;
  const uint32_t players = engine ? obj::Read32(engine + g_config.players_data_offset) : 0;
  const uint32_t player = (int32_t(count) > 0 && players) ? obj::Read32(players) : 0;
  const uint32_t pc = player ? obj::Read32(player + uint32_t(o.actor)) : 0;
  const uint32_t hud = pc ? obj::Read32(pc + uint32_t(o.my_hud)) : 0;
  if (hud) {
    // the match: a HUD that isn't a menu / lobby one, and (where the GRI says so) begun and not over
    const std::string cls = obj::ObjectName(obj::ClassOf(hud));
    match = cls.find("Menu") == std::string::npos && cls.find("FrontEnd") == std::string::npos &&
            cls.find("Lobby") == std::string::npos;
    const uint32_t world = (match && o.world >= 0) ? obj::Read32(pc + uint32_t(o.world)) : 0;
    const uint32_t gri = (world && o.gri >= 0) ? obj::Read32(world + uint32_t(o.gri)) : 0;
    if (gri && o.match_begun >= 0 && o.begun_mask)
      match = match && (obj::Read32(gri + uint32_t(o.match_begun)) & o.begun_mask) != 0;
    if (gri && o.match_over >= 0 && o.over_mask)
      match = match && (obj::Read32(gri + uint32_t(o.match_over)) & o.over_mask) == 0;

    // Only in a match with chat on: elsewhere (menus, campaign, chat off) the engine's own messages are left alone
    const uint32_t array = hud + uint32_t(o.messages);
    const uint32_t data = (match && REXCVAR_GET(cogbox_match_chat)) ? obj::Read32(array) : 0;
    const uint32_t num = data ? obj::Read32(array + 4) : 0;
    if (data && num > 0 && num <= kMaxEntries && obj::Readable(data, num * kEntrySize)) {
      auto* sink = g_sink.load(std::memory_order_acquire);
      for (uint32_t i = 0; i < num; ++i) {
        const uint32_t entry = data + i * kEntrySize;
        // A player's message is "<that PRI's PlayerName>: text". Anything else is the engine's own text (the menus'
        // "Get flag from registry" has a PRI too), and is dropped.
        const uint32_t pri = obj::Read32(entry + kEntryPri);
        Ue3ChatReceived line;
        std::string name;
        if (!pri || o.player_name < 0 || !ReadFString(pri + uint32_t(o.player_name), &name) || name.empty()) continue;
        if (!ReadFString(entry, &line.raw) || line.raw.compare(0, name.size() + 2, name + ": ") != 0) continue;
        line.name_len = name.size();
        if (sink) sink(line);
        // Taken: its text is made "" in place (count 1, first character 0), so the engine draws nothing for it and
        // the next pump skips it (ReadFString wants count >= 2). The entry and its text buffer stay the engine's: it
        // removes them itself (oldest first / when they expire) and frees the buffer with its own allocator, so
        // nothing leaks (the earlier version emptied the array and lost one buffer per line).
        const uint32_t text = obj::Read32(entry);
        obj::Write32(text & ~3u, (text & 2) ? (obj::Read32(text & ~3u) & 0xFFFF0000u) : (obj::Read32(text & ~3u) & 0x0000FFFFu));
        obj::Write32(entry + 4, 1);
      }
    }
  }
  if (!match && g_match.load(std::memory_order_relaxed)) {   // the match ended: what was still waiting is not sent
    std::lock_guard<std::mutex> lock(g_sent_mutex);
    g_waiting.clear();
  }
  g_match.store(match, std::memory_order_relaxed);
}

bool Ue3ChatMatchRunning() { return g_match.load(std::memory_order_relaxed); }

void Ue3ChatSetSink(void (*sink)(const Ue3ChatReceived& line)) { g_sink.store(sink, std::memory_order_release); }
