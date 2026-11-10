// COGBox social step 5: the games' side of the social features - the launcher's social worker
// (cogbox/src/social_client.*) on the SDK's signed-in MasterClient. Only when the game runs online
// (--net_online with a master server); otherwise nothing starts and the Social page / pop-ups stay hidden.
#pragma once

#include <vector>

#include "../../../../cogbox/src/social_client.h"

// The game runs online (the SDK's MasterClient has a server).
bool XenosSocialAvailable();
// Starts the worker once (idempotent); nothing when not available.
void XenosSocialStart();
// The worker's current state (the Social page draws it).
SocialView XenosSocialView();
// The pop-up events since the last call: the previous poll against the newest (social_events); none on the first.
std::vector<SocialEvent> XenosSocialTakeEvents();
