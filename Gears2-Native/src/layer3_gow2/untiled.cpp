// Layer 3 (Gears of War 2): --gow2_untiled renders without predicated tiling (roadmap P3). The manifest's mid-asm hook
// at 0x82806A44 calls this just before the game stores 1 into its tiling flag (0x83122EA4) at startup; clearing r11
// stores 0 instead - the same effect as the community "black shading" emulator patch. On by default (user, 2026-10-20
// 22:44: reference runs use the patches needed for the original Xbox 360 image); --gow2_untiled=false keeps tiling.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include "gow2_addresses.h"

REXCVAR_DEFINE_BOOL(gow2_untiled, true, "Gears2", "Render without predicated tiling (stops the engine enabling it)");

void Gow2UntiledHook(PPCRegister& r11) {
  if (!REXCVAR_GET(gow2_untiled)) return;
  r11.u64 = 0;
  REXLOG_INFO("gow2_untiled: tiling flag cleared at startup ({:08X} = 0, {})", gow2::addr::kTilingFlag, gow2::addr::kVersion);
}
