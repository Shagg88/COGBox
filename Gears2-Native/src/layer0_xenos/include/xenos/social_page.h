// COGBox social step 5: the SOCIAL page of COGBox's in-game menu - the launcher's SOCIAL tab in the menu's look
// (own tag, add friend, invites, requests, friends with Join / Invite / Remove). Drawn by ingame_menu.cpp, UI thread.
#pragma once

namespace xenos::social {

// Draws the page in the menu window's remaining space. pad: a controller is in use (the on-screen keyboard for
// ADD FRIEND). Back: to the settings page. CloseMenu: an invite was accepted or a Join pressed, so the player sees the
// join happening in the game.
enum class PageResult { None, Back, CloseMenu };
PageResult DrawPage(float scale, bool pad, bool ps_names);
// B / Esc on the page: true when the page used it (closes the on-screen keyboard, or ends typing in the text box) -
// the menu then stays on the page. item_active: Dear ImGui has an active item (the text box).
bool ConsumeBack(bool item_active);
// The page is left: the on-screen keyboard closes.
void OnLeave();

}  // namespace xenos::social
