#pragma once

#include "ue3/config_bundle.h"

namespace ue3 {

// Gears 3 title update 6 adds the Forces of Nature Guardian playlists (TU__gearplaylist.ini, ids 70 / 80:
// [DefaultDLC4Guardian GearOnlinePlaylistProvider] and [DefaultDLC4GuardianRanked ...]), but no .int on the disc,
// in the update or in the DLC names them - on the 360 that text came from Xbox LIVE title storage. Without it the
// Versus > Forces of Nature menu shows a blank, selected row above Quick Match / Ranked Match.
// Adds DisplayName (and the quick-match PlaylistDescription) to the bundle's GearGame text file (GearGame.int,
// geargame.fra, ...) where they are missing; a value already there (a mod's) is kept. Returns the keys added.
int AddMissingPlaylistNames(ConfigBundle& bundle);

}  // namespace ue3
