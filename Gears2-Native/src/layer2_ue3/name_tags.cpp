// GitHub #353: see ue3/name_tags.h.
#include <ue3/console.h>
#include <ue3/name_tags.h>
#include <ue3/objects.h>
#include <xenos/player_tags.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(cogbox_name_tags, true, "COGBox",
                    "GitHub #353: show each online player's COGBox tag (#1234) after their name in lobbies and on the "
                    "scoreboard");
REXCVAR_DEFINE_STRING(cogbox_name_tags_style, "", "COGBox",
                      "GitHub #353: markup around the tag as '<before>|<after>' in place of the game's own (empty: the "
                      "game's; testing which markup a game's text fields render)");

namespace {
namespace obj = ue3::obj;

// PlayerName (FString: data, count, max; big-endian UTF-16, count with the terminator) as ASCII. `plain` = false when
// it holds anything else, so such a name is never rewritten (the set command carries the text as typed).
std::string ReadName(uint32_t at, bool* plain) {
  const uint32_t data = obj::Read32(at), num = obj::Read32(at + 4);
  std::string out;
  *plain = true;
  if (!data || num < 2 || num > 200) return out;
  for (uint32_t i = 0; i + 1 < num; ++i) {
    const uint32_t c = (obj::Read32(data + 2 * i) >> 16) & 0xFFFF;
    if (c < 0x20 || c > 0x7E) *plain = false;
    out += (c >= 0x20 && c <= 0x7E) ? char(c) : '?';
  }
  return out;
}

// Gears 1 / 2 (Ue3NameTagStyle::class_set): `text` into the engine's new buffer of this FString (big-endian UTF-16, with
// its terminator); false when the buffer is too small (left alone).
bool WriteName(uint32_t at, const std::string& text) {
  const uint32_t data = obj::Read32(at), max = obj::Read32(at + 8);
  if (!data || max < text.size() + 1) return false;
  for (size_t i = 0; i <= text.size(); ++i) {
    const uint8_t c = i < text.size() ? uint8_t(text[i]) : 0;
    if (!obj::Write8(data + uint32_t(2 * i), 0) || !obj::Write8(data + uint32_t(2 * i + 1), c)) return false;
  }
  return obj::Write32(at + 4, uint32_t(text.size() + 1));
}

uint32_t FindPriClass() {
#if defined(GN_UE3_OBJECTS_NO_RANGE)  // Gears 1's ue3::obj (whole-table lookups only)
  return obj::FindClass("PlayerReplicationInfo");
#else  // slice by slice: one copy of the whole object table fails while part of it is unreadable
  uint32_t total = 0, cls = 0;
  for (uint32_t first = 0; !cls; first += 4096) {
    cls = obj::FindClassRange("PlayerReplicationInfo", first, 4096, &total);
    if (first + 4096 >= total) break;
  }
  return cls;
#endif
}

std::vector<uint32_t> Pris(uint32_t cls) {
#if defined(GN_UE3_OBJECTS_NO_RANGE)
  return obj::FindInstancesOf(cls);
#else
  std::vector<uint32_t> out;
  uint32_t total = 0;
  for (uint32_t first = 0;; first += 4096) {
    const auto part = obj::FindInstancesOfRange(cls, first, 4096, &total);
    out.insert(out.end(), part.begin(), part.end());
    if (first + 4096 >= total) break;
  }
  return out;
#endif
}
}  // namespace

namespace {
// Ue3NameTagStyle::style_font: every state of the UIStyle `style` (its UIStyle_Text objects) draws in `font` from then
// on (Gears 1: its credits headings too - they look right in it). True once done, or when it never can be (no such
// class or property); false while the style or the font is not loaded yet.
bool RestyleFont(const std::string& style, const std::string& font) {
#if defined(GN_UE3_OBJECTS_GOW3)  // Gears 3's own ue3::obj (no layout, no whole-table walk): never asked for there
  return true;
#else
  const uint32_t text_cls = obj::FindClass("UIStyle_Text");
  const int font_off = text_cls ? obj::PropertyOffset(text_cls, "StyleFont") : -1;
  if (font_off < 0) return true;
  const uint32_t font_obj = obj::FindObject(font.c_str());
  if (!font_obj) return false;
  const uint32_t outer_off = obj::CurrentLayout().outer;
  int changed = 0;
  for (const uint32_t o : obj::AllObjects()) {
    if (!obj::IsA(o, text_cls)) continue;
    const uint32_t outer = obj::Read32(o + outer_off);
    if (!outer || obj::ObjectName(outer) != style) continue;
    changed += obj::Write32(o + uint32_t(font_off), font_obj) ? 1 : 0;
  }
  if (changed) REXLOG_INFO("name tags: text style {} now draws in {} ({} states)", style, font, changed);
  return changed > 0;
#endif
}
}  // namespace

namespace {
// What the worker found out once (the class, the two properties) and the style, for the game-thread part.
std::atomic<uint32_t> g_cls{0};
std::atomic<int> g_name_off{-1}, g_id_off{-1};
std::atomic<bool> g_class_set_due{false};  // Gears 1 / 2: a name needs its tag (the game thread does the class-wide set)
std::string g_before, g_after;

std::string TaggedName(uint32_t pri, bool* plain, bool* changed) {
  *changed = false;
  std::string name = ReadName(pri + uint32_t(g_name_off.load()), plain);
  const int id = g_id_off.load();
  const uint64_t xuid = (uint64_t(obj::Read32(pri + uint32_t(id))) << 32) | obj::Read32(pri + uint32_t(id) + 4);
  if (!xuid || name.empty() || !*plain) return name;
  const auto tag = XenosPlayerTag(xuid);
  if (!tag) return name;
  const std::string mark = "#" + tag->discriminator;
  if (name.find(mark) != std::string::npos) return name;  // already tagged (by us, or the host's copy)
  *changed = true;
  // Names are at most 15 characters (the master server's rule): 15 capital Ws and the tag fit Gears 3's every screen.
  return name + g_before + mark + g_after;
}

namespace {
// ---- the kill feed -------------------------------------------------------------------------------------------------
// Every game's HUD keeps its kill feed as an array of DeathData structs (Gears 1: WarHUD.DeathInfo; Gears 2, 3,
// Judgment: GearHUD_Base) whose string fields are copies of the players' names, and draws them as plain text - so the
// tag's markup (grey on the scoreboard, which renders it) would show as text there. Each engine tick, on the game
// thread, the markup comes out of those copies (the tag stays). The array and its string fields are found through the
// engine's reflection: the HUD class's ScriptStruct DeathData, its StrProperty fields, and the HUD's ArrayProperty
// whose inner StructProperty is DeathData.
struct Walk {
  uint32_t super, children, next, elem_size, prop_offset;
};
Walk WalkLayout() {
#if defined(GN_UE3_OBJECTS_GOW3)  // Gears 3's ue3::obj has its offsets built in (objects.cpp FindProperty / PropertySize)
  return {64, 68, 60, 68, 84};
#else
  const obj::Layout& l = obj::CurrentLayout();
  return {l.super, l.children, l.next, l.elem_size, l.prop_offset};
#endif
}

uint32_t ClassNamed(const char* name) {
#if defined(GN_UE3_OBJECTS_NO_RANGE)
  return obj::FindClass(name);
#else
  uint32_t total = 0, cls = 0;
  for (uint32_t first = 0; !cls; first += 4096) {
    cls = obj::FindClassRange(name, first, 4096, &total);
    if (first + 4096 >= total) break;
  }
  return cls;
#endif
}

std::string ClassName(uint32_t object) {
  const uint32_t c = obj::ClassOf(object);
  return c ? obj::ObjectName(c) : std::string();
}

struct FeedPlan {
  uint32_t hud_cls = 0, array_off = 0, elem_size = 0;
  std::vector<uint32_t> str_offs;   // DeathData's string fields
};

// The plan, or nothing when this engine's HUD has no such array (then the kill feed is left alone).
std::optional<FeedPlan> FindFeedPlan() {
  const Walk w = WalkLayout();
  if (!w.super || !w.children || !w.next || !w.elem_size || !w.prop_offset) return std::nullopt;
  FeedPlan p;
  p.hud_cls = ClassNamed("GearHUD_Base");
  if (!p.hud_cls) p.hud_cls = ClassNamed("WarHUD");   // Gears 1
  if (!p.hud_cls) return std::nullopt;
  uint32_t dd = 0;   // the struct: one of the HUD class's own fields
  for (uint32_t f = obj::Read32(p.hud_cls + w.children), n = 0; f && n < 4000 && !dd; f = obj::Read32(f + w.next), n++)
    if (obj::ObjectName(f) == "DeathData" && ClassName(f) == "ScriptStruct") dd = f;
  if (!dd) return std::nullopt;
  for (uint32_t f = obj::Read32(dd + w.children), n = 0; f && n < 200; f = obj::Read32(f + w.next), n++) {
    REXLOG_DEBUG("name tags: DeathData.{} ({}) +{}", obj::ObjectName(f), ClassName(f), obj::Read32(f + w.prop_offset));
    if (ClassName(f) == "StrProperty") p.str_offs.push_back(obj::Read32(f + w.prop_offset));
  }
  // the array: an ArrayProperty of the HUD class (or a parent) whose Inner is a StructProperty of DeathData
  for (uint32_t c = p.hud_cls, g = 0; c && g < 32 && !p.elem_size; c = obj::Read32(c + w.super), g++)
    for (uint32_t f = obj::Read32(c + w.children), n = 0; f && n < 4000 && !p.elem_size; f = obj::Read32(f + w.next), n++) {
      if (ClassName(f) != "ArrayProperty") continue;
      for (uint32_t k = 0x20; k < 0xA0 && !p.elem_size; k += 4) {
        const uint32_t inner = obj::Read32(f + k);
        if (!inner || inner == f || ClassName(inner) != "StructProperty") continue;
        for (uint32_t j = 0x20; j < 0xA0; j += 4)
          if (obj::Read32(inner + j) == dd) {
            p.array_off = obj::Read32(f + w.prop_offset);
            p.elem_size = obj::Read32(inner + w.elem_size);
            break;
          }
      }
    }
  if (!p.elem_size || p.elem_size > 1024 || p.str_offs.empty()) return std::nullopt;
  for (const uint32_t o : p.str_offs)
    if (o + 12 > p.elem_size) return std::nullopt;
  return p;
}

std::mutex g_feed_mu;
std::optional<FeedPlan> g_feed;   // set once by the worker
std::vector<uint32_t> g_huds;     // the HUD objects, refreshed by the worker each second

// Worker (once a second): the plan once, then the HUDs that exist now (a new map makes new ones).
void FeedRefresh() {
  static bool tried = false, logged = false;
  if (g_before.empty() && g_after.empty()) return;   // no markup: nothing ever shows
  std::lock_guard<std::mutex> lock(g_feed_mu);
  if (!tried) {
    tried = true;
    g_feed = FindFeedPlan();
  }
  if (!g_feed) {
    if (!logged) REXLOG_WARN("name tags: the HUD's kill feed array was not found - its names keep the markup");
    logged = true;
    return;
  }
  if (!logged) {
    REXLOG_INFO("name tags: kill feed at HUD +{}, {}-byte entries, {} name field(s)", g_feed->array_off, g_feed->elem_size,
                g_feed->str_offs.size());
    logged = true;
  }
  g_huds = Pris(g_feed->hud_cls);   // (Pris: every instance of a class)
}

// Game thread, each engine tick: the markup out of every kill-feed name (an entry appears in a script call during the
// tick; the HUD draws it after this). A name is written only when it holds the markup, and shorter, into its own buffer.
void FeedStrip() {
  FeedPlan plan;
  std::vector<uint32_t> huds;
  {
    std::lock_guard<std::mutex> lock(g_feed_mu);
    if (!g_feed || g_huds.empty()) return;
    plan = *g_feed;
    huds = g_huds;
  }
  const std::string& mark = g_before.empty() ? g_after : g_before;
  for (const uint32_t hud : huds) {
    if (!obj::IsA(hud, plan.hud_cls)) continue;   // gone with the last map
    const uint32_t data = obj::Read32(hud + plan.array_off), num = obj::Read32(hud + plan.array_off + 4);
    if (!data || num > 64) continue;
    for (uint32_t i = 0; i < num; ++i)
      for (const uint32_t off : plan.str_offs) {
        const uint32_t at = data + i * plan.elem_size + off;
        bool plain = true;
        const std::string name = ReadName(at, &plain);
        if (!plain || name.find(mark) == std::string::npos) continue;
        WriteName(at, xenos::tags::StripMarkup(name, g_before, g_after));
      }
  }
}
}  // namespace

// Once a second, off the game thread: a scan of the game's own objects (no network; the tags come from
// xenos/player_tags.h's cache). Gears 3 / Judgment: one `set <PRI> PlayerName ...` per name that needs its tag.
// Gears 1 / 2: only a flag - the game thread then does the class-wide set and puts every name right in one frame.
void Worker(bool (*objects_ready)(), bool class_set, std::string style_font) {
  std::unordered_map<uint32_t, std::string> asked;  // PRI -> the command last queued for it
  for (;; std::this_thread::sleep_for(std::chrono::seconds(1))) {
    if (!XenosPlayerTagsOnline()) continue;  // offline: no tags, and the object tables are not even opened
    if (!objects_ready()) continue;          // opened here, off the game thread (Gears 2's search takes ~1 s)
    if (!g_cls.load()) {
      const uint32_t cls = FindPriClass();
      if (!cls) continue;
      g_name_off = obj::PropertyOffset(cls, "PlayerName");
      g_id_off = obj::PropertyOffset(cls, "UniqueId");
      g_cls = cls;
      REXLOG_INFO("name tags: PlayerReplicationInfo.PlayerName +{}, .UniqueId +{}", g_name_off.load(), g_id_off.load());
    }
    if (g_name_off.load() < 0 || g_id_off.load() < 0) return;  // not this engine's layout: nothing to do, ever
    FeedRefresh();   // the kill feed (its markup is taken out each tick: FeedStrip)
    if (const size_t eq = style_font.find('='); eq != std::string::npos &&
        RestyleFont(style_font.substr(0, eq), style_font.substr(eq + 1)))
      style_font.clear();  // done (the styles stay loaded: they belong to the game's UI skin)
    for (const uint32_t pri : Pris(g_cls.load())) {
      bool plain = true, changed = false;
      const std::string name = TaggedName(pri, &plain, &changed);
      if (!changed) continue;
      if (class_set) {
        g_class_set_due = true;
        break;
      }
      const std::string command = "set " + obj::ObjectName(pri) + " PlayerName " + name;
      std::string& last = asked[pri];
      if (last == command) continue;  // queued for this name already
      last = command;
      REXLOG_DEBUG("name tags: {}", command);
      Ue3ConsoleQueueTrusted(command);
    }
  }
}
}  // namespace

void Ue3NameTagsTick(bool (*objects_ready)(), const Ue3NameTagStyle& style) {
  static const bool on = REXCVAR_GET(cogbox_name_tags);
  if (!on || !objects_ready) return;
  static std::once_flag started;
  std::call_once(started, [&] {
    const std::string style_text = REXCVAR_GET(cogbox_name_tags_style);
    const size_t bar = style_text.find('|');
    g_before = bar == std::string::npos ? style.before : style_text.substr(0, bar);
    g_after = bar == std::string::npos ? style.after : style_text.substr(bar + 1);
    std::thread(Worker, objects_ready, style.class_set, std::string(style.style_font)).detach();
  });
  // Gears 1 / 2, on the game thread just before Ue3ConsoleRunQueued: every PlayerReplicationInfo, class defaults
  // included (the class-wide set changes them all) - its name now and the name it should have; then the set, and in
  // its callback (the same frame) each name written back into its new engine-owned buffer.
  FeedStrip();   // kill-feed names: the tag without its markup (the HUD draws plain text)
  if (!style.class_set || !g_class_set_due.exchange(false)) return;
  const uint32_t cls = g_cls.load();
  struct Want { uint32_t object; std::string name; };
  std::vector<Want> all;
  bool change = false;
  for (const uint32_t o : obj::AllObjects()) {
    if (!obj::IsA(o, cls)) continue;
    bool plain = true, changed = false;
    std::string name = TaggedName(o, &plain, &changed);
    if (!plain) return;  // a name we could not give back exactly: leave every name alone
    change = change || changed;
    all.push_back({o, std::move(name)});
  }
  if (!change) return;
  const std::string placeholder(160, 'x');
  for (const Want& w : all) if (w.name.size() + 1 > placeholder.size()) return;  // never: names are short
  const int name_off = g_name_off.load();
  Ue3ConsoleQueueTrusted("set PlayerReplicationInfo PlayerName " + placeholder, [all, name_off] {
    int written = 0;
    for (const Want& w : all) written += WriteName(w.object + uint32_t(name_off), w.name) ? 1 : 0;
    REXLOG_DEBUG("name tags: {} of {} names written back", written, all.size());
  });
}

void Ue3NameTagsStripText(uint8_t* base, uint32_t text) {
  // the tag's markup is plain ASCII: its first character shows whether to look further
  if (!base || !text || (text & 1) || (g_before.empty() && g_after.empty())) return;
  const std::string& mark = g_before.empty() ? g_after : g_before;
  constexpr uint32_t kMax = 256;
  if (!obj::Readable(text, 2)) return;
  std::string s;
  bool plain = true;
  for (uint32_t i = 0; i < kMax; ++i) {
    if (((text + 2 * i) & 0xFFF) == 0 && !obj::Readable(text + 2 * i, 2)) return;   // a new page
    const uint16_t c = uint16_t(base[text + 2 * i] << 8 | base[text + 2 * i + 1]);
    if (!c) break;
    if (c >= 0x80) plain = false;
    s += c < 0x80 ? char(c) : '?';
  }
  if (!plain || s.find(mark) == std::string::npos) return;
  const std::string out = xenos::tags::StripMarkup(s, g_before, g_after);
  for (size_t i = 0; i <= out.size(); ++i) {   // shorter: stays within the text
    base[text + 2 * i] = 0;
    base[text + 2 * i + 1] = i < out.size() ? uint8_t(out[i]) : 0;
  }
}
