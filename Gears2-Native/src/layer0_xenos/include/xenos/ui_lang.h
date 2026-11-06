// Layer 0 (game-free): COGBox's own in-game text in the launcher's language (--cogbox_ui_lang). The launcher passes the
// language it shows (cogbox/src/play.cpp) and ships its translations as <COGBox>\lang\<code>.txt ("English = text"
// lines, the launcher's own format, cogbox/src/i18n.cpp); every string falls back to English when it has no line.
// Used by the in-game menu, the Achievements page and the achievement popups of all four games. Our code (MIT).
#pragma once

#include <initializer_list>
#include <string>

// Loads the file once (the first call of any of these does it). Safe from any thread after that.
void XenosUiLangInit();
bool XenosUiLangActive();  // a language other than English, with its file found
std::string XenosUiLangCode();  // "en" when English
// The translation of a whole English string, or the string itself. The pointer stays valid for the whole run.
const char* XenosTr(const char* english);
std::string XenosTr(const std::string& english);
// Text built at run time ("Unlocked 4 Nov 2026"): each of `words` found at a word edge is replaced by its translation.
std::string XenosTrWords(const std::string& text, std::initializer_list<const char*> words);
// Every translated text, for the font atlas to take its glyphs from (achievement_popup_assets.cpp).
std::string XenosUiLangAllText();
