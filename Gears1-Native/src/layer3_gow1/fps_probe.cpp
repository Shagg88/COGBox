// Layer 3 (Gears 1): frame-rate probe and Kismet minimum delay (GitHub #405 / #413 / #414) - this game's options and
// object tables only. The probe itself is ONE shared copy, Gears 2's src/layer2_ue3/fps_probe.cpp (ue3/fps_probe.h).
// Our code (MIT).
#include <ue3/fps_probe.h>
#include <ue3/objects.h>

#include <rex/cvar.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(fps_probe, false, "Gears 1",
                    "Developer: log Kismet op rates, pawn damage per second and stall dumps (GitHub #405 / #414 "
                    "frame-rate logic; compare a 30 fps and an unlocked run)");
REXCVAR_DEFINE_DOUBLE(kismet_min_delay, 0.0, "Gears 1",
                      "Experimental (GitHub #405 / #414): Kismet Delay actions shorter than this many seconds are "
                      "lengthened to it (0.0333 = one 30 fps frame, the Xbox's shortest); 0 = off")
    .range(0.0, 0.5);

namespace {
namespace obj = ue3::obj;

// the object tables as cheats.cpp opens them (TU5: GNames / GObjObjects, the layout given)
constexpr uint32_t kGNamesVar = 0x82FED4F0u;
constexpr uint32_t kGObjectsVar = 0x82FDD1C4u;
constexpr const char* kLayout = "4,40,44,52,60,76,64,72,96,124,16,1,1";
bool OpenObjects() {
  if (obj::Ready()) return true;
  obj::Layout layout;
  obj::ParseLayout(kLayout, &layout);
  return obj::Init(kGNamesVar, kGObjectsVar, &layout);
}
uint32_t OuterOffset() { return obj::CurrentLayout().outer; }
}  // namespace

// OnPostSetup: nothing without one of the two switches.
void Gow1InstallFpsProbe() {
  ue3::FpsProbeConfig c;
  c.probe = REXCVAR_GET(fps_probe);
  c.min_delay = static_cast<float>(REXCVAR_GET(kismet_min_delay));
  c.open_objects = OpenObjects;
  c.outer_offset = OuterOffset;
  ue3::StartFpsProbe(c);
}
