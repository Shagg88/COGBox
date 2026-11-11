// CogBox - test of the social model (social step 3): server JSON, presence text, tags, server calls. Prints FAIL
// lines; exit code = failures.
//   clang++ -std=c++17 -Isrc -Ithird_party test/social_model_test.cpp src/social_model.cpp -o social_model_test.exe
#include "../src/social_model.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static bool same(const SocialCall& c, const char* method, const char* path, const char* body) {
    return c.method == method && c.path == path && c.body == body;
}

int main() {
    // GitLab review: a join code from the server goes on the game's command line - only COG-XXXX is taken
    {
        CHECK(join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-AB12"})").has_value());
        CHECK(!join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-AB12 --x=1"})").has_value());
        // valid JSON with a quote inside the code (an invalid body would fail for another reason)
        CHECK(join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-AB12"})")->game >= 0);
        CHECK(!join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-\"B12"})").has_value());
        CHECK(!join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-A\"12 x"})").has_value());
        CHECK(!join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"cog-ab12"})").has_value());
        CHECK(safe_room_code("COG-7Z9Q") && !safe_room_code("COG-7Z9") && !safe_room_code("XYZ-1234") && !safe_room_code(""));
        auto inv = parse_invites(R"({"invites":[{"id":"i1","from":{"uuid":"u","tag":"A#0001"},"title_id":"4D5308AB","room_code":"COG-12 34"}]})");
        CHECK(inv.size() == 1 && inv[0].room_code.empty());
    }
    // profile pictures: a friend's, a request's and an inviter's; only the games' own picture names are drawn
    {
        auto l = parse_friends(R"({"friends":[{"uuid":"u1","tag":"A#0001","icon":"icons/4D5308AB/8000.png","presence":{"state":"online"}}],)"
                               R"("incoming":[{"uuid":"u2","tag":"B#0002","icon":"icons/4D530A26/3.png"}],"outgoing":[]})");
        CHECK(l && l->friends[0].icon == "icons/4D5308AB/8000.png" && l->incoming[0].icon == "icons/4D530A26/3.png");
        auto inv = parse_invites(R"({"invites":[{"id":"i1","from":{"uuid":"u1","tag":"A#0001","icon":"icons/4D5307D5/12.png"},"title_id":"4d5308ab"}]})");
        CHECK(inv.size() == 1 && inv[0].from_icon == "icons/4D5307D5/12.png");
        CHECK(safe_icon_name("icons/4D5308AB/8000.png"));
        CHECK(!safe_icon_name("") && !safe_icon_name("icons/../x.png") && !safe_icon_name("icons/4D5308AB/8000.jpg") &&
              !safe_icon_name("C:/icons/4D5308AB/1.png") && !safe_icon_name("icons/4d5308ab/1.png") && !safe_icon_name("icons/4D5308AB/1234567.png"));
        CHECK(same(call_for(SocialAction::DeleteAccount, ""), "DELETE", "/api/v1/auth/account", ""));
        CHECK(same(call_for(SocialAction::SetIcon, "icons/4D5308AB/8000.png"), "POST", "/api/v1/auth/icon", R"({"icon":"icons/4D5308AB/8000.png"})"));
    }
    // the friends list
    {
        auto l = parse_friends(R"({"friends":[{"uuid":"u1","tag":"Chud Dumpty#7451","xuid":"5","presence":)"
                               R"({"state":"joinable","title_id":"4D530A26","lobby_id":"L"}},)"
                               R"({"uuid":"u3","tag":"Off Line#0001","xuid":"6","presence":{"state":"offline"}}],)"
                               R"("incoming":[{"uuid":"u2","tag":"A Friend#0001"}],"outgoing":[]})");
        CHECK(l.has_value());
        if (l) {
            CHECK(l->friends.size() == 2);
            CHECK(l->incoming.size() == 1);
            CHECK(l->outgoing.empty());
            CHECK(l->friends[0].state == "joinable");
            CHECK(l->friends[0].title_id == "4D530A26");
            CHECK(l->friends[0].tag == "Chud Dumpty#7451");
            CHECK(l->incoming[0].tag == "A Friend#0001");
            CHECK(presence_text(l->friends[0]) == "Judgment - joinable");
            CHECK(presence_text(l->friends[1]) == "Offline");
        }
        CHECK(!parse_friends("not json").has_value());
        SocialFriend playing;
        playing.state = "playing";
        playing.title_id = "4D5308AB";
        CHECK(presence_text(playing) == "Playing Gears of War 3");
        SocialFriend online;
        online.state = "online";
        CHECK(presence_text(online) == "Online - In Launcher");
    }
    // games: the launcher's own numbering (games.h GameId: Gears 2, Gears 3, Judgment, Gears 1)
    CHECK(game_for_title("4D53082D") == 0);   // G_GEARS2
    CHECK(game_for_title("4d5308ab") == 1);   // G_GEARS3
    CHECK(game_for_title("4D530A26") == 2);   // G_JUDGMENT
    CHECK(game_for_title("4D5307D5") == 3);   // G_GEARS1
    CHECK(join_from_answer(409, R"({"title_id":"4D5308AB","room_code":"COG-AB12"})")->game == 1);   // Gears 3
    CHECK(game_for_title("FFFFFFFF") == -1);
    // tags
    CHECK(valid_tag("Flapper#4293"));
    CHECK(valid_tag("Chud Dumpty#7451"));
    CHECK(!valid_tag("Flapper"));
    CHECK(!valid_tag("Flapper#42a3"));
    CHECK(!valid_tag("Flapper#429"));
    CHECK(!valid_tag("Fl#4293"));
    CHECK(!valid_tag("#4293"));
    CHECK(!valid_tag("Fl@pper#4293"));
    // server calls
    CHECK(same(call_for(SocialAction::Add, "Flapper#4293"), "POST", "/api/v1/friends/requests", R"({"tag":"Flapper#4293"})"));
    CHECK(same(call_for(SocialAction::Accept, "u2"), "POST", "/api/v1/friends/u2/accept", ""));
    CHECK(same(call_for(SocialAction::Decline, "u2"), "POST", "/api/v1/friends/u2/decline", ""));
    CHECK(same(call_for(SocialAction::Block, "u2"), "POST", "/api/v1/friends/u2/block", ""));
    CHECK(same(call_for(SocialAction::Remove, "u2"), "DELETE", "/api/v1/friends/u2", ""));
    // review: a claim is retried only when the server did not answer (no account was made)
    CHECK(claim_retry(0));
    CHECK(!claim_retry(200));
    CHECK(!claim_retry(400));
    CHECK(!claim_retry(429));
    CHECK(!claim_retry(500));
    CHECK(same(call_for(SocialAction::Rename, "Flapper"), "POST", "/api/v1/auth/rename", R"({"display_name":"Flapper"})"));
    CHECK(same(call_for(SocialAction::Join, "u2"), "POST", "/api/v1/friends/u2/join", ""));
    CHECK(same(call_for(SocialAction::Invite, "u2"), "POST", "/api/v1/invites", R"({"to_uuid":"u2"})"));
    CHECK(same(call_for(SocialAction::InviteAccept, "i9"), "POST", "/api/v1/invites/i9/accept", ""));
    CHECK(same(call_for(SocialAction::InviteDecline, "i9"), "POST", "/api/v1/invites/i9/decline", ""));
    // invites
    {
        auto inv = parse_invites(R"({"invites":[{"id":"i9","from":{"uuid":"u1","tag":"Chud Dumpty#7451"},)"
                                 R"("title_id":"4D530A26","room_code":"COG-ABCD"}]})");
        CHECK(inv.size() == 1);
        if (!inv.empty()) {
            CHECK(inv[0].id == "i9");
            CHECK(inv[0].from_uuid == "u1");
            CHECK(inv[0].from_tag == "Chud Dumpty#7451");
            CHECK(inv[0].room_code == "COG-ABCD");
        }
        CHECK(parse_invites("{}").empty());
    }
    // Join / invite accept answers: 409 = that game is not running here, start it with the room code; 200 = told
    {
        auto j = join_from_answer(409, R"({"error":"start the game first","title_id":"4D5308AB","room_code":"COG-ABCD"})");
        CHECK(j.has_value() && j->game == 1 && j->room_code == "COG-ABCD");
        CHECK(!join_from_answer(200, R"({"status":"joining","title_id":"4D5308AB","room_code":"COG-ABCD"})").has_value());
        CHECK(!join_from_answer(404, R"({"error":"no lobby"})").has_value());
        CHECK(!join_from_answer(409, R"({"error":"start the game first","title_id":"FFFFFFFF","room_code":"COG-ABCD"})").has_value());
        CHECK(server_error(R"({"error":"no player with that tag"})") == "no player with that tag");
        CHECK(server_error("x") == "");
    }
    // step 5: pop-up events from two polls
    {
        auto fr = [](const char* uuid, const char* tag, const char* state, const char* title = "") {
            SocialFriend f;
            f.uuid = uuid; f.tag = tag; f.state = state; f.title_id = title;
            return f;
        };
        auto inv = [](const char* id, const char* tag, const char* title) {
            SocialInvite i;
            i.id = id; i.from_tag = tag; i.title_id = title;
            return i;
        };
        SocialEventState st;
        SocialLists a, b;
        a.friends = { fr("f1", "Dom#0002", "offline") };
        a.incoming = { { "r1", "Old#0003" } };
        std::vector<SocialInvite> ia = { inv("i0", "Old#0003", "4D53082D") };
        // first poll: what was already there gives nothing
        CHECK(social_events(a, ia, a, ia, 0, st).empty());
        CHECK(st.primed);
        // a new invite, a new request, a friend coming online in one poll: invite first
        b = a;
        b.friends = { fr("f1", "Dom#0002", "online") };
        b.incoming.push_back({ "r2", "Zo\xC3\xAB#0001" });
        auto ib = ia;
        ib.push_back(inv("i1", "Dom#0002", "4D53082D"));
        auto ev = social_events(a, ia, b, ib, 1000, st);
        CHECK(ev.size() == 3);
        if (ev.size() == 3) {
            CHECK(ev[0].kind == SocialEventKind::Invite && ev[0].tag == "Dom#0002" && ev[0].invite_id == "i1" && ev[0].title_id == "4D53082D");
            CHECK(ev[1].kind == SocialEventKind::Request && ev[1].tag == "Zo\xC3\xAB#0001");   // UTF-8 unchanged
            CHECK(ev[2].kind == SocialEventKind::CameOnline && ev[2].tag == "Dom#0002");
        }
        // online -> playing: started playing, with the game
        SocialLists c = b;
        c.friends = { fr("f1", "Dom#0002", "joinable", "4D53082D") };
        ev = social_events(b, ib, c, ib, 2000, st);
        CHECK(ev.size() == 1 && ev[0].kind == SocialEventKind::StartedPlaying && ev[0].title_id == "4D53082D");
        // offline, then online again within 5 minutes of the last notice: nothing; after: came online
        SocialLists off = c;
        off.friends = { fr("f1", "Dom#0002", "offline") };
        CHECK(social_events(c, ib, off, ib, 3000, st).empty());
        CHECK(social_events(off, ib, b, ib, 1000 + 299999, st).empty());
        CHECK(social_events(b, ib, off, ib, 400000, st).empty());
        ev = social_events(off, ib, b, ib, 1000 + 300000 + 100000, st);
        CHECK(ev.size() == 1 && ev[0].kind == SocialEventKind::CameOnline);
    }
    // Flippage 8 Nov: the friend you asked accepted - a pop-up; any other new friend stays quiet
    {
        SocialEventState st;
        SocialLists before, after;
        before.outgoing = { { "t1", "TesterB#4271" } };
        SocialFriend t; t.uuid = "t1"; t.tag = "TesterB#4271"; t.state = "online";
        social_events(before, {}, before, {}, 0, st);   // prime
        after.friends = { t };
        auto ev = social_events(before, {}, after, {}, 1000, st);
        CHECK(ev.size() == 1 && ev[0].kind == SocialEventKind::RequestAccepted && ev[0].tag == "TesterB#4271");
        SocialFriend o2 = t; o2.uuid = "o2";
        SocialLists after2 = after; after2.friends.push_back(o2);
        CHECK(social_events(after, {}, after2, {}, 2000, st).empty());
    }
    // the game's own friends list is told only when the list or someone's presence changed
    {
        SocialFriend a; a.uuid = "a"; a.state = "offline";
        SocialLists l1, l2; l1.friends = { a }; l2 = l1;
        CHECK(!friends_changed(l1, l2));
        l2.friends[0].state = "online";
        CHECK(friends_changed(l1, l2));
        l2 = l1; l2.friends[0].title_id = "4D5308AB";
        CHECK(friends_changed(l1, l2));
        l2 = l1; l2.friends.clear();
        CHECK(friends_changed(l1, l2));
    }
    {
        const auto c = parse_region_counts(R"({"us-e":500,"us-w":100,"oceania":50,"europe":1000})");
        CHECK(c.size() == 4 && c.at("europe") == 1000);
        CHECK(parse_region_counts("not json").empty());
        CHECK(region_label(kSocialRegions[0], c, "players") == "US-E: ~500 players");
        CHECK(region_label(kSocialRegions[2], c, "players") == "OCE: ~50 players");
        CHECK(region_label(kSocialRegions[0], {}, "players") == "US-E");
        CHECK(default_region_for_utc(-8 * 60) == "us-w");
        CHECK(default_region_for_utc(-5 * 60) == "us-e");
        CHECK(default_region_for_utc(1 * 60) == "europe");
        CHECK(default_region_for_utc(10 * 60) == "oceania");
        CHECK(region_or_default("europe", 10 * 60) == "europe");
        CHECK(region_or_default("", 10 * 60) == "oceania");
        CHECK(region_or_default("mars", -5 * 60) == "us-e");
    }
    if (g_failures == 0) std::printf("social_model_test: all passed\n");
    return g_failures;
}
