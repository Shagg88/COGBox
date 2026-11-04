// Layer 0 (game-free): the running game writes its achievement list and icons to COGBox's shared cache
// (<--cogbox_dir>\achievements\<TitleID>\, see xenos/achievements_data.h), and passes its unlocks to the rarity
// provider. Call from the app's OnPreLaunchModule (the SDK has loaded the title's achievements by then). Without
// --cogbox_dir (a game started by hand) it does nothing. Our code (MIT).
#pragma once

namespace rex {
class Runtime;
}

void XenosExportAchievements(rex::Runtime* runtime);
