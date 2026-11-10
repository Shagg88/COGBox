// CogBox - the SOCIAL tab (social design step 3): the multiplayer server option, the profile's tag, adding friends,
// requests, invites and the friends list with Join / Invite / Remove - in the launcher's own look, like the
// ACHIEVEMENTS tab. The network runs on social_client.cpp's worker; this file only draws and queues actions.
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "util.h"
#include "profiles.h"
#include "social_client.h"
#include "social_net.h"

#include <cstring>
#include <string>

// page.cpp's building blocks, so this tab looks like the others
void ui_section(const char *title_en);
void ui_tip(const std::string &text_en);
void ui_note(const std::string &text_en, ImVec4 c = col::dim);

namespace {
std::string g_remove_uuid;   // the friend REMOVE asked about
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

// a row: lamp, a main line, a dim second line; the buttons go after it on the same line (right side)
void row_text(ImU32 main, const std::string &line1, const std::string &line2, int lamp, float width)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rh = T(46), fb = FS(F_BODY), fsm = FS(F_SMALL);
    th_lamp(dl, ImVec2(p.x + S(8), p.y + rh * 0.5f), S(4.5f), lamp);
    dl->AddText(F_BODY, fb, ImVec2(p.x + S(22), p.y + S(4)), main, line1.c_str());
    if (!line2.empty()) dl->AddText(F_SMALL, fsm, ImVec2(p.x + S(22), p.y + S(4) + fb), U(col::dim), line2.c_str());
    ImGui::Dummy(ImVec2(width, rh));
}

// the buttons of a row, right-aligned on the row just drawn: each one placed at the same height
struct RowButtons {
    float x, y;
};
float button_w(const char *label_en) { return std::max(T(84), text_w(F_BODY, FS(F_BODY), tr(label_en)) + S(24)); }
RowButtons buttons_at_right(float x1, std::initializer_list<const char *> labels)
{
    float w = 0;
    for (const char *l : labels) w += button_w(l) + ImGui::GetStyle().ItemSpacing.x;
    return { x1 - w, ImGui::GetItemRectMin().y + (T(46) - T(26)) * 0.5f };
}
bool row_button_at(RowButtons &r, const char *label_en, bool enabled, bool red = false)
{
    ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
    const bool hit = row_button(label_en, enabled, red);
    r.x += button_w(label_en) + ImGui::GetStyle().ItemSpacing.x;
    return hit;
}
// back to the left edge under the row
void end_row(ImVec2 under) { ImGui::SetCursorScreenPos(under); ImGui::Dummy(ImVec2(0, 0)); }
}  // namespace

void social_restart(App &a)
{
    const bool on = a.ini.get("Online", "Enabled") == "1";
    const std::string server = on ? a.ini.get("Online", "Server", kCogboxServer) : std::string();
    std::string name = a.ini.get(profile_section(), "Gamertag");
    if (name.empty()) name = "Player";
    social_start(server, profile_dir(exe_dir(), std::to_string(current_profile())), name);
}

void social_rename(App &a, const std::string &name)
{
    // the games rename at start anyway; this shows the new tag in the tab at once
    if (a.ini.get("Online", "Enabled") != "1" || name.empty()) return;
    social_restart(a);
    social_do(SocialAction::Rename, name);   // the worker signs in first when it has to
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

    ui_section("YOUR ACCOUNT");
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        th_lamp(dl, ImVec2(p.x + S(6), p.y + h * 0.55f), S(4.5f), v.reachable ? 1 : (v.busy ? 0 : 3));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(18), p.y));
        if (v.have_account) ImGui::TextUnformatted(v.my_tag.c_str());
        else ImGui::TextUnformatted(tr(v.busy ? "Signing in..." : "Can't reach the online server."));
        if (v.have_account) ui_tip("Your tag: friends add you with it.");
    }
    if (v.have_account && !v.reachable && !v.busy) ui_note("Can't reach the online server.", col::amber);
    if (!v.error.empty()) ui_note(v.error, col::amber);
    if (!v.have_account) return;

    ui_section("ADD FRIEND");
    ImGui::SetNextItemWidth(S(260));
    const bool enter = ImGui::InputTextWithHint("##addfriend", tr("Name#1234"), g_add, sizeof g_add, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (row_button("ADD", g_add[0] != 0) || (enter && g_add[0])) {
        if (valid_tag(g_add)) {
            social_do(SocialAction::Add, g_add);
            g_add[0] = 0;
            g_bad_tag = false;
        } else {
            g_bad_tag = true;
        }
    }
    if (g_bad_tag) ui_note("Use Name#1234 - the name, # and the four digits.", col::amber);

    if (!v.invites.empty()) {
        ui_section("INVITES");
        for (const SocialInvite &i : v.invites) {
            ImGui::PushID(i.id.c_str());
            const std::string game = tr(game_name_for_title(i.title_id));
            row_text(U(col::text), i.from_tag, std::string(tr("invited you to")) + " " + game, 1, rowX1 - ImGui::GetCursorScreenPos().x);
            const ImVec2 after = ImGui::GetCursorScreenPos();   // under the row
            RowButtons rb = buttons_at_right(rowX1, { "ACCEPT", "DECLINE" });
            if (row_button_at(rb, "ACCEPT", true, true)) social_do(SocialAction::InviteAccept, i.id);
            if (row_button_at(rb, "DECLINE", true)) social_do(SocialAction::InviteDecline, i.id);
            end_row(after);
            ImGui::PopID();
        }
    }

    if (!v.lists.incoming.empty() || !v.lists.outgoing.empty()) {
        ui_section("REQUESTS");
        for (const SocialRequest &r : v.lists.incoming) {
            ImGui::PushID(r.uuid.c_str());
            row_text(U(col::text), r.tag, tr("wants to be your friend"), 2, rowX1 - ImGui::GetCursorScreenPos().x);
            const ImVec2 after = ImGui::GetCursorScreenPos();   // under the row
            RowButtons rb = buttons_at_right(rowX1, { "ACCEPT", "DECLINE", "BLOCK" });
            if (row_button_at(rb, "ACCEPT", true, true)) social_do(SocialAction::Accept, r.uuid);
            if (row_button_at(rb, "DECLINE", true)) social_do(SocialAction::Decline, r.uuid);
            if (row_button_at(rb, "BLOCK", true)) social_do(SocialAction::Block, r.uuid);
            ui_tip("Blocked players can't send you requests again.");
            end_row(after);
            ImGui::PopID();
        }
        for (const SocialRequest &r : v.lists.outgoing) {
            ImGui::PushID(r.uuid.c_str());
            row_text(U(col::dim), r.tag, tr("Waiting for them to accept"), 0, rowX1 - ImGui::GetCursorScreenPos().x);
            ImGui::PopID();
        }
    }

    ui_section("FRIENDS");
    if (v.lists.friends.empty()) {
        ui_note("No friends yet - add one by their tag.");
        return;
    }
    for (const SocialFriend &f : v.lists.friends) {
        ImGui::PushID(f.uuid.c_str());
        const bool joinable = f.state == "joinable";
        std::string presence = presence_text(f);
        const std::string game = game_name_for_title(f.title_id);
        if (!game.empty()) {   // the game's name in the launcher's language
            const size_t at = presence.find(game);
            if (at != std::string::npos) presence.replace(at, game.size(), tr(game.c_str()));
        }
        const int lamp = f.state == "offline" ? 0 : 1;
        row_text(U(f.state == "offline" ? col::dim : col::text), f.tag, presence, lamp, rowX1 - ImGui::GetCursorScreenPos().x);
        const int fg = game_for_title(f.title_id);
        const bool can_join = joinable && fg >= 0 && a.st[fg].ready == R_READY;
        const ImVec2 after = ImGui::GetCursorScreenPos();   // under the row
        RowButtons rb = buttons_at_right(rowX1, { "JOIN", "INVITE", "REMOVE" });
        if (row_button_at(rb, "JOIN", can_join, true)) social_do(SocialAction::Join, f.uuid);
        if (joinable && !can_join) ui_tip("Set up this game in COGBox to join.");
        else ui_tip(joinable ? "Join their game." : "They're not in a game you can join right now.");
        if (row_button_at(rb, "INVITE", f.state != "offline")) {
            // an invite needs a game you are hosting: from the launcher alone there is nothing to invite to
            if (a.run.proc) social_do(SocialAction::Invite, f.uuid);
            else g_invite_hint = true;
        }
        ui_tip("Invite them to the game you are hosting.");
        if (row_button_at(rb, "REMOVE", true)) {
            g_remove_uuid = f.uuid;
            ask(a, "socialremove", g, tr("Remove this friend?"), f.tag, { tr("No"), tr("Yes, remove") });
        }
        end_row(after);
        ImGui::PopID();
    }
    if (g_invite_hint) {
        ui_note("Launch the game and invite a friend from the F1 social menu.", col::amber);
        if (a.run.proc) g_invite_hint = false;
    }
}
