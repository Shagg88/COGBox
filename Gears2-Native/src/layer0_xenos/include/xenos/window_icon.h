// Layer 0 (game-free): the game window's own icon (GitHub #355). Our code (MIT).
//
// Each game's taskbar / Alt+Tab icon is the game's own title image: the PNG at id 0x8000 in its XDBF (SPA) resource,
// read from the XEX the game has already decrypted in memory. Nothing is shipped with COGBox and nothing is written to
// disk. The picture is set on the window once both are known (the image at module launch, the window when it is made),
// whichever comes first. Windows only; elsewhere these do nothing. --cogbox_window_icon (default on) turns it off.
#pragma once

#include <cstddef>

namespace rex {
class Runtime;
}

namespace xenos::window_icon {

// The title image from the running game's XDBF (achievements_export.cpp calls this before anything else there).
void TakeFromRuntime(rex::Runtime* runtime);
// The PNG bytes themselves (copied); for tests and other callers.
void SetImage(const void* png, size_t size);
// The game window (gpu_system.cpp xenos::display::SetWindow).
void SetWindow(void* native_handle);

}  // namespace xenos::window_icon
