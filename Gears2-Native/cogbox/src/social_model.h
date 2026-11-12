// CogBox - the SOCIAL tab's model (social design step 3): the master server's friends and invites, the text a
// friend's row shows, the tag check and the request each action sends. No SDL, no Dear ImGui, no network:
// test/social_model_test.cpp checks it. The worker is social_client.cpp, the tab social_tab.cpp.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct SocialFriend {
    std::string uuid, tag, xuid;
    std::string icon;   // their profile picture: icons/<TitleID>/<n>.png, or "" for the letter
    std::string state = "offline";   // offline | online | playing | joinable
    std::string title_id, lobby_id;
};
struct SocialRequest {
    std::string uuid, tag, icon;
};
struct SocialInvite {
    std::string id, from_uuid, from_tag, title_id, room_code, from_icon;
};
struct SocialLists {
    std::vector<SocialFriend> friends;
    std::vector<SocialRequest> incoming, outgoing;
};

// The signed-in account (identity.json) and one answer from the server.
struct SocialAccount {
    std::string uuid, token, name, discriminator, xuid;
    std::string tag() const { return name + "#" + discriminator; }
};
struct HttpResult {
    int status = 0;   // 0: no answer (unreachable, timed out)
    std::string body;
};

// GET /api/v1/friends; nullopt when the body is not that answer.
std::optional<SocialLists> parse_friends(const std::string& body);
// GET /api/v1/invites (the ones sent to this player); empty when none or not that answer.
std::vector<SocialInvite> parse_invites(const std::string& body);

// The launcher's game index for a title id (GameId order from games.h: 0 Gears 2, 1 Gears 3, 2 Judgment, 3 Gears of War), -1 for another title.
int game_for_title(const std::string& title_id);
// The game's name for a title id ("Gears of War 3"), "" for another title. English: the tab translates it.
const char* game_name_for_title(const std::string& title_id);
// "Offline", "Online - In Launcher", "Playing <game>", "<game> - joinable" (Judgment is "Judgment").
std::string presence_text(const SocialFriend& f);

// Name#1234: a name of 3-15 letters, digits and spaces, '#', four digits.
bool valid_tag(const std::string& tag);

enum class SocialAction { Add, Accept, Decline, Block, Remove, Join, Invite, InviteAccept, InviteDecline, Rename, SetIcon, DeleteAccount };
// A profile picture name from the server that is safe to look up under <COGBox>: icons/<8 hex digits>/<1-6 digits>.png
// (anything else - a path, "..", another file type - is drawn as the letter).
bool safe_icon_name(const std::string& name);
// A lobby join code from the server that is safe to put on a game's command line: COG- and four of A-Z / 0-9.
bool safe_room_code(const std::string& code);
struct SocialCall {
    std::string method, path, body;
};
// The request an action sends; arg is the tag (Add), a friend's uuid, an invite id, or the new name (Rename).
SocialCall call_for(SocialAction action, const std::string& arg);

// After Join or an invite's Accept: the server answers 409 with the lobby when that game is not running here (the
// launcher starts it with --net_join_lobby=<room code>); 200 means the running game was told. Game index as above.
struct SocialJoin {
    int game = -1;
    std::string room_code;
};
std::optional<SocialJoin> join_from_answer(int status, const std::string& body);
// The server's {"error": "..."} text, "" when there is none.
std::string server_error(const std::string& body);

// The worker claims an account once; it tries again only when the server did not answer (status 0: no account made).
inline bool claim_retry(int status) { return status == 0; }

// The three matchmaking regions, in the dropdown's order (US-W and US-E were merged into US).
struct SocialRegion { const char* id; const char* name; };
inline constexpr SocialRegion kSocialRegions[] = { { "us", "US" }, { "oceania", "OCE" }, { "europe", "Europe" } };
// GET /api/v1/population/regions: region id -> players; empty when the answer is not that.
std::map<std::string, int> parse_region_counts(const std::string& body);
// A first guess from the PC's time zone (minutes east of UTC).
std::string default_region_for_utc(int minutes_east);
// The saved region, or that guess when none (or an unknown one) is saved.
std::string region_or_default(const std::string& saved, int minutes_east);
// "US: ~500 <players>"; just the name while the counts are unknown.
std::string region_label(const SocialRegion& r, const std::map<std::string, int>& counts, const std::string& players);

// Social step 5: the in-game pop-ups, from two polls in a row. Nothing for what the first poll already had; a new
// invite, a new incoming request, a friend going from offline to online (at most once per friend per 5 minutes) or
// from online to playing. Order: invites, requests, friends.
// True when the friends list or anyone's presence differs: the game's own friends screens must read it again.
bool friends_changed(const SocialLists& before, const SocialLists& after);

enum class SocialEventKind { Invite, Request, CameOnline, StartedPlaying, RequestAccepted };
struct SocialEvent {
    SocialEventKind kind;
    std::string tag;
    std::string title_id;    // the game (Invite, StartedPlaying)
    std::string invite_id;   // Invite
};
struct SocialEventState {
    bool primed = false;
    std::map<std::string, int64_t> last_online_notice;   // friend uuid -> ms of their last "came online"
};
std::vector<SocialEvent> social_events(const SocialLists& before, const std::vector<SocialInvite>& inv_before,
                                       const SocialLists& after, const std::vector<SocialInvite>& inv_after,
                                       int64_t now_ms, SocialEventState& st);
