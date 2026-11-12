// Layer 3 (Gears of War 3): developer mode's property log (--mod_log_props, off by default). From KiltedKernel's Gears 3
// port (runtime/ue3/prop_log.cpp there: GOW3_LOG_PROPS). Our code (MIT).
//
// --mod_log_props="Class:Prop,Prop.Sub;Class2:Prop": every 5 s, each live instance of each class (subclasses too) logs
// the properties listed. "Prop.Sub" follows an object reference one step (Mesh.AnimRotationOnly: the pawn's mesh
// component's). A value is shown as its type reads it: bool, byte, int and float, an object by name.
//   props: GearPawn_COGMarcusG2MP_0 bForceStretchToMarcusSkeleton=false Mesh.AnimRotationOnly=1
// A mod creator's tool for checking what an adapter or a converted level's script set.
#include "gow3_addresses.h"

#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(mod_log_props, "", "Gears3",
                      "Developer mode: log properties of live objects every 5 s, \"Class:Prop,Prop.Sub;Class2:Prop\"");

namespace {
namespace obj = ue3::obj;

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= s.size()) {
    const size_t b = std::min(s.find(sep, a), s.size());
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

// one property of one object, as text (cls: the object's class)
std::string Value(uint32_t o, const std::string& prop) {
  const uint32_t cls = obj::ClassOf(o);
  const int off = obj::PropertyOffset(cls, prop.c_str());
  if (off < 0) return "?";
  if (const uint32_t mask = obj::BoolMask(cls, prop.c_str())) return (obj::Read32(o + uint32_t(off)) & mask) ? "true" : "false";
  const int size = obj::PropertySize(cls, prop.c_str());
  const uint32_t word = obj::Read32(o + uint32_t(off));
  if (size == 1) return std::to_string(word >> 24);
  if (size != 4) return "(" + std::to_string(size) + " bytes)";
  // (an object reference reads as an object; else int, and a float beside it when it looks like one)
  if (word && obj::IsA(word, obj::FindClass("Object"))) return obj::ObjectName(word);
  float f;
  std::memcpy(&f, &word, 4);
  const bool floaty = (word & 0x7F800000) && (word & 0x7F800000) != 0x7F800000 && std::abs(f) < 1e7f;
  return std::to_string(int32_t(word)) + (floaty ? " (" + std::to_string(f) + ")" : "");
}

void Run(std::string spec) {
  while (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar))
    std::this_thread::sleep_for(std::chrono::seconds(1));
  REXLOG_INFO("props: logging {}", spec);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    for (const std::string& entry : Split(spec, ';')) {
      const size_t colon = entry.find(':');
      if (colon == std::string::npos) continue;
      const uint32_t cls = obj::FindClass(entry.substr(0, colon).c_str());
      if (!cls) continue;   // (not loaded yet)
      const std::vector<std::string> props = Split(entry.substr(colon + 1), ',');
      for (uint32_t o : obj::FindInstancesOf(cls)) {
        std::string line = obj::ObjectName(o);
        for (const std::string& p : props) {
          const size_t dot = p.find('.');
          std::string v;
          if (dot == std::string::npos) {
            v = Value(o, p);
          } else {
            const uint32_t c = obj::ClassOf(o);
            const int off = obj::PropertyOffset(c, p.substr(0, dot).c_str());
            const uint32_t sub = off >= 0 ? obj::Read32(o + uint32_t(off)) : 0;
            v = sub ? Value(sub, p.substr(dot + 1)) : "None";
          }
          line += " " + p + "=" + v;
        }
        REXLOG_INFO("props: {}", line);
      }
    }
  }
}

}  // namespace

// OnPostSetup: nothing without the switch
void Gow3InstallPropLogIfRequested() {
  const std::string spec = REXCVAR_GET(mod_log_props);
  if (spec.empty()) return;
  std::thread(Run, spec).detach();
}
