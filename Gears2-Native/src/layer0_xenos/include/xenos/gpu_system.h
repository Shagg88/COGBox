// Gears2-Native - MIT. Stage 6 (notes/51): our own graphics system - the SDK's GPU emulation is not loaded.
#pragma once

#include <memory>

namespace rex::system {
class IGraphicsSystem;
}

// --gow2_gpu=ours: our IGraphicsSystem (window plumbing from the SDK's UI library; ring worker, register file,
// interrupts, vblank and the GPU MMIO range ours; every packet runs in our PM4 processor). Null otherwise (the SDK's
// --gpu_plugin is loaded as before). Called from ReXApp::OnPreSetup.
std::unique_ptr<rex::system::IGraphicsSystem> XenosCreateOurGpuSystemIfRequested();
// True once our graphics system has been created (the SDK's GPU cvars do not exist then).
bool XenosOurGpuSystem();
