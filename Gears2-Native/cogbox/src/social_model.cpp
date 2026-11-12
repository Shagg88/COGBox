// CogBox - the SOCIAL tab's model (see social_model.h)
#include <algorithm>
#include "social_model.h"

#include "../third_party/nlohmann/json.hpp"

#include <cctype>

using json = nlohmann::json;

namespace {
std::string str(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}
std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
struct Title {
    const char* id;
    const char* name;
};
// In the launcher's GameId order (games.h: G_GEARS2, G_GEARS3, G_JUDGMENT, G_GEARS1): game_for_title's answer is
// what Join hands to play_start. Release order here once made a Gears 3 join start Judgment.
const Title kTitles[] = {
    { "4D53082D", "Gears of War 2" }, { "4D5308AB", "Gears of War 3" }, { "4D530A26", "Judgment" }, { "4D5307D5", "Gears of War" },
};
}  // namespace

std::optional<SocialLists> parse_friends(const std::string& body) {
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("friends") || !j["friends"].is_array()) return std::nullopt;
    SocialLists out;
    for (const json& f : j["friends"]) {
        SocialFriend x;
        x.uuid = str(f, "uuid");
        x.tag = str(f, "tag");
        x.xuid = str(f, "xuid");
        x.icon = str(f, "icon");
        if (f.contains("presence") && f["presence"].is_object()) {
            const json& p = f["presence"];
            if (!str(p, "state").empty()) x.state = str(p, "state");
            x.title_id = upper(str(p, "title_id"));
            x.lobby_id = str(p, "lobby_id");
        }
        if (!x.uuid.empty()) out.friends.push_back(x);
    }
    for (const char* key : { "incoming", "outgoing" }) {
        if (!j.contains(key) || !j[key].is_array()) continue;
        for (const json& r : j[key]) {
            SocialRequest x{ str(r, "uuid"), str(r, "tag"), str(r, "icon") };
            if (!x.uuid.empty()) (key[0] == 'i' ? out.incoming : out.outgoing).push_back(x);
        }
    }
    return out;
}

std::vector<SocialInvite> parse_invites(const std::string& body) {
    std::vector<SocialInvite> out;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("invites") || !j["invites"].is_array()) return out;
    for (const json& i : j["invites"]) {
        SocialInvite x;
        x.id = str(i, "id");
        if (i.contains("from")) {
            x.from_uuid = str(i["from"], "uuid");
            x.from_tag = str(i["from"], "tag");
            x.from_icon = str(i["from"], "icon");
        }
        x.title_id = upper(str(i, "title_id"));
        x.room_code = str(i, "room_code");
        if (!safe_room_code(x.room_code)) x.room_code.clear();
        if (!x.id.empty()) out.push_back(x);
    }
    return out;
}

int game_for_title(const std::string& title_id) {
    const std::string t = upper(title_id);
    for (int i = 0; i < 4; i++)
        if (t == kTitles[i].id) return i;
    return -1;
}

const char* game_name_for_title(const std::string& title_id) {
    const int g = game_for_title(title_id);
    return g < 0 ? "" : kTitles[g].name;
}

std::string presence_text(const SocialFriend& f) {
    const std::string game = game_name_for_title(f.title_id);
    if (f.state == "joinable") return (game.empty() ? std::string("In a game") : game) + " - joinable";
    if (f.state == "playing") return game.empty() ? std::string("Playing") : "Playing " + game;
    if (f.state == "online") return "Online - In Launcher";
    return "Offline";
}

bool valid_tag(const std::string& tag) {
    const size_t hash = tag.rfind('#');
    if (hash == std::string::npos || tag.size() - hash != 5) return false;
    for (size_t i = hash + 1; i < tag.size(); i++)
        if (!std::isdigit((unsigned char)tag[i])) return false;
    const std::string name = tag.substr(0, hash);
    if (name.size() < 3 || name.size() > 15 || name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name)
        if (!std::isalnum((unsigned char)c) && c != ' ') return false;
    return true;
}

SocialCall call_for(SocialAction action, const std::string& arg) {
    switch (action) {
    case SocialAction::Add: return { "POST", "/api/v1/friends/requests", json{ { "tag", arg } }.dump() };
    case SocialAction::Accept: return { "POST", "/api/v1/friends/" + arg + "/accept", "" };
    case SocialAction::Decline: return { "POST", "/api/v1/friends/" + arg + "/decline", "" };
    case SocialAction::Block: return { "POST", "/api/v1/friends/" + arg + "/block", "" };
    case SocialAction::Remove: return { "DELETE", "/api/v1/friends/" + arg, "" };
    case SocialAction::Join: return { "POST", "/api/v1/friends/" + arg + "/join", "" };
    case SocialAction::Invite: return { "POST", "/api/v1/invites", json{ { "to_uuid", arg } }.dump() };
    case SocialAction::InviteAccept: return { "POST", "/api/v1/invites/" + arg + "/accept", "" };
    case SocialAction::InviteDecline: return { "POST", "/api/v1/invites/" + arg + "/decline", "" };
    case SocialAction::Rename: return { "POST", "/api/v1/auth/rename", json{ { "display_name", arg } }.dump() };
    case SocialAction::SetIcon: return { "POST", "/api/v1/auth/icon", json{ { "icon", arg } }.dump() };
    case SocialAction::DeleteAccount: return { "DELETE", "/api/v1/auth/account", "" };
    }
    return {};
}

std::optional<SocialJoin> join_from_answer(int status, const std::string& body) {
    if (status != 409) return std::nullopt;
    const json j = json::parse(body, nullptr, false);
    SocialJoin out{ game_for_title(str(j, "title_id")), str(j, "room_code") };
    if (out.game < 0 || !safe_room_code(out.room_code)) return std::nullopt;
    return out;
}

std::string server_error(const std::string& body) {
    return str(json::parse(body, nullptr, false), "error");
}

std::vector<SocialEvent> social_events(const SocialLists& before, const std::vector<SocialInvite>& inv_before,
                                       const SocialLists& after, const std::vector<SocialInvite>& inv_after,
                                       int64_t now_ms, SocialEventState& st) {
    std::vector<SocialEvent> out;
    if (!st.primed) {
        st.primed = true;
        return out;
    }
    for (const SocialInvite& i : inv_after) {
        const bool had = std::any_of(inv_before.begin(), inv_before.end(), [&](const SocialInvite& o) { return o.id == i.id; });
        if (!had) out.push_back({ SocialEventKind::Invite, i.from_tag, i.title_id, i.id });
    }
    for (const SocialRequest& r : after.incoming) {
        const bool had = std::any_of(before.incoming.begin(), before.incoming.end(), [&](const SocialRequest& o) { return o.uuid == r.uuid; });
        if (!had) out.push_back({ SocialEventKind::Request, r.tag, "", "" });
    }
    const auto in_game = [](const std::string& s) { return s == "playing" || s == "joinable"; };
    for (const SocialFriend& f : after.friends) {
        const auto was = std::find_if(before.friends.begin(), before.friends.end(), [&](const SocialFriend& o) { return o.uuid == f.uuid; });
        if (was == before.friends.end()) {   // a new friend: a pop-up only when it answers this player's request
            const bool asked = std::any_of(before.outgoing.begin(), before.outgoing.end(), [&](const SocialRequest& o) { return o.uuid == f.uuid; });
            if (asked) out.push_back({ SocialEventKind::RequestAccepted, f.tag, "", "" });
            continue;
        }
        if (was->state == "offline" && f.state != "offline") {
            const auto last = st.last_online_notice.find(f.uuid);
            if (last != st.last_online_notice.end() && now_ms - last->second < 300000) continue;
            st.last_online_notice[f.uuid] = now_ms;
            out.push_back({ SocialEventKind::CameOnline, f.tag, "", "" });
        } else if (was->state == "online" && in_game(f.state)) {
            out.push_back({ SocialEventKind::StartedPlaying, f.tag, f.title_id, "" });
        }
    }
    return out;
}

bool friends_changed(const SocialLists& before, const SocialLists& after) {
    if (before.friends.size() != after.friends.size()) return true;
    for (size_t i = 0; i < after.friends.size(); ++i) {
        const SocialFriend& a = before.friends[i];
        const SocialFriend& b = after.friends[i];
        if (a.uuid != b.uuid || a.state != b.state || a.title_id != b.title_id || a.lobby_id != b.lobby_id) return true;
    }
    return false;
}

std::map<std::string, int> parse_region_counts(const std::string& body) {
    std::map<std::string, int> out;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return out;
    for (const SocialRegion& r : kSocialRegions) {
        if (j.contains(r.id) && j[r.id].is_number_integer()) out[r.id] = j[r.id].get<int>();
    }
    return out;
}

std::string default_region_for_utc(int minutes_east) {
    if (minutes_east < -2 * 60) return "us";
    if (minutes_east < 5 * 60) return "europe";
    return "oceania";
}

std::string region_or_default(const std::string& saved, int minutes_east) {
    if (saved == "us-w" || saved == "us-e") return "us";  // US-W and US-E were merged into US
    for (const SocialRegion& r : kSocialRegions) {
        if (saved == r.id) return saved;
    }
    return default_region_for_utc(minutes_east);
}

std::string region_label(const SocialRegion& r, const std::map<std::string, int>& counts, const std::string& players) {
    const auto it = counts.find(r.id);
    return it == counts.end() ? std::string(r.name) : std::string(r.name) + ": ~" + std::to_string(it->second) + " " + players;
}

bool safe_icon_name(const std::string& n) {
    const std::string pre = "icons/", suf = ".png";
    if (n.size() < pre.size() + 8 + 1 + 1 + suf.size() || n.compare(0, pre.size(), pre) != 0) return false;
    size_t i = pre.size();
    for (size_t k = 0; k < 8; ++k, ++i)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'A' && n[i] <= 'F'))) return false;
    if (n[i++] != '/') return false;
    const size_t digits_at = i;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i;
    const size_t digits = i - digits_at;
    return digits >= 1 && digits <= 6 && n.compare(i, std::string::npos, suf) == 0;
}

bool safe_room_code(const std::string& c) {
    if (c.size() != 8 || c.compare(0, 4, "COG-") != 0) return false;
    for (size_t i = 4; i < 8; ++i)
        if (!((c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= '0' && c[i] <= '9'))) return false;
    return true;
}
