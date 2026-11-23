// Layer 0 (game-free): --gow2_hold_guest keeps the game's main thread suspended after launch, so nothing but the GPU
// runs. Used with the SDK's --trace_gpu_replay (patch 6): the command processor replays a recorded frame while the game
// itself never starts, so guest code can't touch the memory the replay restores.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(gow2_hold_guest, false, "Gears2",
                    "Keep the game's main thread suspended (for --trace_gpu_replay)");

void XenosHoldGuestIfRequested(rex::system::XThread* main_thread) {
  if (!REXCVAR_GET(gow2_hold_guest) || main_thread == nullptr) return;
  uint32_t count = 0;
  main_thread->Suspend(&count);
  REXLOG_INFO("gow2_hold_guest: game main thread held (suspend count was {})", count);
}
