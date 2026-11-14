// Layer 3 (Gears of War 3): optional first-person camera (--gow3_first_person, off by default; the launcher's Gameplay
// tab "First-person camera (experimental)"). Our code (MIT).
//
// The game's own camera, moved to the eyes ("approach B"): the gameplay camera is GearGameplayCamera, one mode object
// per state (GameplayCam_Default, _Targeting, _RoadieRun, _Cover, _Execution, ...; GearGameplayCamera.FindBestCameraMode
// picks one each frame). Each mode puts the camera at its ViewOffset (OffsetHigh / OffsetMid / OffsetLow, chosen by
// the view pitch; X forward, Y right, Z up) from the camera origin. For the walking / crouching / aiming / roadie-run
// modes only, every live mode object gets all three offsets set to --gow3_first_person_offset (X,Y,Z), its split-screen
// and 4:3 adjustments (ViewOffset_ViewportAdjustments, every element) and its sideways strafe / run / big-head shifts zeroed, and
// optionally its FOVAngle (--gow3_first_person_fov). Everything else - cover and cover aim, blind fire, executions,
// chainsaw duels, DBNO, death, vehicles, turrets, the Silverback, meatbag, ziplines, conversations, cutscenes - keeps the
// game's own third-person camera, and the game blends between them as usual (cover = third person again).
// Why this works without engine code: the Longshot's scope mode (GameplayCam_SniperZoom) has no offset at all - an eye
// view is a state the camera code already supports; the fire trace starts from the camera (GearPawn.
// GetWeaponStartTraceLocation reads the player's view point), so the crosshair stays true; and the game hides a pawn
// whose camera is inside its CameraNoRenderCylinder (radius 40) - your own body is not drawn from the inside.
//
// Your own weapon (fpgun, agent062-out/fpgun-research.md): the camera's hide puts the pawn in the player controller's
// per-view HiddenActors list (GearSpectatorCamera.OnBecomeActive undoes exactly that: bIsHiddenByCamera = false,
// HiddenActors emptied). Every primitive attached to the pawn - the gun is attached to its mesh's hand socket
// (GearWeapon.PerformWeaponAttachment) - is left out of that view with it, unless the primitive has
// PrimitiveComponent.bIgnoreHiddenActorsMembership, the engine's own Gears-only exception (the Longshot / Hammer of
// Dawn aiming beams set it while you aim, GearWeap_HeavySniperBase.RecalculateLaserLocation, GearWeap_HODBase.Tick).
// --gow3_first_person_weapon (default on) sets that bit on the local player's current weapon mesh and its magazine,
// muzzle flash and shell-eject / barrel-smoke particles while a first-person mode is the camera's current mode, and
// clears it again on a weapon change, a third-person mode, PageUp off or a level change. --gow3_first_person_body
// (default on) does the same for the pawn's Mesh and its own primitives: the whole character - arms, shoulders, legs -
// is drawn as in third person (socket attachments such as a holstered gun or helmet stay as the game has them). The first-person modes also get bDoPredictiveAvoidance cleared, as the game's own eye view
// (GameplayCam_SniperZoom) has it. --gow3_first_person_diag logs the hide state once a second (log only).
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
REXCVAR_DEFINE_BOOL(gow3_first_person_weapon, true, "Gears3",
                    "First-person camera: draw your own weapon (the game hides it with your body when the camera is "
                    "inside you; this sets the engine's bIgnoreHiddenActorsMembership on the weapon's meshes). With "
                    "--gow3_first_person_body on, the weapon is drawn with the body either way");
REXCVAR_DEFINE_BOOL(gow3_first_person_body, true, "Gears3",
                    "First-person camera: draw your own body too (arms, shoulders, legs - the game's own model, already "
                    "animated; the same engine flag on your pawn's meshes). Off: only the weapon");
REXCVAR_DEFINE_BOOL(gow3_first_person_diag, false, "Gears3",
                    "First-person camera: log the local players' hide state once a second (pawn / weapon flags, "
                    "HiddenActors, camera and weapon positions) - log only, for tests");

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
  int avoid = -1;               // bDoPredictiveAvoidance (GameThirdPersonCameraMode bitfield word), cleared in FP
  uint32_t avoidMask = 0;
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
  if (const uint32_t m = obj::BoolMask(cls, "bDoPredictiveAvoidance"); m)
    if ((info.avoid = at("bDoPredictiveAvoidance", 4)) >= 0) info.avoidMask = m;
  REXLOG_INFO("first person: {} - ViewOffset +0x{:X}, viewport adjustments +0x{:X} x{}, FOVAngle +0x{:X}, {} shift(s) "
              "zeroed, bDoPredictiveAvoidance +0x{:X} mask {:08X}", n, info.viewOffset, info.viewportAdj,
              info.viewportCount, info.fovAngle, info.zero.size(), info.avoid, info.avoidMask);
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

// ---- single bits (read-modify-write of one bit only; the rest of a bitfield word stays the game's) ----------------
// A bit we changed is kept with the object's class; it is put back only while the object is still live with that
// class and the bit still holds our value (obj::IsLive: a freed object's memory is never written).
struct Bit { uint32_t object, cls, address, mask; bool set, weapon; };   // set = the value we wrote
std::unordered_map<uint32_t, Bit> g_bits;   // by address (game thread only)

void SetBit(uint32_t object, uint32_t address, uint32_t mask, bool set, bool weapon) {
  const uint32_t w = obj::Read32(address), cls = obj::ClassOf(object);
  if (((w & mask) != 0) == set) return;   // already so: ours from before, or the game's own value (nothing to own)
  if (!obj::Write32(address, set ? (w | mask) : (w & ~mask))) return;
  if (auto it = g_bits.find(address); it != g_bits.end() && it->second.object == object && it->second.cls == cls &&
                                      it->second.mask == mask)
    return;   // ours already (the game had put its value back): keep the record
  g_bits.insert_or_assign(address, Bit{object, cls, address, mask, set, weapon});   // a new object here: new record
}
void RestoreBit(const Bit& b) {
  if (!obj::IsLive(b.object) || obj::ClassOf(b.object) != b.cls) return;
  const uint32_t w = obj::Read32(b.address);
  if (((w & b.mask) != 0) == b.set) obj::Write32(b.address, b.set ? (w & ~b.mask) : (w | b.mask));
}
void RestoreBits(uint32_t object = 0) {   // 0 = all
  for (auto it = g_bits.begin(); it != g_bits.end();)
    if (!object || it->second.object == object) RestoreBit(it->second), it = g_bits.erase(it);
    else ++it;
}

// ---- reflection cache: property offset / bool mask per (class, property) --------------------------------------------
struct PropKey {
  uint32_t cls, name; const char* p;
  bool operator==(const PropKey& o) const { return cls == o.cls && name == o.name && p == o.p; }
};
struct PropHash { size_t operator()(const PropKey& k) const { return k.cls * 31u + k.name * 7u + size_t(k.p); } };
struct PropInfo { int offset = -1; uint32_t mask = 0; };
std::unordered_map<PropKey, PropInfo, PropHash> g_props;
PropInfo Prop(uint32_t cls, const char* p) {   // by value: the cache may be cleared
  if (!cls) return {};
  const PropKey k{cls, obj::Read32(cls + 44), p};
  if (auto it = g_props.find(k); it != g_props.end()) return it->second;
  if (g_props.size() > 4096) g_props.clear();
  PropInfo i;
  if (const int o = obj::PropertyOffset(cls, p); o >= 32 && o < 0x4000) i.offset = o, i.mask = obj::BoolMask(cls, p);
  return g_props[k] = i;
}
uint32_t Ptr(uint32_t object, const char* p) {   // an object property's value (0 when absent / unreadable)
  if (!object) return 0;
  const uint32_t cls = obj::ClassOf(object);
  const PropInfo i = Prop(cls, p);
  return i.offset >= 0 ? obj::Read32(object + uint32_t(i.offset)) : 0;
}
int Flag(uint32_t object, const char* p) {   // a bool property: 1 / 0, -1 when absent
  if (!object) return -1;
  const uint32_t cls = obj::ClassOf(object);
  if (!cls) return -1;
  const PropInfo i = Prop(cls, p);
  return i.offset >= 0 && i.mask ? int((obj::Read32(object + uint32_t(i.offset)) & i.mask) != 0) : -1;
}

// ---- the local players' cameras (GearPlayerCamera), found from the first-person mode objects in each pass ---------
// A mode object is made by its camera (new(self)): its Outer chain reaches the GearPlayerCamera, the object that has
// both PCOwner and GameplayCam. Up to 4 levels.
std::vector<uint32_t> g_cams, g_seenCams;
uint32_t g_camCls = 0;   // GearPlayerCamera (job 1850: the modes' Outer chain did not reach it - found directly)
// a local player's camera: its PCOwner's Player is a LocalPlayer (a co-op host also has the remote players' cameras)
bool LocalCamera(uint32_t cam) {
  if (!cam || !obj::IsLive(cam)) return false;
  const uint32_t pc = Ptr(cam, "PCOwner");
  if (!pc || !obj::IsLive(pc)) return false;
  if (Prop(obj::ClassOf(pc), "Player").offset < 0) return true;   // (not found: taken as local)
  const uint32_t player = Ptr(pc, "Player");
  return player && obj::ObjectName(obj::ClassOf(player)).find("LocalPlayer") != std::string::npos;
}
uint32_t CameraOf(uint32_t mode) {
  uint32_t o = mode;
  for (int i = 0; i < 4 && o; i++) {
    o = obj::Read32(o + 40);   // UObject::Outer
    const uint32_t cls = o ? obj::ClassOf(o) : 0;
    if (cls && Prop(cls, "PCOwner").offset >= 0 && Prop(cls, "GameplayCam").offset >= 0) return o;
  }
  return 0;
}

// the weapon's primitives: the mesh, its magazine, its muzzle flash and shell / smoke particles (all attached to the
// weapon mesh, so all hidden with the pawn); only objects whose class has bIgnoreHiddenActorsMembership are taken
constexpr const char* kWeaponParts[] = {"Mesh", "MagazineMesh", "PSC_ShellEject", "PSC_ReloadBarrelSmoke"};
std::vector<uint32_t> WeaponParts(uint32_t weapon) {
  std::vector<uint32_t> v;
  const auto add = [&](uint32_t c) {
    if (c && obj::IsLive(c) && obj::ClassOf(c) && Prop(obj::ClassOf(c), "bIgnoreHiddenActorsMembership").mask &&
        std::find(v.begin(), v.end(), c) == v.end())   // review r24 B1: only live objects
      v.push_back(c);
  };
  for (const char* p : kWeaponParts) add(Ptr(weapon, p));
  if (const uint32_t mf = Ptr(weapon, "MuzzleFlash"); mf && obj::IsLive(mf)) add(Ptr(mf, "MuzFlashEmitter"));
  return v;
}

// The pawn's own primitives (Shagg 11-14 15:03: the whole character is already drawn and animated in third person -
// in first person it is only hidden by the camera's HiddenActors list, like the weapon): the pawn's Mesh and every
// primitive in its Components array that has the flag (socket attachments such as a helmet are not in that list and
// stay as the game has them).
// The camera sits at the face; what is behind it (the inside of the head) is not drawn (back faces, behind the view).
std::vector<uint32_t> BodyParts(uint32_t pawn) {
  std::vector<uint32_t> v;
  const auto add = [&](uint32_t c) {
    if (c && obj::IsLive(c) && obj::ClassOf(c) && Prop(obj::ClassOf(c), "bIgnoreHiddenActorsMembership").mask &&
        std::find(v.begin(), v.end(), c) == v.end())
      v.push_back(c);
  };
  add(Ptr(pawn, "Mesh"));
  if (const PropInfo i = Prop(obj::ClassOf(pawn), "Components"); i.offset >= 0) {   // TArray: data, count, max
    const uint32_t data = obj::Read32(pawn + uint32_t(i.offset)), n = obj::Read32(pawn + uint32_t(i.offset) + 4);
    if (data && n <= 64 && obj::Readable(data, n * 4))
      for (uint32_t k = 0; k < n; k++) add(obj::Read32(data + 4 * k));
  }
  return v;
}

// Every engine tick while first person is on (a few dozen reads per local player): the current weapon of each local
// player whose camera is in a first-person mode gets bIgnoreHiddenActorsMembership; anything we set it on that is no
// longer such a part (weapon switched, dropped, holstered, cover / third-person mode, player gone) gets it back.
std::unordered_set<uint32_t> g_wantParts;
void WeaponTick() {
  static const bool weaponOn = REXCVAR_GET(gow3_first_person_weapon), bodyOn = REXCVAR_GET(gow3_first_person_body);
  if (!weaponOn && !bodyOn) return;
  g_wantParts.clear();
  for (uint32_t cam : g_cams) {
    if (!obj::IsLive(cam)) continue;
    // review r24 R1 / R2: only while the gameplay camera is the active one and its current mode is one of ours; a
    // property not found = nothing un-hidden (the earlier behaviour)
    const uint32_t gc = Ptr(cam, "GameplayCam");
    if (!gc || !obj::IsLive(gc) || Prop(obj::ClassOf(gc), "CurrentCamMode").offset < 0) continue;
    if (Prop(obj::ClassOf(cam), "CurrentCamera").offset >= 0 && Ptr(cam, "CurrentCamera") != gc) continue;
    if (!g_patched.count(Ptr(gc, "CurrentCamMode"))) continue;
    const uint32_t pc = Ptr(cam, "PCOwner");
    if (!pc || !obj::IsLive(pc)) continue;
    const uint32_t pawn = Ptr(pc, "MyGearPawn");
    std::vector<uint32_t> parts;
    if (bodyOn && pawn && obj::IsLive(pawn)) parts = BodyParts(pawn);
    if (const uint32_t weapon = Ptr(pawn, "MyGearWeapon"); (weaponOn || bodyOn) && weapon && obj::IsLive(weapon))
      for (uint32_t c : WeaponParts(weapon)) parts.push_back(c);
    for (uint32_t c : parts) {
      const PropInfo i = Prop(obj::ClassOf(c), "bIgnoreHiddenActorsMembership");
      if (i.offset < 0 || !i.mask) continue;
      g_wantParts.insert(c);
      SetBit(c, c + uint32_t(i.offset), i.mask, true, true);
    }
  }
  for (auto it = g_bits.begin(); it != g_bits.end();) {   // ours, no longer wanted (mode-object bits are kept)
    const Bit& b = it->second;
    if (b.weapon && !g_wantParts.count(b.object)) RestoreBit(b), it = g_bits.erase(it);
    else ++it;
  }
}

// ---- --gow3_first_person_diag: once a second, per local player, log only --------------------------------------------
void Vec(uint32_t a, float v[3]) { for (int i = 0; i < 3; i++) v[i] = obj::ReadFloat(a + 4 * i); }
std::string Parts(uint32_t c) {   // a primitive's hide-related flags and last render time
  if (!c) return "none";
  char b[200];
  std::snprintf(b, sizeof b, "%s HiddenGame %d OwnerNoSee %d OnlyOwnerSee %d IgnoreHiddenActors %d LastRender %.2f",
                obj::ObjectName(obj::ClassOf(c)).c_str(), Flag(c, "HiddenGame"), Flag(c, "bOwnerNoSee"),
                Flag(c, "bOnlyOwnerSee"), Flag(c, "bIgnoreHiddenActorsMembership"),
                Prop(obj::ClassOf(c), "LastRenderTime").offset >= 0
                    ? obj::ReadFloat(c + uint32_t(Prop(obj::ClassOf(c), "LastRenderTime").offset)) : -1.f);
  return b;
}
void DiagTick() {
  static const bool on = REXCVAR_GET(gow3_first_person_diag);
  if (!on) return;
  static Clock::time_point next{};
  if (Clock::now() < next) return;
  next = Clock::now() + std::chrono::seconds(1);
  if (g_cams.empty()) { REXLOG_INFO("first person diag: no camera yet"); return; }
  for (uint32_t cam : g_cams) {
    if (!obj::IsLive(cam)) continue;
    const uint32_t pc = Ptr(cam, "PCOwner"), pawn = Ptr(pc, "MyGearPawn"), weapon = Ptr(pawn, "MyGearWeapon");
    const uint32_t gc = Ptr(cam, "GameplayCam"), mode = Ptr(gc, "CurrentCamMode");
    const uint32_t wmesh = Ptr(weapon, "Mesh");
    const uint32_t wi = Ptr(pawn, "WorldInfo");
    const PropInfo ts = Prop(obj::ClassOf(wi), "TimeSeconds");
    const float now = wi && ts.offset >= 0 ? obj::ReadFloat(wi + uint32_t(ts.offset)) : -1.f;
    // PCOwner.HiddenActors (TArray: data, count): is the pawn / the weapon actor in it?
    int hidden = -1; bool pawnIn = false, weapIn = false;
    if (const PropInfo h = Prop(obj::ClassOf(pc), "HiddenActors"); pc && h.offset >= 0) {
      const uint32_t data = obj::Read32(pc + uint32_t(h.offset)), n = obj::Read32(pc + uint32_t(h.offset) + 4);
      hidden = int(std::min<uint32_t>(n, 9999));
      for (uint32_t i = 0; data && i < std::min<uint32_t>(n, 64); i++) {
        const uint32_t a = obj::Read32(data + 4 * i);
        pawnIn |= a && a == pawn, weapIn |= a && a == weapon;
      }
    }
    REXLOG_INFO("first person diag: cam {:08X} mode {} ({}) | pawn {} bIsHiddenByCamera {} | HiddenActors {} pawn {} "
                "weapon {} | time {:.2f}", cam, mode ? obj::ObjectName(obj::ClassOf(mode)) : "?",
                g_patched.count(mode) ? "FP" : "3P", pawn ? obj::ObjectName(obj::ClassOf(pawn)) : "none",
                Flag(pawn, "bIsHiddenByCamera"), hidden, pawnIn, weapIn, now);
    REXLOG_INFO("first person diag:   pawn mesh {} | weapon {} bHidden {} | weapon mesh {} (owner {}) | magazine {}",
                Parts(Ptr(pawn, "Mesh")), weapon ? obj::ObjectName(obj::ClassOf(weapon)) : "none", Flag(weapon, "bHidden"),
                Parts(wmesh), obj::ObjectName(obj::ClassOf(Ptr(wmesh, "Owner"))), Parts(Ptr(weapon, "MagazineMesh")));
    // camera (CameraCache.POV: TimeStamp, then Location, Rotation, FOV) vs the weapon mesh's bounds centre and the pawn
    const PropInfo cc = Prop(obj::ClassOf(cam), "CameraCache");
    const PropInfo bo = Prop(obj::ClassOf(wmesh), "Bounds");
    const PropInfo lo = Prop(obj::ClassOf(pawn), "Location");
    if (cc.offset < 0 || bo.offset < 0) continue;
    float cl[3], wl[3], pl[3] = {0, 0, 0};
    const uint32_t pov = cam + uint32_t(cc.offset) + 4;
    Vec(pov, cl);
    Vec(wmesh + uint32_t(bo.offset), wl);
    if (lo.offset >= 0) Vec(pawn + uint32_t(lo.offset), pl);
    const float k = 3.14159265f / 32768.f;
    const float pitch = float(int16_t(obj::Read32(pov + 12) & 0xFFFF)) * k, yaw = float(int32_t(obj::Read32(pov + 16))) * k;
    const float fov = obj::ReadFloat(pov + 24);
    const float fw[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const float rt[3] = {-std::sin(yaw), std::cos(yaw), 0};
    const float up[3] = {-std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
    const float d[3] = {wl[0] - cl[0], wl[1] - cl[1], wl[2] - cl[2]};
    const float f = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2], r = d[0] * rt[0] + d[1] * rt[1],
                u = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float deg = 180.f / 3.14159265f;
    const float halfH = fov * 0.5f, halfV = std::atan(std::tan(halfH / deg) * 9.f / 16.f) * deg;
    REXLOG_INFO("first person diag:   camera ({:.0f}, {:.0f}, {:.0f}) = pawn + ({:.0f}, {:.0f}, {:.0f}), FOV {:.0f} | "
                "weapon bounds centre in view space fwd {:.0f} right {:.0f} up {:.0f} -> {:.0f} deg right, {:.0f} deg up "
                "(16:9 half FOV {:.0f} x {:.0f}){}", cl[0], cl[1], cl[2], cl[0] - pl[0], cl[1] - pl[1], cl[2] - pl[2],
                fov, f, r, u, std::atan2(r, f) * deg, std::atan2(u, f) * deg, halfH, halfV,
                f > 0 && std::abs(std::atan2(r, f) * deg) < halfH && std::abs(std::atan2(u, f) * deg) < halfV
                    ? " IN VIEW" : " out of view");
  }
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

}  // namespace

// OnPostSetup: the hotkey (only with the option on).
void Gow3InstallFirstPerson() {
  if (!REXCVAR_GET(gow3_first_person)) return;
  ReadSettings();
  rex::ui::RegisterBind("bind_cogbox_first_person", "PageUp", "First-person camera off / on (Gears 3)", [] {
    // review r20 R1: a held key repeats - one switch per press (300 ms quiet before the next)
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (now - last.exchange(now, std::memory_order_relaxed) < 300) return;   // r21: every repeat restarts the wait
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
      g_applied = false;
      g_passActive = false;
      g_restoring = true;
      g_restoreCursor = 0;
      g_restored = 0;
      const size_t bits = g_bits.size();
      RestoreBits();   // weapon bIgnoreHiddenActorsMembership and modes' bDoPredictiveAvoidance (live objects only)
      g_cams.clear();
      REXLOG_INFO("first person: off - {} flag(s) back", bits);
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
  if (!obj::Ready()) {   // review r20: tried every 2 s, not every tick
    static Clock::time_point nextInit{};
    if (Clock::now() < nextInit) return;
    nextInit = Clock::now() + std::chrono::seconds(2);
    if (!obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  }
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
  WeaponTick();   // the local players' weapons: every tick (a weapon switch shows at once)
  DiagTick();     // --gow3_first_person_diag: once a second, log only
  if (!g_passActive) {
    const Clock::time_point now = Clock::now();
    if (now < g_nextPass) return;
    g_nextPass = now + kPassEvery;
    g_passActive = true;
    g_cursor = 0;
    g_seen.clear();
    g_seenCams.clear();
    ReadSettings();
    if (!g_camCls) {   // once per pass until found (a full-table lookup: at most every 2 s)
      static int tries = 0;
      if (tries++ < 30) g_camCls = obj::FindClass("GearPlayerCamera");
    }
  }
  uint32_t total = 0;
  for (uint32_t o : obj::FindInstancesOfRange(g_base, g_cursor, kSlice, &total)) {
    const uint32_t cls = obj::ClassOf(o);
    if (!cls) continue;
    const ClassInfo& info = InfoOf(cls);
    if (!info.fp || info.viewOffset < 0) continue;
    g_seen.insert(o);
    Apply(o, cls, info);
    if (info.avoid >= 0) SetBit(o, o + uint32_t(info.avoid), info.avoidMask, false, false);
    if (const uint32_t cam = CameraOf(o);
        cam && LocalCamera(cam) && std::find(g_seenCams.begin(), g_seenCams.end(), cam) == g_seenCams.end() &&
        g_seenCams.size() < 8)
      g_seenCams.push_back(cam);
  }
  if (g_camCls)   // the same slice of the table, for the cameras themselves
    for (uint32_t cam : obj::FindInstancesOfRange(g_camCls, g_cursor, kSlice, nullptr))
      if (LocalCamera(cam) && std::find(g_seenCams.begin(), g_seenCams.end(), cam) == g_seenCams.end() &&
          g_seenCams.size() < 8)
        g_seenCams.push_back(cam);
  g_cursor += kSlice;
  if (g_cursor < total) return;
  // pass over: forget objects no longer in the table (a level unloaded); log when the set changes
  g_passActive = false;
  const size_t before = g_patched.size();
  for (auto it = g_patched.begin(); it != g_patched.end();)
    it = g_seen.count(it->first) ? std::next(it) : g_patched.erase(it);
  for (auto it = g_bits.begin(); it != g_bits.end();)   // mode bits of objects gone: forgotten, never written
    it = !it->second.weapon && !g_seen.count(it->second.object) ? g_bits.erase(it) : std::next(it);
  if (g_seenCams != g_cams)
    REXLOG_INFO("first person: {} player camera(s) for the body / weapon{}", g_seenCams.size(),
                REXCVAR_GET(gow3_first_person_weapon) ? "" : " (weapon option off)");
  g_cams = g_seenCams;
  static size_t logged = size_t(-1);
  if (g_patched.size() != logged) {
    REXLOG_INFO("first person: {} camera mode(s) at eye level ({} dropped)", g_patched.size(), before - g_patched.size());
    logged = g_patched.size();
  }
}
