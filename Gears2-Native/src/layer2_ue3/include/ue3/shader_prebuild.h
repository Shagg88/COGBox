// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

// Patch 0068 (--ue3_shader_prebuild, default on): when the game opens a package, read it on a background thread, find
// its pixel shaders and hand them to our renderer to build before their first draw (xenos/dev_tools.h
// XenosPrebuildPixelShader). Needs SDK patch 32 (the file-open observer); without it this logs once and does nothing.
// Call after the renderer is installed (OnPostSetup).
void Ue3InstallShaderPrebuildIfRequested();
