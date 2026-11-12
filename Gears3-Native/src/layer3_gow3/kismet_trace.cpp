// Layer 3 (Gears of War 3): developer mode's Kismet trace (--mod_kismet_trace=<package part>, off by default). From
// KiltedKernel's Gears 3 port (runtime/ue3/kismet_trace.cpp there: GOW3_KISMET_TRACE). Our code (MIT).
//
// Which Kismet ops run: every 100 ms the live sequence ops are sampled through UE3 reflection, and each change of an
// op's SequenceOp.ActivateCount (actions, conditions, sequences) or an event's SequenceEvent.TriggerCount is logged
// with its package and sequence path:
//   kismet: [GOW1_SP_Prison_P] Main_Sequence.initialPlayerStuff.SeqAct_Teleport_4 (SeqAct_Teleport) x1
// The value filters by package (a part of the package name: GOW1_ for the trilogy's converted levels); "1" traces every
// package. Sampling sees the count, not each activation: two within 100 ms log once, as x2. A mod creator's tool for
// finding where a converted level's scripting stops.
#include "gow3_addresses.h"

#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>

REXCVAR_DEFINE_STRING(mod_kismet_trace, "", "Gears3",
                      "Developer mode: log the Kismet ops that run in packages whose name has this text (1: all)");

namespace {
namespace obj = ue3::obj;
constexpr uint32_t kOuter = 40;   // UObject::Outer (mod_chapters.cpp's)

std::string PackageOf(uint32_t o) {
  uint32_t top = o;
  for (int k = 0; k < 24 && obj::Read32(top + kOuter); k++) top = obj::Read32(top + kOuter);
  return obj::ObjectName(top);
}

// the op's path below its level: Main_Sequence.<subsequence>... .<op>
std::string PathOf(uint32_t o) {
  std::string path = obj::ObjectName(o);
  uint32_t outer = obj::Read32(o + kOuter);
  for (int k = 0; k < 16 && outer; k++) {
    const std::string n = obj::ObjectName(outer);
    if (n == "PersistentLevel") break;
    path = n + "." + path;
    outer = obj::Read32(outer + kOuter);
  }
  return path;
}

void Run(std::string filter) {
  while (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar))
    std::this_thread::sleep_for(std::chrono::seconds(1));
  uint32_t opClass = 0, eventClass = 0;   // (Engine's classes load after the tables are found)
  for (int tries = 0; tries < 600 && !opClass; tries++)
    if (!(opClass = obj::FindClass("SequenceOp"))) std::this_thread::sleep_for(std::chrono::seconds(1));
  eventClass = obj::FindClass("SequenceEvent");
  if (!opClass) { REXLOG_WARN("kismet: SequenceOp not found - no trace"); return; }
  std::unordered_map<uint32_t, int> opOff, evOff;   // per class: ActivateCount / TriggerCount offsets
  std::unordered_map<uint32_t, uint32_t> last;      // op -> last count
  // op -> (its outermost object, in the filtered packages): an address freed with its level and reused by another
  // level's op is judged again
  std::unordered_map<uint32_t, std::pair<uint32_t, bool>> wanted;
  int logged = 0;
  REXLOG_INFO("kismet: tracing sequence ops{}", filter.empty() ? "" : " in packages with \"" + filter + "\"");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (uint32_t o : obj::FindInstancesOf(opClass)) {
      uint32_t top = o;
      for (int k = 0; k < 24 && obj::Read32(top + kOuter); k++) top = obj::Read32(top + kOuter);
      auto w = wanted.find(o);
      if (w == wanted.end() || w->second.first != top) {
        wanted[o] = {top, filter.empty() || PackageOf(o).find(filter) != std::string::npos};
        last.erase(o);
        w = wanted.find(o);
      }
      if (!w->second.second) continue;
      const uint32_t cls = obj::ClassOf(o);
      const bool isEvent = eventClass && obj::IsA(o, eventClass);
      auto& offs = isEvent ? evOff : opOff;
      auto it = offs.find(cls);
      if (it == offs.end()) it = offs.emplace(cls, obj::PropertyOffset(cls, isEvent ? "TriggerCount" : "ActivateCount")).first;
      if (it->second < 0) continue;
      const uint32_t count = obj::Read32(o + uint32_t(it->second));
      auto [l, fresh] = last.emplace(o, count);
      if (fresh) {
        if (count == 0) continue;   // first seen: report what already ran
      } else if (l->second == count) {
        continue;
      }
      const uint32_t before = fresh || count < l->second ? 0 : l->second;   // (lower: the level was reloaded)
      l->second = count;
      if (count == before) continue;
      if (logged++ < 20000)
        REXLOG_INFO("kismet: [{}] {} ({}) x{}", PackageOf(o), PathOf(o), obj::ObjectName(cls), count - before);
    }
  }
}

}  // namespace

// OnPostSetup: nothing without the switch
void Gow3InstallKismetTraceIfRequested() {
  const std::string spec = REXCVAR_GET(mod_kismet_trace);
  if (spec.empty()) return;
  std::thread(Run, spec == "1" ? std::string() : spec).detach();
}
