// Layer 0 (game-free): the in-game menu's Achievements page - see xenos/achievements_browser.h. Our code (MIT).
#include <xenos/achievements_browser.h>
#include <xenos/achievement_popup_assets.h>
#include <xenos/achievements_export.h>

#include "popup_styles.h"
#include <xenos/ui_lang.h>   // --cogbox_ui_lang: the page in the launcher's language

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

#include <imgui.h>

#include <algorithm>
#include <ctime>

namespace xenos {

namespace {
using popup::Col;

// The PC's offset from UTC at that moment, in minutes (summer time included).
int LocalOffsetMinutes(int64_t unix_seconds) {
  const std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm local{};
  if (localtime_s(&local, &t) != 0) return 0;
  return static_cast<int>((_mkgmtime(&local) - t) / 60);
}

const ImU32 kCrimson = IM_COL32(200, 34, 26, 255);
const ImU32 kPanel = IM_COL32(29, 31, 35, 255);
const ImU32 kTrack = IM_COL32(42, 44, 49, 255);
const ImU32 kText = IM_COL32(232, 232, 232, 255);
const ImU32 kDim = IM_COL32(139, 143, 150, 255);
const ImU32 kSoft = IM_COL32(181, 184, 189, 255);

void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t percent) {
  dl->AddRectFilled(a, b, kTrack, (b.y - a.y) * 0.5f);
  const float w = (b.x - a.x) * std::min(percent, 100u) / 100.f;
  if (w > 0) dl->AddRectFilled(a, ImVec2(a.x + w, b.y), kCrimson, (b.y - a.y) * 0.5f);
}

// A small filled tag; returns its width.
float Tag(ImDrawList* dl, ImVec2 pos, float size, const char* text) {
  const float w = popup::TextWidth(true, size, text) + size * 0.8f;
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + size * 1.35f), IM_COL32(51, 54, 60, 255), size * 0.2f);
  popup::Text(dl, true, size, ImVec2(pos.x + size * 0.4f, pos.y + size * 0.15f), IM_COL32(207, 210, 214, 255), text, w);
  return w;
}
}  // namespace

AchievementsBrowser::AchievementsBrowser(rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime)
    : immediate_(immediate), runtime_(runtime), stale_(std::make_shared<std::atomic<bool>>(true)) {
  Attach();
}

rex::Runtime* AchievementsBrowser::Rt() const { return runtime_ ? runtime_ : rex::Runtime::instance(); }

void AchievementsBrowser::Attach() {
  if (running_icons_) return;  // attached
  rex::Runtime* rt = Rt();
  if (!rt || !rt->kernel_state() || !rt->kernel_state()->GetExecutableModule()) return;  // not loaded yet
  runtime_ = rt;
  auto* kernel = rt->kernel_state();
  running_title_ = kernel->title_id();
  running_icons_ = std::make_unique<rex::ui::AchievementIconCache>(immediate_, rt);
  std::weak_ptr<std::atomic<bool>> stale = stale_;
  unlock_listener_ = kernel->achievements().RegisterUnlockCallback([stale](const rex::system::AchievementEvent&) {
    if (auto s = stale.lock()) s->store(true);
  });
}

AchievementsBrowser::~AchievementsBrowser() {
  if (unlock_listener_ && runtime_ && runtime_->kernel_state())
    runtime_->kernel_state()->achievements().UnregisterCallback(unlock_listener_);
}

void AchievementsBrowser::Reload() {
  Attach();
  stale_->store(false);
  dir_ = XenosCogboxDir();
  games_ = ach::LoadAllGames(dir_, ach::CurrentRarityProvider());
  // The running game from the SDK when the cache has no catalog for it (it could not be written).
  for (size_t i = 0; i < ach::kGames.size() && i < games_.size(); ++i) {
    if (ach::kGames[i].title_id != running_title_ || games_[i].loaded) continue;
    ach::Catalog c = XenosCatalogFromRuntime(Rt());
    if (c.achievements.empty()) break;
    games_[i] = ach::BuildGame(ach::kGames[i], c, XenosUnlocksFromRuntime(Rt()), ach::LoadRevealed(dir_),
                               ach::CurrentRarityProvider(), dir_);
  }
  sidebar_ = ach::BuildSidebar(games_, running_title_);
  for (ach::BrowserEntry& e : sidebar_) {  // the model's English (shared with the launcher and its tests)
    if (e.all) e.name = XenosTr(e.name);
    if (!e.loaded) e.line = XenosTr(e.line);
  }
  sel_ = 0;
  for (size_t i = 0; i < sidebar_.size(); ++i)
    if (sel_game_ != -2 && sidebar_[i].all == sel_all_ && (sel_all_ || sidebar_[i].game == sel_game_)) sel_ = i;
  RebuildRows();
}

void AchievementsBrowser::RebuildRows() {
  if (sidebar_.empty()) {
    rows_.clear();
    return;
  }
  rows_ = ach::BuildRows(games_, sidebar_[sel_], sort_, show_, LocalOffsetMinutes);
  if (XenosUiLangActive())
    for (ach::BrowserRow& r : rows_) {
      if (r.hidden_secret) r.name = XenosTr(r.name), r.text = XenosTr(r.text);  // ours; a real name is the game's
      r.status = XenosTrWords(r.status, {"Unlocked", "Locked"});
      r.rarity = XenosTrWords(r.rarity, {"Rare"});
    }
}

void AchievementsBrowser::Select(size_t index) {
  if (index >= sidebar_.size()) return;
  sel_ = index;
  sel_all_ = sidebar_[index].all;
  sel_game_ = sidebar_[index].game;
  scroll_top_ = true;
  RebuildRows();
}

void AchievementsBrowser::Previous() {
  if (!sidebar_.empty()) Select((sel_ + sidebar_.size() - 1) % sidebar_.size());
}
void AchievementsBrowser::Next() {
  if (!sidebar_.empty()) Select((sel_ + 1) % sidebar_.size());
}
void AchievementsBrowser::CycleSort() {
  sort_ = ach::NextSort(sort_);
  scroll_top_ = true;
  RebuildRows();
}
void AchievementsBrowser::CycleShow() {
  show_ = ach::NextShow(show_);
  scroll_top_ = true;
  RebuildRows();
}

ImTextureID AchievementsBrowser::Icon(const ach::Achievement& a) {
  if (running_icons_ && a.title_id == running_title_ && runtime_ && runtime_->kernel_state()) {
    if (auto info = runtime_->kernel_state()->achievements().FindAchievement(a.id)) {
      if (rex::ui::ImmediateTexture* t = running_icons_->GetIcon(*info)) return reinterpret_cast<ImTextureID>(t);
    }
  }
  const auto key = std::make_pair(a.title_id, a.id);
  auto it = icons_.find(key);
  if (it == icons_.end()) {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    std::string bytes;
    if (immediate_ && !a.icon.empty() && ach::ReadFile(a.icon, &bytes)) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba =
          rex::ui::DecodeImageRGBA(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), w, h);
      if (!rgba.empty() && w > 0 && h > 0)
        tex = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                        rgba.data());
    }
    it = icons_.emplace(key, std::move(tex)).first;
  }
  return it->second ? reinterpret_cast<ImTextureID>(it->second.get()) : ImTextureID(0);
}

std::string AchievementsBrowser::Draw(float scale, float height) {
  if (stale_->load() || sidebar_.empty()) Reload();
  const float s = scale * 1.15f;   // part 4 player check: bigger text at 720p
  std::string tip;
  DrawSidebar(s, height);
  ImGui::SameLine();
  DrawMain(s, height, &tip);
  DrawReveal(s);
  return tip;
}

void AchievementsBrowser::DrawSidebar(float s, float height) {
  if (!ImGui::BeginChild("##ach_side", ImVec2(240.f * s, height), ImGuiChildFlags_NavFlattened)) {
    ImGui::EndChild();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, h = 62.f * s, pad = 9.f * s;
  // ImGui's own highlight is a little taller than our box (only strips of it showed): off; we draw the whole border.
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
  for (size_t i = 0; i < sidebar_.size(); ++i) {
    const ach::BrowserEntry& e = sidebar_[i];
    ImGui::PushID(int(i));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##entry", i == sel_, 0, ImVec2(w, h)) && i != sel_) Select(i);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, kPanel, 3.f * s);
    if (i == sel_ || hot) dl->AddRect(p, q, kCrimson, 3.f * s, 0, 2.f * s);
    const ImU32 name_col = e.loaded ? kText : IM_COL32(119, 119, 119, 255);
    float x = p.x + pad;
    popup::Text(dl, true, 17.f * s, ImVec2(x, p.y + pad * 0.6f), name_col, e.name, w - 2 * pad);
    if (e.this_game) Tag(dl, ImVec2(x + popup::TextWidth(true, 17.f * s, e.name) + 6.f * s, p.y + pad * 0.7f), 11.f * s, XenosTr("This game"));
    if (e.loaded) Bar(dl, ImVec2(x, p.y + 30.f * s), ImVec2(q.x - pad, p.y + 35.f * s), ach::PercentByGp(e.totals));
    popup::Text(dl, false, 15.f * s, ImVec2(x, p.y + (e.loaded ? 39.f : 33.f) * s), kDim, e.line, w - 2 * pad);
  }
  ImGui::PopStyleColor(3);
  ImGui::EndChild();
}

void AchievementsBrowser::DrawMain(float s, float height, std::string* tip) {
  if (!ImGui::BeginChild("##ach_main", ImVec2(0, height), ImGuiChildFlags_NavFlattened)) {
    ImGui::EndChild();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x;
  // header
  if (!sidebar_.empty()) {
    ach::BrowserHeader hd = ach::HeaderFor(sidebar_[sel_]);
    if (sidebar_[sel_].all) hd.title = XenosTr("All played games");
    hd.counts = XenosTrWords(hd.counts, {"achievements"});
    const ImVec2 p = ImGui::GetCursorScreenPos();
    popup::Text(dl, true, 21.f * s, p, kText, hd.title, w);
    popup::Text(dl, false, 17.f * s, ImVec2(p.x, p.y + 28.f * s), kSoft, hd.counts, w);
    Bar(dl, ImVec2(p.x, p.y + 52.f * s), ImVec2(p.x + w, p.y + 58.f * s), hd.percent);
    ImGui::Dummy(ImVec2(w, 64.f * s));
  }
  // Sort / Show
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(XenosTr("Sort:"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(190.f * s);
  if (ImGui::BeginCombo("##sort", XenosTr(ach::SortName(sort_)))) {
    for (ach::SortOrder o : {ach::SortOrder::kUnlockTime, ach::SortOrder::kRarity, ach::SortOrder::kGameOrder,
                             ach::SortOrder::kGamerscore, ach::SortOrder::kName}) {
      if (ImGui::Selectable(XenosTr(ach::SortName(o)), o == sort_) && o != sort_) {
        sort_ = o;
        scroll_top_ = true;
        RebuildRows();
      }
      if (o == sort_) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::Text("  %s", XenosTr("Show:"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140.f * s);
  if (ImGui::BeginCombo("##show", XenosTr(ach::ShowName(show_)))) {
    for (ach::ShowFilter f : {ach::ShowFilter::kAll, ach::ShowFilter::kUnlocked, ach::ShowFilter::kLocked}) {
      if (ImGui::Selectable(XenosTr(ach::ShowName(f)), f == show_) && f != show_) {
        show_ = f;
        scroll_top_ = true;
        RebuildRows();
      }
      if (f == show_) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  // rows
  if (ImGui::BeginChild("##ach_rows", ImVec2(0, 0), ImGuiChildFlags_NavFlattened)) {
    if (scroll_top_) {
      ImGui::SetScrollY(0);
      scroll_top_ = false;
    }
    ImDrawList* rl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x, rh = 82.f * s, icon = 52.f * s, pad = 10.f * s;   // part 4: room for a 2-line description
    if (rows_.empty() && !sidebar_.empty()) {
      // "Start <game> once..." has the game's name in it: one sentence per case for the translations
      const ach::BrowserEntry& se = sidebar_[sel_];
      const std::string msg = !se.loaded && !se.all ? std::string(XenosTr("Start this game once from COGBox to load its achievements."))
                                                    : XenosTr(ach::EmptyMessage(se, show_));
      const float tw = popup::TextWidth(false, 16.f * s, msg);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      popup::Text(rl, false, 16.f * s, ImVec2(p.x + std::max(0.f, (rw - tw) * 0.5f), p.y + 40.f * s), kDim, msg, rw);
    }
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));   // our own border instead (see the left column)
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
    ImGuiListClipper clip;
    clip.Begin(int(rows_.size()), rh + ImGui::GetStyle().ItemSpacing.y);
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const ach::BrowserRow& r = rows_[size_t(i)];
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", false, 0, ImVec2(rw, rh)) && r.hidden_secret && r.a) {
          reveal_ = {r.a->title_id, r.a->id};
          reveal_failed_ = false;
          want_reveal_ = true;
        }
        const bool focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::PopID();
        if (focused) *tip = r.name + (r.text.empty() ? "" : " - " + r.text);
        const ImVec2 q(p.x + rw, p.y + rh);
        rl->AddRectFilled(p, q, kPanel, 3.f * s);
        if (focused) rl->AddRect(p, q, kCrimson, 3.f * s, 0, 2.f * s);
        // icon
        const ImVec2 ia(p.x + pad, p.y + (rh - icon) * 0.5f), ib(ia.x + icon, ia.y + icon);
        const ImTextureID tex = r.hidden_secret || !r.a ? ImTextureID(0) : Icon(*r.a);
        if (tex) {
          rl->AddImageRounded(tex, ia, ib, ImVec2(0, 0), ImVec2(1, 1),
                              r.locked ? IM_COL32(115, 115, 115, 255) : IM_COL32_WHITE, 3.f * s);
        } else {
          rl->AddRectFilled(ia, ib, IM_COL32(44, 46, 51, 255), 3.f * s);
          const float qs = 26.f * s;
          popup::Text(rl, true, qs, ImVec2(ia.x + (icon - popup::TextWidth(true, qs, "?")) * 0.5f, ia.y + (icon - qs) * 0.45f),
                      IM_COL32(119, 119, 119, 255), "?", icon);
        }
        // right column: GP, status, rarity
        const float right_w = std::max({popup::TextWidth(true, 16.f * s, r.gp), popup::TextWidth(false, 15.f * s, r.status),
                                        popup::TextWidth(false, 15.f * s, r.rarity)});
        const float rx = q.x - pad;
        // the name and up to two lines of text, centred in the row as one block (part 4 player check: wrap, not "...")
        const float tx = ib.x + 12.f * s, text_w = std::max(10.f, rx - right_w - 14.f * s - tx);
        const std::vector<std::string> lines = ach::WrapText(
            r.text, text_w, [&](const std::string& t) { return popup::TextWidth(false, 16.f * s, t); }, 2);
        const float block = (25.f + 19.f * float(lines.size())) * s, ny = p.y + (rh - block) * 0.5f;
        popup::Text(rl, true, 16.f * s, ImVec2(rx - popup::TextWidth(true, 16.f * s, r.gp), ny - 1.f * s), kText, r.gp, right_w);
        popup::Text(rl, false, 15.f * s, ImVec2(rx - popup::TextWidth(false, 15.f * s, r.status), ny + 21.f * s), kDim,
                    r.status, right_w);
        if (!r.rarity.empty())
          popup::Text(rl, false, 15.f * s, ImVec2(rx - popup::TextWidth(false, 15.f * s, r.rarity), ny + 38.f * s),
                      IM_COL32(230, 195, 74, 255), r.rarity, right_w);
        // name (+ tags) and text
        float tags = 0;
        if (r.secret_tag) tags += popup::TextWidth(true, 11.f * s, XenosTr("Secret")) + 14.f * s;
        if (!r.game_label.empty()) tags += popup::TextWidth(true, 11.f * s, r.game_label.c_str()) + 14.f * s;
        const ImU32 name_col = r.hidden_secret ? IM_COL32(153, 153, 153, 255) : r.locked ? IM_COL32(187, 187, 187, 255) : kText;
        const float name_w = std::max(10.f, text_w - tags);
        popup::Text(rl, true, 17.f * s, ImVec2(tx, ny), name_col, r.name, name_w);
        float tag_x = tx + std::min(name_w, popup::TextWidth(true, 17.f * s, r.name)) + 6.f * s;
        if (r.secret_tag) tag_x += Tag(rl, ImVec2(tag_x, ny + 3.f * s), 11.f * s, XenosTr("Secret")) + 6.f * s;
        if (!r.game_label.empty()) Tag(rl, ImVec2(tag_x, ny + 3.f * s), 11.f * s, r.game_label.c_str());
        for (size_t li = 0; li < lines.size(); ++li)
          popup::Text(rl, false, 16.f * s, ImVec2(tx, ny + (25.f + 19.f * float(li)) * s), kSoft, lines[li], text_w + 1.f);
      }
    }
    ImGui::PopStyleColor(3);
  }
  ImGui::EndChild();
  ImGui::EndChild();
}

void AchievementsBrowser::DrawReveal(float s) {
  if (want_reveal_) {
    ImGui::OpenPopup("##reveal");
    want_reveal_ = false;
  }
  reveal_open_ = false;
  ImGui::SetNextWindowSize(ImVec2(460.f * s, 0));
  if (!ImGui::BeginPopupModal("##reveal", nullptr,
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_AlwaysAutoResize))
    return;
  reveal_open_ = true;
  ImGui::TextUnformatted(XenosTr("Reveal this secret achievement?"));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", XenosTr("Its name, description and icon will be shown in every game and in the launcher."));
  if (reveal_failed_) ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.25f, 1.f), "%s", XenosTr("Couldn't save - is the COGBox folder read-only?"));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  bool close = false;
  if (ImGui::Button(XenosTr("Yes, reveal it"), ImVec2(bw, 0))) {
    if (ach::AddRevealed(dir_, reveal_.first, reveal_.second)) {
      REXLOG_INFO("achievements: revealed {:08X}:{}", reveal_.first, reveal_.second);
      close = true;
      stale_->store(true);
    } else {
      REXLOG_WARN("achievements: could not save the reveal of {:08X}:{} in {}", reveal_.first, reveal_.second,
                  rex::path_to_utf8(ach::RevealedPath(dir_)));
      reveal_failed_ = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.10f, 0.08f, 1.0f));
  if (ImGui::Button(XenosTr("No, keep it secret"), ImVec2(bw, 0))) close = true;
  ImGui::PopStyleColor();
  // B / Esc: No (a modal does not close on Cancel by itself)
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
  if (close) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace xenos
