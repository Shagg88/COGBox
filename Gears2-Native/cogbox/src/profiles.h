// COGBox - a player profile's online account: identity.json in <exe>\profiles\<id>, shared by all four games.
// The profiles themselves, and their save folders, are the launcher's (games.h: current_profile, game_user_dir).
#pragma once
#include <string>

// <exe_dir>\profiles\<id>: the profile's identity folder.
std::string profile_dir(const std::string& exe_dir, const std::string& id);
