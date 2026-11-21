// Layer 0 (game-free): the in-match chat panel - COGBox's overlay on the left of the screen (Flippage's approved
// Option B: a see-through dark panel with a red left edge, at most 5 rows above a typing row, its bottom edge fixed so a
// full panel is centred on the screen's middle; it shrinks as lines fade). Y / U open the typing row
// (docs/superpowers/specs/2026-11-21-match-chat-design.md). The engine side is ue3/chat.h; layer 3 connects the two
// (XenosChatSetHooks, Ue3ChatSetSink -> XenosChatReceive). Our code (MIT).
#pragma once

#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, cogbox_match_chat);

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

// Made once with COGBox's in-game menu (UI thread).
void XenosInstallChatPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
void XenosShutdownChatPanel();
// Any thread: a received "Name#1234: text" line; `team` draws it as a team line.
void XenosChatReceive(const std::string& raw, bool team);
// Layer 3: whether a match is being played now (any thread), and how to send (false: nothing was sent).
void XenosChatSetHooks(bool (*match_running)(), bool (*send)(const std::string& text, bool team));
