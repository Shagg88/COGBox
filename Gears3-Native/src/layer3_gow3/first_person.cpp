// Layer 3 (Gears of War 3): optional first-person camera (--gow3_first_person, off by default; the launcher's Gameplay
// tab "First-person camera (experimental)"). Our code (MIT).
//
// The game's own camera, moved to the eyes ("approach B"): the gameplay camera is GearGameplayCamera, one mode object
// per state (GameplayCam_Default, _Targeting, _RoadieRun, _Cover, _Execution, ...; GearGameplayCamera.FindBestCameraMode
// picks one each frame). Each mode puts the camera at its ViewOffset (OffsetHigh / OffsetMid / OffsetLow, chosen by
// the view pitch; X forward, Y right, Z up) from the camera origin. For the walking / crouching / aiming / roadie-run
// modes only, every live mode object gets all three offsets set to --gow3_first_person_offset (X,Y,Z), its split-screen
// and 4:3 adjustments (ViewOffset_ViewportAdjustments[0..4]) and its sideways strafe / run / big-head shifts zeroed, and
// optionally its FOVAngle (--gow3_first_person_fov). Everything else - cover and cover aim, blind fire, executions,
// chainsaw duels, DBNO, death, vehicles, turrets, the Silverback, meatbag, ziplines, conversations, cutscenes - keeps the
// game's own third-person camera, and the game blends between them as usual (cover = third person again).
// Why this works without engine code: the Longshot's scope mode (GameplayCam_SniperZoom) has no offset at all - an eye
// view is a state the camera code already supports; the fire trace starts from the camera (GearPawn.
// GetWeaponStartTraceLocation reads the player's view point), so the crosshair stays true; and the game hides a pawn
// whose camera is inside its CameraNoRenderCylinder (radius 40) - your own body is not drawn from the inside.
//
// No guest address, no hook of our own: reflection (ue3/objects.h) finds the GearGameplayCameraMode objects a slice of
// the object table per engine tick (16384 slots; a pass over ~168,000 objects takes ~11 ticks), one pass every 2 s, so
// a level load's new camera (a new PlayerCamera actor) is patched within a few seconds and nothing ever scans the whole
// table in one frame. The original values of every word written are kept, so the hotkey (PageUp, bind_cogbox_first_
// person) puts the third-person camera back exactly and brings first person back. Off (the default): nothing is
// registered or read but one flag per tick.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/keybinds.h>

#include <ue3/objects.h>

#include "gow3_addresses.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(gow3_first_person, false, "Gears3",
                    "First-person camera (experimental): the game's own camera at eye level while walking, crouching, "
                    "aiming and roadie running; cover, executions, vehicles, turrets and cutscenes stay third person. "
                    "PageUp switches it off and on in the game (bind_cogbox_first_person)");
REXCVAR_DEFINE_STRING(gow3_first_person_offset, "8,0,0", "Gears3",
                      "First-person camera: where the camera sits from the game's camera origin, 'forward,right,up' "
                      "(game units; read again every 2 s, so it can be tuned from the SDK console)");
REXCVAR_DEFINE_INT32(gow3_first_person_fov, 0, "Gears3",
                     "First-person camera: field of view while walking / crouching (0 = the game's own or the launcher's "
                     "FOV setting; aiming and roadie run keep theirs)");

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

// The modes moved to the eyes (exact class names, Gears 3's GearGame script; a subclass not listed stays as it is).
// fov: --gow3_first_person_fov applies (aiming keeps its zoom, roadie run its wider view).
struct Rule { const char* cls; bool fov; };
constexpr Rule kModes[] = {
    {"GameplayCam_Default", true},  {"GameplayCam_Crouch", true},           {"GameplayCam_Targeting", false},
    {"GameplayCam_TargetingGrenade", false}, {"GameplayCam_TargetingVisualAimAssist", false},
    {"GameplayCam_RoadieRun", false},
};
// sideways / forward shifts the camera adds while strafing, running or with the big-head mutator: zeroed in first
// person (12-byte vectors; a name the class doesn't have is skipped)
constexpr const char* kZeroVectors[] = {"StrafeLeftAdjustment", "StrafeRightAdjustment", "RunFwdAdjustment",
                                        "RunBackAdjustment", "BigHeadViewOffsetAdjustment"};
constexpr uint32_t kViewportTypes = 5;    // ViewOffset_ViewportAdjustments[0..4]: [4] is in the defaults [V]
constexpr uint32_t kSlice = 16384;        // object table slots per engine tick while a pass runs
constexpr auto kPassEvery = std::chrono::seconds(2);

uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float Float(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct ClassInfo {
  uint32_t name = 0;   // the class's name index (a class freed with its package and the address reused: looked up again)
  bool fp = false, fov = false;
  int viewOffset = -1, viewportAdj = -1, fovAngle = -1;
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
  const std::string s = REXCVAR_GET(gow3_first_person_offset);
  if (std::sscanf(s.c_str(), "%f , %f , %f", &x, &y, &z) == 3 && std::abs(x) < 400 && std::abs(y) < 400 &&
      std::abs(z) < 400) {
    if (x != g_off[0] || y != g_off[1] || z != g_off[2])
      REXLOG_INFO("first person: camera offset ({}, {}, {})", x, y, z);
    g_off[0] = x, g_off[1] = y, g_off[2] = z;
  }
  g_fov = std::clamp(REXCVAR_GET(gow3_first_person_fov), 0, 170);
}

const ClassInfo& InfoOf(uint32_t cls) {
  const uint32_t name = obj::Read32(cls + 44);
  if (auto it = g_classes.find(cls); it != g_classes.end() && it->second.name == name) return it->second;
  if (g_classes.size() > 512) g_classes.clear();
  ClassInfo& info = g_classes[cls];
  info = ClassInfo{};
  info.name = name;
  const std::string n = obj::ObjectName(cls);
  for (const Rule& r : kModes)
    if (n == r.cls) info.fp = true, info.fov = r.fov;
  if (!info.fp) return info;
  if (obj::PropertySize(cls, "ViewOffset") == 36) info.viewOffset = obj::PropertyOffset(cls, "ViewOffset");
  if (obj::PropertySize(cls, "ViewOffset_ViewportAdjustments") == 36)
    info.viewportAdj = obj::PropertyOffset(cls, "ViewOffset_ViewportAdjustments");
  if (obj::PropertySize(cls, "FOVAngle") == 4) info.fovAngle = obj::PropertyOffset(cls, "FOVAngle");
  for (const char* v : kZeroVectors)
    if (obj::PropertySize(cls, v) == 12)
      if (const int o = obj::PropertyOffset(cls, v); o >= 0) info.zero.push_back(o);
  REXLOG_INFO("first person: {} - ViewOffset +0x{:X}, viewport adjustments +0x{:X}, FOVAngle +0x{:X}, {} shift(s) zeroed",
              n, info.viewOffset, info.viewportAdj, info.fovAngle, info.zero.size());
  if (info.viewOffset < 0) REXLOG_WARN("first person: {} has no ViewOffset - left as it is", n);
  return info;
}

// the words first person wants in this mode object now
std::vector<std::pair<uint32_t, uint32_t>> Wanted(uint32_t o, const ClassInfo& info) {
  std::vector<std::pair<uint32_t, uint32_t>> w;
  for (int i = 0; i < 9; i++) w.push_back({o + uint32_t(info.viewOffset) + 4 * i, Bits(g_off[i % 3])});
  if (info.viewportAdj >= 0)
    for (uint32_t i = 0; i < kViewportTypes * 9; i++) w.push_back({o + uint32_t(info.viewportAdj) + 4 * i, 0});
  for (int z : info.zero)
    for (int i = 0; i < 3; i++) w.push_back({o + uint32_t(z) + 4 * i, 0});
  if (info.fov && g_fov > 0 && info.fovAngle >= 0) w.push_back({o + uint32_t(info.fovAngle), Bits(float(g_fov))});
  return w;
}

void Apply(uint32_t o, uint32_t cls, const ClassInfo& info) {
  const auto want = Wanted(o, info);
  if (auto it = g_patched.find(o); it != g_patched.end()) {
    bool intact = it->second.cls == cls;   // still holding what we wrote? (else a new object at this address)
    for (size_t i = 0; intact && i < it->second.words.size(); i++)
      intact = obj::Read32(it->second.words[i].address) == it->second.words[i].written;
    if (intact) {
      bool same = it->second.words.size() == want.size();
      for (size_t i = 0; same && i < want.size(); i++)
        same = it->second.words[i].address == want[i].first && it->second.words[i].written == want[i].second;
      if (same) return;
      for (const Word& w : it->second.words) obj::Write32(w.address, w.original);   // new settings: from the originals
    }
    g_patched.erase(it);
  }
  Patched p{cls, {}};
  p.words.reserve(want.size());
  for (const auto& [address, value] : want) p.words.push_back({address, obj::Read32(address), value});
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

void RestoreAll() {
  size_t n = 0;
  for (auto& [o, p] : g_patched) {
    if (obj::ClassOf(o) != p.cls) continue;   // freed since
    for (const Word& w : p.words)
      if (obj::Read32(w.address) == w.written) obj::Write32(w.address, w.original);
    n++;
  }
  g_patched.clear();
  REXLOG_INFO("first person: off - {} camera mode(s) back to the game's third-person offsets", n);
}

}  // namespace

// OnPostSetup: the hotkey (only with the option on).
void Gow3InstallFirstPerson() {
  if (!REXCVAR_GET(gow3_first_person)) return;
  ReadSettings();
  rex::ui::RegisterBind("bind_cogbox_first_person", "PageUp", "First-person camera off / on (Gears 3)", [] {
    g_want.store(!g_want.load(std::memory_order_relaxed), std::memory_order_relaxed);
  });
  REXLOG_INFO("first person: on (experimental) - walking, crouching, aiming and roadie run at eye level; PageUp toggles");
}

// Game thread, every engine tick (game_patches.cpp Gow3MaxTickRate). Off: one flag test.
void Gow3FirstPersonTick() {
  static const bool enabled = REXCVAR_GET(gow3_first_person);   // read once (a launcher restart changes it)
  if (!enabled) return;
  const bool want = g_want.load(std::memory_order_relaxed);
  if (!want) {
    if (g_applied) {
      RestoreAll();
      g_applied = false;
      g_passActive = false;
    }
    return;
  }
  if (!g_applied) {
    g_applied = true;
    g_nextPass = Clock::time_point{};   // first person again: a pass right away
    REXLOG_INFO("first person: on again");
  }
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  if (!g_base) {   // the mode base class, a slice of the table per tick (it is in GearGame.xxx, loaded at start-up)
    static Clock::time_point next{};
    if (Clock::now() < next) return;
    next = Clock::now() + std::chrono::milliseconds(100);
    uint32_t total = 0;
    g_base = obj::FindClassRange("GearGameplayCameraMode", g_classCursor, kSlice, &total);
    g_classCursor = g_classCursor + kSlice >= total ? 0 : g_classCursor + kSlice;
    if (g_base) REXLOG_INFO("first person: GearGameplayCameraMode found ({})", gow3::addr::kVersion);
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
