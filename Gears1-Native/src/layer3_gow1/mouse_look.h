// COGBox 0.2.1: native mouse look (mouse_look.cpp, ue3_mouse_look.h).
#pragma once

namespace rex::ui {
class Window;
}

// After setup: hooks the engine's input when the launcher asked for it (--ue3_mouse_look); logs why when not.
void CogMouseLookInstall(rex::ui::Window* window);
void CogMouseLookNoteFov(double fov);   // 0.5.0: the view FOV (Gow1ViewFov) for the aim / zoom sensitivity
