// Layer 3 (Gears of War 2): optional first-person camera (--gow2_first_person, off by default; the launcher's Gameplay
// tab "First-person camera (experimental)"). Our code (MIT).
//
// Gears3-Native's layer3_gow3/first_person.cpp with Gears 2's classes and object layout (ue3/objects.h, GitHub #245's
// port; the tables and the layout are found on a worker thread, as for the DualSense triggers).
// The game's own camera, moved to the eyes ("approach B"): the gameplay camera is GearGameplayCamera (GearCameraBase),
// one mode object per state (GameplayCam_Default, _Targeting, _RoadieRun, _Cover, _Mantle, ...; GearGameplayCamera.
// FindBestCameraMode picks one each frame). Each mode puts the camera at its ViewOffset (OffsetHigh / OffsetMid /
// OffsetLow, chosen by the view pitch; X forward, Y right, Z up) from the camera origin. For the walking / crouching /
// aiming / roadie-run modes only, every live mode object gets all three offsets set to --gow2_first_person_offset
// (X,Y,Z), its split-screen and 4:3 adjustments (ViewOffset_ViewportAdjustments[0..4]) and its sideways strafe / run
// shifts zeroed, and optionally its FOVAngle (--gow2_first_person_fov). Everything else - cover and cover aim, blind
// fire, executions, chainsaw duels, DBNO, death, mantling, vehicles, turrets, the Brumak, the shield, conversations,
// cutscenes - keeps the game's own third-person camera, and the game blends between them as usual (cover = third
// person again).
// Why this works without engine code: the Longshot's scope mode (GameplayCam_SniperZoom) has no offset at all - an eye
// view is a state the camera code already supports; and the game hides a pawn whose camera is inside its
// CameraNoRenderCylinder - your own body is not drawn from the inside (Gears 3's script shows both; for Gears 2 the
// same names are in GearGame.xxx, the behaviour is expected, not yet seen).
//
// No guest address, no hook of our own: reflection (ue3/objects.h) finds the GearGameplayCameraMode objects a slice of
// the object table per engine tick (16384 slots per tick), one pass every 2 s, so a level load's new camera (a new
// PlayerCamera actor) is patched within a few seconds and nothing ever scans the whole table in one frame. The
// original values of every word written are kept, so the hotkey (PageUp, bind_cogbox_first_person) puts the
// third-person camera back exactly and brings first person back. Off (the default): nothing is registered or read but
// one flag per tick.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/keybinds.h>

#include <ue3/objects.h>

#include "gow2_addresses.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(gow2_first_person, false, "Gears2",
                    "First-person camera (experimental): the game's own camera at eye level while walking, crouching, "
                    "aiming and roadie running; cover, executions, vehicles, turrets and cutscenes stay third person. "
                    "PageUp switches it off and on in the game (bind_cogbox_first_person)");
REXCVAR_DEFINE_STRING(gow2_first_person_offset, "8,0,0", "Gears2",
                      "First-person camera: where the camera sits from the game's camera origin, 'forward,right,up' "
                      "(game units; read again every 2 s, so it can be tuned from the SDK console)");
REXCVAR_DEFINE_INT32(gow2_first_person_fov, 0, "Gears2",
                     "First-person camera: field of view while walking / crouching (0 = the game's own or the launcher's "
                     "FOV setting; aiming and roadie run keep theirs)");

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

// The modes moved to the eyes (exact class names, Gears 2's GearGame script; a subclass not listed stays as it is).
// fov: --gow2_first_person_fov applies (aiming keeps its zoom, roadie run its wider view).
struct Rule { const char* cls; bool fov; };
constexpr Rule kModes[] = {
    {"GameplayCam_Default", true}, {"GameplayCam_Crouch", true}, {"GameplayCam_Targeting", false},
    {"GameplayCam_TargetingGrenade", false}, {"GameplayCam_RoadieRun", false},
};
// sideways / forward shifts the camera adds while strafing or running (Default: strafe +-15 right, run +20 / -30
// forward [V package defaults]): zeroed in first person (12-byte vectors; a name the class doesn't have is skipped)
constexpr const char* kZeroVectors[] = {"StrafeLeftAdjustment", "StrafeRightAdjustment", "RunFwdAdjustment",
                                        "RunBackAdjustment"};
constexpr uint32_t kSlice = 16384;        // object table slots per engine tick while a pass runs
constexpr auto kPassEvery = std::chrono::seconds(2);

uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float Float(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct ClassInfo {
  uint32_t name = 0;   // the class's name index (a class freed with its package and the address reused: looked up again)
  bool fp = false, fov = false;
  int viewOffset = -1, viewportAdj = -1, fovAngle = -1;
  uint32_t viewportCount = 0;   // ViewOffset_ViewportAdjustments' array length, read from the property (review r20 R2)
  std::vector<int> zero;
};
std::unordered_map<uint32_t, ClassInfo> g_classes;

struct Word { uint32_t address, original, written; };
struct Patched { uint32_t cls; std::vector<Word> words; };
std::unordered_map<uint32_t, Patched> g_patched;   // mode object -> what was written (game thread only)

std::atomic<bool> g_want{true};   // the hotkey's state
bool g_applied = true;
uint32_t g_base = 0, g_cursor = 0, g_classCursor = 0;
bool g_passActive = false;
Clock::time_point g_nextPass{};
std::unordered_set<uint32_t> g_seen;   // mode objects met in this pass
float g_off[3] = {8, 0, 0};
int g_fov = 0;

void ReadSettings() {
  float x = 0, y = 0, z = 0;
  const std::string s = REXCVAR_GET(gow2_first_person_offset);
  if (std::sscanf(s.c_str(), "%f , %f , %f", &x, &y, &z) == 3 && std::abs(x) < 400 && std::abs(y) < 400 &&
      std::abs(z) < 400) {
    if (x != g_off[0] || y != g_off[1] || z != g_off[2])
      REXLOG_INFO("first person: camera offset ({}, {}, {})", x, y, z);
    g_off[0] = x, g_off[1] = y, g_off[2] = z;
  }
  g_fov = std::clamp(REXCVAR_GET(gow2_first_person_fov), 0, 170);
}

const ClassInfo& InfoOf(uint32_t cls) {
  const uint32_t name = obj::Read32(cls + obj::CurrentLayout().name);
  if (auto it = g_classes.find(cls); it != g_classes.end() && it->second.name == name) return it->second;
  if (g_classes.size() > 512) g_classes.clear();
  ClassInfo& info = g_classes[cls];
  info = ClassInfo{};
  info.name = name;
  const std::string n = obj::ObjectName(cls);
  for (const Rule& r : kModes)
    if (n == r.cls) info.fp = true, info.fov = r.fov;
  if (!info.fp) return info;
  // review r20 R3: a property is used only at a sane place in the object (past the UObject header, within 8 KB)
  const auto at = [&](const char* p, int size, int dim = 1) {
    if (obj::PropertySize(cls, p) != size) return -1;
    const int o = obj::PropertyOffset(cls, p);
    return o >= 32 && o + size * dim <= 0x2000 ? o : -1;
  };
  info.viewOffset = at("ViewOffset", 36);
  if (const int dim = obj::PropertyArrayDim(cls, "ViewOffset_ViewportAdjustments"); dim >= 1 && dim <= 16)
    if ((info.viewportAdj = at("ViewOffset_ViewportAdjustments", 36, dim)) >= 0) info.viewportCount = uint32_t(dim);
  info.fovAngle = at("FOVAngle", 4);
  for (const char* v : kZeroVectors)
    if (const int o = at(v, 12); o >= 0) info.zero.push_back(o);
  REXLOG_INFO("first person: {} - ViewOffset +0x{:X}, viewport adjustments +0x{:X} x{}, FOVAngle +0x{:X}, {} shift(s) "
              "zeroed", n, info.viewOffset, info.viewportAdj, info.viewportCount, info.fovAngle, info.zero.size());
  if (info.viewOffset < 0) REXLOG_WARN("first person: {} has no ViewOffset - left as it is", n);
  return info;
}

// the words first person wants in this mode object now
std::vector<std::pair<uint32_t, uint32_t>> Wanted(uint32_t o, const ClassInfo& info) {
  std::vector<std::pair<uint32_t, uint32_t>> w;
  for (int i = 0; i < 9; i++) w.push_back({o + uint32_t(info.viewOffset) + 4 * i, Bits(g_off[i % 3])});
  if (info.viewportAdj >= 0)
    for (uint32_t i = 0; i < info.viewportCount * 9; i++) w.push_back({o + uint32_t(info.viewportAdj) + 4 * i, 0});
  for (int z : info.zero)
    for (int i = 0; i < 3; i++) w.push_back({o + uint32_t(z) + 4 * i, 0});
  if (info.fov && g_fov > 0 && info.fovAngle >= 0) w.push_back({o + uint32_t(info.fovAngle), Bits(float(g_fov))});
  return w;
}

void Apply(uint32_t o, uint32_t cls, const ClassInfo& info) {
  const auto want = Wanted(o, info);
  // review r20 B2: a word that still holds what we wrote keeps its real original (re-reading it would take our value
  // as the original and third person could never come back); a word the game has changed since gets the new value
  std::unordered_map<uint32_t, Word> prev;
  if (auto it = g_patched.find(o); it != g_patched.end()) {
    if (it->second.cls == cls) {   // else a new object at this address: everything read afresh
      bool same = it->second.words.size() == want.size();
      for (size_t i = 0; same && i < want.size(); i++)
        same = it->second.words[i].address == want[i].first && it->second.words[i].written == want[i].second &&
               obj::Read32(want[i].first) == want[i].second;
      if (same) return;
      for (const Word& w : it->second.words)
        if (obj::Read32(w.address) == w.written) prev[w.address] = w;
    }
    g_patched.erase(it);
  }
  for (const auto& [address, w] : prev)   // a word no longer written (e.g. the FOV switch set back to 0): original back
    if (std::none_of(want.begin(), want.end(), [a = address](const auto& x) { return x.first == a; }))
      obj::Write32(address, w.original);
  Patched p{cls, {}};
  p.words.reserve(want.size());
  for (const auto& [address, value] : want) {
    const auto pv = prev.find(address);
    p.words.push_back({address, pv != prev.end() ? pv->second.original : obj::Read32(address), value});
  }
  for (Word& w : p.words)
    if (!obj::Write32(w.address, w.written)) w.written = w.original;
  static std::unordered_set<uint32_t> logged;   // the first object of each class (by name index)
  if (logged.insert(info.name).second) {
    const Word* v = p.words.data();
    REXLOG_INFO("first person: {} {:08X} - offsets High ({}, {}, {}) Mid ({}, {}, {}) Low ({}, {}, {}) -> ({}, {}, {})",
                obj::ObjectName(cls), o, Float(v[0].original), Float(v[1].original), Float(v[2].original),
                Float(v[3].original), Float(v[4].original), Float(v[5].original), Float(v[6].original),
                Float(v[7].original), Float(v[8].original), g_off[0], g_off[1], g_off[2]);
  }
  g_patched[o] = std::move(p);
}

// Off again (the hotkey): review r20 B1 - only objects still in the engine's object table are written. A mode object
// freed by a level load can stay in g_patched until a pass ends without it, and the garbage collector does not clear
// freed memory, so its class word and our values can still be there (or a new allocation can be); the table is the
// only proof of life. A slice per tick like the patching pass (~11 ticks), objects not met are dropped unwritten.
bool g_restoring = false;
uint32_t g_restoreCursor = 0;
size_t g_restored = 0;

void RestoreSlice() {
  uint32_t total = 0;
  if (g_base && !g_patched.empty()) {
    for (uint32_t o : obj::FindInstancesOfRange(g_base, g_restoreCursor, kSlice, &total)) {
      const auto it = g_patched.find(o);
      if (it == g_patched.end() || obj::ClassOf(o) != it->second.cls) continue;
      for (const Word& w : it->second.words)
        if (obj::Read32(w.address) == w.written) obj::Write32(w.address, w.original);
      g_patched.erase(it);
      g_restored++;
    }
    g_restoreCursor += kSlice;
    if (g_restoreCursor < total) return;
  }
  REXLOG_INFO("first person: off - {} camera mode(s) back to the game's third-person offsets ({} gone)", g_restored,
              g_patched.size());
  g_patched.clear();
  g_restoring = false;
}

// The tables and the layout, on a worker thread (as adaptive_triggers.cpp: obj::Init is guarded, so the two can run
// together): every 2 s while the engine starts, then every 15 s; the first class lookup warms the name cache there.
std::atomic<bool> g_objects_warm{false};
void StartObjectsInit() {
  std::thread([] {
    for (int attempt = 0; attempt < 60 && !obj::Ready(); attempt++) {
      if (obj::Init(gow2::addr::kGNamesVar, gow2::addr::kGObjectsVar)) break;
      std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    }
    if (!obj::Ready()) {
      REXLOG_WARN("first person: the engine's object tables were not found - the camera is left as it is");
      return;
    }
    obj::FindClass("BoolProperty");   // warms the name / class caches off the game thread
    g_objects_warm.store(true, std::memory_order_release);
  }).detach();
}

}  // namespace

// OnPostSetup: the hotkey and the object tables (only with the option on).
void Gow2InstallFirstPerson() {
  if (!REXCVAR_GET(gow2_first_person)) return;
  ReadSettings();
  rex::ui::RegisterBind("bind_cogbox_first_person", "PageUp", "First-person camera off / on (Gears 2)", [] {
    // review r20 R1: a held key repeats - one switch per 300 ms
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (now - last.load(std::memory_order_relaxed) < 300) return;
    last.store(now, std::memory_order_relaxed);
    g_want.store(!g_want.load(std::memory_order_relaxed), std::memory_order_relaxed);
  });
  REXLOG_INFO("first person: on (experimental) - walking, crouching, aiming and roadie run at eye level; PageUp toggles");
  StartObjectsInit();
}

// Game thread, every engine tick (cheats.cpp, the UEngine::GetMaxTickRate override). Off: one flag test.
void Gow2FirstPersonTick() {
  static const bool enabled = REXCVAR_GET(gow2_first_person);   // read once (a launcher restart changes it)
  if (!enabled) return;
  const bool want = g_want.load(std::memory_order_relaxed);
  if (!want) {
    if (g_applied) {
      g_applied = false;
      g_passActive = false;
      g_restoring = true;
      g_restoreCursor = 0;
      g_restored = 0;
    }
    if (g_restoring) RestoreSlice();
    return;
  }
  if (!g_applied) {
    g_applied = true;
    g_restoring = false;   // on again before the restore finished: the objects not yet restored still hold our values
    g_nextPass = Clock::time_point{};   // first person again: a pass right away
    REXLOG_INFO("first person: on again");
  }
  if (!obj::Ready() || !g_objects_warm.load(std::memory_order_acquire)) return;
  if (!g_base) {   // the mode base class, a slice of the table per tick (it is in GearGame.xxx, loaded at start-up)
    static Clock::time_point next{};
    if (Clock::now() < next) return;
    next = Clock::now() + std::chrono::milliseconds(100);
    uint32_t total = 0;
    g_base = obj::FindClassRange("GearGameplayCameraMode", g_classCursor, kSlice, &total);
    g_classCursor = g_classCursor + kSlice >= total ? 0 : g_classCursor + kSlice;
    if (g_base) REXLOG_INFO("first person: GearGameplayCameraMode found ({})", gow2::addr::kVersion);
    return;
  }
  if (!g_passActive) {
    const Clock::time_point now = Clock::now();
    if (now < g_nextPass) return;
    g_nextPass = now + kPassEvery;
    g_passActive = true;
    g_cursor = 0;
    g_seen.clear();
    ReadSettings();
  }
  uint32_t total = 0;
  for (uint32_t o : obj::FindInstancesOfRange(g_base, g_cursor, kSlice, &total)) {
    const uint32_t cls = obj::ClassOf(o);
    if (!cls) continue;
    const ClassInfo& info = InfoOf(cls);
    if (!info.fp || info.viewOffset < 0) continue;
    g_seen.insert(o);
    Apply(o, cls, info);
  }
  g_cursor += kSlice;
  if (g_cursor < total) return;
  // pass over: forget objects no longer in the table (a level unloaded); log when the set changes
  g_passActive = false;
  const size_t before = g_patched.size();
  for (auto it = g_patched.begin(); it != g_patched.end();)
    it = g_seen.count(it->first) ? std::next(it) : g_patched.erase(it);
  static size_t logged = size_t(-1);
  if (g_patched.size() != logged) {
    REXLOG_INFO("first person: {} camera mode(s) at eye level ({} dropped)", g_patched.size(), before - g_patched.size());
    logged = g_patched.size();
  }
}
