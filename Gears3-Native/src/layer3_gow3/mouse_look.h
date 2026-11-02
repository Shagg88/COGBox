// COGBox 0.2.1: native mouse look (mouse_look.cpp, ue3_mouse_look.h).
#pragma once

namespace rex::ui {
class Window;
}

// After setup: hooks the engine's input when the launcher asked for it (--ue3_mouse_look); logs why when not.
void CogMouseLookInstall(rex::ui::Window* window);

// GitHub #114: the projection set-up's field of view (degrees) for the wide-camera mouse compensation.
void CogMouseLookNoteFov(double fov);
