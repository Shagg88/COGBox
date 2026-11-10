// Layer 2 (Unreal Engine 3), from Gears 2 (option renamed ue3_launch). Boot straight into a map. UE3 on the 360 builds its command line as "dummy " + the title's
// launch data (Gears 2: sub_8280F0B0, prefix string 0x82176A80; Gears 3: XamLoaderGetLaunchData caller in recomp file
// 183, prefix "dummy " at 0x821CB974 - notes/03). So
// --ue3_launch="SP_Hospital_P" puts that text in the launch data and the game opens that map instead of the front end.
// Map names are the *_P packages in GearGame\CookedXbox360 on the Gears 3 disc (SP_* campaign, MP_* multiplayer).
// Off unless set.
#include <ue3/launch.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>

#include <string>

REXCVAR_DEFINE_STRING(ue3_launch, "", "UE3",
                      "UE3 command line after the exe name, e.g. \"SP_Hospital_P\" (sets the title's launch data)");

void Ue3SetLaunchCommandLineIfRequested() {
  const std::string cmd = REXCVAR_GET(ue3_launch);
  if (cmd.empty()) return;
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return;
  auto xam = ks->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) { REXLOG_WARN("ue3_launch: xam module not found"); return; }
  auto& ld = xam->loader_data();
  ld.launch_data.assign(cmd.begin(), cmd.end());
  ld.launch_data.push_back(0);
  ld.launch_data_present = true;
  REXLOG_INFO("ue3_launch: launch data = '{}'", cmd);
}
