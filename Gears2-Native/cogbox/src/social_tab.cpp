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
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// page.cpp's building blocks, so this tab looks like the others
void ui_section(const char *title_en);
void ui_tip(const std::string &text_en);
void ui_note(const std::string &text_en, ImVec4 c = col::dim);

SDL_Texture *player_icon_texture(App &a, const std::string &rel);   // player_icon.cpp

namespace {
std::string g_remove_uuid;   // the friend REMOVE asked about
bool g_deleting = false;     // DELETE ONLINE ACCOUNT is waiting for the server (social_delete_poll)
char g_add[40] = {};
bool g_bad_tag = false;
bool g_invite_hint = false;   // INVITE pressed with no game running

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
             const std::vector<LineButton> &buttons)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = T(32), r = h * 0.36f, bh = T(24);
    const ImVec2 q(x1, p.y + h);
    const bool hot = ImGui::IsMouseHoveringRect(p, q);
    dl->AddRectFilled(p, q, U(ImVec4(1, 1, 1, hot ? 0.07f : 0.035f)));
    const ImVec2 c(p.x + S(6) + r, p.y + h * 0.5f);
    avatar(a, dl, c, r, icon, tag, lamp);
    float bw_all = 0;
    for (const LineButton &b : buttons) bw_all += std::max(T(54), text_w(F_SMALL, FS(F_SMALL), tr(b.label)) + S(18)) + S(6);
    const float tx = c.x + r + S(10), name_w = std::min(S(170), (x1 - S(6) - bw_all - tx) * 0.45f);
    const float ty = p.y + (h - FS(F_SMALL)) * 0.5f - S(1);
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(tx + name_w, q.y), true);
    dl->AddText(F_BOLD, FS(F_SMALL), ImVec2(tx, ty), U(col::text), tag.c_str());
    dl->PopClipRect();
    const float sx = tx + name_w + S(10);
    dl->PushClipRect(ImVec2(sx, p.y), ImVec2(std::max(sx, x1 - S(6) - bw_all), q.y), true);
    dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(sx, ty), U(state_col), state.c_str());
    dl->PopClipRect();
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

}  // namespace

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
    const std::string dir = profile_dir(exe_dir(), std::to_string(current_profile()));
    const std::wstring f = widen(join(dir, "identity.json"));
    MoveFileExW(f.c_str(), (f + L".deleted").c_str(), MOVEFILE_REPLACE_EXISTING);
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

void social_tab(App &a, int g, float labelW, float ctrlW, float rowX1)
{
    (void)labelW;
    (void)ctrlW;
    // a Join / invite Accept for a game that is not running here: start it, joining that lobby
    if (auto j = social_take_join()) {
        if (a.run.proc) set_message(a, tr("Close the running game first, then press JOIN again."), true);
        else if (a.st[j->game].ready != R_READY) set_message(a, tr("That game isn't set up in COGBox yet."), true);
        else {
            a.joinLobby[j->game] = j->room_code;
            play_start(a, j->game);
        }
    }

    ui_section("ONLINE PLAY");
    bool on = a.ini.get("Online", "Enabled") == "1";
    if (ImGui::Checkbox(tr("Use the COGBox multiplayer server"), &on)) {
        a.ini.seti("Online", "Enabled", on ? 1 : 0);
        a.ini.save();
        social_restart(a);
    }
    ui_tip("Play online with friends: an account for this profile on the COGBox server, a friends list, Join and invites. "
           "Off: the profile stays offline.");
    if (!on) {
        ui_note("Online play is off. This profile stays offline.");
        return;
    }
    {
        // the matchmaking region: Quick Match finds this region's games. Friends can join and invite across regions.
        const SocialView regionView = social_view();
        const std::string cur = region_or_default(a.ini.get("Online", "Region"), local_utc_minutes());
        const std::string players = tr("players");
        std::string shown = cur;
        for (const SocialRegion &r : kSocialRegions) if (cur == r.id) shown = region_label(r, regionView.region_counts, players);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Region"));
        ui_tip("Quick Match only finds players in this region. Joining and inviting friends works in any region. "
               "Takes effect the next time a game starts. Player counts are approximate.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::BeginCombo("##region", shown.c_str())) {
            for (const SocialRegion &r : kSocialRegions) {
                const bool sel = cur == r.id;
                if (ImGui::Selectable(region_label(r, regionView.region_counts, players).c_str(), sel)) {
                    a.ini.set("Online", "Region", r.id);
                    a.ini.save();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    const SocialView v = social_view();
    if (v.have_account) {
        ImGui::SameLine(0, S(24));
        if (row_button("DELETE ONLINE ACCOUNT", true))
            ask(a, "socialdelete", g, tr("Delete your online account?"),
                tr("Your tag, friends list and picture are removed from the COGBox server. Saves and achievements stay."),
                { tr("No"), tr("Yes, delete") });
        ui_tip("Removes this profile's account from the COGBox multiplayer server and turns online play off.");
    }

    // the top strip: your picture and tag, then Add friend on the same line (layout A, as the in-game page)
    const float x0 = ImGui::GetCursorScreenPos().x;
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = T(40), w = rowX1 - p.x, r = h * 0.36f;
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
    if (g_bad_tag) ui_note("Use Name#1234 - the name, # and the four digits.", col::amber);
    if (!v.error.empty()) ui_note(v.error, col::amber);
    if (g_invite_hint) {
        ui_note("Launch the game and invite a friend from the F1 social menu.", col::amber);
        if (a.run.proc) g_invite_hint = false;
    }
    if (!v.have_account) return;

    // two columns: friends (requests first) on the left, party and match invites on the right
    const float total = rowX1 - x0, gap = S(18), left_w = std::floor((total - gap) * 0.62f);
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
                                     { "BLOCK", true, false, "Blocked players can't send you requests again." } });
            if (b == 0) social_do(SocialAction::Accept, r.uuid);
            if (b == 1) social_do(SocialAction::Decline, r.uuid);
            if (b == 2) social_do(SocialAction::Block, r.uuid);
            ImGui::PopID();
        }
        for (const SocialRequest &r : v.lists.outgoing) {
            ImGui::PushID(r.uuid.c_str());
            line_row(a, r.tag, tr("Waiting for them to accept"), col::dim, 0, r.icon, x1, {});
            ImGui::PopID();
        }
        if (v.lists.friends.empty()) ui_note("No friends yet - add one by their tag.");
        // online friends first, then offline
        std::vector<const SocialFriend *> order;
        for (const SocialFriend &f : v.lists.friends) if (f.state != "offline") order.push_back(&f);
        for (const SocialFriend &f : v.lists.friends) if (f.state == "offline") order.push_back(&f);
        for (const SocialFriend *fp : order) {
            const SocialFriend &f = *fp;
            ImGui::PushID(f.uuid.c_str());
            const bool joinable = f.state == "joinable", offline = f.state == "offline";
            std::string presence = presence_text(f);
            const std::string game = game_name_for_title(f.title_id);
            if (!game.empty()) {   // the game's name in the launcher's language
                const size_t at = presence.find(game);
                if (at != std::string::npos) presence.replace(at, game.size(), tr(game.c_str()));
            }
            const int fg = game_for_title(f.title_id);
            const bool can_join = joinable && fg >= 0 && a.st[fg].ready == R_READY;
            // only the buttons that do something here: Join when joinable, Invite when online, Remove (asks first)
            std::vector<LineButton> bs;
            if (joinable) bs.push_back({ "JOIN", can_join, true, can_join ? "Join their game." : "Set up this game in COGBox to join." });
            if (!offline) bs.push_back({ "INVITE", true, false, "Invite them to the game you are hosting." });
            bs.push_back({ "REMOVE", true, false, "Remove this friend (asks first)." });
            const int b = line_row(a, f.tag, presence, offline ? col::dim : joinable ? col::amber : col::good, offline ? 0 : (joinable ? 2 : 1),
                                   f.icon, x1, bs);
            if (b >= 0) {
                const std::string what = bs[size_t(b)].label;
                if (what == "JOIN") social_do(SocialAction::Join, f.uuid);
                else if (what == "INVITE") {
                    // an invite needs a game you are hosting: from the launcher alone there is nothing to invite to
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
    ImGui::BeginChild("##invites", ImVec2(0, col_h), ImGuiChildFlags_NavFlattened);
    {
        const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - S(4);
        th_heading(tr("PARTY AND MATCH INVITES"), x1);
        if (v.invites.empty()) ui_note("No invites right now.");
        for (const SocialInvite &i : v.invites) {
            ImGui::PushID(i.id.c_str());
            const int b = line_row(a, i.from_tag, tr(game_name_for_title(i.title_id)), col::amber, 1, i.from_icon, x1,
                                   { { "ACCEPT", true, true, nullptr }, { "DECLINE", true, false, nullptr } });
            if (b == 0) social_do(SocialAction::InviteAccept, i.id);
            if (b == 1) social_do(SocialAction::InviteDecline, i.id);
            ImGui::PopID();
        }
        ui_note("You can invite friends while a game hosts a lobby or match.");
    }
    ImGui::EndChild();
}
