// Layer 3 (Gears of War 3): optional first-person camera (--gow3_first_person, off by default; the launcher's Gameplay
// tab "First-person camera (experimental)"). Our code (MIT).
//
// The game's own camera, moved to the eyes ("approach B"): the gameplay camera is GearGameplayCamera, one mode object
// per state (GameplayCam_Default, _Targeting, _RoadieRun, _Cover, _Execution, ...; GearGameplayCamera.FindBestCameraMode
// picks one each frame). Each mode puts the camera at its ViewOffset (OffsetHigh / OffsetMid / OffsetLow, chosen by
// the view pitch; X forward, Y right, Z up) from the camera origin. For the walking / crouching / aiming
// modes only, every live mode object gets all three offsets set to --gow3_first_person_offset (X,Y,Z), its split-screen
// and 4:3 adjustments (ViewOffset_ViewportAdjustments, every element) and its sideways strafe / run / big-head shifts zeroed, and
// optionally its FOVAngle (--gow3_first_person_fov). Everything else - cover and cover aim, blind fire, executions,
// chainsaw duels, DBNO, death, vehicles, turrets, the Silverback, meatbag, ziplines, conversations, cutscenes - keeps the
// game's own third-person camera, and the game blends between them as usual (cover = third person again). Roadie run
// and the like (the dash and slide into cover, the roadie-run stabs) are third person too, by the pawn's own
// GearPawn.SpecialMove: that player's modes get the game's offsets back while the move lasts (AsideTick).
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
// (GameplayCam_SniperZoom) has it. With the body drawn, your own head (--gow3_first_person_head, default off = hidden)
// is collapsed by the game's big-head skeletal control (HeadScaler, BoneScale 0.01) for a single local player, and put
// back exactly when first person ends. --gow3_first_person_diag logs the hide state once a second (log only).
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
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(gow3_first_person, false, "Gears3",
                    "First-person camera (experimental): the game's own camera at eye level while walking, crouching, "
                    "aiming; roadie run, cover, executions, vehicles, turrets and cutscenes stay third person. "
                    "PageUp switches it off and on in the game (bind_cogbox_first_person)");
REXCVAR_DEFINE_STRING(gow3_first_person_offset, "8,0,0", "Gears3",
                      "First-person camera: where the camera sits from the game's camera origin, 'forward,right,up' "
                      "(game units; read again every 2 s, so it can be tuned from the SDK console)");
REXCVAR_DEFINE_INT32(gow3_first_person_fov, 0, "Gears3",
                     "First-person camera: field of view while walking / crouching (0 = the game's own or the launcher's "
                     "FOV setting; aiming keeps its zoom)");
REXCVAR_DEFINE_BOOL(gow3_first_person_weapon, true, "Gears3",
                    "First-person camera: draw your own weapon (the game hides it with your body when the camera is "
                    "inside you; this sets the engine's bIgnoreHiddenActorsMembership on the weapon's meshes). With "
                    "--gow3_first_person_body on, the weapon is drawn with the body either way");
REXCVAR_DEFINE_BOOL(gow3_first_person_body, true, "Gears3",
                    "First-person camera: draw your own body too (arms, shoulders, legs - the game's own model, already "
                    "animated; the same engine flag on your pawn's meshes). Off: only the weapon");
REXCVAR_DEFINE_BOOL(gow3_first_person_head, false, "Gears3",
                    "First-person camera: draw your own head too (off, the default: hidden while the body is drawn in "
                    "first person - the game's own big-head control, HeadScaler, set to bone scale 0; one local player "
                    "only, as the scale shows in every view)");
REXCVAR_DEFINE_STRING(gow3_first_person_third_person_moves, "", "Gears3",
                      "First-person camera, for tests: more special moves (GearPawn ESpecialMove names, comma separated, "
                      "e.g. SM_EvadeFwd,SM_EvadeBwd,SM_EvadeLt,SM_EvadeRt) during which the camera goes back to "
                      "third person, as it does for roadie run, the dash and slide into cover and the roadie-run "
                      "stabs");
REXCVAR_DEFINE_BOOL(gow3_first_person_diag, false, "Gears3",
                    "First-person camera: log the local players' hide state once a second (pawn / weapon flags, "
                    "HiddenActors, camera and weapon positions) - log only, for tests");

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

// The modes moved to the eyes (exact class names, Gears 3's GearGame script; a subclass not listed stays as it is).
// fov: --gow3_first_person_fov applies (aiming keeps its zoom).
struct Rule { const char* cls; bool fov; };
constexpr Rule kModes[] = {
    {"GameplayCam_Default", true},  {"GameplayCam_Crouch", true},           {"GameplayCam_Targeting", false},
    {"GameplayCam_TargetingGrenade", false}, {"GameplayCam_TargetingVisualAimAssist", false},
    // roadie run stays third person (Shagg 11-14 16:26: a first-person roadie run would make people sick)
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
  if (Prop(obj::ClassOf(pc), "Player").offset < 0) return false;   // (not found: not taken - r29 R2)
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
// The camera sits inside the head: PC job 1852 showed hair / bandana / the inside of the head at the screen edges, so
// the head is collapsed separately (HeadTick, --gow3_first_person_head).
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

// ---- your own head (--gow3_first_person_head off, the default; agent062-out/fphead-research.md) ------------------
// With the body drawn, the camera sits inside the head: hair, bandana and the inside of the skull cut into the screen
// edges. The game has a skeletal control made to resize exactly that: the big-head mutator's 'HeadScaler' in the
// pawn's AnimTree (GearPawn.SetHeadScale: HeadControl = SkelControlSingleBone(Mesh.FindSkelControl('HeadScaler'));
// SetSkelControlStrength(1, 0); BoneScale = HeadScale). SkelControlBase.BoneScale scales the bone and every child
// bone (UDK docs), so a scale of 0 collapses the head, its hair and headgear skinned to it into the neck. Nothing
// else changes: GearPawn.HeadScale (read by the headshot death and the big-head voice) and the WeaponScaler control
// (which SetHeadScale would also change) are not touched; no script is called.
// Found by reflection from the pawn's Mesh: Mesh.Animations (the AnimTree instance) -> SkelControlLists (one
// SkelControlListHead per bone: BoneName, ControlHead, DrawY) -> ControlHead -> NextControl chain, by ControlName.
// Written while one local player's camera is in a first-person mode with the body drawn; put back (only where the
// word still holds our value, only while the control is live with the same class) when that ends, on PageUp off, a
// level change, cover or any third-person mode. Split screen: not hidden (the scale shows in every view, so player 2
// would see player 1 headless). Before the first person mode ends the body is hidden again by the camera in the same
// tick, so the head never shows alone.
constexpr const char* kHeadControls[] = {"HeadScaler", "NeckScale"};   // NeckScale (headshot death) only if no HeadScaler
struct HeadWord { uint32_t address, original, written; };
struct HeadState {
  uint32_t mesh = 0, tree = 0, ctrl = 0, cls = 0;   // the control found for this mesh / AnimTree instance
  std::string control;                             // which of kHeadControls
  std::vector<HeadWord> words;                     // what we wrote (BoneScale; ControlStrength / StrengthTarget if < 1)
  Clock::time_point retry{};                       // not found: looked for again after this
  const char* state = "not wanted";                // for the diag log
  int listsVia = -1;                               // 1 = SkelControlListHead by reflection, 0 = UE3 layout (16 / +8)
};
HeadState g_head;

// A property object (UStruct +64 super, +68 first child, UField +60 Next - ue3/objects.h's layout), for the array's
// element struct; obj::PropertyOffset gives offsets only.
uint32_t PropertyObject(uint32_t cls, const char* p) {
  for (uint32_t c = cls, guard = 0; c && guard < 64; c = obj::Read32(c + 64), guard++)
    for (uint32_t f = obj::Read32(c + 68), n = 0; f && n < 10000; f = obj::Read32(f + 60), n++)
      if (obj::ObjectName(f) == p) return f;
  return 0;
}

// AnimTree.SkelControlLists' element size and ControlHead offset: from the ArrayProperty's Inner (StructProperty, +104,
// the first field after UProperty like UBoolProperty's BitMask) and its Struct (+104), each checked by name; else the
// UE3 layout read from Gears 2's Engine package (BoneName 8, ControlHead +8, DrawY +12: 16 bytes).
struct ListLayout { uint32_t stride = 16, head = 8; int via = 0; };
ListLayout ListsLayout(uint32_t treeCls) {
  static uint32_t cachedCls = 0, cachedName = 0;
  static ListLayout cached;
  if (treeCls == cachedCls && obj::Read32(treeCls + 44) == cachedName) return cached;
  ListLayout l;
  if (const uint32_t arr = PropertyObject(treeCls, "SkelControlLists"); arr && obj::ObjectName(obj::ClassOf(arr)) == "ArrayProperty")
    if (const uint32_t inner = obj::Read32(arr + 104); inner && obj::ObjectName(obj::ClassOf(inner)) == "StructProperty")
      if (const uint32_t st = obj::Read32(inner + 104); st && obj::ObjectName(st) == "SkelControlListHead") {
        const uint32_t size = obj::Read32(inner + 68);
        const int head = obj::PropertyOffset(st, "ControlHead");
        if (size >= 12 && size <= 64 && head >= 0 && uint32_t(head) + 4 <= size) l = {size, uint32_t(head), 1};
      }
  REXLOG_INFO("first person: AnimTree.SkelControlLists element {} bytes, ControlHead +{} ({})", l.stride, l.head,
              l.via ? "reflection" : "UE3 layout, reflection not matched");
  cachedCls = treeCls, cachedName = obj::Read32(treeCls + 44), cached = l;
  return l;
}

// The mesh's head control (0 = none): every list's chain, by ControlName (a name, number 0), first of kHeadControls.
uint32_t FindHeadControl(uint32_t mesh, uint32_t& tree, std::string& which) {
  tree = Ptr(mesh, "Animations");
  if (!tree || !obj::IsLive(tree)) return 0;
  const uint32_t tcls = obj::ClassOf(tree);
  const PropInfo lists = Prop(tcls, "SkelControlLists");
  if (lists.offset < 0) return 0;
  const ListLayout l = ListsLayout(tcls);
  g_head.listsVia = l.via;
  const uint32_t data = obj::Read32(tree + uint32_t(lists.offset)), n = obj::Read32(tree + uint32_t(lists.offset) + 4);
  if (!data || !n || n > 512 || !obj::Readable(data, n * l.stride)) return 0;
  uint32_t found[std::size(kHeadControls)] = {}, foundBone[std::size(kHeadControls)] = {};
  for (uint32_t i = 0; i < n; i++) {
    uint32_t c = obj::Read32(data + i * l.stride + l.head);
    const uint32_t bone = obj::Read32(data + i * l.stride);   // SkelControlListHead.BoneName (index; r27: which bone)
    for (int k = 0; c && k < 64; k++, c = Ptr(c, "NextControl")) {
      if (!obj::IsLive(c)) break;
      const PropInfo cn = Prop(obj::ClassOf(c), "ControlName");
      if (cn.offset < 0 || obj::Read32(c + uint32_t(cn.offset) + 4) != 0) continue;
      const std::string name = obj::Name(obj::Read32(c + uint32_t(cn.offset)));
      for (size_t j = 0; j < std::size(kHeadControls); j++)
        if (!found[j] && name == kHeadControls[j]) found[j] = c, foundBone[j] = bone;
    }
  }
  for (size_t j = 0; j < std::size(kHeadControls); j++)
    if (found[j]) {
      // the control belongs to this mesh (SkelControlBase.SkelComponent, when the class has it and it is set)
      const uint32_t sc = Ptr(found[j], "SkelComponent");
      if (sc && sc != mesh) continue;
      which = std::string(kHeadControls[j]) + " on bone " + obj::Name(foundBone[j]);
      return found[j];
    }
  return 0;
}

void RestoreHead() {
  const bool live = g_head.ctrl && obj::IsLive(g_head.ctrl) && obj::ClassOf(g_head.ctrl) == g_head.cls;
  size_t back = 0;
  if (live)
    for (const HeadWord& w : g_head.words)
      if (obj::Read32(w.address) == w.written && obj::Write32(w.address, w.original)) back++;
  if (!g_head.words.empty())
    REXLOG_INFO("first person: head shown again ({} of {} value(s) put back{})", back, g_head.words.size(),
                live ? "" : ", control gone");
  g_head = HeadState{};
}

// Every tick: mesh = the one local first-person pawn's Mesh, or 0 (not wanted: restore).
void HeadTick(uint32_t mesh, const char* why) {
  if (!mesh) {
    if (g_head.mesh || !g_head.words.empty()) RestoreHead();
    g_head.state = why;
    return;
  }
  // a new mesh, a new AnimTree instance or the control freed: put back what still can be, look again
  if (g_head.mesh && (g_head.mesh != mesh || Ptr(mesh, "Animations") != g_head.tree ||
                      (g_head.ctrl && (!obj::IsLive(g_head.ctrl) || obj::ClassOf(g_head.ctrl) != g_head.cls))))
    RestoreHead();
  if (!g_head.ctrl) {
    if (g_head.mesh == mesh && Clock::now() < g_head.retry) return;   // not found: every 2 s
    g_head.mesh = mesh;
    g_head.retry = Clock::now() + std::chrono::seconds(2);
    std::string which;
    uint32_t tree = 0;
    const uint32_t c = FindHeadControl(mesh, tree, which);
    g_head.tree = tree;
    if (!c) {
      static int warned = 0;
      if (warned++ < 3) REXLOG_WARN("first person: no head control (HeadScaler / NeckScale) in the pawn's AnimTree {:08X} "
                                    "- head left as it is", tree);
      g_head.state = "no control";
      return;
    }
    g_head.ctrl = c, g_head.cls = obj::ClassOf(c), g_head.control = which;
    REXLOG_INFO("first person: head control {} ({} {:08X}) - head hidden while the body is drawn", which,
                obj::ObjectName(g_head.cls), c);
  }
  const uint32_t c = g_head.ctrl, cls = g_head.cls;
  // review r27 R5: each a 4-byte FloatProperty (checked once per class)
  static uint32_t checkedCls = 0;
  static bool floatsOk = false, strengthOk = false;
  if (cls != checkedCls) {
    const auto isFloat = [cls](const char* p) {
      return obj::PropertySize(cls, p) == 4 && obj::PropertyType(cls, p) == "FloatProperty";
    };
    checkedCls = cls, floatsOk = isFloat("BoneScale"), strengthOk = isFloat("ControlStrength") && isFloat("StrengthTarget");
  }
  const PropInfo bs = Prop(cls, "BoneScale");
  const PropInfo cs = strengthOk ? Prop(cls, "ControlStrength") : PropInfo{}, st = strengthOk ? Prop(cls, "StrengthTarget") : PropInfo{};
  if (bs.offset < 0 || !floatsOk) { g_head.state = "no BoneScale"; return; }
  // the words wanted: BoneScale 0; the strength 1 when it is below (SetHeadScale's SetSkelControlStrength(1, 0)) or ours
  // review r27 R1: 0.01, not 0 - the same picture, but the bone's matrix stays invertible (headshot direction, face IK)
  std::vector<std::pair<uint32_t, uint32_t>> want{{c + uint32_t(bs.offset), Bits(0.01f)}};
  for (const PropInfo* p : {&cs, &st})
    if (p->offset >= 0) {
      const uint32_t a = c + uint32_t(p->offset);
      const bool ours = std::any_of(g_head.words.begin(), g_head.words.end(), [a](const HeadWord& w) { return w.address == a; });
      if (ours || obj::ReadFloat(a) < 0.999f) want.push_back({a, Bits(1.f)});
    }
  for (const auto& [a, v] : want) {
    const uint32_t now = obj::Read32(a);
    auto it = std::find_if(g_head.words.begin(), g_head.words.end(), [a = a](const HeadWord& w) { return w.address == a; });
    if (it != g_head.words.end() && now == it->written) continue;   // still ours
    if (now == v) continue;   // already so (the game's own value): nothing to own
    if (!obj::IsLive(c) || !obj::Write32(a, v)) continue;
    // a new word, or the game changed it since (e.g. SetHeadScale again): its value now is the one to put back
    if (it != g_head.words.end()) it->original = now;
    else g_head.words.push_back({a, now, v});
  }
  g_head.state = "hidden";
}

// Gears 3's object layout (ue3/objects.h): UObject Outer +40; ArrayProperty Inner / StructProperty Struct +104;
// UProperty ElementSize +68
uint32_t OuterOff() { return 40; }
uint32_t InnerOff() { return 104; }
uint32_t ElemSizeOff() { return 68; }
uint32_t NextOff() { return 60; }      // UField Next
uint32_t NameStride() { return 8; }   // FName: index, number
uint32_t NameOf(uint32_t object) { return obj::Read32(object + 44); }

// ---- one-time dump (--gow3_first_person_diag): what is drawn near the eye camera ---------------------------------
// For what is left at the screen edges with the body drawn and the head hidden (job 1857: Judgment's two dark shapes at
// the top-right / left edges; Gears 3's left shoulder pad at the bottom left) and for where the real eyes are: once per
// pawn mesh (at most 4 a run), about 1.5 s after its camera first enters a first-person mode (the camera has settled at
// the eyes) and before its head is hidden (WeaponTick holds the head back until then, so the eye bones are not yet
// collapsed into the head bone) -
//   1. every primitive we un-hide (pawn mesh, the pawn's Components, the weapon's parts) and every attachment of the
//      pawn mesh (SkeletalMeshComponent.Attachments: socket / bone attachments, which we never un-hide): class, name,
//      asset (SkeletalMesh / StaticMesh / Template), the bone it is attached to, ParentAnimComponent, bounds in view
//      space and the hide flags;
//   2. the AnimTree's skeletal controls, one line per bone list (bone, then each control: name, class, strength /
//      target, BoneScale) - which bones a control could scale;
//   3. the skeleton (SkeletalMeshComponent.SpaceBases x LocalToWorld, in the camera's view space): the 20 bones nearest
//      the camera, then every head / eye / neck / jaw / shoulder / clavicle / armour bone by name, then the mesh's
//      sockets (SkeletalMesh.Sockets) with an eye / camera / head / view / helmet name - where the real eyes are versus
//      the camera, and which bone a shoulder pad or collar hangs on. Bone names come from the SkeletalMesh's native bone
//      table, found by a checked search (indices only when not found).
// Log only: nothing is written.
void Vec3(uint32_t a, float v[3]) { for (int i = 0; i < 3; i++) v[i] = obj::ReadFloat(a + 4 * i); }
struct View { float c[3] = {}, fw[3] = {}, rt[3] = {}, up[3] = {}; float fov = 0; bool ok = false; };
View ViewOf(uint32_t cam) {   // CameraCache.POV: TimeStamp, then Location, Rotation, FOV (as DiagTick reads it)
  View v;
  const PropInfo cc = Prop(obj::ClassOf(cam), "CameraCache");
  if (cc.offset < 0) return v;
  const uint32_t pov = cam + uint32_t(cc.offset) + 4;
  Vec3(pov, v.c);
  const float k = 3.14159265f / 32768.f;
  const float pitch = float(int16_t(obj::Read32(pov + 12) & 0xFFFF)) * k, yaw = float(int32_t(obj::Read32(pov + 16))) * k;
  v.fov = obj::ReadFloat(pov + 24);
  const float fw[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
  const float rt[3] = {-std::sin(yaw), std::cos(yaw), 0};
  const float up[3] = {-std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
  std::memcpy(v.fw, fw, sizeof fw), std::memcpy(v.rt, rt, sizeof rt), std::memcpy(v.up, up, sizeof up);
  v.ok = true;
  return v;
}
// a world point in view space: "fwd F right R up U dist D (in view / out)"
std::string InView(const View& v, const float p[3]) {
  const float d[3] = {p[0] - v.c[0], p[1] - v.c[1], p[2] - v.c[2]};
  const float f = d[0] * v.fw[0] + d[1] * v.fw[1] + d[2] * v.fw[2], r = d[0] * v.rt[0] + d[1] * v.rt[1],
              u = d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2];
  const float deg = 180.f / 3.14159265f, halfH = v.fov * 0.5f,
              halfV = std::atan(std::tan(halfH / deg) * 9.f / 16.f) * deg;
  const bool in = f > 0 && std::abs(std::atan2(r, f) * deg) < halfH && std::abs(std::atan2(u, f) * deg) < halfV;
  char b[128];
  std::snprintf(b, sizeof b, "fwd %.0f right %.0f up %.0f dist %.0f%s", f, r, u, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]),
                in ? " IN VIEW" : "");
  return b;
}
std::string FullName(uint32_t o) {   // Outer.Outer.Name (3 levels at most)
  std::string s;
  for (int i = 0; o && i < 3; i++, o = obj::Read32(o + OuterOff())) s = s.empty() ? obj::ObjectName(o) : obj::ObjectName(o) + "." + s;
  return s.empty() ? "-" : s;
}
std::string Asset(uint32_t c) {
  for (const char* p : {"SkeletalMesh", "StaticMesh", "Template"})
    if (Prop(obj::ClassOf(c), p).offset >= 0) return std::string(p) + " " + FullName(Ptr(c, p));
  return "no asset";
}
// an array-of-struct property's element: its struct object and ElementSize (ArrayProperty Inner -> StructProperty Struct,
// both at the first field after UProperty; 0 when not matched)
struct StructArray { uint32_t st = 0, stride = 0; };
StructArray ArrayOf(uint32_t cls, const char* p) {
  StructArray a;
  const uint32_t arr = PropertyObject(cls, p);
  if (!arr || obj::ObjectName(obj::ClassOf(arr)) != "ArrayProperty") return a;
  const uint32_t inner = obj::Read32(arr + InnerOff());
  if (!inner || obj::ObjectName(obj::ClassOf(inner)) != "StructProperty") return a;
  const uint32_t st = obj::Read32(inner + InnerOff()), size = obj::Read32(inner + ElemSizeOff());
  if (st && size >= 4 && size <= 256) a.st = st, a.stride = size;
  return a;
}
// the pawn mesh's attachments: component -> bone (SkeletalMeshComponent.Attachments: Component, BoneName, ...)
std::vector<std::pair<uint32_t, std::string>> Attachments(uint32_t mesh) {
  std::vector<std::pair<uint32_t, std::string>> v;
  const uint32_t cls = obj::ClassOf(mesh);
  const PropInfo at = Prop(cls, "Attachments");
  const StructArray a = ArrayOf(cls, "Attachments");
  if (at.offset < 0 || !a.st) return v;
  const int comp = obj::PropertyOffset(a.st, "Component"), bone = obj::PropertyOffset(a.st, "BoneName");
  const uint32_t data = obj::Read32(mesh + uint32_t(at.offset)), n = obj::Read32(mesh + uint32_t(at.offset) + 4);
  if (comp < 0 || bone < 0 || !data || n > 64 || !obj::Readable(data, n * a.stride)) return v;
  for (uint32_t i = 0; i < n; i++)
    v.push_back({obj::Read32(data + i * a.stride + uint32_t(comp)), obj::Name(obj::Read32(data + i * a.stride + uint32_t(bone)))});
  return v;
}
void DumpPart(const View& v, uint32_t mesh, const std::vector<std::pair<uint32_t, std::string>>& att, const char* role,
              uint32_t c) {
  const uint32_t cls = obj::ClassOf(c);
  std::string bone = "-";
  for (const auto& [comp, b] : att)
    if (comp == c) bone = b;
  const uint32_t parent = Ptr(c, "ParentAnimComponent");
  std::string where = "no bounds";
  if (const PropInfo bo = Prop(cls, "Bounds"); bo.offset >= 0 && v.ok) {
    float o[3];
    Vec3(c + uint32_t(bo.offset), o);
    char r[32];
    std::snprintf(r, sizeof r, " radius %.0f", obj::ReadFloat(c + uint32_t(bo.offset) + 24));
    where = "bounds centre " + InView(v, o) + r;
  }
  REXLOG_INFO("first person diag: part {} {} {:08X} | {} | bone {} | ParentAnimComponent {} | {} | HiddenGame {} "
              "OwnerNoSee {} OnlyOwnerSee {} IgnoreHiddenActors {}", role, obj::ObjectName(cls) + " " + obj::ObjectName(c),
              c, Asset(c), bone, !parent ? "none" : parent == mesh ? "= pawn mesh" : FullName(parent), where,
              Flag(c, "HiddenGame"), Flag(c, "bOwnerNoSee"), Flag(c, "bOnlyOwnerSee"), Flag(c, "bIgnoreHiddenActorsMembership"));
}
void DumpControls(uint32_t mesh) {
  const uint32_t tree = Ptr(mesh, "Animations");
  if (!tree || !obj::IsLive(tree)) { REXLOG_INFO("first person diag: skel controls: no AnimTree"); return; }
  const uint32_t tcls = obj::ClassOf(tree);
  const PropInfo lists = Prop(tcls, "SkelControlLists");
  const ListLayout l = ListsLayout(tcls);
  const uint32_t data = lists.offset >= 0 ? obj::Read32(tree + uint32_t(lists.offset)) : 0,
                 n = lists.offset >= 0 ? obj::Read32(tree + uint32_t(lists.offset) + 4) : 0;
  REXLOG_INFO("first person diag: skel controls of AnimTree {} {:08X}: {} bone list(s)", FullName(tree), tree, n);
  if (!data || n > 512 || !obj::Readable(data, n * l.stride)) return;
  for (uint32_t i = 0; i < n && i < 96; i++) {
    std::string line;
    uint32_t c = obj::Read32(data + i * l.stride + l.head);
    for (int k = 0; c && k < 16 && obj::IsLive(c); k++, c = Ptr(c, "NextControl")) {
      const uint32_t cls = obj::ClassOf(c);
      const PropInfo cn = Prop(cls, "ControlName");
      const auto f = [&](const char* p) { const PropInfo q = Prop(cls, p); return q.offset >= 0 ? obj::ReadFloat(c + uint32_t(q.offset)) : -1.f; };
      char b[96];
      std::snprintf(b, sizeof b, " (%s strength %.2f/%.2f BoneScale %.2f)", obj::ObjectName(cls).c_str(),
                    f("ControlStrength"), f("StrengthTarget"), f("BoneScale"));
      line += std::string(k ? " ->" : "") + " '" + (cn.offset >= 0 ? obj::Name(obj::Read32(c + uint32_t(cn.offset))) : "?") + "'" + b;
    }
    REXLOG_INFO("first person diag:   bone {}:{}", obj::Name(obj::Read32(data + i * l.stride)), line.empty() ? " none" : line);
  }
}
// The SkeletalMesh's bone table (native RefSkeleton: TArray<FMeshBone>, FName first in each element) - not a script
// property, so it is searched for: a TArray in the first 1 KB of the SkeletalMesh with exactly the mesh's bone count
// whose elements, at one stride, all start with a distinct identifier-like name. Empty when not found.
std::vector<std::string> BoneNames(uint32_t skel, uint32_t count) {
  std::vector<std::string> names;
  if (!skel || count < 8 || count > 400) return names;
  const auto ident = [](const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char ch : s)
      if (!std::isalnum(uint8_t(ch)) && ch != '_') return false;
    return true;
  };
  for (uint32_t off = 32; off < 0x400; off += 4) {
    const uint32_t data = obj::Read32(skel + off), n = obj::Read32(skel + off + 4), max = obj::Read32(skel + off + 8);
    if (n != count || max < n || !data) continue;
    for (uint32_t stride = 8; stride <= 128; stride += 4) {
      if (!obj::Readable(data, n * stride)) break;
      std::vector<std::string> v;
      std::unordered_set<std::string> seen;
      for (uint32_t i = 0; i < n; i++) {
        std::string s = obj::Name(obj::Read32(data + i * stride));
        if (!ident(s) || !seen.insert(s).second) break;
        v.push_back(std::move(s));
      }
      if (v.size() == n) {
        REXLOG_INFO("first person diag: bone names: SkeletalMesh +0x{:X}, {} bytes per bone", off, stride);
        return v;
      }
    }
  }
  REXLOG_INFO("first person diag: bone names: the SkeletalMesh's bone table was not found (indices only)");
  return names;
}

// a bone's mesh-space transform from SpaceBases: an FMatrix (rows X, Y, Z, origin) or an FBoneAtom (Rotation quat,
// Translation, Scale) - made into rows
struct Bones {
  uint32_t data = 0, n = 0, stride = 0;
  int rot = -1, tr = -1, sc = -1;   // FBoneAtom fields (-1: FMatrix)
  float l2w[16] = {};
  bool ok = false;
};
Bones BonesOf(uint32_t mesh) {
  Bones b;
  const uint32_t cls = obj::ClassOf(mesh);
  const PropInfo sb = Prop(cls, "SpaceBases"), lw = Prop(cls, "LocalToWorld");
  const StructArray a = ArrayOf(cls, "SpaceBases");
  if (sb.offset < 0 || lw.offset < 0 || obj::PropertySize(cls, "LocalToWorld") != 64 || !a.st) return b;
  const std::string st = obj::ObjectName(a.st);
  if (st == "Matrix" && a.stride == 64) {
  } else {
    b.rot = obj::PropertyOffset(a.st, "Rotation"), b.tr = obj::PropertyOffset(a.st, "Translation");
    b.sc = obj::PropertyOffset(a.st, "Scale");
    if (b.rot < 0 || b.tr < 0 || uint32_t(b.rot) + 16 > a.stride || uint32_t(b.tr) + 12 > a.stride) return b;
  }
  b.data = obj::Read32(mesh + uint32_t(sb.offset)), b.n = obj::Read32(mesh + uint32_t(sb.offset) + 4), b.stride = a.stride;
  if (!b.data || !b.n || b.n > 400 || !obj::Readable(b.data, b.n * b.stride)) return b;
  for (int i = 0; i < 16; i++) b.l2w[i] = obj::ReadFloat(mesh + uint32_t(lw.offset) + 4 * i);
  b.ok = true;
  return b;
}
// the world position of a point given in bone i's space (0,0,0 = the bone itself); row vectors: p x M
void BonePoint(const Bones& b, uint32_t i, const float local[3], float out[3]) {
  const uint32_t e = b.data + i * b.stride;
  float m[12];   // rows X, Y, Z, origin (3 floats each)
  if (b.rot < 0) {
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 3; k++) m[r * 3 + k] = obj::ReadFloat(e + uint32_t(r * 16 + k * 4));
  } else {   // FBoneAtom: quaternion (x, y, z, w), uniform scale
    const float x = obj::ReadFloat(e + uint32_t(b.rot)), y = obj::ReadFloat(e + uint32_t(b.rot) + 4),
                z = obj::ReadFloat(e + uint32_t(b.rot) + 8), w = obj::ReadFloat(e + uint32_t(b.rot) + 12);
    const float s = b.sc >= 0 ? obj::ReadFloat(e + uint32_t(b.sc)) : 1.f;
    const float q[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y),
                        2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x),
                        2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
    for (int k = 0; k < 9; k++) m[k] = q[k] * s;
    for (int k = 0; k < 3; k++) m[9 + k] = obj::ReadFloat(e + uint32_t(b.tr) + 4 * uint32_t(k));
  }
  float p[3];
  for (int k = 0; k < 3; k++) p[k] = local[0] * m[k] + local[1] * m[3 + k] + local[2] * m[6 + k] + m[9 + k];
  for (int k = 0; k < 3; k++) out[k] = p[0] * b.l2w[k] + p[1] * b.l2w[4 + k] + p[2] * b.l2w[8 + k] + b.l2w[12 + k];
}
bool NameHas(std::string s, std::initializer_list<const char*> keys) {
  for (char& ch : s) ch = char(std::tolower(uint8_t(ch)));
  for (const char* k : keys)
    if (s.find(k) != std::string::npos) return true;
  return false;
}
void DumpBones(const View& v, uint32_t mesh) {
  const Bones b = BonesOf(mesh);
  if (!v.ok || !b.ok) {
    REXLOG_INFO("first person diag: bones: SpaceBases / LocalToWorld not readable");
    return;
  }
  const std::vector<std::string> names = BoneNames(Ptr(mesh, "SkeletalMesh"), b.n);
  const auto nameOf = [&](uint32_t i) { return i < names.size() ? names[i] : std::string("?"); };
  const float zero[3] = {0, 0, 0};
  std::vector<std::pair<float, uint32_t>> nearest;   // distance to the camera, bone index
  std::vector<std::array<float, 3>> world(b.n);
  for (uint32_t i = 0; i < b.n; i++) {
    BonePoint(b, i, zero, world[i].data());
    const float d[3] = {world[i][0] - v.c[0], world[i][1] - v.c[1], world[i][2] - v.c[2]};
    nearest.push_back({std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), i});
  }
  std::sort(nearest.begin(), nearest.end());
  REXLOG_INFO("first person diag: bones nearest the camera ({} bones, {}, names {}):", b.n,
              b.rot < 0 ? "FMatrix" : "FBoneAtom", names.empty() ? "not found" : "found");
  for (size_t k = 0; k < nearest.size() && k < 20 && nearest[k].first < 80.f; k++)
    REXLOG_INFO("first person diag:   bone #{} {} | {}", nearest[k].second, nameOf(nearest[k].second),
                InView(v, world[nearest[k].second].data()));
  // the bones the eye camera and the shapes at the edges are about (by name)
  for (uint32_t i = 0; i < names.size(); i++)
    if (NameHas(names[i], {"head", "eye", "neck", "jaw", "cam", "clav", "shoulder", "sho", "armor", "armour", "pad",
                           "collar", "scarf", "hood", "spine3", "spine_3"}))
      REXLOG_INFO("first person diag:   named bone #{} {} | {}", i, names[i], InView(v, world[i].data()));
  // the mesh's sockets (SkeletalMesh.Sockets: SkeletalMeshSocket objects - SocketName, BoneName, RelativeLocation)
  const uint32_t skel = Ptr(mesh, "SkeletalMesh");
  const PropInfo so = Prop(obj::ClassOf(skel), "Sockets");
  if (!skel || so.offset < 0) { REXLOG_INFO("first person diag: sockets: none readable"); return; }
  const uint32_t data = obj::Read32(skel + uint32_t(so.offset)), n = obj::Read32(skel + uint32_t(so.offset) + 4);
  if (!data || n > 128 || !obj::Readable(data, n * 4)) return;
  std::string all;
  for (uint32_t k = 0; k < n; k++) {
    const uint32_t s = obj::Read32(data + 4 * k);
    if (!s || !obj::IsLive(s)) continue;
    const PropInfo sn = Prop(obj::ClassOf(s), "SocketName"), bn = Prop(obj::ClassOf(s), "BoneName"),
                   rl = Prop(obj::ClassOf(s), "RelativeLocation");
    if (sn.offset < 0 || bn.offset < 0) continue;
    const std::string sname = obj::Name(obj::Read32(s + uint32_t(sn.offset))), bone = obj::Name(obj::Read32(s + uint32_t(bn.offset)));
    all += " " + sname + "@" + bone;
    if (!NameHas(sname, {"eye", "cam", "head", "view", "helmet", "hat", "face"})) continue;
    const auto it = std::find(names.begin(), names.end(), bone);
    if (it == names.end() || rl.offset < 0) { REXLOG_INFO("first person diag:   socket {} on {} (bone not resolved)", sname, bone); continue; }
    float rel[3], w[3];
    Vec3(s + uint32_t(rl.offset), rel);
    BonePoint(b, uint32_t(it - names.begin()), rel, w);
    REXLOG_INFO("first person diag:   socket {} on {} (+{:.1f}, {:.1f}, {:.1f}) | {}", sname, bone, rel[0], rel[1], rel[2],
                InView(v, w));
  }
  REXLOG_INFO("first person diag: sockets ({}):{}", n, all.empty() ? " none" : all);
}

// Pawn meshes not dumped yet (diag on): first seen in first person at; WeaponTick holds their head back until the dump.
std::unordered_map<uint32_t, Clock::time_point> g_fpSince;
std::unordered_set<uint32_t> g_dumped;
bool DumpPending(uint32_t mesh) {
  static const bool on = REXCVAR_GET(gow3_first_person_diag);
  if (!on || !mesh || g_dumped.count(mesh) || g_dumped.size() >= 4) return false;
  g_fpSince.try_emplace(mesh, Clock::now());
  return true;
}
void DumpOnce(uint32_t cam, uint32_t pawn, uint32_t weapon) {
  const uint32_t mesh = Ptr(pawn, "Mesh");
  const auto since = g_fpSince.find(mesh);
  if (!mesh || !obj::IsLive(mesh) || g_dumped.count(mesh) || since == g_fpSince.end() ||
      Clock::now() - since->second < std::chrono::milliseconds(1500))
    return;
  g_dumped.insert(mesh);
  const View v = ViewOf(cam);
  const auto att = Attachments(mesh);
  REXLOG_INFO("first person diag: ---- parts of {} {:08X} (mesh {}, {} mesh attachment(s)) seen from camera ({:.0f}, "
              "{:.0f}, {:.0f}) FOV {:.0f}, head not hidden yet ----", obj::ObjectName(obj::ClassOf(pawn)), pawn,
              Asset(mesh), att.size(), v.c[0], v.c[1], v.c[2], v.fov);
  std::vector<uint32_t> done;
  for (uint32_t c : BodyParts(pawn)) DumpPart(v, mesh, att, c == mesh ? "pawn-mesh" : "pawn-component", c), done.push_back(c);
  if (weapon && obj::IsLive(weapon))
    for (uint32_t c : WeaponParts(weapon)) DumpPart(v, mesh, att, "weapon", c), done.push_back(c);
  for (const auto& [c, b] : att)
    if (c && obj::IsLive(c) && std::find(done.begin(), done.end(), c) == done.end())
      DumpPart(v, mesh, att, "mesh-attachment(not un-hidden by us)", c);
  DumpControls(mesh);
  DumpBones(v, mesh);
  REXLOG_INFO("first person diag: ---- end of parts ----");
}

// ---- third-person moves: roadie run and the like step first person aside (Shagg 11-14: roadie run in third person) ----
// Each game's GearGameplayCamera.FindBestCameraMode (decompiled from its GearGame script; Gears 2, 3 and Judgment alike,
// agent062-out/roadie-notes.md) picks GearCamRoadie (GameplayCam_RoadieRun, never patched) only when
// GearPawn.SpecialMove == SM_RoadieRun and no zoom, crouch or cover check won first. The dash to cover (SM_CoverRun),
// the slide into cover or the stop that ends a run (SM_Run2MidCov / SM_Run2StdCov / SM_Run2Idle) and the roadie-run
// stabs keep GameplayCam_Default, and aiming while running (bCanRoadieRunWhileTargeting) picks GameplayCam_Targeting -
// modes of ours. So the pawn's own state decides: while a local player's pawn does one of kAsideMoves (GearPawn.
// SpecialMove, a byte of enum ESpecialMove; the values are taken from the enum's names in memory), every patched mode
// object of that player's GearGameplayCamera (its current mode and every mode whose GameplayCam is it) gets its
// original words back as on PageUp off (only a word that still holds our value, only while the object is live with its
// class; bDoPredictiveAvoidance too) and is left out of the pass, and WeaponTick un-hides nothing for that player, so
// the head is shown again: the game's own third-person pawn. When the move ends those modes are patched again at once
// (Apply reads their originals afresh). SpecialMove not found or not a byte: nothing is stepped aside.
constexpr const char* kAsideMoves[] = {"SM_RoadieRun", "SM_CoverRun", "SM_Run2MidCov", "SM_Run2StdCov", "SM_Run2Idle",
                                       "SM_RoadieRunStab", "SM_RoadieRunCleaverStab", "SM_RoadieRunHitReactionStumble"};
// the enum's names not readable: the values every game's script has for the first five (ESpecialMove 3, 4, 12, 13, 14)
constexpr uint8_t kAsideFallback[] = {3, 4, 12, 13, 14};

struct MoveTable {
  uint32_t name = 0;                // the pawn class's name index (a class freed and the address reused: looked up again)
  int offset = -1;                  // GearPawn.SpecialMove (a 1-byte ByteProperty), -1 = not found
  std::vector<std::string> names;   // ESpecialMove's names (empty: not read)
  std::vector<bool> aside;          // by value: a third-person move
};
std::unordered_map<uint32_t, MoveTable> g_moves;   // by pawn class

// ESpecialMove's names: the ByteProperty's Enum (its first field after UProperty, like an ArrayProperty's Inner) ->
// UEnum.Names (TArray<FName>, right after UField; Next + 4, or + 8 where UField also has a SuperField), checked by
// class and object name and by its first entries
std::vector<std::string> EnumNames(uint32_t cls) {
  std::vector<std::string> v;
  const uint32_t p = PropertyObject(cls, "SpecialMove");
  if (!p || obj::ObjectName(obj::ClassOf(p)) != "ByteProperty") return v;
  const uint32_t e = obj::Read32(p + InnerOff());
  if (!e || obj::ObjectName(obj::ClassOf(e)) != "Enum" || obj::ObjectName(e) != "ESpecialMove") return v;
  for (uint32_t at : {NextOff() + 4, NextOff() + 8}) {
    const uint32_t data = obj::Read32(e + at), n = obj::Read32(e + at + 4), stride = NameStride();
    if (!data || n < 16 || n > 512 || !obj::Readable(data, n * stride)) continue;
    v.clear();
    for (uint32_t i = 0; i < n; i++) v.push_back(obj::Name(obj::Read32(data + i * stride)));
    if (v[0] == "SM_None" && v[4] == "SM_RoadieRun") return v;
  }
  return {};
}

const MoveTable& MovesOf(uint32_t cls) {
  static const std::string extra = REXCVAR_GET(gow3_first_person_third_person_moves);
  const uint32_t name = NameOf(cls);
  if (auto it = g_moves.find(cls); it != g_moves.end() && it->second.name == name) return it->second;
  if (g_moves.size() > 64) g_moves.clear();
  MoveTable& t = g_moves[cls];
  t = MoveTable{};
  t.name = name;
  if (obj::PropertyType(cls, "SpecialMove") != "ByteProperty" || obj::PropertySize(cls, "SpecialMove") != 1 ||
      (t.offset = Prop(cls, "SpecialMove").offset) < 0) {
    t.offset = -1;
    REXLOG_WARN("first person: {} has no SpecialMove byte - roadie run is left to the game's camera modes",
                obj::ObjectName(cls));
    return t;
  }
  t.names = EnumNames(cls);
  t.aside.assign(256, false);
  std::string list;
  if (t.names.empty()) {
    for (uint8_t v : kAsideFallback) t.aside[v] = true, list += " " + std::to_string(v);
  } else {
    std::vector<std::string> want(std::begin(kAsideMoves), std::end(kAsideMoves));
    std::stringstream in(extra);   // --gow3_first_person_third_person_moves: more names, comma separated
    for (std::string s; std::getline(in, s, ',');) {
      s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
      if (!s.empty()) want.push_back(s);
    }
    for (const std::string& w : want)
      if (const auto it = std::find(t.names.begin(), t.names.end(), w); it != t.names.end() && it - t.names.begin() < 256)
        t.aside[size_t(it - t.names.begin())] = true, list += " " + w + "=" + std::to_string(it - t.names.begin());
  }
  REXLOG_INFO("first person: {}.SpecialMove +0x{:X} - third person while:{} ({})", obj::ObjectName(cls), t.offset,
              list.empty() ? " none" : list,
              t.names.empty() ? "ESpecialMove not read: the script's values" : "names from ESpecialMove");
  return t;
}

// the pawn's special move now (-1: no pawn / no SpecialMove byte)
int MoveOf(uint32_t pawn) {
  if (!pawn || !obj::IsLive(pawn)) return -1;
  const MoveTable& t = MovesOf(obj::ClassOf(pawn));
  return t.offset < 0 ? -1 : int(obj::Read32(pawn + uint32_t(t.offset)) >> 24);   // the byte at the offset
}
bool IsAsideMove(uint32_t pawn, int move) {
  return move >= 0 && MovesOf(obj::ClassOf(pawn)).aside[size_t(move)];
}
std::string MoveText(uint32_t pawn) {   // for the diag: "SM_RoadieRun (4), third person"
  const int move = MoveOf(pawn);
  if (move < 0) return "-";
  const MoveTable& t = MovesOf(obj::ClassOf(pawn));
  return (size_t(move) < t.names.size() ? t.names[size_t(move)] : "#" + std::to_string(move)) + " (" +
         std::to_string(move) + ")" + (t.aside[size_t(move)] ? ", third person" : "");
}

struct Aside { uint32_t cam, cls; };
std::unordered_map<uint32_t, Aside> g_aside;   // mode object stepped aside (original words back) -> camera, class
std::unordered_set<uint32_t> g_asideCams;     // local cameras whose pawn does a third-person move now

// one patched mode object back to the game's words (the exact restore of PageUp off; a dead object is only forgotten)
bool StepAside(uint32_t o, uint32_t cam) {
  const auto it = g_patched.find(o);
  if (it == g_patched.end()) return false;
  const uint32_t cls = it->second.cls;
  const bool live = obj::IsLive(o) && obj::ClassOf(o) == cls;
  if (live)
    for (const Word& w : it->second.words)
      if (obj::Read32(w.address) == w.written) obj::Write32(w.address, w.original);
  g_patched.erase(it);
  if (!live) return false;
  RestoreBits(o);   // its bDoPredictiveAvoidance
  g_aside[o] = {cam, cls};
  return true;
}

// Every tick before WeaponTick: which local players do a third-person move; their modes stepped aside, or patched again.
void AsideTick() {
  g_asideCams.clear();
  for (uint32_t cam : g_cams) {
    if (!obj::IsLive(cam)) continue;
    const uint32_t pc = Ptr(cam, "PCOwner");
    const uint32_t pawn = pc && obj::IsLive(pc) ? Ptr(pc, "MyGearPawn") : 0;
    const int move = MoveOf(pawn);
    // review r31: 0.5 s more after the move ends (a run, its stop, a tap and run again stay one third-person stretch)
    static std::unordered_map<uint32_t, Clock::time_point> lastAside;
    if (IsAsideMove(pawn, move)) lastAside[cam] = Clock::now();
    else if (auto it = lastAside.find(cam); it == lastAside.end() || Clock::now() - it->second > std::chrono::milliseconds(500)) {
      if (it != lastAside.end()) lastAside.erase(it);
      continue;
    }
    g_asideCams.insert(cam);
    const uint32_t gc = Ptr(cam, "GameplayCam");
    if (!gc || !obj::IsLive(gc)) continue;
    std::vector<uint32_t> modes;
    if (const uint32_t cur = Ptr(gc, "CurrentCamMode"); g_patched.count(cur)) modes.push_back(cur);
    for (const auto& [o, p] : g_patched)   // the camera's other modes (GearGameplayCamera.CreateCameraMode sets this)
      if (Ptr(o, "GameplayCam") == gc && std::find(modes.begin(), modes.end(), o) == modes.end()) modes.push_back(o);
    size_t n = 0;
    for (uint32_t o : modes) n += StepAside(o, cam);
    static int logged = 0;
    if (n && logged++ < 20)
      REXLOG_INFO("first person: {} - third person ({} camera mode(s) back to the game's offsets)", MoveText(pawn), n);
  }
  size_t back = 0;
  for (auto it = g_aside.begin(); it != g_aside.end();) {
    if (g_asideCams.count(it->second.cam)) { ++it; continue; }
    const uint32_t o = it->first, cls = it->second.cls;
    it = g_aside.erase(it);
    if (!obj::IsLive(o) || obj::ClassOf(o) != cls) continue;   // gone: forgotten, never written
    const ClassInfo& info = InfoOf(cls);
    if (!info.fp || info.viewOffset < 0) continue;
    Apply(o, cls, info);
    if (info.avoid >= 0) SetBit(o, o + uint32_t(info.avoid), info.avoidMask, false, false);
    back++;
  }
  static int loggedBack = 0;
  if (back && loggedBack++ < 20)
    REXLOG_INFO("first person: the move is over - {} camera mode(s) at eye level again", back);
}

// Every engine tick while first person is on (a few dozen reads per local player): the current weapon of each local
// player whose camera is in a first-person mode gets bIgnoreHiddenActorsMembership; anything we set it on that is no
// longer such a part (weapon switched, dropped, holstered, cover / third-person mode, player gone) gets it back.
std::unordered_set<uint32_t> g_wantParts;
void WeaponTick() {
  static const bool weaponOn = REXCVAR_GET(gow3_first_person_weapon), bodyOn = REXCVAR_GET(gow3_first_person_body);
  static const bool headOn = REXCVAR_GET(gow3_first_person_head);
  if (!weaponOn && !bodyOn) return;
  g_wantParts.clear();
  uint32_t headMesh = 0;
  int liveCams = 0;
  for (uint32_t cam : g_cams) {
    if (!obj::IsLive(cam)) continue;
    liveCams++;
    if (g_asideCams.count(cam)) continue;   // roadie run and the like: the game's third-person pawn
    // review r24 R1 / R2: only while the gameplay camera is the active one and its current mode is one of ours; a
    // property not found = nothing un-hidden (the earlier behaviour)
    const uint32_t gc = Ptr(cam, "GameplayCam");
    if (!gc || !obj::IsLive(gc) || Prop(obj::ClassOf(gc), "CurrentCamMode").offset < 0) continue;
    if (Prop(obj::ClassOf(cam), "CurrentCamera").offset < 0 || Ptr(cam, "CurrentCamera") != gc) continue;   // r29 R1
    if (!g_patched.count(Ptr(gc, "CurrentCamMode"))) continue;
    const uint32_t pc = Ptr(cam, "PCOwner");
    if (!pc || !obj::IsLive(pc)) continue;
    const uint32_t pawn = Ptr(pc, "MyGearPawn");
    if (pawn && obj::IsLive(pawn)) DumpPending(Ptr(pawn, "Mesh"));   // --gow3_first_person_diag: the one-time dump
    std::vector<uint32_t> parts;
    if (bodyOn && pawn && obj::IsLive(pawn)) {
      parts = BodyParts(pawn);
      if (const uint32_t mesh = Ptr(pawn, "Mesh"); mesh && obj::IsLive(mesh)) headMesh = mesh;
    }
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
  // your own head: hidden only with the body drawn, for a single local player (split screen: every view would show it)
  // (--gow3_first_person_diag: a pawn mesh not dumped yet keeps its head ~1.5 s longer, so the dump sees the real eyes)
  const bool dumpFirst = liveCams == 1 && headMesh && DumpPending(headMesh);
  if (bodyOn && !headOn)
    HeadTick(liveCams == 1 && !dumpFirst ? headMesh : 0, dumpFirst ? "diag: parts dump first"
             : liveCams > 1 ? "split screen (shown)" : !g_asideCams.empty() ? "third-person move (roadie run...)"
             : "not first person");
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
    const uint32_t active = Ptr(cam, "CurrentCamera");   // the gate WeaponTick uses (must be the gameplay camera)
    REXLOG_INFO("first person diag: cam {:08X} active {} | mode {} ({}) | pawn {} bIsHiddenByCamera {} | HiddenActors {} "
                "pawn {} weapon {} | special move {} | time {:.2f}", cam,
                Prop(obj::ClassOf(cam), "CurrentCamera").offset < 0 ? std::string("(no property)")
                    : active == gc ? std::string("gameplay") : active ? obj::ObjectName(obj::ClassOf(active)) : std::string("none"),
                mode ? obj::ObjectName(obj::ClassOf(mode)) : "?",
                g_patched.count(mode) ? "FP" : g_aside.count(mode) ? "3P, stepped aside" : "3P",
                pawn ? obj::ObjectName(obj::ClassOf(pawn)) : "none",
                Flag(pawn, "bIsHiddenByCamera"), hidden, pawnIn, weapIn, MoveText(pawn), now);
    REXLOG_INFO("first person diag:   pawn mesh {} | weapon {} bHidden {} | weapon mesh {} (owner {}) | magazine {}",
                Parts(Ptr(pawn, "Mesh")), weapon ? obj::ObjectName(obj::ClassOf(weapon)) : "none", Flag(weapon, "bHidden"),
                Parts(wmesh), obj::ObjectName(obj::ClassOf(Ptr(wmesh, "Owner"))), Parts(Ptr(weapon, "MagazineMesh")));
    // once per pawn mesh, ~1.5 s into first person: every part, the skeletal controls, the bones / sockets near the camera
    if (g_patched.count(mode) && pawn && obj::IsLive(pawn)) DumpOnce(cam, pawn, weapon);
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
  // the head (--gow3_first_person_head off): the control and its values now, what we will put back
  if (!REXCVAR_GET(gow3_first_person_body) || REXCVAR_GET(gow3_first_person_head)) {
    REXLOG_INFO("first person diag:   head drawn (option)");
    return;
  }
  const uint32_t c = g_head.ctrl, cls = c ? obj::ClassOf(c) : 0;
  const auto f = [&](const char* p) {
    const PropInfo i = Prop(cls, p);
    return c && i.offset >= 0 ? obj::ReadFloat(c + uint32_t(i.offset)) : -1.f;
  };
  std::string orig;
  for (const HeadWord& w : g_head.words) {
    char b[48];
    std::snprintf(b, sizeof b, " +0x%X %.3f->%.3f", w.address - c, Float(w.original), Float(w.written));
    orig += b;
  }
  const uint32_t sc = c ? Ptr(c, "SkelComponent") : 0;
  REXLOG_INFO("first person diag:   head {} | control {} {} {:08X} (lists via {}) | BoneScale {:.3f} ControlStrength {:.3f} "
              "StrengthTarget {:.3f} BlendTimeToGo {:.3f} bSetStrengthFromAnimNode {} IgnoreAtOrAboveLOD {} | "
              "SkelComponent {} | mesh {:08X} tree {:08X} | ours:{}", g_head.state,
              g_head.control.empty() ? "-" : g_head.control, c ? obj::ObjectName(cls) : "-", c,
              g_head.listsVia < 0 ? "-" : g_head.listsVia ? "reflection" : "UE3 layout", f("BoneScale"),
              f("ControlStrength"), f("StrengthTarget"), f("BlendTimeToGo"), Flag(c, "bSetStrengthFromAnimNode"),
              c && Prop(cls, "IgnoreAtOrAboveLOD").offset >= 0
                  ? int(obj::Read32(c + uint32_t(Prop(cls, "IgnoreAtOrAboveLOD").offset))) : -1,
              !c ? "-" : sc == g_head.mesh ? "= mesh" : sc ? "OTHER" : "none", g_head.mesh, g_head.tree,
              orig.empty() ? " none" : orig);
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
  REXLOG_INFO("first person: on (experimental) - walking, crouching and aiming at eye level; PageUp toggles");
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
      RestoreHead();   // the head control's BoneScale / strength (live control only)
      g_cams.clear();
      g_aside.clear(), g_asideCams.clear();   // their words are the game's already
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
  AsideTick();    // roadie run and the like: that player's modes back to third person while it lasts
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
    if (const uint32_t cam = CameraOf(o);
        cam && LocalCamera(cam) && std::find(g_seenCams.begin(), g_seenCams.end(), cam) == g_seenCams.end() &&
        g_seenCams.size() < 8)
      g_seenCams.push_back(cam);
    if (g_aside.count(o)) continue;   // stepped aside for a third-person move (AsideTick puts it back; r31 R2: its camera is kept above)
    Apply(o, cls, info);
    if (info.avoid >= 0) SetBit(o, o + uint32_t(info.avoid), info.avoidMask, false, false);
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
  for (auto it = g_aside.begin(); it != g_aside.end();)   // stepped-aside objects gone: forgotten, never written
    it = g_seen.count(it->first) ? std::next(it) : g_aside.erase(it);
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
