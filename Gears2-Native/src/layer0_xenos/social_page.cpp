// COGBox social step 5: see xenos/social_page.h. The network runs on the social worker (social_ingame.cpp); this
// file only draws and queues actions. The look follows the Achievements page (achievements_browser.cpp): the Noto
// achievement font, one dark slot per friend / request / invite with a crimson border when it has the focus.
#include <xenos/social_page.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include <xenos/achievement_popup_assets.h>   // XenosAchievementFontFor
#include <xenos/social_ingame.h>
#include <xenos/ui_lang.h>
#include "popup_styles.h"                       // popup::Text / TextWidth (the achievement font)

namespace xenos::social {
namespace {

char g_add[40] = {};
bool g_bad_tag = false;
bool g_keyboard = false;          // the on-screen keyboard is open (controller)
std::string g_remove_uuid;        // the friend REMOVE asks about
std::string g_note;               // a line for the player (a join they can't do from here)

// the Achievements page's colours
const ImU32 kCrimson = IM_COL32(200, 34, 26, 255);
const ImU32 kPanel = IM_COL32(29, 31, 35, 255);
const ImU32 kText = IM_COL32(232, 232, 232, 255);
const ImU32 kDim = IM_COL32(139, 143, 150, 255);
const ImU32 kSoft = IM_COL32(181, 184, 189, 255);
const ImU32 kGreen = IM_COL32(110, 200, 110, 255);
const ImU32 kAmber = IM_COL32(230, 195, 74, 255);
const ImU32 kBadge = IM_COL32(13, 13, 15, 255);
const ImVec4 kRedButton(0.62f, 0.10f, 0.08f, 1.0f);

// a section title: crimson bar, the title in the achievement font
void Section(const char* title, float s) {
  ImGui::Dummy(ImVec2(0, 6.f * s));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(ImVec2(p.x, p.y + 3.f * s), ImVec2(p.x + 4.f * s, p.y + 19.f * s), kCrimson);
  popup::Text(dl, true, 16.f * s, ImVec2(p.x + 12.f * s, p.y), kSoft, XenosTr(title), ImGui::GetContentRegionAvail().x);
  ImGui::Dummy(ImVec2(0, 24.f * s));
}

// the width a slot button takes
float ButtonWidth(const char* label, float s) {
  return std::max(84.f * s, ImGui::CalcTextSize(XenosTr(label)).x + 26.f * s);
}

// One slot: a dark panel with a badge (the tag's first letter), the tag and a coloured second line, and buttons on
// the right. Returns the index of the button pressed, or -1.
struct SlotButton {
  const char* label;
  bool enabled = true;
  bool red = false;
};
int Slot(const char* id, const std::string& tag, const std::string& line2, ImU32 line2_col, ImU32 lamp,
         const std::vector<SlotButton>& buttons, float s) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, h = 58.f * s, pad = 10.f * s;
  const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
  const bool hot = ImGui::IsMouseHoveringRect(p, q);
  dl->AddRectFilled(p, q, kPanel, 3.f * s);
  // badge
  const float r = 18.f * s;
  const ImVec2 c(p.x + pad + r, p.y + h * 0.5f);
  dl->AddCircleFilled(c, r, kBadge, 32);
  dl->AddCircle(c, r, kCrimson, 32, 2.f * s);
  const std::string letter = tag.empty() ? "?" : std::string(1, char(std::toupper(static_cast<unsigned char>(tag[0]))));
  const float lw = popup::TextWidth(true, 18.f * s, letter);
  popup::Text(dl, true, 18.f * s, ImVec2(c.x - lw * 0.5f, c.y - 11.f * s), kText, letter, r * 2);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), 5.f * s, lamp, 16);   // presence lamp on the badge
  dl->AddCircle(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), 5.f * s, kPanel, 16, 2.f * s);
  // the buttons, right to left
  float bx = q.x - pad, buttons_w = 0;
  for (const SlotButton& b : buttons) buttons_w += ButtonWidth(b.label, s) + 8.f * s;
  // text
  const float tx = c.x + r + 14.f * s, text_w = std::max(10.f, q.x - pad - buttons_w - tx);
  popup::Text(dl, true, 17.f * s, ImVec2(tx, p.y + 9.f * s), kText, tag, text_w);
  popup::Text(dl, false, 15.f * s, ImVec2(tx, p.y + 31.f * s), line2_col, line2, text_w);
  int pressed = -1;
  bool focused = false;
  const float bh = ImGui::GetFrameHeight();
  ImGui::PushID(id);
  for (int i = int(buttons.size()) - 1; i >= 0; --i) {
    const SlotButton& b = buttons[size_t(i)];
    const float bw = ButtonWidth(b.label, s);
    bx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - bh) * 0.5f));
    bx -= 8.f * s;
    if (!b.enabled) ImGui::BeginDisabled();
    if (b.red) ImGui::PushStyleColor(ImGuiCol_Button, kRedButton);
    if (ImGui::Button((std::string(XenosTr(b.label)) + "##" + b.label).c_str(), ImVec2(bw, 0))) pressed = i;
    if (b.red) ImGui::PopStyleColor();
    focused |= ImGui::IsItemFocused();
    if (!b.enabled) ImGui::EndDisabled();
  }
  ImGui::PopID();
  if (hot || focused) dl->AddRect(p, q, kCrimson, 3.f * s, 0, 2.f * s);
  ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 6.f * s));
  ImGui::Dummy(ImVec2(w, 0));
  return pressed;
}

// a line of text in the achievement font
void Line(const std::string& text, ImU32 col, float s) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  popup::Text(ImGui::GetWindowDrawList(), false, 15.f * s, p, col, text, ImGui::GetContentRegionAvail().x);
  ImGui::Dummy(ImVec2(0, 21.f * s));
}

// the launcher's name-box keyboard: D-pad to a key, A types it
void Keyboard(float s) {
  static bool upper = true;
  auto put = [](char ch) {
    const size_t n = std::strlen(g_add);
    if (n < sizeof(g_add) - 1) { g_add[n] = ch; g_add[n + 1] = 0; }
  };
  static const char* rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL#", "ZXCVBNM"};
  const float k = ImGui::GetFrameHeight() * 1.2f, g = 4.0f * s;
  for (int r = 0; r < 4; ++r) {
    for (const char* c = rows[r]; *c; ++c) {
      const char ch = (upper || !std::isalpha(static_cast<unsigned char>(*c))) ? *c : char(std::tolower(static_cast<unsigned char>(*c)));
      if (c != rows[r]) ImGui::SameLine(0, g);
      ImGui::PushID(int(static_cast<unsigned char>(*c)));
      const char lbl[2] = {ch, 0};   // the label alone: "#" + "###id" would read as "####" (hidden)
      if (ImGui::Button(lbl, ImVec2(k, k))) put(ch);
      ImGui::PopID();
    }
  }
  if (ImGui::Button(upper ? "abc###skcase" : "ABC###skcase", ImVec2(k * 2 + g, k))) upper = !upper;
  ImGui::SameLine(0, g);
  if (ImGui::Button((std::string(XenosTr("DEL")) + "###skdel").c_str(), ImVec2(k * 2 + g, k))) {
    const size_t n = std::strlen(g_add);
    if (n) g_add[n - 1] = 0;
  }
  ImGui::SameLine(0, g);
  if (ImGui::Button((std::string(XenosTr("DONE")) + "###skdone").c_str(), ImVec2(k * 3 + g * 2, k))) g_keyboard = false;
}

std::string GameName(const std::string& title_id) {
  const char* n = game_name_for_title(title_id);
  return n && *n ? XenosTr(n) : std::string();
}

}  // namespace

bool ConsumeBack(bool item_active) {
  if (g_keyboard) {
    g_keyboard = false;
    return true;
  }
  return item_active;   // Esc / B in the text box only ends typing (Dear ImGui handles it)
}

void OnLeave() {
  g_keyboard = false;
  g_remove_uuid.clear();
}

PageResult DrawPage(float scale, bool pad, bool ps_names) {
  PageResult result = PageResult::None;
  // a Join for a game that is not this one: the server answered "start that game" (the launcher does that)
  if (auto j = social_take_join()) {
    (void)j;
    g_note = XenosTr("They're in another game. Start that game from COGBox to join them.");
  }
  const float s = scale * 1.15f;   // the Achievements page's size
  // the Noto achievement font for the widgets too (buttons, the text box)
  ImFont* font = XenosAchievementFontFor(false, 16.f * s);
  if (font) ImGui::PushFont(font, 16.f * s);
  const SocialView v = XenosSocialView();
  const float bar_h = ImGui::GetFrameHeightWithSpacing() * 1.6f + 22.f * s;
  if (ImGui::BeginChild("##social", ImVec2(0, ImGui::GetContentRegionAvail().y - bar_h), ImGuiChildFlags_NavFlattened)) {
    Section("YOUR ACCOUNT", s);
    {
      const ImU32 lamp = v.reachable ? kGreen : (v.busy ? kDim : kAmber);
      const std::string tag = v.have_account ? v.my_tag : std::string(XenosTr(v.busy ? "Signing in..." : "Not signed in"));
      const std::string state = v.reachable ? XenosTr("Online") : v.busy ? XenosTr("Connecting...") : XenosTr("Can't reach the online server.");
      Slot("me", tag, state, v.reachable ? kGreen : kAmber, lamp, {}, s);
    }
    if (!v.error.empty()) Line(v.error, kAmber, s);
    if (!g_note.empty()) Line(g_note, kAmber, s);

    if (v.have_account) {
      Section("ADD FRIEND", s);
      ImGui::SetNextItemWidth(280.f * s);
      const bool enter = ImGui::InputTextWithHint("##addfriend", "Name#1234", g_add, sizeof g_add, ImGuiInputTextFlags_EnterReturnsTrue);
      if (pad) {
        ImGui::SameLine();
        if (ImGui::Button((std::string(XenosTr("KEYBOARD")) + "##kb").c_str())) g_keyboard = !g_keyboard;
      }
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, kRedButton);
      const bool add = ImGui::Button((std::string(XenosTr("ADD")) + "##add").c_str(), ImVec2(ButtonWidth("ADD", s), 0));
      ImGui::PopStyleColor();
      if (add || (enter && g_add[0])) {
        if (valid_tag(g_add)) {
          social_do(SocialAction::Add, g_add);
          g_add[0] = 0;
          g_bad_tag = false;
          g_keyboard = false;
        } else {
          g_bad_tag = g_add[0] != 0;
        }
      }
      if (g_keyboard) Keyboard(s);
      if (g_bad_tag) Line(XenosTr("Use Name#1234 - the name, # and the four digits."), kAmber, s);

      if (!v.invites.empty()) {
        Section("INVITES", s);
        for (const SocialInvite& i : v.invites) {
          const int b = Slot(("inv" + i.id).c_str(), i.from_tag, std::string(XenosTr("invited you to")) + " " + GameName(i.title_id),
                             kAmber, kGreen, {{"ACCEPT", true, true}, {"DECLINE"}}, s);
          if (b == 0) {
            social_do(SocialAction::InviteAccept, i.id);
            result = PageResult::CloseMenu;
          }
          if (b == 1) social_do(SocialAction::InviteDecline, i.id);
        }
      }

      if (!v.lists.incoming.empty() || !v.lists.outgoing.empty()) {
        Section("REQUESTS", s);
        for (const SocialRequest& r : v.lists.incoming) {
          const int b = Slot(("req" + r.uuid).c_str(), r.tag, XenosTr("wants to be your friend"), kAmber, kAmber,
                             {{"ACCEPT", true, true}, {"DECLINE"}, {"BLOCK"}}, s);
          if (b == 0) social_do(SocialAction::Accept, r.uuid);
          if (b == 1) social_do(SocialAction::Decline, r.uuid);
          if (b == 2) social_do(SocialAction::Block, r.uuid);
        }
        for (const SocialRequest& r : v.lists.outgoing)
          Slot(("out" + r.uuid).c_str(), r.tag, XenosTr("Waiting for them to accept"), kDim, kDim, {}, s);
      }

      Section("FRIENDS", s);
      if (v.lists.friends.empty()) Line(XenosTr("No friends yet - add one by their tag."), kDim, s);
      // online friends first, then offline; each group as the server sorts it
      std::vector<const SocialFriend*> order;
      for (const SocialFriend& f : v.lists.friends) if (f.state != "offline") order.push_back(&f);
      for (const SocialFriend& f : v.lists.friends) if (f.state == "offline") order.push_back(&f);
      for (const SocialFriend* fp : order) {
        const SocialFriend& f = *fp;
        const bool offline = f.state == "offline", joinable = f.state == "joinable";
        std::string presence = presence_text(f);
        const char* g = game_name_for_title(f.title_id);
        if (g && *g) {   // the game's name in the menu's language
          const size_t at = presence.find(g);
          if (at != std::string::npos) presence.replace(at, std::strlen(g), XenosTr(g));
        }
        const ImU32 col = offline ? kDim : joinable ? kAmber : kGreen;
        const std::string id = "fr" + f.uuid;
        if (g_remove_uuid == f.uuid) {
          const int b = Slot(id.c_str(), f.tag, XenosTr("Remove this friend?"), kAmber, col, {{"YES, REMOVE", true, true}, {"NO"}}, s);
          if (b == 0) {
            social_do(SocialAction::Remove, f.uuid);
            g_remove_uuid.clear();
          }
          if (b == 1) g_remove_uuid.clear();
        } else {
          const int b = Slot(id.c_str(), f.tag, XenosTr(presence.c_str()), col, col,
                             {{"JOIN", joinable, true}, {"INVITE", !offline}, {"REMOVE"}}, s);
          if (b == 0) {
            g_note.clear();
            social_do(SocialAction::Join, f.uuid);
            result = PageResult::CloseMenu;
          }
          if (b == 1) social_do(SocialAction::Invite, f.uuid);
          if (b == 2) g_remove_uuid = f.uuid;
        }
      }
    }
  }
  ImGui::EndChild();
  ImGui::Separator();
  Line(XenosTr(ps_names ? "Circle: back      Invite works while you host a lobby or match"
                        : "B: back      Invite works while you host a lobby or match"), kDim, s);
  const float bw = ImGui::GetContentRegionAvail().x;
  if (ImGui::Button(XenosTr("Back to settings"), ImVec2(bw, 0))) result = PageResult::Back;
  if (font) ImGui::PopFont();
  return result;
}

}  // namespace xenos::social
