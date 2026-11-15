// Layer 3 (Gears of War: Judgment): optional first-person camera (--gowj_first_person, off by default; the launcher's
// Gameplay tab "First-person camera (experimental)"). Our code (MIT).
//
// Gears3-Native's layer3_gow3/first_person.cpp (read it for the why: the game's own camera modes moved to the eyes by
// their ViewOffset, the body and the weapon un-hidden from the camera's HiddenActors list with the engine's
// PrimitiveComponent.bIgnoreHiddenActorsMembership, the head collapsed by the big-head HeadScaler control, PageUp
// toggles, --..._diag) with Judgment's object tables and layout. Judgment's GearGame script has every class, mode and
// property this uses under Gears 3's names (agent062-out/jfp-notes.md: the decompressed GearGame.xxx compared name for
// name): GearGameplayCameraMode extends GameFramework's GameThirdPersonCameraMode (ViewOffset, ViewOffset_Viewport
// Adjustments, FOVAngle, bDoPredictiveAvoidance); GameplayCam_Default / _Crouch / _Targeting / _TargetingGrenade /
// _TargetingVisualAimAssist / _RoadieRun with Gears 3's ViewOffset defaults; GearPlayerCamera.GameplayCam,
// Camera.PCOwner, GamePlayerCamera.CurrentCamera, GameThirdPersonCamera.CurrentCamMode, GearPC.MyGearPawn,
// GearPawn.MyGearWeapon / bIsHiddenByCamera / SetHeadScale, GearWeapon.MagazineMesh / PSC_ShellEject /
// PSC_ReloadBarrelSmoke / MuzzleFlash, GearMuzzleFlash.MuzFlashEmitter. Judgment's own modes (GameplayCam_Passenger,
// _PassengerTargeting) stay third person.
//
// The object reflection is Gears 2's (Gears2-Native/src/layer2_ue3/objects.cpp, built into this game's layer 2: its
// obj::IsLive, FindClassRange, FindInstancesOfRange, PropertyArrayDim), opened with Judgment's globals and Gears 3's
// object layout on a worker thread, as adaptive_triggers.cpp does (the two can run together: obj::Init is guarded).
// Field offsets come from that layout (obj::CurrentLayout(): a class's Name, an object's Outer, UStruct / UField /
// StructProperty fields), never from constants. The player camera class is looked up a slice of the table at a time
// (no whole-table lookup on the game thread). Off (the default): nothing is registered or read but one flag per tick.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/keybinds.h>

#include <ue3/objects.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(gowj_first_person, false, "Judgment",
                    "First-person camera (experimental): the game's own camera at eye level while walking, crouching, "
                    "aiming and roadie running; cover, executions, vehicles, turrets and cutscenes stay third person. "
                    "PageUp switches it off and on in the game (bind_cogbox_first_person)");
REXCVAR_DEFINE_STRING(gowj_first_person_offset, "8,0,0", "Judgment",
                      "First-person camera: where the camera sits from the game's camera origin, 'forward,right,up' "
                      "(game units; read again every 2 s, so it can be tuned from the SDK console)");
REXCVAR_DEFINE_INT32(gowj_first_person_fov, 0, "Judgment",
                     "First-person camera: field of view while walking / crouching (0 = the game's own or the launcher's "
                     "FOV setting; aiming and roadie run keep theirs)");
REXCVAR_DEFINE_BOOL(gowj_first_person_weapon, true, "Judgment",
                    "First-person camera: draw your own weapon (the game hides it with your body when the camera is "
                    "inside you; this sets the engine's bIgnoreHiddenActorsMembership on the weapon's meshes). With "
                    "--gowj_first_person_body on, the weapon is drawn with the body either way");
REXCVAR_DEFINE_BOOL(gowj_first_person_body, true, "Judgment",
                    "First-person camera: draw your own body too (arms, shoulders, legs - the game's own model, already "
                    "animated; the same engine flag on your pawn's meshes). Off: only the weapon");
REXCVAR_DEFINE_BOOL(gowj_first_person_head, false, "Judgment",
                    "First-person camera: draw your own head too (off, the default: hidden while the body is drawn in "
                    "first person - the game's own big-head control, HeadScaler, set to bone scale 0; one local player "
                    "only, as the scale shows in every view)");
REXCVAR_DEFINE_BOOL(gowj_first_person_diag, false, "Judgment",
                    "First-person camera: log the local players' hide state once a second (pawn / weapon flags, "
                    "HiddenActors, camera and weapon positions) - log only, for tests");

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

// Judgment TU4's object tables and object layout: adaptive_triggers.cpp's (its comment: where they come from)
constexpr const char* kVersion = "TU4";
constexpr uint32_t kGNamesVar = 0x8355A328u;
constexpr uint32_t kGObjectsVar = 0x8355A368u;
constexpr const char* kLayout = "32,40,44,52,64,68,60,68,84,104,8,0,1";   // ue3::obj::LayoutText order
const obj::Layout& L() { return obj::CurrentLayout(); }
uint32_t NameOf(uint32_t object) { return obj::Read32(object + L().name); }   // its FName's index

// The modes moved to the eyes (exact class names, Judgment's GearGame script: Gears 3's list - the same classes and
// ViewOffset defaults; a subclass not listed stays as it is).
// fov: --gowj_first_person_fov applies (aiming keeps its zoom, roadie run its wider view).
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
  const std::string s = REXCVAR_GET(gowj_first_person_offset);
  if (std::sscanf(s.c_str(), "%f , %f , %f", &x, &y, &z) == 3 && std::abs(x) < 400 && std::abs(y) < 400 &&
      std::abs(z) < 400) {
    if (x != g_off[0] || y != g_off[1] || z != g_off[2])
      REXLOG_INFO("first person: camera offset ({}, {}, {})", x, y, z);
    g_off[0] = x, g_off[1] = y, g_off[2] = z;
  }
  g_fov = std::clamp(REXCVAR_GET(gowj_first_person_fov), 0, 170);
}

const ClassInfo& InfoOf(uint32_t cls) {
  const uint32_t name = NameOf(cls);
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
  const PropKey k{cls, NameOf(cls), p};
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
uint32_t g_camCls = 0, g_camClsCursor = 0;   // GearPlayerCamera (Gears 3 job 1850: the modes' Outer chain did not
                                              // reach it - found directly)
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
    o = obj::Read32(o + L().outer);   // UObject::Outer
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
// the head is collapsed separately (HeadTick, --gowj_first_person_head).
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

// ---- your own head (--gowj_first_person_head off, the default; agent062-out/fphead-research.md) ------------------
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

// A property object (UStruct super / first child, UField Next - the layout's), for the array's element struct;
// obj::PropertyOffset gives offsets only.
uint32_t PropertyObject(uint32_t cls, const char* p) {
  for (uint32_t c = cls, guard = 0; c && guard < 64; c = obj::Read32(c + L().super), guard++)
    for (uint32_t f = obj::Read32(c + L().children), n = 0; f && n < 10000; f = obj::Read32(f + L().next), n++)
      if (obj::ObjectName(f) == p) return f;
  return 0;
}

// AnimTree.SkelControlLists' element size and ControlHead offset: from the ArrayProperty's Inner (StructProperty, at
// the layout's bool_mask - the first field after UProperty, like UBoolProperty's BitMask: +104 in Judgment as in Gears 3)
// and its Struct (the same field), each checked by name; its ElementSize at the layout's elem_size; else the UE3 layout
// read from Gears 2's Engine package (BoneName 8, ControlHead +8, DrawY +12: 16 bytes).
struct ListLayout { uint32_t stride = 16, head = 8; int via = 0; };
ListLayout ListsLayout(uint32_t treeCls) {
  static uint32_t cachedCls = 0, cachedName = 0;
  static ListLayout cached;
  if (treeCls == cachedCls && NameOf(treeCls) == cachedName) return cached;
  ListLayout l;
  if (const uint32_t arr = PropertyObject(treeCls, "SkelControlLists"); arr && obj::ObjectName(obj::ClassOf(arr)) == "ArrayProperty")
    if (const uint32_t inner = obj::Read32(arr + L().bool_mask); inner && obj::ObjectName(obj::ClassOf(inner)) == "StructProperty")
      if (const uint32_t st = obj::Read32(inner + L().bool_mask); st && obj::ObjectName(st) == "SkelControlListHead") {
        const uint32_t size = obj::Read32(inner + L().elem_size);
        const int head = obj::PropertyOffset(st, "ControlHead");
        if (size >= 12 && size <= 64 && head >= 0 && uint32_t(head) + 4 <= size) l = {size, uint32_t(head), 1};
      }
  REXLOG_INFO("first person: AnimTree.SkelControlLists element {} bytes, ControlHead +{} ({})", l.stride, l.head,
              l.via ? "reflection" : "UE3 layout, reflection not matched");
  cachedCls = treeCls, cachedName = NameOf(treeCls), cached = l;
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

// Every engine tick while first person is on (a few dozen reads per local player): the current weapon of each local
// player whose camera is in a first-person mode gets bIgnoreHiddenActorsMembership; anything we set it on that is no
// longer such a part (weapon switched, dropped, holstered, cover / third-person mode, player gone) gets it back.
std::unordered_set<uint32_t> g_wantParts;
void WeaponTick() {
  static const bool weaponOn = REXCVAR_GET(gowj_first_person_weapon), bodyOn = REXCVAR_GET(gowj_first_person_body);
  static const bool headOn = REXCVAR_GET(gowj_first_person_head);
  if (!weaponOn && !bodyOn) return;
  g_wantParts.clear();
  uint32_t headMesh = 0;
  int liveCams = 0;
  for (uint32_t cam : g_cams) {
    if (!obj::IsLive(cam)) continue;
    liveCams++;
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
  if (bodyOn && !headOn)
    HeadTick(liveCams == 1 ? headMesh : 0, liveCams > 1 ? "split screen (shown)" : "not first person");
}

// ---- --gowj_first_person_diag: once a second, per local player, log only --------------------------------------------
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
  static const bool on = REXCVAR_GET(gowj_first_person_diag);
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
    const uint32_t wi = Ptr(pawn ? pawn : pc, "WorldInfo");
    const PropInfo ts = Prop(obj::ClassOf(wi), "TimeSeconds");
    const float now = wi && ts.offset >= 0 ? obj::ReadFloat(wi + uint32_t(ts.offset)) : -1.f;
    // WorldInfo.Pauser (the pause menu sets it; for unattended tests: gameplay = not paused) - Gears 2's diag
    const char* paused = !wi || Prop(obj::ClassOf(wi), "Pauser").offset < 0 ? "?" : Ptr(wi, "Pauser") ? "yes" : "no";
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
                "pawn {} weapon {} | time {:.2f} paused {}", cam,
                Prop(obj::ClassOf(cam), "CurrentCamera").offset < 0 ? std::string("(no property)")
                    : active == gc ? std::string("gameplay") : active ? obj::ObjectName(obj::ClassOf(active)) : std::string("none"),
                mode ? obj::ObjectName(obj::ClassOf(mode)) : "?",
                g_patched.count(mode) ? "FP" : "3P", pawn ? obj::ObjectName(obj::ClassOf(pawn)) : "none",
                Flag(pawn, "bIsHiddenByCamera"), hidden, pawnIn, weapIn, now, paused);
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
  // the head (--gowj_first_person_head off): the control and its values now, what we will put back
  if (!REXCVAR_GET(gowj_first_person_body) || REXCVAR_GET(gowj_first_person_head)) {
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

// The tables and the layout, on a worker thread (adaptive_triggers.cpp's): every 2 s while the engine starts, then every
// 15 s; the first class lookup (a walk of the whole name table) runs there too, so the game thread never stalls on it.
std::atomic<bool> g_objects_warm{false};
void StartObjectsInit() {
  std::thread([] {
    obj::Layout layout;
    obj::ParseLayout(kLayout, &layout);
    for (int attempt = 0; attempt < 60 && !obj::Ready(); attempt++) {
      if (obj::Init(kGNamesVar, kGObjectsVar, &layout)) break;
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
void GowjInstallFirstPerson() {
  if (!REXCVAR_GET(gowj_first_person)) return;
  ReadSettings();
  rex::ui::RegisterBind("bind_cogbox_first_person", "PageUp", "First-person camera off / on (Judgment)", [] {
    // review r20 R1: a held key repeats - one switch per press (300 ms quiet before the next)
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (now - last.exchange(now, std::memory_order_relaxed) < 300) return;   // r21: every repeat restarts the wait
    g_want.store(!g_want.load(std::memory_order_relaxed), std::memory_order_relaxed);
  });
  REXLOG_INFO("first person: on (experimental) - walking, crouching, aiming and roadie run at eye level; PageUp toggles");
  StartObjectsInit();
}

// Game thread, every engine tick (cheats.cpp, the UEngine::GetMaxTickRate override). Off: one flag test.
void GowjFirstPersonTick() {
  static const bool enabled = REXCVAR_GET(gowj_first_person);   // read once (a launcher restart changes it)
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
  if (!obj::Ready() || !g_objects_warm.load(std::memory_order_acquire)) return;   // the worker thread's (above)
  if (!g_base) {   // the mode base class, a slice of the table per tick (it is in GearGame.xxx, loaded at start-up)
    static Clock::time_point next{};
    if (Clock::now() < next) return;
    next = Clock::now() + std::chrono::milliseconds(100);
    uint32_t total = 0;
    g_base = obj::FindClassRange("GearGameplayCameraMode", g_classCursor, kSlice, &total);
    g_classCursor = g_classCursor + kSlice >= total ? 0 : g_classCursor + kSlice;
    if (g_base) REXLOG_INFO("first person: GearGameplayCameraMode found (Judgment {})", kVersion);
    return;
  }
  WeaponTick();   // the local players' weapons: every tick (a weapon switch shows at once)
  DiagTick();     // --gowj_first_person_diag: once a second, log only
  if (!g_passActive) {
    const Clock::time_point now = Clock::now();
    if (now < g_nextPass) return;
    g_nextPass = now + kPassEvery;
    g_passActive = true;
    g_cursor = 0;
    g_seen.clear();
    g_seenCams.clear();
    ReadSettings();
  }
  if (!g_camCls) {   // GearPlayerCamera, a slice of the table per tick until found (Gears 2's; then a cached lookup)
    uint32_t n = 0;
    g_camCls = obj::FindClassRange("GearPlayerCamera", g_camClsCursor, kSlice, &n);
    g_camClsCursor = g_camClsCursor + kSlice >= n ? 0 : g_camClsCursor + kSlice;
    if (g_camCls) REXLOG_INFO("first person: GearPlayerCamera found");
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
                REXCVAR_GET(gowj_first_person_weapon) ? "" : " (weapon option off)");
  g_cams = g_seenCams;
  static size_t logged = size_t(-1);
  if (g_patched.size() != logged) {
    REXLOG_INFO("first person: {} camera mode(s) at eye level ({} dropped)", g_patched.size(), before - g_patched.size());
    logged = g_patched.size();
  }
}
