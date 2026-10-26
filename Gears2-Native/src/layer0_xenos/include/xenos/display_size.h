// Layer 0 (game-free): the size of the game window and of the monitor it is on, for "Match my monitor" (native
// resolution, Shagg 23 Oct 23:37-23:44). The app tells layer 0 about its window (any game: Gears 2, Gears 3); the
// renderer picks its render scale from the monitor and makes the presented image exactly the window's size.
// Our code (MIT).
#pragma once

#include <cstdint>

namespace xenos::display {

// From the app, when the window exists: its native handle (HWND on Windows) and client size in physical pixels.
void SetWindow(void* native_handle);
// From the app, on every physical size change of the window's client area.
void SetSurfaceSize(uint32_t width, uint32_t height);
// The window's client size in physical pixels; false until the app has told us.
bool SurfaceSize(uint32_t& width, uint32_t& height);
// The size of the monitor the window is on (the primary monitor before the window exists); false if unknown.
bool MonitorSize(uint32_t& width, uint32_t& height);

}  // namespace xenos::display
