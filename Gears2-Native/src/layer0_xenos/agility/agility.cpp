// 0.2.0 (GitHub #9): the DirectX 12 Agility SDK. These two exports in the game program tell Windows to load the bundled
// D3D12Core.dll from <exe folder>\D3D12\ instead of its own Direct3D 12 runtime. Windows 10's own runtime doesn't know
// Shader Model 6.6, which the renderer needs - without this the renderer stayed off and the game was a black window.
// Compiled into each game's exe (not the layer 0 library: the exports must be in the .exe). Our code (MIT).
#include <cstdint>

// 0.4.9: the version comes from CMake (cmake/gn_deps.cmake), from the same Agility folder whose D3D12Core.dll is copied to
// D3D12\ - the two must match (620 = Microsoft.Direct3D.D3D12 1.620.1, 614 = 1.614.0, the 0.4.8 one).
#ifndef GN_AGILITY_SDK_VERSION
#error "GN_AGILITY_SDK_VERSION not set (cmake/gn_deps.cmake)"
#endif

extern "C" {
__declspec(dllexport) extern const uint32_t D3D12SDKVersion = GN_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
