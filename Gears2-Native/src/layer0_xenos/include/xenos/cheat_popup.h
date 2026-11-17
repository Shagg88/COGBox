// Layer 0 (game-free): GitHub #359 - a short notice when a cheat key runs ("God mode [ON]", "Slow motion [0.5x]", "Cheats
// are off in Versus"). Its own design (Shagg's mock-up): a fully rounded near-black pill, a filled circle in the state's
// colour holding a white icon drawn from shapes (no art), the cheat's name and a small state chip. Not an achievement: no
// achievement wording, look or sound (it is silent). It shares only the achievement popups' UI fonts and position names.
// Shown for about 2 s, drawn with the game's frames; a new key press replaces the one showing.
// Only the cheat keys push it (ue3 console.cpp), never the launcher's always-on cheats or other console commands.
// Our code (MIT).
#pragma once

#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, cogbox_cheat_popups);
REXCVAR_DECLARE(std::string, cogbox_cheat_popup_position);

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

// On = green, Off = grey, Value = blue (a speed, "GIVEN"), Refused = orange (no chip: the label is the message,
// or "label: state" when a state is given).
enum class XenosCheatTone { On, Off, Value, Refused };
enum class XenosCheatIcon { Generic, Shield, Stopwatch, Weapon, Eye, Camera, Freeze, Fly, Refused };

// Made once with COGBox's in-game menu (UI thread).
void XenosInstallCheatPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
void XenosShutdownCheatPopup();
// Any thread (the console runs the keys on the game thread). `label` and `state` are English; they are translated here
// (--cogbox_ui_lang). Nothing when --cogbox_cheat_popups is off or the popup is not installed.
void XenosCheatPopupPush(const std::string& label, const std::string& state, XenosCheatTone tone, XenosCheatIcon icon);
