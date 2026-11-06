// CogBox - the ACHIEVEMENTS tab (achievements part 4, docs/achievements/README.md): the
// same achievements as the in-game browser, in the launcher's own look - its headings, buttons, lamps, rows and message
// box. What is shown comes from the shared model (Gears2-Native/src/layer0_xenos/achievements_browser_model.h); the
// popup settings above it are ordinary option rows (gears_settings.cpp, Tab::Achievements).
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "util.h"
#include "achievements_tab_logic.h"

#include <xenos/achievements_browser_model.h>
#include <xenos/achievements_popup_logic.h>

#include "../third_party/stb_image.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace ach = xenos::ach;

// page.cpp's 0.4.6 building blocks, so this tab looks like the others: a section heading, a hover tooltip, a short note
void ui_section(const char *title_en);
void ui_tip(const std::string &text_en);
void ui_note(const std::string &text_en, ImVec4 c = col::dim);

namespace {
std::vector<ach::GameAchievements> g_games;
std::vector<ach::BrowserEntry> g_side;
std::vector<ach::BrowserRow> g_rows;
std::filesystem::path g_dir;
bool g_stale = true, g_open_all = false;
int g_last_frame = -10, g_last_page = -1;
int g_sel_game = -2;   // -1 All, 0-3 a game (kGames index), -2 the page's game
ach::SortOrder g_sort = ach::SortOrder::kUnlockTime;
ach::ShowFilter g_show = ach::ShowFilter::kAll;
std::pair<uint32_t, uint32_t> g_reveal{0, 0};
// icons, (title, id) -> texture (null = could not be loaded); the renderer frees them when it goes
std::map<std::pair<uint32_t, uint32_t>, SDL_Texture *> g_icons;

int local_offset_minutes(int64_t unix_seconds)
{
    const std::time_t t = (std::time_t)unix_seconds;
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return 0;
    return (int)((_mkgmtime(&local) - t) / 60);
}

size_t selected_index()
{
    for (size_t i = 0; i < g_side.size(); i++)
        if (g_sel_game == -1 ? g_side[i].all : (!g_side[i].all && g_side[i].game == g_sel_game)) return i;
    return 0;
}

void rebuild_rows()
{
    g_rows = g_side.empty() ? std::vector<ach::BrowserRow>{} : ach::BuildRows(g_games, g_side[selected_index()], g_sort, g_show, local_offset_minutes);
}

void reload(int page_game)
{
    g_stale = false;
    g_dir = cogbox::cogbox_path(exe_dir());
    g_games = ach::LoadAllGames(g_dir, ach::CurrentRarityProvider());
    const uint32_t page_title = page_game >= 0 && page_game < G_COUNT ? game_def(page_game).titleId : 0;
    g_side = ach::BuildSidebar(g_games, page_title);
    rebuild_rows();
}

SDL_Texture *icon(App &a, const ach::Achievement &x)
{
    const auto key = std::make_pair(x.title_id, x.id);
    auto it = g_icons.find(key);
    if (it != g_icons.end()) return it->second;
    SDL_Texture *t = nullptr;
    std::string data;
    if (!x.icon.empty() && ach::ReadFile(x.icon, &data)) {
        int w = 0, h = 0, n = 0;
        if (unsigned char *px = stbi_load_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &n, 4)) {
            t = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
            if (t) {
                SDL_UpdateTexture(t, nullptr, px, w * 4);
                SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            }
            stbi_image_free(px);
        }
    }
    g_icons[key] = t;
    return t;
}

// one line of text cut to max_w with "..." (the shared FitText), drawn at p
void line(ImDrawList *dl, ImFont *f, float size, ImVec2 p, ImU32 c, const std::string &s, float max_w)
{
    float scale = 1.0f;
    const std::string fit = ach::FitText(s, max_w, [&](const std::string &t) { return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x; }, &scale);
    dl->AddText(f, size * scale, p, c, fit.c_str());
}

float text_w(ImFont *f, float size, const std::string &s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; }

// a small tag box ("Secret", "Gears 3"); returns its width
float tag(ImDrawList *dl, ImVec2 p, const char *s)
{
    const float sz = FS(F_SMALL), w = text_w(F_SMALL, sz, s) + S(10);
    dl->AddRect(p, ImVec2(p.x + w, p.y + sz + S(4)), U(col::faint), S(3));
    dl->AddText(F_SMALL, sz, ImVec2(p.x + S(5), p.y + S(2)), U(col::dim), s);
    return w;
}

// a Sort / Show row, laid out like the option rows above it
template <typename T, size_t N>
bool choice_row(const char *label, const char *id, T &value, const T (&values)[N], const char *(*name)(T), float labelW, float ctrlW)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(ctrlW);
    bool changed = false;
    if (ImGui::BeginCombo(id, tr(name(value)))) {
        for (T v : values) {
            if (ImGui::Selectable(tr(name(v)), v == value) && v != value) { value = v; changed = true; }
            if (v == value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
}  // namespace

void achievements_tab_reload() { g_stale = true; }

void achievements_tab_open_all() { g_open_all = true; g_stale = true; }

void achievements_reveal_answer(App &a, int answer)
{
    if (answer != 1 || !g_reveal.first) return;   // 0: No, keep it secret
    if (ach::AddRevealed(g_dir, g_reveal.first, g_reveal.second)) g_stale = true;
    else set_message(a, tr("Couldn't save - is the COGBox folder read-only?"), true);
    g_reveal = {0, 0};
}

void achievements_tab(App &a, int g, float labelW, float ctrlW, float rowX1)
{
    // read again when the tab has just been shown (it was not drawn last frame), on another game's page, or when asked
    const int frame = ImGui::GetFrameCount();
    if (frame != g_last_frame + 1 || g != g_last_page) {
        g_stale = true;
        if (g != g_last_page || g_open_all) g_sel_game = g_open_all ? -1 : -2;
        g_open_all = false;
    }
    g_last_frame = frame;
    g_last_page = g;
    if (g_stale) reload(g);
    if (g_sel_game == -2 && !g_side.empty()) g_sel_game = g_side[0].all ? -1 : g_side[0].game;

    ui_section("YOUR ACHIEVEMENTS");   // under the ACHIEVEMENT POPUPS group (page.cpp k_groups)

    // the game buttons
    const float bh = T(26);
    for (size_t i = 0; i < g_side.size(); i++) {
        const ach::BrowserEntry &e = g_side[i];
        const char *label = tr(cogbox::button_label(e));
        const ImVec2 sz(text_w(F_BODY, FS(F_BODY), label) + S(28), bh);
        if (i) ImGui::SameLine();
        ImGui::PushID((int)i);
        const bool chosen = i == selected_index();
        const bool hit = chosen ? th_button_red(label, sz, true, F_BODY, false) : th_button(label, sz, true, F_BODY, false);
        if (!e.loaded) ui_tip("Not played yet");
        ImGui::PopID();
        if (hit && !chosen) {
            g_sel_game = e.all ? -1 : e.game;
            rebuild_rows();
        }
    }
    if (g_side.empty()) return;
    const ach::BrowserEntry &sel = g_side[selected_index()];

    // the status line, with a lamp
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        th_lamp(dl, ImVec2(p.x + S(6), p.y + h * 0.55f), S(4.5f), sel.totals.unlocked > 0 ? 1 : 0);
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(18), p.y));
        ImGui::TextUnformatted(sel.loaded ? cogbox::status_line(sel.totals, tr("achievements")).c_str() : tr("Not played yet"));
    }

    // Sort and Show
    static const ach::SortOrder kSorts[] = { ach::SortOrder::kUnlockTime, ach::SortOrder::kRarity, ach::SortOrder::kGameOrder,
                                             ach::SortOrder::kGamerscore, ach::SortOrder::kName };
    static const ach::ShowFilter kShows[] = { ach::ShowFilter::kAll, ach::ShowFilter::kUnlocked, ach::ShowFilter::kLocked };
    if (choice_row("Sort", "##achsort", g_sort, kSorts, ach::SortName, labelW, ctrlW)) rebuild_rows();
    if (choice_row("Show", "##achshow", g_show, kShows, ach::ShowName, labelW, ctrlW)) rebuild_rows();
    ImGui::Dummy(ImVec2(0, S(4)));

    // the list
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_rows.empty()) {
        // the model's "Start <game> once..." has the game's name in it: one sentence per case for the translations
        ui_note(!sel.loaded && !sel.all ? std::string("Start this game once from COGBox to load its achievements.") : ach::EmptyMessage(sel, g_show));
        return;
    }
    const float x0 = ImGui::GetCursorScreenPos().x, rw = rowX1 - x0, rh = T(46), ic = T(38), pad = S(6);
    const float fb = FS(F_BODY), fsm = FS(F_SMALL);
    ImGuiListClipper clip;
    clip.Begin((int)g_rows.size(), rh + ImGui::GetStyle().ItemSpacing.y);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const ach::BrowserRow &r = g_rows[(size_t)i];
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##achrow", false, 0, ImVec2(rw, rh)) && r.hidden_secret && r.a) {
                g_reveal = { r.a->title_id, r.a->id };
                ask(a, "achreveal", g, tr("Reveal this secret achievement?"),
                    tr("Its name, description and icon will be shown in every game and in the launcher."),
                    { tr("No, keep it secret"), tr("Yes, reveal it") });
            }
            ImGui::PopID();
            // icon
            const ImVec2 ia(p.x + pad, p.y + (rh - ic) * 0.5f), ib(ia.x + ic, ia.y + ic);
            SDL_Texture *t = r.hidden_secret || !r.a ? nullptr : icon(a, *r.a);
            if (t) {
                dl->AddImage((ImTextureID)(intptr_t)t, ia, ib, ImVec2(0, 0), ImVec2(1, 1),
                             r.locked ? IM_COL32(120, 120, 120, 255) : IM_COL32_WHITE);
            } else {
                dl->AddRectFilled(ia, ib, U(col::steel), S(3));
                dl->AddText(F_BOLD, fb, ImVec2(ia.x + (ic - text_w(F_BOLD, fb, "?")) * 0.5f, ia.y + (ic - fb) * 0.5f), U(col::faint), "?");
            }
            // right: GP, status, rarity
            const std::string status = tr(r.status);   // "Unlocked <date>" / "Locked" (the date stays as the model wrote it)
            const float right_w = std::max({ text_w(F_BOLD, fb, r.gp), text_w(F_SMALL, fsm, status), text_w(F_SMALL, fsm, r.rarity) });
            const float rx = p.x + rw - pad;
            dl->AddText(F_BOLD, fb, ImVec2(rx - text_w(F_BOLD, fb, r.gp), p.y + S(3)), U(col::text), r.gp.c_str());
            dl->AddText(F_SMALL, fsm, ImVec2(rx - text_w(F_SMALL, fsm, status), p.y + S(3) + fb), U(col::dim), status.c_str());
            if (!r.rarity.empty())
                dl->AddText(F_SMALL, fsm, ImVec2(rx - right_w - S(10) - text_w(F_SMALL, fsm, r.rarity), p.y + S(3) + fb), U(col::amber), r.rarity.c_str());
            // name (+ tags), text
            const float tx = ib.x + S(10), tw = std::max(S(20), rx - right_w - S(14) - tx);
            float tags = 0;
            const char *secret = tr("Secret");
            const std::string name = r.hidden_secret ? tr(r.name) : r.name;   // "Secret achievement" is ours, a real name the game's
            if (r.secret_tag) tags += text_w(F_SMALL, fsm, secret) + S(16);
            if (!r.game_label.empty()) tags += text_w(F_SMALL, fsm, r.game_label) + S(16);
            const float nw = std::max(S(20), tw - tags);
            line(dl, F_BODY, fb, ImVec2(tx, p.y + S(3)), U(r.hidden_secret || r.locked ? col::dim : col::text), name, nw);
            float x = tx + std::min(nw, text_w(F_BODY, fb, name)) + S(6);
            if (r.secret_tag) x += tag(dl, ImVec2(x, p.y + S(5)), secret) + S(6);
            if (!r.game_label.empty()) tag(dl, ImVec2(x, p.y + S(5)), r.game_label.c_str());
            line(dl, F_SMALL, fsm, ImVec2(tx, p.y + S(3) + fb), U(col::dim), r.text, tw);
        }
    }
}
