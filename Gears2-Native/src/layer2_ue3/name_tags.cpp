// GitHub #353: see ue3/name_tags.h.
#include <ue3/console.h>
#include <ue3/name_tags.h>
#include <ue3/objects.h>
#include <xenos/player_tags.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
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
void CheckTextHooks();
void AllocScratch();
}  // namespace

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
std::atomic<int> g_backoff_s{1};  // Gears 1 / 2: seconds before the next class-wide attempt (review r366 (4))
std::chrono::steady_clock::time_point g_next_try{};  // Gears 1 / 2: game thread only (the tick and the set's callback)
// the style's markup around "#1234", set once (Ue3NameTagsTick) before anything reads it; published through an atomic
// pointer for the worker and the text hooks
struct Markup {
  std::string before, after;
};
std::atomic<const Markup*> g_markup{nullptr};
const Markup& Mk() {
  static const Markup none;
  const Markup* m = g_markup.load(std::memory_order_acquire);
  return m ? *m : none;
}

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
  return name + Mk().before + mark + Mk().after;
}

namespace {
// ---- the kill feed: reflection helpers for the start-up check of the text hooks (CheckTextHooks, below) ----------
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
    CheckTextHooks();   // ... and as the HUD draws it, when this build has text hooks
    if (const size_t eq = style_font.find('='); eq != std::string::npos &&
        RestyleFont(style_font.substr(0, eq), style_font.substr(eq + 1)))
      style_font.clear();  // done (the styles stay loaded: they belong to the game's UI skin)
    for (const uint32_t pri : Pris(g_cls.load())) {
      bool plain = true, changed = false;
      const std::string name = TaggedName(pri, &plain, &changed);
      if (!changed) {
        // review r366 (3): tagged now (or needs none) - forget what was queued, so a name the engine puts back later
        // (a new round, a re-replicated PRI at the same address) is tagged again; it also keeps the map small
        asked.erase(pri);
        continue;
      }
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
    g_markup.store(new Markup{bar == std::string::npos ? std::string(style.before) : style_text.substr(0, bar),
                              bar == std::string::npos ? std::string(style.after) : style_text.substr(bar + 1)},
                   std::memory_order_release);   // (kept for the process's life)
    AllocScratch();
    std::thread(Worker, objects_ready, style.class_set, std::string(style.style_font)).detach();
  });
  // Gears 1 / 2, on the game thread just before Ue3ConsoleRunQueued: every PlayerReplicationInfo, class defaults
  // included (the class-wide set changes them all) - its name now and the name it should have; then the set, and in
  // its callback (the same frame) each name written back into its new engine-owned buffer.
  if (!style.class_set || !g_class_set_due.exchange(false)) return;
  // review r366 (4): an attempt that can't finish (a name that isn't plain ASCII, a `set` the engine refused, a write
  // that didn't fit) used to come back every second, each time a whole object-table walk on the game thread. The next
  // attempt waits 1, 2, 4 ... 64 s while attempts keep being needed; an attempt that finds nothing to change resets it.
  const auto now_t = std::chrono::steady_clock::now();
  if (now_t < g_next_try) return;
  const int wait_s = g_backoff_s.load();
  g_next_try = now_t + std::chrono::seconds(wait_s);
  g_backoff_s = std::min(wait_s * 2, 64);  // put back to 1 by a write-back that wrote every name
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
  if (!change) {
    g_backoff_s = 1;  // every name already right: the next need is tried at once
    g_next_try = {};
    return;
  }
  const std::string placeholder(160, 'x');
  for (const Want& w : all) if (w.name.size() + 1 > placeholder.size()) return;  // never: names are short
  const int name_off = g_name_off.load();
  Ue3ConsoleQueueTrusted("set PlayerReplicationInfo PlayerName " + placeholder, [all, name_off] {
    int written = 0;
    for (const Want& w : all) written += WriteName(w.object + uint32_t(name_off), w.name) ? 1 : 0;
    if (written == int(all.size())) {  // all done: a player who joins later is tagged at once
      g_backoff_s = 1;
      g_next_try = {};
    }
    REXLOG_DEBUG("name tags: {} of {} names written back", written, all.size());
  });
}

// ---- the kill feed's draw-time half: the text arguments of the engine's text routines (layer 3 hooks) -------------
namespace {
const Ue3TextHookCheck* g_checks = nullptr;   // the game's (constant-initialized: set by a static initializer)
size_t g_check_count = 0;
std::atomic<bool> g_text_ok{false};           // the checks passed: the hooks clean text
thread_local int t_canvas_calls = 0;          // this thread is inside a Canvas exec native (Ue3CanvasTextCall)

// Scratch copies: a ring of guest-memory slots of our own (system heap), each a text of up to kMaxText characters and
// an FString after it. A slot is needed only for the one call it is passed to: the hooked routines measure the text, or
// draw it (turning it into glyph quads) before they return - they keep no pointer to it. Only inside a Canvas exec native
// (Ue3CanvasTextCall), so only the script's draw calls on the game thread take slots. At most a few slots per call
// (only arguments holding the markup take one); the ring is far longer than any nesting of these calls.
constexpr uint32_t kMaxText = 256, kSlots = 64, kSlotBytes = 0x220, kSlotFString = 0x210;
std::atomic<uint32_t> g_scratch{0}, g_scratch_next{0};

void AllocScratch() {
  if (g_check_count == 0 || g_scratch.load()) return;   // no hooks in this game
  rex::Runtime* rt = rex::Runtime::instance();
  const uint32_t a = rt && rt->memory() ? rt->memory()->SystemHeapAlloc(kSlots * kSlotBytes) : 0;
  g_scratch.store(a, std::memory_order_release);
  if (!a) REXLOG_WARN("name tags: no memory for the kill feed's text copies - its names keep the markup");
}

uint16_t Read16(uint32_t a) {
  const uint32_t w = obj::Read32(a & ~3u);
  return uint16_t((a & 2) ? (w & 0xFFFF) : (w >> 16));
}

// The guest text at `text` (big-endian UTF-16) in `out` (kMaxText + 1 bytes), its length in `len`; false when it isn't
// readable, isn't all printable ASCII, has no '#' (it can't hold a tag), or has no terminator within kMaxText characters
// (then it is left alone, never cut short). Fast for what isn't text: the first character that isn't printable ASCII
// ends the look (most argument registers hold no text at all).
bool ReadText(uint32_t text, char* out, uint32_t* len) {
  if (text < 0x40000000 || text >= 0x80000000 || (text & 1) || !obj::Readable(text, 2)) return false;
  bool hash = false;
  for (uint32_t i = 0; i <= kMaxText; ++i) {
    const uint32_t a = text + 2 * i;
    if (i && (a & 0xFFF) == 0 && !obj::Readable(a, 2)) return false;   // a new page
    const uint16_t c = Read16(a);
    if (!c) {
      *len = i;
      return hash;
    }
    if (c < 0x20 || c >= 0x7F || i == kMaxText) return false;
    hash = hash || c == '#';
    out[i] = char(c);
  }
  return false;
}

// A scratch copy of `text` without the tag's markup, or 0 when it holds none; `len` gets the original text's length.
uint32_t CleanCopy(uint32_t text, uint32_t* count, uint32_t* len) {
  const Markup& m = Mk();
  char buf[kMaxText + 1];
  if (!ReadText(text, buf, len)) return 0;
  std::string s(buf, *len);
  if (!xenos::tags::StripMarkup(&s, m.before, m.after)) return 0;
  const uint32_t base = g_scratch.load(std::memory_order_acquire);
  if (!base) return 0;
  const uint32_t slot = base + (g_scratch_next.fetch_add(1, std::memory_order_relaxed) % kSlots) * kSlotBytes;
  for (size_t i = 0; i <= s.size(); ++i) {
    const uint8_t c = i < s.size() ? uint8_t(s[i]) : 0;
    if (!obj::Write8(slot + uint32_t(2 * i), 0) || !obj::Write8(slot + uint32_t(2 * i + 1), c)) return 0;
  }
  *count = uint32_t(s.size() + 1);
  return slot;
}

// Worker, once: are the game's Canvas natives the ones the hooks were written for (this build)? Each check names a
// Canvas function and its native's code address, which the UFunction holds.
void CheckTextHooks() {
  static bool done = false;
  if (done || g_check_count == 0) return;
  done = true;
  const Walk w = WalkLayout();
  const uint32_t canvas = ClassNamed("Canvas");
  size_t passed = 0;
  for (size_t i = 0; i < g_check_count; ++i) {
    bool ok = false;
    for (uint32_t f = canvas ? obj::Read32(canvas + w.children) : 0, n = 0; f && n < 400 && !ok; f = obj::Read32(f + w.next), n++) {
      if (obj::ObjectName(f) != g_checks[i].function) continue;
      for (uint32_t k = 0x30; k < 0xC0 && !ok; k += 4) ok = obj::Read32(f + k) == g_checks[i].exec;
    }
    passed += ok ? 1 : 0;
  }
  g_text_ok.store(passed == g_check_count, std::memory_order_release);
  if (passed == g_check_count) REXLOG_INFO("name tags: kill feed text hooks on ({} Canvas natives as expected)", passed);
  else REXLOG_WARN("name tags: kill feed text hooks off - {} of {} Canvas natives are not this build's", g_check_count - passed, g_check_count);
}
}  // namespace

void Ue3NameTagsTextHookChecks(const Ue3TextHookCheck* checks, size_t count) {
  g_checks = checks;
  g_check_count = count;
}

namespace {
// Once per run, the first time a draw call's text is cleaned: proof in the log that the kill feed's names reach the
// hooks through the Canvas natives (review r368b).
uint32_t NoteCleaned(uint32_t copy) {
  static std::atomic<bool> noted{false};
  if (!noted.exchange(true, std::memory_order_relaxed)) REXLOG_INFO("name tags: a tagged name cleaned in a Canvas draw call (the kill feed's path; first time this run)");
  return copy;
}
}  // namespace

Ue3CanvasTextCall::Ue3CanvasTextCall() { ++t_canvas_calls; }
Ue3CanvasTextCall::~Ue3CanvasTextCall() { --t_canvas_calls; }

uint32_t Ue3NameTagsTextArg(uint32_t arg) {
  if (t_canvas_calls <= 0 || !g_text_ok.load(std::memory_order_acquire)) return arg;   // only the script's draw calls
  if (Mk().before.empty() && Mk().after.empty()) return arg;   // tags without markup: nothing to take out
  uint32_t count = 0, len = 0;
  if (const uint32_t c = CleanCopy(arg, &count, &len)) return NoteCleaned(c);   // the text itself
  // an FString {data, count, max} - only when its count is its text's length + 1 and max at least that, so another
  // struct whose first word points at a name is never taken for one: a scratch FString of the clean copy
  if (arg < 0x40000000 || arg >= 0x80000000 || (arg & 3) || !obj::Readable(arg, 12)) return arg;
  const uint32_t data = obj::Read32(arg), num = obj::Read32(arg + 4), max = obj::Read32(arg + 8);
  if (!data || num < 2 || num > kMaxText + 1 || max < num) return arg;
  const uint32_t c = CleanCopy(data, &count, &len);
  if (!c || len + 1 != num) return arg;
  const uint32_t f = c + kSlotFString;
  if (!obj::Write32(f, c) || !obj::Write32(f + 4, count) || !obj::Write32(f + 8, count)) return arg;
  return NoteCleaned(f);
}
