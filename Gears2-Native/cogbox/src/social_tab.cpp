// CogBox - the SOCIAL tab (social design step 3): the multiplayer server option, the profile's tag, adding friends,
// requests, invites and the friends list with Join / Invite / Remove - in the launcher's own look, like the
// ACHIEVEMENTS tab. The network runs on social_client.cpp's worker; this file only draws and queues actions.
#include "app.h"
#include "social_model.h"   // safe_icon_name
#include "theme.h"
#include "i18n.h"
#include "util.h"
#include "profiles.h"
#include "social_client.h"
#include "social_net.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

// page.cpp's building blocks, so this tab looks like the others
void ui_section(const char *title_en);
void ui_tip(const std::string &text_en);
void ui_note(const std::string &text_en, ImVec4 c = col::dim);
void ui_columns(const std::function<void(int)> &body);   // page.cpp (0.6.5)
float ui_row_x1();

SDL_Texture *player_icon_texture(App &a, const std::string &rel);   // player_icon.cpp

namespace {
std::string g_remove_uuid;   // the friend REMOVE asked about
bool g_deleting = false;     // DELETE ONLINE ACCOUNT is waiting for the server (social_delete_poll)
int g_delete_profile = 0;    // the profile whose account DELETE ONLINE ACCOUNT is deleting
char g_add[40] = {};
bool g_bad_tag = false;
bool g_invite_hint = false;
int g_right = -1;            // 0.6.5: the right half shows 0 invites, 1 open matches, 2 recent players (-1: not chosen yet)   // INVITE pressed with no game running

float text_w(ImFont *f, float size, const std::string &s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; }

bool row_button(const char *label_en, bool enabled, bool red = false)
{
    const char *label = tr(label_en);
    const ImVec2 sz(std::max(T(84), text_w(F_BODY, FS(F_BODY), label) + S(24)), T(26));
    return red ? th_button_red(label, sz, enabled, F_BODY, false) : th_button(label, sz, enabled, F_BODY, false);
}

float button_w(const char *label_en) { return std::max(T(84), text_w(F_BODY, FS(F_BODY), tr(label_en)) + S(24)); }

// a profile picture (the tag's first letter when this PC doesn't have it) in a crimson ring, the presence lamp on it
void avatar(App &a, ImDrawList *dl, ImVec2 c, float r, const std::string &icon, const std::string &tag, int lamp)
{
    SDL_Texture *pic = safe_icon_name(icon) ? player_icon_texture(a, icon) : nullptr;
    dl->AddCircleFilled(c, r, U(ImVec4(0.05f, 0.05f, 0.06f, 1)), 32);
    if (pic) {
        dl->AddImageRounded((ImTextureID)(intptr_t)pic, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, r);
    } else {
        const std::string letter = tag.empty() ? "?" : std::string(1, char(std::toupper(static_cast<unsigned char>(tag[0]))));
        const float fs = r * 1.15f;
        const ImVec2 ts = F_BOLD->CalcTextSizeA(fs, FLT_MAX, 0, letter.c_str());
        dl->AddText(F_BOLD, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), U(col::text), letter.c_str());
    }
    dl->AddCircle(c, r, U(col::red), 32, S(1.5f));
    th_lamp(dl, ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), S(3.5f), lamp);
}

// Layout A: one line per entry - picture, tag, its state, and the row's buttons at the right edge (x1). Returns the
// index of the button pressed, or -1.
struct LineButton {
    const char *label;
    bool enabled = true, red = false;
    const char *tip = nullptr;
};
int line_row(App &a, const std::string &tag, const std::string &state, ImVec4 state_col, int lamp, const std::string &icon, float x1,
             const std::vector<LineButton> &buttons, bool two_lines = false, bool picture = false, int map_lamp = -1,
             const char *map_tip = nullptr)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = two_lines ? T(44) : T(32), r = T(32) * 0.36f, bh = T(24);
    const ImVec2 q(x1, p.y + h);
    const bool hot = ImGui::IsMouseHoveringRect(p, q);
    dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, hot ? 0.07f : 0.035f)));
    const ImVec2 c(p.x + S(6) + r, p.y + h * 0.5f);
    float bw_all = 0;
    for (const LineButton &b : buttons) bw_all += std::max(T(54), text_w(F_SMALL, FS(F_SMALL), tr(b.label)) + S(18)) + S(6);
    if (two_lines) {
        // open matches: no picture (the text's colour says its state); the name on top, the details under it, each with
        // the row's whole width up to the buttons. 0.6.5: friends and recent players the same, with their picture.
        if (picture) avatar(a, dl, c, r, icon, tag, lamp);
        const float tx = picture ? c.x + r + S(10) : p.x + S(10), right = std::max(tx, x1 - S(6) - bw_all - S(6));
        const float gap = S(3), y1 = p.y + (h - FS(F_SMALL) * 2 - gap) * 0.5f - S(1);
        dl->PushClipRect(ImVec2(tx, p.y), ImVec2(right, q.y), true);
        dl->AddText(F_BOLD, FS(F_SMALL), ImVec2(tx, y1), U(col::text), tag.c_str());
        dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(tx, y1 + FS(F_SMALL) + gap), U(state_col), state.c_str());
        dl->PopClipRect();
        if (map_lamp >= 0) {   // 0.6.5 DLC light after the mode's name: 0 grey (no map yet), 1 green (have it), 2 red
            const float lr = S(4.5f), lx = std::min(right - lr, tx + text_w(F_BOLD, FS(F_SMALL), tag.c_str()) + S(8) + lr);
            const ImVec2 lc(lx, y1 + FS(F_SMALL) * 0.5f + S(1));
            dl->AddCircleFilled(lc, lr, U(map_lamp == 1 ? col::good : map_lamp == 2 ? col::bad : col::faint), 16);
            if (map_tip && ImGui::IsMouseHoveringRect(ImVec2(lc.x - lr * 2, lc.y - lr * 2), ImVec2(lc.x + lr * 2, lc.y + lr * 2)))
                ImGui::SetTooltip("%s", map_tip);
        }
    } else {
        avatar(a, dl, c, r, icon, tag, lamp);
        const float tx = c.x + r + S(10), name_w = std::min(S(170), (x1 - S(6) - bw_all - tx) * 0.45f);
        const float ty = p.y + (h - FS(F_SMALL)) * 0.5f - S(1);
        dl->PushClipRect(ImVec2(tx, p.y), ImVec2(tx + name_w, q.y), true);
        dl->AddText(F_BOLD, FS(F_SMALL), ImVec2(tx, ty), U(col::text), tag.c_str());
        dl->PopClipRect();
        const float sx = tx + name_w + S(10);
        dl->PushClipRect(ImVec2(sx, p.y), ImVec2(std::max(sx, x1 - S(6) - bw_all), q.y), true);
        dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(sx, ty), U(state_col), state.c_str());
        dl->PopClipRect();
    }
    int pressed = -1;
    float bx = x1 - S(6);
    for (int i = int(buttons.size()) - 1; i >= 0; --i) {
        const LineButton &b = buttons[size_t(i)];
        const float bw = std::max(T(54), text_w(F_SMALL, FS(F_SMALL), tr(b.label)) + S(18));
        bx -= bw;
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - bh) * 0.5f));
        ImGui::PushID(i);
        const bool hit = b.red ? th_button_red(tr(b.label), ImVec2(bw, bh), b.enabled, F_SMALL, false)
                               : th_button(tr(b.label), ImVec2(bw, bh), b.enabled, F_SMALL, false);
        ImGui::PopID();
        if (b.tip) ui_tip(b.tip);
        if (hit) pressed = i;
        bx -= S(6);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + S(3)));
    ImGui::Dummy(ImVec2(x1 - p.x, 0));
    return pressed;
}

// 0.6.5 DLC light: the lower-case names of the player's map packages (*.xxx) in the game's own folders and its installed
// DLC, read once per game and again when the DLC list changes (an add or remove).
bool has_map_package(App &a, int g, const std::string &low)
{
    static std::set<std::string> known[4];
    static std::string sig[4];
    if (g < 0 || g > 3) return false;
    std::string now = a.st[g].dataDir + "|" + a.st[g].updateDir;
    for (const DlcItem &d : dlc_list(g)) now += "|" + d.dir;
    if (now != sig[g]) {
        sig[g] = now;
        known[g].clear();
        std::vector<std::string> roots{ a.st[g].dataDir, a.st[g].updateDir };
        for (const DlcItem &d : dlc_list(g)) roots.push_back(d.dir);
        for (const std::string &root : roots) {
            if (root.empty()) continue;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(widen(root), std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string ext = narrow(it->path().extension().wstring()), stem = narrow(it->path().stem().wstring());
                for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".xxx") continue;
                for (char &c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
                known[g].insert(stem);
            }
        }
    }
    return known[g].count(low) != 0;
}

}  // namespace

bool social_delete_busy() { return g_deleting; }   // 0.6.0: deleting a profile waits for this

void social_restart(App &a)
{
    const bool on = a.ini.get("Online", "Enabled") == "1";
    const std::string server = on ? a.ini.get("Online", "Server", kCogboxServer) : std::string();
    std::string name = a.ini.get(profile_section(), "Gamertag");
    if (name.empty()) name = "Player";
    social_start(server, profile_dir(exe_dir(), std::to_string(current_profile())), name);
    social_send_icon(a);
}

void social_send_icon(App &a)
{
    const std::string icon = a.ini.get(profile_section(), "Icon");
    social_do(SocialAction::SetIcon, safe_icon_name(icon) ? icon : std::string());   // friends see the letter otherwise
}

void social_rename(App &a, const std::string &name)
{
    // the games rename at start anyway; this shows the new tag in the tab at once
    if (a.ini.get("Online", "Enabled") != "1" || name.empty()) return;
    social_restart(a);
    social_do(SocialAction::Rename, name);   // the worker signs in first when it has to
}

void social_delete_answer(App &a, int answer)
{
    if (answer != 1 || g_deleting) return;
    // sent at once, not queued (stopping the worker would drop a queued call), on its own thread so the launcher
    // keeps drawing; social_delete_poll picks up the answer
    g_deleting = true;
    g_delete_profile = current_profile();   // the account being deleted is this profile's, even if the player switches
    social_delete_account_start(20000);
    set_message(a, tr("Deleting your online account..."), false);
}

void social_delete_poll(App &a)
{
    if (!g_deleting) return;
    const std::optional<int> result = social_delete_account_result();
    if (!result) return;
    g_deleting = false;
    const int status = *result;
    if (status != 200 && status != 401) {   // 401: the server no longer knows this account
        social_restart(a);
        if (status == kSocialDeleteUnconfirmed)   // the server may still delete it; asking again tells (401 = gone)
            set_message(a, tr("The online server hasn't confirmed the delete yet. Press DELETE ONLINE ACCOUNT again in a minute to check."), true);
        else
            set_message(a, status == 0 ? tr("Can't reach the online server. Your online account was not deleted.")
                                       : tr("The online server refused. Your online account was not deleted."), true);
        return;
    }
    // the profile the delete was started for, not the current one: the player may have switched while it ran
    const std::string dir = profile_dir(exe_dir(), std::to_string(g_delete_profile));
    const std::wstring f = widen(join(dir, "identity.json"));
    MoveFileExW(f.c_str(), (f + L".deleted").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (current_profile() != g_delete_profile) {   // Online/Enabled is launcher-wide: leave it for the other profile
        set_message(a, tr("The online account of the profile you switched from was deleted."), false);
        return;
    }
    a.ini.seti("Online", "Enabled", 0);
    a.ini.save();
    social_restart(a);
    set_message(a, tr("Your online account was deleted. Online play is off for this profile."), false);
}

void social_remove_answer(int answer)
{
    if (answer == 1 && !g_remove_uuid.empty()) social_do(SocialAction::Remove, g_remove_uuid);
    g_remove_uuid.clear();
}

// 0.6.4 (Gears 3 Open matches): the game's own matchmaking finds the match (SDK patch 112), so the host's game picks
// the joiner's team - only with a gow3.exe that has it (its --net_prefer_lobby description); otherwise the invite join.
static bool joins_through_matchmaking(const App &a, int game)
{
    const gs::Environment &e = a.env[game];
    return game == G_GEARS3 && e.cap.exe_has && e.cap.exe_has("its own Social matchmaking finds only this match");
}

// "In Gears 3, choose Versus > Social > <mode> ..." for a matchmaking join.
static std::string matchmaking_note(const SocialJoin &j)
{
    SocialMatch m;
    m.game = j.game;
    m.playlist = j.playlist;
    const std::string mode = match_mode(m);
    return trf("Gears 3: go to Versus > Social > {1} now. Its matchmaking finds that match, and the host's game picks "
               "your team.", { j.playlist < 0 || mode == "Match" ? tr("the match's playlist") : tr(mode) });
}

void social_tab(App &a, int g, float labelW, float ctrlW, float rowX1)
{
    (void)labelW;
    (void)ctrlW;
    // a Join / invite Accept for a game that is not running here: start it, joining that lobby
    if (auto j = social_take_join()) {
        if (j->told) set_message(a, matchmaking_note(*j));   // the running game was told: the player picks the playlist
        else if (a.run.proc) set_message(a, tr("Close the running game first, then press JOIN again."), true);
        else if (a.st[j->game].ready != R_READY) set_message(a, tr("That game isn't set up in COGBox yet."), true);
        else {
            a.joinLobby[j->game] = (j->search ? std::string(kPreferLobbyPrefix) : std::string()) + j->room_code;
            play_start(a, j->game);
            if (j->search && a.run.proc) set_message(a, matchmaking_note(*j));
        }
    }

    ui_section("ONLINE PLAY");
    bool on = a.ini.get("Online", "Enabled") == "1";
    const SocialView v = social_view();
    // 0.6.5 (Shagg, the bigger window): the server switch and Region on the left, your tag and Add friend on the right
    ui_columns([&](int side) {
        if (side != 1) {
            if (ImGui::Checkbox(tr("Use the COGBox multiplayer server"), &on)) {
                a.ini.seti("Online", "Enabled", on ? 1 : 0);
                a.ini.save();
                social_restart(a);
            }
            ui_tip("Play online with friends: an account for this profile on the COGBox server, a friends list, Join and invites. "
                   "Off: the profile stays offline.");
            if (on) {
                const std::string cur = region_or_default(a.ini.get("Online", "Region"), local_utc_minutes());
                const std::string players = tr("players");
                std::string shown = cur;
                for (const SocialRegion &r : kSocialRegions) if (cur == r.id) shown = region_label({ r.id, tr(r.name) }, v.region_counts, players);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr("Region"));
                ui_tip("Quick Match only finds players in this region. Joining and inviting friends works in any region. "
                       "Takes effect the next time a game starts. Player counts are approximate.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(S(260));
                if (ImGui::BeginCombo("##region", shown.c_str())) {
                    for (const SocialRegion &r : kSocialRegions) {
                        const bool sel = cur == r.id;
                        if (ImGui::Selectable(region_label({ r.id, tr(r.name) }, v.region_counts, players).c_str(), sel)) {
                            a.ini.set("Online", "Region", r.id);
                            a.ini.save();
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (v.have_account) {
                    ImGui::SameLine(0, S(24));
                    if (row_button("DELETE ONLINE ACCOUNT", true))
                        ask(a, "socialdelete", g, tr("Delete your online account?"),
                            tr("Your tag, friends list and picture are removed from the COGBox server. Saves and achievements stay."),
                            { tr("No"), tr("Yes, delete") });
                    ui_tip("Removes this profile's account from the COGBox multiplayer server and turns online play off.");
                }
            }
        }
        if (side != 0 && on) {
            const float x1 = side < 0 ? rowX1 : ui_row_x1();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = T(40), w = x1 - p.x, r = h * 0.36f;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(ImVec4(1, 1, 1, 0.04f)));
            const ImVec2 c(p.x + S(8) + r, p.y + h * 0.5f);
            const std::string my_icon = a.ini.get(profile_section(), "Icon");
            const std::string tag = v.have_account ? v.my_tag : std::string(tr(v.busy ? "Signing in..." : "Can't reach the online server."));
            avatar(a, dl, c, r, my_icon, tag, v.reachable ? 1 : (v.busy ? 0 : 3));
            const float ty = p.y + (h - FS(F_BODY)) * 0.5f;
            dl->AddText(F_BOLD, FS(F_BODY), ImVec2(c.x + r + S(12), ty), U(col::text), tag.c_str());
            const float tag_w = F_BOLD->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, tag.c_str()).x;
            if (v.have_account)
                dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(c.x + r + S(24) + tag_w, ty + S(2)), U(v.reachable ? col::good : col::amber),
                            tr(v.reachable ? "Online" : "Can't reach the online server."));
            if (v.have_account) {
                const float add_w = button_w("ADD"), box_w = S(220), by = p.y + (h - T(26)) * 0.5f;
                float x = p.x + w - S(8) - add_w - S(8) - box_w;
                const char *lbl = tr("Add friend");
                const float lw = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, lbl).x;
                dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(x - lw - S(10), ty + S(2)), U(col::dim), lbl);
                ImGui::SetCursorScreenPos(ImVec2(x, by));
                ImGui::SetNextItemWidth(box_w);
                const bool enter = ImGui::InputTextWithHint("##addfriend", tr("Name#1234"), g_add, sizeof g_add, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SetCursorScreenPos(ImVec2(x + box_w + S(8), by));
                if (row_button("ADD", g_add[0] != 0, true) || (enter && g_add[0])) {
                    if (valid_tag(g_add)) {
                        social_do(SocialAction::Add, g_add);
                        g_add[0] = 0;
                        g_bad_tag = false;
                    } else {
                        g_bad_tag = true;
                    }
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(8)));
            ImGui::Dummy(ImVec2(w, 0));
        }
    });
    if (!on) {
        ui_note("Online play is off. This profile stays offline.");
        return;
    }
    const float x0 = ImGui::GetCursorScreenPos().x;
    if (g_bad_tag) ui_note("Use Name#1234 - the name, # and the four digits.", col::amber);
    if (!v.error.empty()) ui_note(v.error, col::amber);
    if (g_invite_hint) {
        ui_note("Launch the game and invite a friend from the F1 social menu.", col::amber);
        if (a.run.proc) g_invite_hint = false;
    }
    if (!v.have_account) return;

    // 0.6.5 (Shagg): two even halves - friends on the left, and on the right INVITES / MATCHES / RECENT (one at a time)
    const float total = rowX1 - x0, gap = S(18), left_w = std::floor((total - gap) * 0.5f);
    const float col_h = std::max(T(120), ImGui::GetContentRegionAvail().y - S(4));
    ImGui::BeginChild("##friends", ImVec2(left_w, col_h), ImGuiChildFlags_NavFlattened);
    {
        const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - S(4);
        std::string head = std::string(tr("FRIENDS")) + " (" + std::to_string(v.lists.friends.size()) + ")";
        if (!v.lists.incoming.empty())
            head += "  " + std::to_string(v.lists.incoming.size()) + " " + tr(v.lists.incoming.size() == 1 ? "request" : "requests");
        th_heading(head.c_str(), x1);
        for (const SocialRequest &r : v.lists.incoming) {
            ImGui::PushID(r.uuid.c_str());
            const int b = line_row(a, r.tag, tr("wants to be your friend"), col::amber, 2, r.icon, x1,
                                   { { "ACCEPT", true, true, nullptr }, { "DECLINE", true, false, nullptr },
                                     { "BLOCK", true, false, "Blocked players can't send you requests again." } }, true, true);
            if (b == 0) social_do(SocialAction::Accept, r.uuid);
            if (b == 1) social_do(SocialAction::Decline, r.uuid);
            if (b == 2) social_do(SocialAction::Block, r.uuid);
            ImGui::PopID();
        }
        for (const SocialRequest &r : v.lists.outgoing) {
            ImGui::PushID(r.uuid.c_str());
            if (line_row(a, r.tag, tr("Waiting for them to accept"), col::dim, 0, r.icon, x1,
                         { { "CANCEL", true, false, "Take back your friend request." } }, true, true) == 0)
                social_do(SocialAction::Remove, r.uuid);
            ImGui::PopID();
        }
        if (v.lists.friends.empty()) ui_note("No friends yet - add one by their tag.");
        std::vector<const SocialFriend *> order;
        for (const SocialFriend &f : v.lists.friends) if (f.state != "offline") order.push_back(&f);
        for (const SocialFriend &f : v.lists.friends) if (f.state == "offline") order.push_back(&f);
        for (const SocialFriend *fp : order) {
            const SocialFriend &f = *fp;
            ImGui::PushID(f.uuid.c_str());
            const bool joinable = f.state == "joinable", offline = f.state == "offline";
            std::string presence = tr(presence_text(f));   // whole lines ("Playing Gears of War 3") are lang keys
            const std::string game = game_name_for_title(f.title_id);
            if (!game.empty()) {   // the game's name in the launcher's language
                const size_t at = presence.find(game);
                if (at != std::string::npos) presence.replace(at, game.size(), tr(game.c_str()));
            }
            const int fg = game_for_title(f.title_id);
            const bool can_join = joinable && fg >= 0 && a.st[fg].ready == R_READY;
            std::vector<LineButton> bs;
            if (joinable) bs.push_back({ "JOIN", can_join, true, can_join ? "Join their game." : "Set up this game in COGBox to join." });
            if (!offline) bs.push_back({ "INVITE", true, false, "Invite them to the game you are hosting." });
            bs.push_back({ "REMOVE", true, false, "Remove this friend (asks first)." });
            const int b = line_row(a, f.tag, presence, offline ? col::dim : joinable ? col::amber : col::good, offline ? 0 : (joinable ? 2 : 1),
                                   f.icon, x1, bs, true, true);
            if (b >= 0) {
                const std::string what = bs[size_t(b)].label;
                if (what == "JOIN") social_do(SocialAction::Join, f.uuid);
                else if (what == "INVITE") {
                    if (a.run.proc) social_do(SocialAction::Invite, f.uuid);
                    else g_invite_hint = true;
                } else {
                    g_remove_uuid = f.uuid;
                    ask(a, "socialremove", g, tr("Remove this friend?"), f.tag, { tr("No"), tr("Yes, remove") });
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("##right", ImVec2(0, col_h), ImGuiChildFlags_NavFlattened);
    {
        const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - S(4);
        // the switcher: click, or LT / RT on a controller
        if (g_right < 0) g_right = v.invites.empty() ? 1 : 0;   // invites first when there are some
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) g_right = (g_right + 2) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) g_right = (g_right + 1) % 3;
        {
            const std::string labels[3] = {
                std::string(tr("INVITES")) + (v.invites.empty() ? std::string() : " (" + std::to_string(v.invites.size()) + ")"),
                std::string(tr("MATCHES")) + (v.matches.empty() ? std::string() : " (" + std::to_string(v.matches.size()) + ")"),
                std::string(tr("RECENT")) };
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float tw = (x1 - p.x) / 3, th = T(28);
            for (int i = 0; i < 3; i++) {
                ImGui::SetCursorScreenPos(ImVec2(p.x + i * tw, p.y));
                ImGui::PushID(i);
                if (th_tab(labels[i].c_str(), g_right == i, ImVec2(tw - S(3), th))) g_right = i;
                ImGui::PopID();
            }
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + th), ImVec2(x1, p.y + th), U(col::redHi, 0.7f), S(2));
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + S(8)));
            ImGui::Dummy(ImVec2(x1 - p.x, 0));
        }
        if (g_right == 0) {
            if (v.invites.empty()) ui_note("No invites right now.");
            for (const SocialInvite &i : v.invites) {
                ImGui::PushID(i.id.c_str());
                const int b = line_row(a, i.from_tag, tr(game_name_for_title(i.title_id)), col::amber, 1, i.from_icon, x1,
                                       { { "ACCEPT", true, true, nullptr }, { "DECLINE", true, false, nullptr } }, true, true);
                if (b == 0) social_do(SocialAction::InviteAccept, i.id);
                if (b == 1) social_do(SocialAction::InviteDecline, i.id);
                ImGui::PopID();
            }
            ui_note("You can invite friends while a game hosts a lobby or match.");
        } else if (g_right == 1) {
            if (!v.matches_known) ui_note("Looking for open matches...");
            else if (v.matches.empty()) ui_note("No open matches right now - host a public match and it shows up here.");
            for (const SocialMatch &m : v.matches) {
                ImGui::PushID(m.lobby_id.c_str());
                const std::string mode = match_mode(m), label = match_label(m);
                std::string details = label.size() > mode.size() + 4 ? label.substr(mode.size() + 4) : label;   // after "<mode> · "
                for (const SocialRegion &r : kSocialRegions) {   // the region, last in the label, in the launcher's language
                    const std::string tail = std::string(" \xC2\xB7 ") + r.name;
                    if (details.size() >= tail.size() && details.compare(details.size() - tail.size(), tail.size(), tail) == 0) {
                        details.replace(details.size() - tail.size() + 4, std::string::npos, tr(r.name));
                        break;
                    }
                }
                if (m.full() || m.in_progress) details += std::string(" \xC2\xB7 ") + tr(m.full() ? "Full" : "In progress");
                const bool ready = m.game >= 0 && a.st[m.game].ready == R_READY, can = ready && !m.full();
                const bool search = ready && !m.in_progress && joins_through_matchmaking(a, m.game);
                // 0.6.5 DLC light: does this player have the host's map (base game or their DLC)?
                const MapLight ml = ready ? match_map_light(m, [&](const std::string &p) { return has_map_package(a, m.game, p); })
                                          : MapLight::Unknown;
                const int lamp = !ready || ml == MapLight::Unknown ? -1 : ml == MapLight::Have ? 1 : ml == MapLight::Missing ? 2 : 0;
                const char *lamp_tip = ml == MapLight::Have ? tr("You have this match's map.")
                                     : ml == MapLight::Missing ? tr("You may not have this match's map (DLC). Add the map pack in COGBox to join.")
                                     : tr("The host hasn't chosen a map yet.");
                const int b = line_row(a, tr(mode.c_str()), details, m.full() ? col::dim : m.in_progress ? col::amber : col::good,
                                       m.full() ? 0 : (m.in_progress ? 2 : 1), "", x1,
                                       { { "JOIN", can, true, m.full() ? "This match is full." :
                                                              !ready ? "Set up this game in COGBox to join." :
                                                              search ? "Join through Gears 3's own matchmaking: the host's game picks your team." :
                                                              m.in_progress ? "Join the match in progress: you take a bot's place." :
                                                                              "Join this match." } }, true, false, lamp, lamp_tip);
                if (b == 0) social_do(search ? SocialAction::JoinMatchSearch : SocialAction::JoinMatch, m.lobby_id);
                ImGui::PopID();
            }
        } else {
            if (!v.recent_known) ui_note("Looking for the players you met...");
            else if (v.recent.empty()) ui_note("Players with a COGBox tag you play online with show up here, so you can add them as friends.");
            const long long now_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (const SocialRecent &r : v.recent) {
                ImGui::PushID(r.uuid.c_str());
                std::string where = tr(game_name_for_title(r.title_id));
                where += std::string(" \xC2\xB7 ") + tr(recent_when(r.seen_at, now_ms).c_str());
                std::vector<LineButton> bs;
                ImVec4 colr = col::dim;
                if (r.relation == "friend") { where += std::string(" \xC2\xB7 ") + tr("Friend"); colr = col::good; }
                else if (r.relation == "sent") { where += std::string(" \xC2\xB7 ") + tr("Request sent"); colr = col::amber; }
                else if (r.relation == "incoming") bs.push_back({ "ACCEPT", true, true, "They asked to be your friend." });
                else bs.push_back({ "ADD FRIEND", true, true, "Send them a friend request." });
                const int b = line_row(a, r.tag, where, colr, 0, r.icon, x1, bs, true, true);
                if (b == 0 && r.relation == "incoming") social_do(SocialAction::Accept, r.uuid);
                else if (b == 0) social_do(SocialAction::Add, r.tag);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}
