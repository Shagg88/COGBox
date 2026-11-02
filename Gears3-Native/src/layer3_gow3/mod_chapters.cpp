// Layer 3 (Gears of War 3, disc XEX or Title Update 6): new campaign chapters and the content packages mods need. From
// KiltedKernel's Gears 3 port (runtime/ue3/campaign_chapters.cpp and content_packages.cpp there), where the trilogy
// (Gears 1 and 2 campaigns converted into Gears 3 mods) runs on them. Our code (MIT).
//
// Chapters 32-49. Gears 3 starts a chapter as GearGame_P?chapter=N (the launcher's --ue3_launch, or the Select Chapter
// menu), and native code loads the map named by Default__GearTypes.ChapterLevelNames[N]: 31 FNames in GearGame.xxx, up
// to CHAP_Gameover. The menu lists chapters from config ([ChapterN GearCampaignChapterData] in Xbox360-GearGame.ini;
// their MapName is shown, not loaded). EChapterPoint reserves CHAP_DLC_1..18 (32..49) for downloadable campaigns. So a
// chapter N >= 32 a mod declares in config (a config fragment, ue3/mod_extras.h) gets its MapName put into
// ChapterLevelNames[N] here, on the game thread: GearGame.xxx stays the disc's. FName construction: the game's own
// FName::FName(FName* out r3, const TCHAR* name r4 (UTF-16BE), EFindName r5 = 1 FNAME_Add), run through the dispatcher.
// While a new chapter's map is loaded:
//  - its number is kept in GearGameSP_Base.CurrentChapter and GearGRI_SP.ChapterNum (a load from a checkpoint has none:
//    Restart Chapter loaded the prologue);
//  - a checkpoint load streams back the sections the map had loaded at the save (a converted chapter streams by its
//    Kismet alone, and every LevelStreamingKismet is unloaded by the load);
//  - objects the chapter lists in Kismet object lists named Trilogy* join the root set (garbage collection over a
//    checkpoint load otherwise dropped shared textures and character classes).
// Content packages (mod.ini content = MPCH_COGMarcusG2MP_SF, ...): the game's own seekfree packages a mod's levels import
// but the game would not load there (a multiplayer character in the campaign), loaded once the engine is up through
// UObject::LoadPackageAsync(const FString& r3, callback r4, user data r5, const FGuid* r6, FName type r7) and kept: the
// package's own objects (Outer = the package) join the root set.
// --gow3_mod_chapters=false turns all of it off.
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include <ue3/mod_extras.h>
#include <ue3/objects.h>

#include "gow3_addresses.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(gow3_mod_chapters, true, "Gears3",
                    "Mods' campaign chapters (32-49) and content packages (KiltedKernel's mod support)");
REXCVAR_DEFINE_BOOL(gow3_mod_content_packages, true, "Gears3",
                    "Load the content packages mods name (mod.ini content = ...); false to test without them");
REXCVAR_DEFINE_BOOL(gow3_mod_file_log, false, "Gears3",
                    "Developers: log every package the game opens (to see where a mod's chapter stops loading)");

// layer 2 shader_prebuild.cpp: the one file-open observer, with a listener chained after it
void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));

namespace {
namespace obj = ue3::obj;
constexpr int kFirstChapter = 32, kLastChapter = 49;

std::map<int, std::string> g_chapters;      // chapter number -> map name
std::atomic<uint32_t> g_types{0};           // Default__GearTypes, once found
std::atomic<bool> g_tableDone{false};
std::atomic<uint32_t> g_chapterGame{0};     // the GearGameSP_Base while a new chapter's map is loaded
std::atomic<bool> g_restore{false};         // a checkpoint has loaded: its sections back, on the game thread
std::atomic<bool> g_started{false};

rex::memory::Memory* Mem() { return rex::Runtime::instance()->memory(); }
uint32_t Alloc(uint32_t size) { return Mem()->SystemHeapAlloc(size); }
void Put16(uint32_t a, uint16_t v) { *Mem()->TranslateVirtual<uint16_t*>(a) = _byteswap_ushort(v); }

// a guest function on this (guest) thread, its registers restored after
uint64_t CallGuest(uint32_t address, std::initializer_list<uint64_t> args) {
  auto* ts = rex::runtime::ThreadState::Get();
  auto* fd = rex::Runtime::instance()->function_dispatcher();
  if (!ts || !fd) return 0;
  std::vector<uint64_t> a(args);
  return fd->ExecuteTrap(ts, address, a.data(), a.size());
}

// ---- the chapters mods declare --------------------------------------------------------------------------------------

// the packed config the game reads: the mods' merged copy when there is one (ue3/mod_extras.h), else the game's
void ReadChapters() {
  auto* rt = rex::Runtime::instance();
  std::filesystem::path path = rt->user_data_root() / "mod-config" / "Coalesced_INT.bin";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return;   // no mod has config fragments: no new chapters
  std::ifstream f(path, std::ios::binary);
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
  size_t p = 0;
  bool ok = true;
  auto i32 = [&]() -> int32_t {
    if (p + 4 > d.size()) { ok = false; return 0; }
    const int32_t v = int32_t(uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3]);
    p += 4;
    return v;
  };
  auto str = [&]() -> std::string {
    const int32_t n = i32();
    std::string s;
    if (n > 0) {
      if (p + size_t(n) > d.size()) { ok = false; return s; }
      s.assign(reinterpret_cast<const char*>(&d[p]), size_t(n) - 1);
      p += size_t(n);
    } else if (n < 0) {
      const size_t units = size_t(-int64_t(n));
      if (p + 2 * units > d.size()) { ok = false; return s; }
      for (size_t i = 0; i + 1 < units; i++) {
        const uint16_t c = uint16_t(d[p + 2 * i] | d[p + 2 * i + 1] << 8);
        s += c < 128 ? char(c) : '?';
      }
      p += 2 * units;
    }
    return s;
  };
  const int32_t files = i32();
  for (int32_t fi = 0; fi < files && ok; fi++) {
    str();
    const int32_t sections = i32();
    for (int32_t si = 0; si < sections && ok; si++) {
      const std::string sec = str();
      const int32_t keys = i32();
      int num = -1;
      if (sec.rfind("Chapter", 0) == 0 && sec.find(" GearCampaignChapterData") != std::string::npos) num = atoi(sec.c_str() + 7);
      for (int32_t k = 0; k < keys && ok; k++) {
        const std::string key = str(), value = str();
        if (num >= kFirstChapter && num <= kLastChapter && key == "MapName" && !value.empty()) g_chapters[num] = value;
      }
    }
  }
  if (!ok) REXLOG_WARN("mod chapters: {} unreadable", rex::path_to_utf8(path));
  for (auto& [n, m] : g_chapters) REXLOG_INFO("mod chapters: chapter {} -> {}", n, m);
}

// an FName (index, number) for a string, through the game's own constructor (game thread)
void MakeName(const std::string& s, uint32_t& index, uint32_t& number) {
  static uint32_t out = 0, text = 0;
  if (!out) { out = Alloc(8); text = Alloc(256); }
  const size_t n = std::min<size_t>(s.size(), 120);
  for (size_t i = 0; i < n; i++) Put16(text + 2 * uint32_t(i), uint16_t(uint8_t(s[i])));
  Put16(text + 2 * uint32_t(n), 0);
  CallGuest(gow3::addr::kFNameCtor, {out, text, 1});
  index = obj::Read32(out);
  number = obj::Read32(out + 4);
}

// ChapterLevelNames grown to the last new chapter, the new chapters' maps in it (game thread, once). False while the
// class isn't ready yet (its properties are linked a little after its default object exists): asked again later.
bool FillChapterTable() {
  const uint32_t types = g_types;
  const int off = obj::PropertyOffset(obj::ClassOf(types), "ChapterLevelNames");
  if (off < 0) return false;
  const uint32_t arr = types + uint32_t(off);
  const uint32_t oldData = obj::Read32(arr), oldCount = obj::Read32(arr + 4);
  const uint32_t count = std::max<uint32_t>(oldCount, uint32_t(g_chapters.rbegin()->first + 1));
  const uint32_t data = Alloc(8 * count);
  for (uint32_t k = 0; k < count; k++) {
    const bool old = k < oldCount && oldData;
    obj::Write32(data + 8 * k, old ? obj::Read32(oldData + 8 * k) : 0);  // 'None' in the gap (CHAP_Gameover...)
    obj::Write32(data + 8 * k + 4, old ? obj::Read32(oldData + 8 * k + 4) : 0);
  }
  for (auto& [n, map] : g_chapters) {
    uint32_t index = 0, number = 0;
    MakeName(map, index, number);
    obj::Write32(data + 8 * uint32_t(n), index);
    obj::Write32(data + 8 * uint32_t(n) + 4, number);
    REXLOG_INFO("mod chapters: ChapterLevelNames[{}] = {} (name {} {})", n, obj::Name(index), index, number);
  }
  obj::Write32(arr, data);
  obj::Write32(arr + 4, count);
  obj::Write32(arr + 8, count);
  REXLOG_INFO("mod chapters: {} chapter maps (was {})", count, oldCount);
  return true;
}

// ---- while a new chapter's map is loaded ----------------------------------------------------------------------------

void ChapterKeeper() {
  uint32_t lastGame = 0;
  int chapter = -1;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!g_tableDone || g_chapters.empty() || !obj::Ready()) continue;
    if (static int t = 0; ++t % 5 == 0 && !g_chapterGame)   // (every 10 s until a chapter map is seen)
      for (auto& [n, map] : g_chapters) {
        const uint32_t p = obj::FindObject(map.c_str());
        REXLOG_INFO("mod chapters: chapter {} map {} {}", n, map, p ? "in memory (class " + obj::ObjectName(obj::ClassOf(p)) + ")" : "not loaded");
      }
    static uint32_t gameClass = 0, griClass = 0;   // (looked up until found: GearGame's classes load after boot)
    if (!gameClass) gameClass = obj::FindClass("GearGameSP_Base");
    if (!griClass) griClass = obj::FindClass("GearGRI_SP");
    if (!gameClass) continue;
    const auto games = obj::FindInstancesOf(gameClass);
    const uint32_t game = games.empty() ? 0 : games.front();
    if (!game) { lastGame = 0; g_chapterGame = 0; continue; }
    if (game != lastGame) {  // a new map: which chapter's (its package loaded)
      lastGame = game;
      chapter = -1;
      for (auto& [n, map] : g_chapters)
        if (const uint32_t p = obj::FindObject(map.c_str()); p && obj::ObjectName(obj::ClassOf(p)) == "Package") chapter = n;
      g_chapterGame = chapter >= 0 ? game : 0;
      uint32_t top = game;
      for (int k = 0; k < 8 && obj::Read32(top + 40); k++) top = obj::Read32(top + 40);
      REXLOG_INFO("mod chapters: map {} loaded{}", obj::ObjectName(top), chapter >= 0 ? " (chapter " + std::to_string(chapter) + "'s)" : "");
    }
    if (chapter < 0) continue;
    static const int off = obj::PropertyOffset(gameClass, "CurrentChapter"), size = obj::PropertySize(gameClass, "CurrentChapter");
    if (off < 0 || (size != 1 && size != 4)) continue;
    const uint32_t at = game + uint32_t(off);
    const uint32_t cur = size == 4 ? obj::Read32(at) : obj::Read32(at) >> 24;
    if (cur != uint32_t(chapter)) {
      REXLOG_INFO("mod chapters: the game's chapter {} -> {} (the map is chapter {}'s)", cur, chapter, chapter);
      if (size == 4) obj::Write32(at, uint32_t(chapter));
      else obj::Write8(at, uint8_t(chapter));
    }
    if (griClass)
      for (uint32_t gri : obj::FindInstancesOf(griClass)) {
        static const int goff = obj::PropertyOffset(griClass, "ChapterNum"), gsize = obj::PropertySize(griClass, "ChapterNum");
        if (goff < 0) continue;
        if (gsize == 4) obj::Write32(gri + uint32_t(goff), uint32_t(chapter));
        else if (gsize == 1) obj::Write8(gri + uint32_t(goff), uint8_t(chapter));
      }
  }
}

struct Section { bool loaded = false, visible = false; };
using Sections = std::map<uint32_t, Section>;  // by LevelStreamingKismet
std::mutex g_sectionsMutex;
Sections g_atSave, g_beforeLoad;
bool g_atStart = true;  // no checkpoint saved since the chapter started

struct StreamingProps {
  uint32_t cls = 0;
  int name = -1, loaded = -1, visible = -1, block = -1;
  uint32_t loadedMask = 0, visibleMask = 0, blockMask = 0;
  bool Find() {
    if (cls) return name >= 0 && loaded >= 0 && visible >= 0 && loadedMask && visibleMask;
    cls = obj::FindClass("LevelStreamingKismet");
    if (!cls) return false;
    name = obj::PropertyOffset(cls, "PackageName");
    loaded = obj::PropertyOffset(cls, "bShouldBeLoaded");
    loadedMask = obj::BoolMask(cls, "bShouldBeLoaded");
    visible = obj::PropertyOffset(cls, "bShouldBeVisible");
    visibleMask = obj::BoolMask(cls, "bShouldBeVisible");
    block = obj::PropertyOffset(cls, "bShouldBlockOnLoad");
    blockMask = obj::BoolMask(cls, "bShouldBlockOnLoad");
    return name >= 0 && loaded >= 0 && visible >= 0 && loadedMask && visibleMask;
  }
  Section Read(uint32_t ls) const {
    return {(obj::Read32(ls + uint32_t(loaded)) & loadedMask) != 0, (obj::Read32(ls + uint32_t(visible)) & visibleMask) != 0};
  }
  std::string NameOf(uint32_t ls) const { return obj::Name(obj::Read32(ls + uint32_t(name))); }
} g_sp;

std::string List(const Sections& sections) {
  std::string list;
  for (auto& [ls, s] : sections)
    if (s.loaded || s.visible) list += " " + g_sp.NameOf(ls) + (s.visible ? "" : "(hidden)");
  return list;
}

void CheckpointStreaming() {
  uint32_t lastGame = 0, lastVisMan = 0, pc = 0;
  float lastSave = 0;
  std::vector<uint32_t> streams;
  int ticks = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint32_t game = g_chapterGame;
    if (!game) { lastGame = 0; continue; }
    static const uint32_t gameClass = obj::FindClass("GearGameSP_Base"), pcClass = obj::FindClass("GearPC");
    static const int visManOff = obj::PropertyOffset(gameClass, "AIVisMan");
    static const int saveTimeOff = pcClass ? obj::PropertyOffset(pcClass, "LastCheckpointSaveTime") : -1;
    if (visManOff < 0 || saveTimeOff < 0 || !g_sp.Find()) {
      static bool warned = false;
      if (!warned) REXLOG_WARN("mod chapters: checkpoint streaming off (properties not found)");
      warned = true;
      continue;
    }
    const bool start = game != lastGame;
    if (start || ++ticks % 20 == 0) {
      const auto pcs = obj::FindInstancesOf(pcClass);
      pc = pcs.empty() ? 0 : pcs.front();
      streams = obj::FindInstancesOf(g_sp.cls);
    }
    const uint32_t visMan = obj::Read32(game + uint32_t(visManOff));
    const float saveTime = pc ? obj::ReadFloat(pc + uint32_t(saveTimeOff)) : 0.f;
    Sections now;
    bool anyVisible = false;
    for (uint32_t ls : streams) {
      now[ls] = g_sp.Read(ls);
      anyVisible |= now[ls].visible;
    }
    std::lock_guard lock(g_sectionsMutex);
    if (start) {
      lastGame = game;
      lastVisMan = visMan;
      g_beforeLoad.clear();
      g_atSave = now;
      g_atStart = true;
      lastSave = saveTime;
      REXLOG_INFO("mod chapters: {} sections; at the start:{}", streams.size(), List(now));
    } else if (pc && saveTime != lastSave && saveTime > 0) {
      lastSave = saveTime;
      g_atSave = now;
      g_atStart = false;
      REXLOG_INFO("mod chapters: checkpoint saved with{}", List(now));
    }
    if (visMan && visMan != lastVisMan) {  // a checkpoint loaded (GearGameSP_Base makes a new AIVisMan)
      lastVisMan = visMan;
      g_restore = true;
    } else if (anyVisible) {
      g_beforeLoad = now;  // (not once the load has cleared them)
    }
  }
}

void RestoreSections() {  // game thread
  Sections sections;
  {
    std::lock_guard lock(g_sectionsMutex);
    sections = g_atSave;
    if (g_atStart)  // the chapter's first checkpoint: its start again, shown (its opening then plays again)
      for (auto& [ls, s] : sections) s.visible |= s.loaded;
    else
      for (auto& [ls, s] : g_beforeLoad) {
        sections[ls].loaded |= s.loaded;
        sections[ls].visible |= s.visible;
      }
  }
  auto set = [](uint32_t ls, int off, uint32_t mask, bool on) {
    if (off < 0 || !mask) return;
    const uint32_t v = obj::Read32(ls + uint32_t(off));
    obj::Write32(ls + uint32_t(off), on ? (v | mask) : (v & ~mask));
  };
  for (auto& [ls, s] : sections) {
    if (!s.loaded && !s.visible) continue;
    if (!obj::IsA(ls, g_sp.cls)) continue;  // (gone)
    set(ls, g_sp.loaded, g_sp.loadedMask, true);
    set(ls, g_sp.visible, g_sp.visibleMask, s.visible);
    set(ls, g_sp.block, g_sp.blockMask, true);
  }
  REXLOG_INFO("mod chapters: checkpoint loaded: streamed back in:{}", List(sections));
}

constexpr uint32_t kFlagsLow = 12, kRootSet = 0x4000, kClassDefault = 0x200, kOuter = 40;

void SetRoot(uint32_t o, bool on) {
  if (!obj::Readable(o + kFlagsLow, 4)) return;
  const uint32_t v = obj::Read32(o + kFlagsLow);
  obj::Write32(o + kFlagsLow, on ? (v | kRootSet) : (v & ~kRootSet));
}

void TrilogyRoots() {
  std::set<uint32_t> rooted;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_chapterGame) {
      if (!rooted.empty()) {
        for (uint32_t o : rooted) SetRoot(o, false);
        REXLOG_INFO("mod chapters: {} objects out of the root set", rooted.size());
        rooted.clear();
      }
      continue;
    }
    static const uint32_t listClass = obj::FindClass("SeqVar_ObjectList");
    static const int listOff = listClass ? obj::PropertyOffset(listClass, "ObjList") : -1;
    if (!listClass || listOff < 0) continue;
    const size_t before = rooted.size();
    for (uint32_t list : obj::FindInstancesOf(listClass)) {
      if (obj::ObjectName(list).rfind("Trilogy", 0) != 0) continue;
      const uint32_t data = obj::Read32(list + uint32_t(listOff)), count = obj::Read32(list + uint32_t(listOff) + 4);
      if (!data || count > 100000) continue;
      for (uint32_t k = 0; k < count; k++) {
        const uint32_t o = obj::Read32(data + 4 * k);
        if (o && rooted.insert(o).second) SetRoot(o, true);
      }
    }
    if (rooted.size() != before) REXLOG_INFO("mod chapters: {} objects in the root set (+{})", rooted.size(), rooted.size() - before);
  }
}

// ---- content packages -----------------------------------------------------------------------------------------------

struct Request {
  std::string name;
  bool sent = false;
  uint32_t package = 0;
  std::chrono::steady_clock::time_point seen{};
  std::set<uint32_t> rooted;
  bool done = false;
};
std::vector<Request> g_requests;
std::mutex g_scanMutex;
std::vector<std::pair<size_t, uint32_t>> g_scanFound;  // (request, object)
std::atomic<bool> g_scanBusy{false}, g_scanDone{false};
bool g_contentOff = false, g_engineUp = false;
int g_frame = 0;

bool FlagsLayoutOk() {
  static int ok = -1;
  if (ok < 0) {
    const uint32_t cdo = obj::FindObject("Default__GearTypes");
    ok = cdo && (obj::Read32(cdo + kFlagsLow) & kClassDefault) ? 1 : 0;
    if (!ok) REXLOG_WARN("content packages: object flags not where expected; packages are loaded but not kept");
  }
  return ok == 1;
}

void Send(Request& r) {  // game thread
  r.sent = true;
  const uint32_t units = uint32_t(r.name.size()) + 1;
  const uint32_t str = Alloc(12), data = Alloc(2 * units);
  for (uint32_t i = 0; i < units; i++) Put16(data + 2 * i, uint16_t(i + 1 < units ? uint8_t(r.name[i]) : 0));
  obj::Write32(str, data);
  obj::Write32(str + 4, units);
  obj::Write32(str + 8, units);
  CallGuest(gow3::addr::kLoadPackageAsync, {str, 0, 0, 0, 0});
  REXLOG_INFO("content packages: loading {}", r.name);
}

// the package's own objects (Outer = the package: its ObjectReferencers reference everything it carries) into the root
// set; the object table walk runs on a thread of its own, the game thread only sets the flags found
void StartScan() {
  if (g_scanBusy.exchange(true)) return;
  std::vector<std::pair<size_t, uint32_t>> packages;
  for (size_t k = 0; k < g_requests.size(); k++)
    if (g_requests[k].package && !g_requests[k].done) packages.push_back({k, g_requests[k].package});
  std::thread([packages] {
    std::vector<std::pair<size_t, uint32_t>> found;
    for (uint32_t o : obj::AllObjects()) {
      const uint32_t outer = obj::Read32(o + kOuter);
      for (auto [k, p] : packages)
        if (outer == p) found.push_back({k, o});
    }
    {
      std::lock_guard lock(g_scanMutex);
      g_scanFound = std::move(found);
    }
    g_scanDone = true;
  }).detach();
}

void ApplyScan() {
  if (!g_scanDone.exchange(false)) return;
  std::vector<std::pair<size_t, uint32_t>> found;
  {
    std::lock_guard lock(g_scanMutex);
    found.swap(g_scanFound);
  }
  for (auto [k, o] : found) {
    Request& r = g_requests[k];
    if (r.rooted.count(o) || obj::Read32(o + kOuter) != r.package) continue;
    SetRoot(o, true);
    r.rooted.insert(o);
  }
  for (auto& r : g_requests)
    if (!r.rooted.empty()) REXLOG_INFO("content packages: {}: {} objects kept loaded", r.name, r.rooted.size());
  g_scanBusy = false;
}

// the requests sent at once (game thread): from the boot (FName hook), before any chapter's map can import from them
void SendContentPackages() {
  if (!REXCVAR_GET(gow3_mod_content_packages)) g_contentOff = true;
  if (g_contentOff) return;
  if (g_requests.empty())
    for (const std::string& n : ue3::ModContentPackages()) g_requests.push_back({n});
  if (g_requests.empty()) { g_contentOff = true; return; }
  for (auto& r : g_requests)
    if (!r.sent) Send(r);
}

void ContentPackagesTick() {  // game thread
  if (!REXCVAR_GET(gow3_mod_content_packages)) g_contentOff = true;
  if (g_contentOff) return;
  if (g_requests.empty()) {
    for (const std::string& n : ue3::ModContentPackages()) g_requests.push_back({n});
    if (g_requests.empty()) { g_contentOff = true; return; }
    REXLOG_INFO("content packages: {} requested", g_requests.size());
  }
  ApplyScan();
  if (++g_frame % 30 || !obj::Ready()) return;
  if (!g_engineUp) {
    g_engineUp = obj::FindObject("Default__GearTypes") != 0;
    if (!g_engineUp) return;
  }
  bool pending = false;
  for (auto& r : g_requests) {
    if (r.done) continue;
    pending = true;
    if (!r.sent) { Send(r); continue; }
    if (!r.package) {
      const uint32_t p = obj::FindObject(r.name.c_str());
      if (p && obj::ClassOf(p) == obj::FindClass("Package")) {
        r.package = p;
        r.seen = std::chrono::steady_clock::now();
        REXLOG_INFO("content packages: {} is package {:08X}", r.name, p);
      }
      continue;
    }
    if (g_frame % 120 == 0 && FlagsLayoutOk()) StartScan();
    if (std::chrono::steady_clock::now() - r.seen > std::chrono::seconds(60) && !g_scanBusy) r.done = true;
  }
  if (!pending && !g_scanBusy) g_contentOff = true;
}

void Worker() { ReadChapters(); }
}  // namespace

// OnPostSetup, after the mods' config is merged (Ue3InstallConfigEditsIfRequested)
void Gow3InstallModChapters() {
  if (REXCVAR_GET(gow3_mod_file_log))
    Ue3ShaderPrebuildChainFileOpened([](std::string_view guest, const std::filesystem::path* host) {
      if (guest.size() > 4 && (guest.ends_with(".xxx") || guest.ends_with(".XXX") || guest.ends_with(".txt")))
        REXLOG_INFO("mod files: opened {}{}", guest, host ? " <- " + rex::path_to_utf8(*host) : std::string());
    });
  if (!REXCVAR_GET(gow3_mod_chapters) || g_started.exchange(true)) return;
  std::thread(Worker).detach();
  std::thread(ChapterKeeper).detach();
  std::thread(CheckpointStreaming).detach();
  std::thread(TrilogyRoots).detach();
}

// The entry of FName::FName (gow3_manifest*.toml): runs on the game thread from the first moments of the boot, so the
// chapter table is in place before a chapter URL given at start looks its map up (the engine tick, below, only starts
// once the first map has loaded). The table's own names come through the same constructor: not re-entered.
void Gow3ModFName() {
  static thread_local bool busy = false;
  static std::atomic<uint32_t> calls{0};
  if (busy || g_tableDone || (g_chapters.empty() && ue3::ModContentPackages().empty()) || (++calls & 255)) return;
  busy = true;
  // the engine's tables and Default__GearTypes looked up here, on the game thread, as the boot goes (cheap once the
  // names read so far are cached): the table is filled the first time GearTypes' default object exists
  if (obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) {
    if (!g_types) g_types = obj::FindObject("Default__GearTypes");
    if (g_types && !g_tableDone.exchange(true)) {
      if (g_chapters.empty() || FillChapterTable()) {
        REXLOG_INFO("mod chapters: table filled at FName call {}", calls.load());
        SendContentPackages();
      } else {
        g_tableDone = false;   // (its properties not linked yet)
      }
    }
  }
  busy = false;
}

// Every engine tick, on the game thread (game_patches.cpp, the Gow3MaxTickRate hook)
void Gow3ModChaptersTick() {
  if (!g_started) return;
  if (g_restore.exchange(false)) RestoreSections();
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  if (!g_tableDone && !g_chapters.empty()) {
    if (!g_types) g_types = obj::FindObject("Default__GearTypes");
    if (g_types && !g_tableDone.exchange(true)) {
      if (FillChapterTable()) SendContentPackages();   // (before a chapter's map, which imports from them, can load)
      else g_tableDone = false;
    }
  }
  ContentPackagesTick();
}
