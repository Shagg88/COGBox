// Layer 3 (Gears 3): frame-rate probe and Kismet minimum delay (GitHub #405 / #413 / #414) - this game's options and
// object tables only. The probe itself is ONE shared copy, Gears 2's src/layer2_ue3/fps_probe.cpp (ue3/fps_probe.h).
// Our code (MIT).
#include "gow3_addresses.h"

#include <ue3/fps_probe.h>
#include <ue3/objects.h>

#include <rex/cvar.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(fps_probe, false, "Gears3",
                    "Developer: log Kismet op rates, pawn damage per second and stall dumps (GitHub #405 / #414 "
                    "frame-rate logic; compare a 30 fps and an unlocked run)");
REXCVAR_DEFINE_DOUBLE(kismet_min_delay, 0.0, "Gears3",
                      "Experimental (GitHub #405 / #414): Kismet Delay actions shorter than this many seconds are "
                      "lengthened to it (0.0333 = one 30 fps frame, the Xbox's shortest); 0 = off")
    .range(0.0, 0.5);

namespace {
namespace obj = ue3::obj;

// Gears 3's own ue3::obj (fixed layout): UObject.Outer at +40, the shared file's default
bool OpenObjects() { return obj::Ready() || obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar); }
}  // namespace

// OnPostSetup: nothing without one of the two switches.
void Gow3InstallFpsProbe() {
  ue3::FpsProbeConfig c;
  c.probe = REXCVAR_GET(fps_probe);
  c.min_delay = static_cast<float>(REXCVAR_GET(kismet_min_delay));
  c.open_objects = OpenObjects;
  c.outer_offset = nullptr;
  ue3::StartFpsProbe(c);
}
