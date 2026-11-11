// COGBox - a player profile's online account folder (see profiles.h)
#include "profiles.h"
#include "util.h"

std::string profile_dir(const std::string& exe_dir, const std::string& id) {
    return join(join(exe_dir, "profiles"), id);
}
