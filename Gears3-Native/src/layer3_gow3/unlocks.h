// Layer 3 (Gears 3): GitHub #174 - "Unlock Insane from the start" (--gow3_unlock_insane, off by default). See unlocks.cpp.
#pragma once

// After the game image is loaded (OnPostSetup). Does nothing unless --gow3_unlock_insane is on.
void Gow3InstallUnlocks();
