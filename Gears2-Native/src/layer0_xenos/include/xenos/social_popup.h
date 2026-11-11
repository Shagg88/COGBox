// COGBox social step 5: the in-game social pop-ups (see social_popup.cpp).
#pragma once

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

// Made once with COGBox's in-game menu (UI thread).
void XenosInstallSocialPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
void XenosShutdownSocialPopup();
// Any thread, called often (the menu's pad thread): at most once a second takes the new social events and queues
// their pop-ups. Nothing when the game is not online.
void XenosPumpSocialPopup();
