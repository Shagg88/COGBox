// COGBox social step 5: see xenos/social_page.h. The network runs on the social worker (social_ingame.cpp); this
// file only draws and queues actions. The look follows the Achievements page (achievements_browser.cpp): the Noto
// achievement font, one dark slot per friend / request / invite with a crimson border when it has the focus.
#include <xenos/social_page.h>

#include <algorithm>
#include <cmath>
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
  return std::max(64.f * s, ImGui::CalcTextSize(XenosTr(label)).x + 26.f * s);
}

// A row's buttons.
struct SlotButton {
  const char* label;
  bool enabled = true;
  bool red = false;
};
// Layout A: one line per entry - a presence dot, the tag, its state, and small buttons on the right. Returns the
// index of the button pressed, or -1.
int Row(const char* id, const std::string& tag, const std::string& state, ImU32 state_col, ImU32 lamp,
        const std::vector<SlotButton>& buttons, float s, const std::string& icon = std::string()) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, bh = ImGui::GetFrameHeight(), h = bh + 10.f * s, pad = 10.f * s;
  const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
  const bool hot = ImGui::IsMouseHoveringRect(p, q);
  dl->AddRectFilled(p, q, kPanel, 3.f * s);
  const float r = h * 0.36f;
  const ImVec2 c(p.x + pad + r, p.y + h * 0.5f);
  dl->AddCircleFilled(c, r, kBadge, 32);
  if (const ImTextureID pic = ProfileIcon(icon)) {
    dl->AddImageRounded(pic, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
  } else {
    const std::string letter = tag.empty() ? "?" : std::string(1, char(std::toupper(static_cast<unsigned char>(tag[0]))));
    const float lw = popup::TextWidth(true, 14.f * s, letter);
    popup::Text(dl, true, 14.f * s, ImVec2(c.x - lw * 0.5f, c.y - 9.f * s), kText, letter, r * 2);
  }
  dl->AddCircle(c, r, kCrimson, 32, 1.5f * s);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), 4.f * s, lamp, 16);
  dl->AddCircle(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), 4.f * s, kPanel, 16, 1.5f * s);
  float buttons_w = 0;
  for (const SlotButton& b : buttons) buttons_w += ButtonWidth(b.label, s) + 6.f * s;
  const float tx = c.x + r + 12.f * s, name_w = std::min(190.f * s, (q.x - pad - buttons_w - tx) * 0.45f);
  const float ty = p.y + (h - 17.f * s) * 0.5f - 2.f * s;
  popup::Text(dl, true, 16.f * s, ImVec2(tx, ty), kText, tag, name_w);
  popup::Text(dl, false, 15.f * s, ImVec2(tx + name_w + 12.f * s, ty + 1.f * s), state_col, state,
              std::max(10.f, q.x - pad - buttons_w - tx - name_w - 16.f * s));
  int pressed = -1;
  bool focused = false;
  float bx = q.x - pad;
  ImGui::PushID(id);
  for (int i = int(buttons.size()) - 1; i >= 0; --i) {
    const SlotButton& b = buttons[size_t(i)];
    const float bw = ButtonWidth(b.label, s);
    bx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - bh) * 0.5f));
    bx -= 6.f * s;
    if (!b.enabled) ImGui::BeginDisabled();
    if (b.red) ImGui::PushStyleColor(ImGuiCol_Button, kRedButton);
    if (ImGui::Button((std::string(XenosTr(b.label)) + "##" + b.label).c_str(), ImVec2(bw, 0))) pressed = i;
    if (b.red) ImGui::PopStyleColor();
    focused |= ImGui::IsItemFocused();
    if (!b.enabled) ImGui::EndDisabled();
  }
  ImGui::PopID();
  if (hot || focused) dl->AddRect(p, q, kCrimson, 3.f * s, 0, 2.f * s);
  ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 4.f * s));
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
  (void)ps_names;   // the frame's legend names the buttons
  // the top strip: your account, then Add friend on the same line
  {
    const ImU32 lamp = v.reachable ? kGreen : (v.busy ? kDim : kAmber);
    const std::string tag = v.have_account ? v.my_tag : std::string(XenosTr(v.busy ? "Signing in..." : "Not signed in"));
    const std::string state = v.reachable ? XenosTr("Online") : v.busy ? XenosTr("Connecting...") : XenosTr("Can't reach the online server.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, h = 52.f * s, pad = 12.f * s;
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, kPanel, 3.f * s);
    const float r = 17.f * s;
    const ImVec2 c(p.x + pad + r, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r, kBadge, 32);
    dl->AddCircle(c, r, kCrimson, 32, 2.f * s);
    const std::string letter = tag.empty() ? "?" : std::string(1, char(std::toupper(static_cast<unsigned char>(tag[0]))));
    const float lw = popup::TextWidth(true, 17.f * s, letter);
    popup::Text(dl, true, 17.f * s, ImVec2(c.x - lw * 0.5f, c.y - 10.f * s), kText, letter, r * 2);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), 5.f * s, lamp, 16);
    const float tx = c.x + r + 14.f * s, ty = p.y + (h - 17.f * s) * 0.5f - 2.f * s;
    const float tag_w = std::min(popup::TextWidth(true, 17.f * s, tag), w * 0.3f);
    popup::Text(dl, true, 17.f * s, ImVec2(tx, ty), kText, tag, w * 0.3f);
    popup::Text(dl, false, 15.f * s, ImVec2(tx + tag_w + 14.f * s, ty + 1.f * s), v.reachable ? kGreen : kAmber, state, w * 0.25f);
    if (v.have_account) {
      const float add_w = ButtonWidth("ADD", s), kb_w = pad ? ButtonWidth("KEYBOARD", s) : 0.f, box_w = 260.f * s;
      const float by = p.y + (h - ImGui::GetFrameHeight()) * 0.5f;
      float x = q.x - pad - add_w - 8.f * s - box_w - (pad ? kb_w + 8.f * s : 0.f);
      const std::string lbl = XenosTr("Add friend");
      const float lbl_w = popup::TextWidth(false, 15.f * s, lbl);
      popup::Text(dl, false, 15.f * s, ImVec2(x - lbl_w - 12.f * s, ty + 1.f * s), kDim, lbl, lbl_w + 4.f * s);
      ImGui::SetCursorScreenPos(ImVec2(x, by));
      ImGui::SetNextItemWidth(box_w);
      const bool enter = ImGui::InputTextWithHint("##addfriend", "Name#1234", g_add, sizeof g_add, ImGuiInputTextFlags_EnterReturnsTrue);
      x += box_w + 8.f * s;
      if (pad) {
        ImGui::SetCursorScreenPos(ImVec2(x, by));
        if (ImGui::Button((std::string(XenosTr("KEYBOARD")) + "##kb").c_str(), ImVec2(kb_w, 0))) g_keyboard = !g_keyboard;
        x += kb_w + 8.f * s;
      }
      ImGui::SetCursorScreenPos(ImVec2(x, by));
      ImGui::PushStyleColor(ImGuiCol_Button, kRedButton);
      const bool add = ImGui::Button((std::string(XenosTr("ADD")) + "##add").c_str(), ImVec2(add_w, 0));
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
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 10.f * s));
    ImGui::Dummy(ImVec2(w, 0));
  }
  if (g_keyboard) Keyboard(s);
  if (g_bad_tag) Line(XenosTr("Use Name#1234 - the name, # and the four digits."), kAmber, s);
  if (!v.error.empty()) Line(v.error, kAmber, s);
  if (!g_note.empty()) Line(g_note, kAmber, s);

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float gap = 24.f * s, left_w = std::floor((avail.x - gap) * 0.6f);
  if (ImGui::BeginChild("##social", ImVec2(left_w, avail.y), ImGuiChildFlags_NavFlattened) && v.have_account) {
    const size_t nreq = v.lists.incoming.size();
    std::string head = std::string(XenosTr("FRIENDS")) + " (" + std::to_string(v.lists.friends.size()) + ")";
    if (nreq) head += "  " + std::to_string(nreq) + " " + XenosTr(nreq == 1 ? "REQUEST" : "REQUESTS");
    Section(head.c_str(), s);
    for (const SocialRequest& r : v.lists.incoming) {
      const int b = Row(("req" + r.uuid).c_str(), r.tag, XenosTr("wants to be your friend"), kAmber, kAmber,
                        {{"ACCEPT", true, true}, {"DECLINE"}, {"BLOCK"}}, s, r.icon);
      if (b == 0) social_do(SocialAction::Accept, r.uuid);
      if (b == 1) social_do(SocialAction::Decline, r.uuid);
      if (b == 2) social_do(SocialAction::Block, r.uuid);
    }
    for (const SocialRequest& r : v.lists.outgoing)
      Row(("out" + r.uuid).c_str(), r.tag, XenosTr("Waiting for them to accept"), kDim, kDim, {}, s, r.icon);
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
        const int b = Row(id.c_str(), f.tag, XenosTr("Remove this friend?"), kAmber, col, {{"YES, REMOVE", true, true}, {"NO"}}, s, f.icon);
        if (b == 0) {
          social_do(SocialAction::Remove, f.uuid);
          g_remove_uuid.clear();
        }
        if (b == 1) g_remove_uuid.clear();
        continue;
      }
      // only the buttons that do something here: Join when joinable, Invite when online, "..." to remove
      std::vector<SlotButton> buttons;
      if (joinable) buttons.push_back({"JOIN", true, true});
      if (!offline) buttons.push_back({"INVITE"});
      buttons.push_back({"..."});
      const int b = Row(id.c_str(), f.tag, XenosTr(presence.c_str()), col, col, buttons, s, f.icon);
      if (b < 0) continue;
      const std::string what = buttons[size_t(b)].label;
      if (what == "JOIN") {
        g_note.clear();
        social_do(SocialAction::Join, f.uuid);
        result = PageResult::CloseMenu;
      } else if (what == "INVITE") {
        social_do(SocialAction::Invite, f.uuid);
      } else {
        g_remove_uuid = f.uuid;
      }
    }
  }
  ImGui::EndChild();
  ImGui::SameLine(0, gap);
  if (ImGui::BeginChild("##social_invites", ImVec2(0, avail.y), ImGuiChildFlags_NavFlattened)) {
    Section("PARTY AND MATCH INVITES", s);
    if (!v.have_account) {
      Line(XenosTr("Invites show here once you're signed in."), kDim, s);
    } else {
      if (v.invites.empty()) Line(XenosTr("No invites right now."), kDim, s);
      for (const SocialInvite& i : v.invites) {
        const int b = Row(("inv" + i.id).c_str(), i.from_tag, GameName(i.title_id), kAmber, kGreen,
                          {{"ACCEPT", true, true}, {"DECLINE"}}, s, i.from_icon);
        if (b == 0) {
          social_do(SocialAction::InviteAccept, i.id);
          result = PageResult::CloseMenu;
        }
        if (b == 1) social_do(SocialAction::InviteDecline, i.id);
      }
    }
    Line(XenosTr("You can invite friends while you host a lobby or match."), kDim, s);
  }
  ImGui::EndChild();
  if (font) ImGui::PopFont();
  return result;
}

}  // namespace xenos::social
