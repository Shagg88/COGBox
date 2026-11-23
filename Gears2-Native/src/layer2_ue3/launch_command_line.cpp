// Layer 2 (Unreal Engine 3): boot straight into a map. UE3 on the 360 builds its command line as "dummy " + the title's
// launch data (Gears 2: sub_8280F0B0 reads XamLoaderGetLaunchData, prefix string at 0x82176A80, job 021; Gears 3:
// XamLoaderGetLaunchData caller in recomp file 183, prefix "dummy " at 0x821CB974 - notes/03). So
// --ue3_launch="SP_Hospital_P" (Gears 2: --gow2_launch) puts that text in the launch data and the game opens that map
// instead of the front end. Map names are the *_P packages in GearGame\CookedXenon / CookedXbox360 (SP_* campaign,
// MP_* multiplayer). Off unless set.
// ONE copy for all four games (Gears 1, 3 and Judgment build it from GN_LAYER0_HOME). The option keeps each game's
// name: Gears 2 builds it with GN_UE3_LAUNCH_GOW2=1 (--gow2_launch, group Gears2), the others --ue3_launch (group UE3).
#include <ue3/launch.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>

#include <string>

#if defined(GN_UE3_LAUNCH_GOW2) && GN_UE3_LAUNCH_GOW2
REXCVAR_DEFINE_STRING(gow2_launch, "", "Gears2",
                      "UE3 command line after the exe name, e.g. \"SP_Hospital_P\" (sets the title's launch data)");
static std::string LaunchOption() { return REXCVAR_GET(gow2_launch); }
static constexpr const char* kLaunchName = "gow2_launch";
#else
REXCVAR_DEFINE_STRING(ue3_launch, "", "UE3",
                      "UE3 command line after the exe name, e.g. \"SP_Hospital_P\" (sets the title's launch data)");
static std::string LaunchOption() { return REXCVAR_GET(ue3_launch); }
static constexpr const char* kLaunchName = "ue3_launch";
#endif

void Ue3SetLaunchCommandLineIfRequested() {
  const std::string cmd = LaunchOption();
  if (cmd.empty()) return;
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return;
  auto xam = ks->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) { REXLOG_WARN("{}: xam module not found", kLaunchName); return; }
  auto& ld = xam->loader_data();
  ld.launch_data.assign(cmd.begin(), cmd.end());
  ld.launch_data.push_back(0);
  ld.launch_data_present = true;
  REXLOG_INFO("{}: launch data = '{}'", kLaunchName, cmd);
}
