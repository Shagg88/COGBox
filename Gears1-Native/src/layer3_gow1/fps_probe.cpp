// Layer 3 (Gears of War (Gears 1), ported 23 Nov from Judgment's so all four games have it): frame-rate probe and Kismet minimum delay - GitHub #405 / #413 (Judgment, Monroe Commons: endless
// loading after the last door) and #414 (Gears 3, RAAM's Shadow "Under the Shadow": a drone dies, the scene does not go
// on). Both only above 30 fps [reporters]. A frame-rate cap was turned down: the goal is to find the game logic that
// counts frames and make it count time. Our code (MIT).
//
// --fps_probe (developer; off): a worker thread reads the engine's objects through UE3 reflection (ue3/objects.h):
//   * Kismet rates: every 10 s, the sequence ops that were activated most often (ActivateCount / TriggerCount
//     deltas per second, with package, path, class, and a SeqAct_Delay's Duration). An op whose rate grows with the
//     frame rate (compare a 30 fps and a 120 fps run of the same scene) is a per-frame loop:
//       fps_probe: kismet 10 s: 118.4/s [SPE_E2_P] Main_Sequence.Door.SeqAct_Delay_12 (SeqAct_Delay, Duration 0.000)
//   * Pawn damage: every 100 ms each pawn's Health; damage per second by damage type and pawn class every 10 s, and
//     each death with its HitDamageType and LastHitBy. Damage dealt per frame (not per second) shows as a rate that
//     grows with the frame rate.
//   * A dump of the active (latent) Kismet ops and the streaming levels' state, at each pawn death of a non-player
//     pawn (at most one per 30 s) and when (not wired in this game yet) FpsProbeDump is called (a load that does not end).
// --kismet_min_delay=S (experimental; 0 = off): every SeqAct_Delay whose Duration / DefaultDuration is below S (and not
//   negative) is set to S, re-checked every 2 s (levels stream in). On the Xbox at 30 fps a Delay of 0 lasted one frame
//   (33 ms); above 30 fps it lasts one shorter frame, so a Kismet loop through a short Delay runs (and counts) faster.
//   S = 0.0333 gives such loops the Xbox's timing at any frame rate. Each change is logged once per op.
#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(fps_probe, false, "Gears 1",
                    "Developer: log Kismet op rates, pawn damage per second and stall dumps (GitHub #405 / #414 "
                    "frame-rate logic; compare a 30 fps and an unlocked run)");
REXCVAR_DEFINE_DOUBLE(kismet_min_delay, 0.0, "Gears 1",
                      "Experimental (GitHub #405 / #414): Kismet Delay actions shorter than this many seconds are "
                      "lengthened to it (0.0333 = one 30 fps frame, the Xbox's shortest); 0 = off")
    .range(0.0, 0.5);

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

// the object tables as cheats.cpp opens them (TU5: GNames / GObjObjects, the layout given)
constexpr uint32_t kGNamesVar = 0x82FED4F0u;
constexpr uint32_t kGObjectsVar = 0x82FDD1C4u;
constexpr const char* kLayout = "4,40,44,52,60,76,64,72,96,124,16,1,1";
// Gears 1's layer 2 copy has no obj::IsLive (Gears 2's has): the same test here - the object is still in the table at
// its own slot (not freed / reused).
bool IsLive(uint32_t o) {
  if (!o || !obj::Ready()) return false;
  const uint32_t table = obj::Read32(kGObjectsVar), count = obj::Read32(kGObjectsVar + 4);
  const uint32_t s = obj::Read32(o + obj::CurrentLayout().slot);
  return table && s < count && count < 4000000 && obj::Read32(table + 4 * s) == o;
}
bool OpenObjects() {
  if (obj::Ready()) return true;
  obj::Layout layout;
  obj::ParseLayout(kLayout, &layout);
  return obj::Init(kGNamesVar, kGObjectsVar, &layout);
}

uint32_t OuterOff() {
  const uint32_t o = obj::CurrentLayout().outer;
  return o ? o : 40u;
}
std::string PackageOf(uint32_t o) {
  uint32_t top = o;
  for (int k = 0; k < 24 && obj::Read32(top + OuterOff()); k++) top = obj::Read32(top + OuterOff());
  return obj::ObjectName(top);
}
std::string PathOf(uint32_t o) {
  std::string path = obj::ObjectName(o);
  uint32_t outer = obj::Read32(o + OuterOff());
  for (int k = 0; k < 16 && outer; k++) {
    const std::string n = obj::ObjectName(outer);
    if (n == "PersistentLevel") break;
    path = n + "." + path;
    outer = obj::Read32(outer + OuterOff());
  }
  return path;
}
float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint32_t FloatBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// per class: a property offset (-1: none), looked up once
struct OffCache {
  std::unordered_map<uint64_t, int> map;
  int Get(uint32_t cls, const char* prop, uint32_t tag) {
    const uint64_t key = (uint64_t(cls) << 8) | tag;
    auto it = map.find(key);
    if (it == map.end()) it = map.emplace(key, obj::PropertyOffset(cls, prop)).first;
    return it->second;
  }
};

struct Classes {
  uint32_t op = 0, event = 0, delay = 0, pawn = 0, streaming = 0, controller = 0;
};

std::mutex g_dump_mutex;
std::atomic<int64_t> g_last_dump{0};   // Clock ticks of the last dump (0: none)

// The active Kismet ops (SequenceOp.bActive) and the streaming levels, for a stall or a death.
void Dump(const Classes& c, OffCache& offs, const std::string& why) {
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  g_last_dump = Clock::now().time_since_epoch().count();
  REXLOG_INFO("fps_probe: dump ({}) - active Kismet ops and streaming levels follow", why);
  int shown = 0;
  if (c.op) {
    for (uint32_t o : obj::FindInstancesOf(c.op)) {
      const uint32_t cls = obj::ClassOf(o);
      const uint32_t mask = obj::BoolMask(cls, "bActive");
      const int off = offs.Get(cls, "bActive", 1);
      if (off < 0 || !mask || !(obj::Read32(o + uint32_t(off)) & mask)) continue;
      const std::string pkg = PackageOf(o);
      if (pkg.empty() || pkg == "Transient") continue;
      std::string extra;
      if (c.delay && obj::IsA(o, c.delay)) {
        const int d = offs.Get(cls, "Duration", 2), r = offs.Get(cls, "RemainingTime", 3);
        if (d >= 0) extra += " Duration " + std::to_string(AsFloat(obj::Read32(o + uint32_t(d))));
        if (r >= 0) extra += " RemainingTime " + std::to_string(AsFloat(obj::Read32(o + uint32_t(r))));
      }
      if (shown++ < 200) REXLOG_INFO("fps_probe:   active [{}] {} ({}){}", pkg, PathOf(o), obj::ObjectName(cls), extra);
    }
  }
  REXLOG_INFO("fps_probe:   {} active op(s)", shown);
  if (c.streaming) {
    int n = 0;
    for (uint32_t s : obj::FindInstancesOf(c.streaming)) {
      const uint32_t cls = obj::ClassOf(s);
      std::string flags;
      for (const char* b : {"bShouldBeLoaded", "bShouldBeVisible", "bHasLoadRequestPending", "bIsRequestingUnloadAndRemoval",
                            "bShouldBlockOnLoad", "bHasUnloadRequestPending"}) {
        const uint32_t mask = obj::BoolMask(cls, b);
        const int off = obj::PropertyOffset(cls, b);
        if (mask && off >= 0) flags += std::string(" ") + b + "=" + ((obj::Read32(s + uint32_t(off)) & mask) ? "1" : "0");
      }
      const int pn = obj::PropertyOffset(cls, "PackageName"), ll = obj::PropertyOffset(cls, "LoadedLevel");
      const std::string name = pn >= 0 ? obj::Name(obj::Read32(s + uint32_t(pn))) : std::string("?");
      const bool loaded = ll >= 0 && obj::Read32(s + uint32_t(ll)) != 0;
      if (n++ < 300) REXLOG_INFO("fps_probe:   streaming {} loaded={}{}", name, loaded ? 1 : 0, flags);
    }
    REXLOG_INFO("fps_probe:   {} streaming level object(s)", n);
  }
}

Classes g_classes;
OffCache* g_dump_offs = nullptr;
std::atomic<bool> g_ready{false};

void ApplyMinDelay(const Classes& c, OffCache& offs, std::unordered_set<uint32_t>& changed, float min_s) {
  if (!c.delay) return;
  for (uint32_t o : obj::FindInstancesOf(c.delay)) {
    const uint32_t cls = obj::ClassOf(o);
    bool any = false;
    std::string was;
    for (const char* prop : {"Duration", "DefaultDuration"}) {
      const int off = offs.Get(cls, prop, prop[0] == 'D' && prop[1] == 'u' ? 2 : 4);
      if (off < 0) continue;
      const float v = AsFloat(obj::Read32(o + uint32_t(off)));
      if (v >= 0.0f && v < min_s) {
        obj::Write32(o + uint32_t(off), FloatBits(min_s));
        was += std::string(" ") + prop + " " + std::to_string(v);
        any = true;
      }
    }
    if (any && changed.insert(o).second && changed.size() <= 2000)
      REXLOG_INFO("kismet_min_delay: [{}] {} ({}):{} -> {:.4f}", PackageOf(o), PathOf(o), obj::ObjectName(cls), was, min_s);
  }
}

void Run(bool probe, float min_delay) {
  while (!OpenObjects()) std::this_thread::sleep_for(std::chrono::seconds(1));
  Classes c;
  for (int tries = 0; tries < 900 && !c.op; tries++)
    if (!(c.op = obj::FindClass("SequenceOp"))) std::this_thread::sleep_for(std::chrono::seconds(1));
  if (!c.op) {
    REXLOG_WARN("fps_probe: SequenceOp not found - nothing done");
    return;
  }
  c.event = obj::FindClass("SequenceEvent");
  c.delay = obj::FindClass("SeqAct_Delay");
  c.pawn = obj::FindClass("Pawn");
  c.streaming = obj::FindClass("LevelStreaming");
  c.controller = obj::FindClass("Controller");
  REXLOG_INFO("fps_probe: classes SequenceOp {:08X} SeqAct_Delay {:08X} Pawn {:08X} LevelStreaming {:08X} (probe {}, "
              "kismet_min_delay {:.4f})", c.op, c.delay, c.pawn, c.streaming, probe, min_delay);
  static OffCache dump_offs;
  g_classes = c;
  g_dump_offs = &dump_offs;
  g_ready = true;

  OffCache offs;
  std::unordered_set<uint32_t> delays_changed;
  std::unordered_map<uint32_t, uint32_t> last_count;                    // op -> count
  std::unordered_map<uint32_t, uint64_t> window;                        // op -> activations in this window
  std::unordered_map<uint32_t, int32_t> last_health;                    // pawn -> health
  std::unordered_map<std::string, double> dmg_window;                   // "PawnClass / DamageType" -> damage
  auto win_start = Clock::now(), last_ops = Clock::now() - std::chrono::seconds(5), last_delay = last_ops;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto now = Clock::now();
    if (min_delay > 0.0f && now - last_delay >= std::chrono::seconds(2)) {
      last_delay = now;
      ApplyMinDelay(c, offs, delays_changed, min_delay);
    }
    if (!probe) continue;
    // Kismet counts: every 500 ms (a full table walk)
    if (now - last_ops >= std::chrono::milliseconds(500)) {
      last_ops = now;
      for (uint32_t o : obj::FindInstancesOf(c.op)) {
        const uint32_t cls = obj::ClassOf(o);
        const bool ev = c.event && obj::IsA(o, c.event);
        const int off = offs.Get(cls, ev ? "TriggerCount" : "ActivateCount", ev ? 5 : 6);
        if (off < 0) continue;
        const uint32_t n = obj::Read32(o + uint32_t(off));
        auto [it, fresh] = last_count.emplace(o, n);
        if (!fresh) {
          if (n > it->second) window[o] += n - it->second;
          it->second = n;
        }
      }
    }
    // pawns: every 100 ms
    if (c.pawn) {
      for (uint32_t p : obj::FindInstancesOf(c.pawn)) {
        const uint32_t cls = obj::ClassOf(p);
        const int ho = offs.Get(cls, "Health", 7);
        if (ho < 0) continue;
        const int32_t h = static_cast<int32_t>(obj::Read32(p + uint32_t(ho)));
        auto [it, fresh] = last_health.emplace(p, h);
        if (fresh) continue;
        const int32_t before = it->second;
        it->second = h;
        if (h >= before) continue;
        const int dt = offs.Get(cls, "HitDamageType", 8), lh = offs.Get(cls, "LastHitBy", 9);
        const uint32_t dtype = dt >= 0 ? obj::Read32(p + uint32_t(dt)) : 0;
        const std::string type = dtype ? obj::ObjectName(dtype) : std::string("?");
        dmg_window[obj::ObjectName(cls) + " / " + type] += double(before - h);
        if (before > 0 && h <= 0) {
          const uint32_t by = lh >= 0 ? obj::Read32(p + uint32_t(lh)) : 0;
          REXLOG_INFO("fps_probe: death [{}] {} ({}) health {} -> {}, HitDamageType {}, LastHitBy {}", PackageOf(p),
                      obj::ObjectName(p), obj::ObjectName(cls), before, h, type,
                      by ? obj::ObjectName(by) + " (" + obj::ObjectName(obj::ClassOf(by)) + ")" : std::string("none"));
          const std::string cn = obj::ObjectName(cls);
          const bool player = cn.find("Marcus") != std::string::npos || cn.find("Player") != std::string::npos;
          if (!player && (g_last_dump.load() == 0 ||
                          Clock::now() - Clock::time_point(Clock::duration(g_last_dump.load())) >= std::chrono::seconds(30))) Dump(c, dump_offs, "death of " + obj::ObjectName(p));
        }
      }
    }
    // the 10 s summary
    if (now - win_start >= std::chrono::seconds(10)) {
      const double secs = std::chrono::duration<double>(now - win_start).count();
      win_start = now;
      std::vector<std::pair<uint64_t, uint32_t>> top;
      for (auto& [o, n] : window) top.emplace_back(n, o);
      std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (size_t i = 0; i < top.size() && i < 12; i++) {
        const uint32_t o = top[i].second;
        if (!IsLive(o)) continue;
        const uint32_t cls = obj::ClassOf(o);
        std::string extra;
        if (c.delay && obj::IsA(o, c.delay)) {
          const int d = offs.Get(cls, "Duration", 2);
          if (d >= 0) extra = ", Duration " + std::to_string(AsFloat(obj::Read32(o + uint32_t(d))));
        }
        REXLOG_INFO("fps_probe: kismet {:.0f} s: {:.1f}/s [{}] {} ({}{})", secs, double(top[i].first) / secs, PackageOf(o),
                    PathOf(o), obj::ObjectName(cls), extra);
      }
      for (auto& [k, d] : dmg_window) REXLOG_INFO("fps_probe: damage {:.0f} s: {:.1f}/s {}", secs, d / secs, k);
      window.clear();
      dmg_window.clear();
    }
  }
}

}  // namespace

// game_patches.cpp (the loading-screen watchdog): a load that has not ended after 20 s (any thread; returns at once, the dump runs on its own thread).
void FpsProbeDump(const char* why) {
  if (!REXCVAR_GET(fps_probe) || !g_ready.load()) return;
  std::string w = why;
  std::thread([w] { Dump(g_classes, *g_dump_offs, w); }).detach();
}

// OnPostSetup: nothing without one of the two switches.
void Gow1InstallFpsProbe() {
  const bool probe = REXCVAR_GET(fps_probe);
  const float min_delay = static_cast<float>(REXCVAR_GET(kismet_min_delay));
  if (!probe && min_delay <= 0.0f) return;
  std::thread(Run, probe, min_delay).detach();
}
