// Layer 1 (XDK 7645) Stage 1 tools: the resource-call watch (d3d_watch.cpp) and the comparison mode (resource_check.cpp).
#pragma once

#include <cstdint>

namespace rex { class Runtime; }

// --gow2_res_check=N: hash GPU-read resources every N frames and report changes no hooked event explains.
void Gow2InstallResourceCheckIfRequested(rex::Runtime* runtime);
// Marks memory at addr as about to change (called from the resource-call hooks).
void Gow2ResourceExpect(uint32_t addr, const char* event, bool all_textures = false);
