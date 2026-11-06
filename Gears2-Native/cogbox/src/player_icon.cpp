// CogBox 0.4.6 (Shagg 6 Nov): the player icon - a picture from the player's games in place of the letter on the
// front screen's player plate. CogBox.ini [Profile] Icon = a path under the COGBox folder ("" or missing: the letter).
// Where the pictures come from, per added game:
//   1. its default.xex's XDBF images (xdbf_icons.cpp): the gamer pictures (character faces), the title image and the
//      achievement icons - copied once into <COGBox>\icons\<TitleID>\<id>.png (xdbf.ok marks a game as done);
//   2. when the XEX could not be read: the achievement icons the game itself wrote, achievements\<TitleID>\icons\*.png.
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "util.h"
#include "achievements_tab_logic.h"
#include "xdbf_icons.h"

#include "../third_party/stb_image.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Pic { std::string rel; uint64_t order; };
struct Group { int game; std::vector<Pic> pics; };

std::map<std::string, SDL_Texture *> g_tex;   // rel path -> texture (null: could not be loaded); freed with the renderer
std::vector<Group> g_groups;
std::mutex g_mu;
std::atomic<int> g_scan{0};   // 0 not started, 1 running, 2 done

std::string tid_hex(uint32_t t) { char b[16]; std::snprintf(b, sizeof b, "%08X", t); return b; }

bool safe_rel(const std::string &rel)
{
    return !rel.empty() && rel.find("..") == std::string::npos && rel.find(':') == std::string::npos && rel[0] != '/' && rel[0] != '\\';
}

std::string full_path(const std::string &rel) { return exe_dir() + "\\" + rel; }

// the PNGs in a folder, as paths under the COGBox folder; key(name) orders them (gamer pictures first)
void list_pngs(const std::string &rel_dir, std::vector<Pic> &out, bool xdbf_names)
{
    std::error_code ec;
    for (fs::directory_iterator it(cogbox::cogbox_path(full_path(rel_dir)), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        std::string ext = p.extension().string();
        for (char &c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".png") continue;
        const std::string stem = p.stem().string();
        uint64_t order = 0;
        if (xdbf_names) {
            const uint64_t id = std::strtoull(stem.c_str(), nullptr, 16);
            order = id >= 0x20000 ? id - 0x20000 : id == 0x8000 ? 0x100000 : 0x200000 + id;   // faces, title, achievements
        } else {
            order = std::strtoull(stem.c_str(), nullptr, 10);
        }
        out.push_back({ rel_dir + "/" + p.filename().string(), order });
    }
    std::sort(out.begin(), out.end(), [](const Pic &a, const Pic &b) { return a.order != b.order ? a.order < b.order : a.rel < b.rel; });
}

// one game's default.xex -> icons\<TitleID>\ (once: xdbf.ok)
void extract_xdbf(const std::string &dataDir, uint32_t tid)
{
    const std::string dir = "icons/" + tid_hex(tid);
    std::error_code ec;
    if (fs::exists(cogbox::cogbox_path(full_path(dir + "/xdbf.ok")), ec)) return;
    if (dataDir.empty()) return;
    std::vector<xdbf::Image> imgs;
    uint32_t got = 0;
    std::string err;
    if (!xdbf::read_xex_images(cogbox::cogbox_path(dataDir + "\\default.xex"), imgs, &got, &err) || (got && got != tid)) {
        // review: a copy whose XEX can't be read is marked too, so it is not decrypted again on every ICON press
        if (fs::exists(cogbox::cogbox_path(dataDir + "\\default.xex"), ec)) {
            fs::create_directories(cogbox::cogbox_path(full_path(dir)), ec);
            write_file(full_path(dir + "/xdbf.ok"), "0 (" + err + ")\n");
        }
        return;
    }
    fs::create_directories(cogbox::cogbox_path(full_path(dir)), ec);
    int n = 0;
    for (const xdbf::Image &im : imgs) {
        if (im.id >= 0x10000 && im.id < 0x20000) continue;   // the small (32x32) copies of the gamer pictures
        char name[32];
        std::snprintf(name, sizeof name, "/%llX.png", (unsigned long long)im.id);
        if (write_file(full_path(dir + name), im.png)) n++;
    }
    if (n) write_file(full_path(dir + "/xdbf.ok"), std::to_string(n) + "\n");
}

void scan(std::vector<std::pair<int, std::string>> games)
{
    std::vector<Group> groups;
    for (const auto &g : games) {
        const uint32_t tid = game_def(g.first).titleId;
        extract_xdbf(g.second, tid);
        Group grp{ g.first, {} };
        list_pngs("icons/" + tid_hex(tid), grp.pics, true);
        if (grp.pics.empty()) list_pngs("achievements/" + tid_hex(tid) + "/icons", grp.pics, false);
        if (!grp.pics.empty()) groups.push_back(std::move(grp));
    }
    std::lock_guard<std::mutex> l(g_mu);
    g_groups = std::move(groups);
    g_scan = 2;
}

void start_scan(App &a)
{
    if (g_scan != 0) return;
    std::vector<std::pair<int, std::string>> games;
    for (int g = 0; g < G_COUNT; g++) {
        const int gi = game_at_slot(g);   // release order
        if (gi < 0 || gi >= G_COUNT) continue;
        if (game_in_library(a.st[gi])) games.push_back({ gi, a.st[gi].dataDir });
    }
    g_scan = 1;
    std::thread(scan, std::move(games)).detach();
}

}  // namespace

// look again next time the grid opens (a game added since; the XEX of a done game is not read again: xdbf.ok)
void player_icon_rescan() { if (g_scan == 2) g_scan = 0; }

// the texture of a picture under the COGBox folder (null: missing or not a picture - the plate then shows the letter)
SDL_Texture *player_icon_texture(App &a, const std::string &rel)
{
    if (!safe_rel(rel)) return nullptr;
    auto it = g_tex.find(rel);
    if (it != g_tex.end()) return it->second;
    SDL_Texture *t = nullptr;
    std::string data;
    if (read_file(full_path(rel), data, 4u << 20)) {
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
    g_tex[rel] = t;
    return t;
}

// The ICON view of the player box: "Letter" and every game's pictures in a grid. D-pad / mouse to a picture, A or a
// click picks it (saved at once). Returns true when one was picked.
bool player_icon_picker(App &a, const std::string &letter)
{
    start_scan(a);
    ImGui::PushFont(F_BOLD, FS(F_BOLD)); ImGui::TextUnformatted(tr("PLAYER ICON")); ImGui::PopFont();
    ImGui::TextColored(col::dim, "%s", tr("A picture from your games in place of your letter."));
    const std::string cur = a.ini.get("Profile", "Icon");
    const float k = T(48), gap = S(6);
    const int per = 8;
    const float gridW = per * k + (per - 1) * gap + S(16);
    bool picked = false;
    ImGui::BeginChild("##icongrid", ImVec2(gridW, T(330)), ImGuiChildFlags_NavFlattened);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const bool appearing = ImGui::IsWindowAppearing();
    auto frame = [&](ImVec2 p, bool chosen) {
        const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (chosen || lit) dl->AddRect(ImVec2(p.x - S(2), p.y - S(2)), ImVec2(p.x + k + S(2), p.y + k + S(2)),
                                       U(lit ? col::redHi : col::red), S(5), 0, S(chosen ? 3.0f : 2.0f));
    };
    // the letter (the default)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool first = appearing && (cur.empty() || g_scan != 2);   // the pad starts here (or on the chosen picture)
        if (first) ImGui::SetKeyboardFocusHere();
        if (ImGui::InvisibleButton("##iconletter", ImVec2(k, k))) { a.ini.set("Profile", "Icon", ""); picked = true; }
        if (first) ImGui::SetItemDefaultFocus();
        const ImVec2 c(p.x + k * 0.5f, p.y + k * 0.5f);
        dl->AddCircleFilled(c, k * 0.42f, U(ImVec4(0.05f, 0.05f, 0.06f, 1)), 32);
        dl->AddCircle(c, k * 0.42f, U(col::red), 32, S(2.2f));
        const ImVec2 ts = F_HEAD->CalcTextSizeA(FS(F_HEAD), FLT_MAX, 0, letter.c_str());
        dl->AddText(F_HEAD, FS(F_HEAD), ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), U(col::text), letter.c_str());
        frame(p, cur.empty());
        ImGui::SameLine(0, S(10));
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (k - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::TextUnformatted(tr("Letter"));
    }
    if (g_scan != 2) {
        ImGui::TextColored(col::dim, "%s", tr("Reading your games' pictures..."));
    } else {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_groups.empty())
            ImGui::TextColored(col::dim, "%s", tr("No pictures found. Add a game, or start one once from COGBox."));
        for (const Group &grp : g_groups) {
            ImGui::Spacing();
            ImGui::PushFont(F_SMALL, FS(F_SMALL));
            ImGui::TextColored(col::red, "%s", game_def(grp.game).name);
            ImGui::PopFont();
            for (size_t i = 0; i < grp.pics.size(); i++) {
                const Pic &pic = grp.pics[i];
                if (i % per) ImGui::SameLine(0, gap);
                ImGui::PushID(pic.rel.c_str());
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool chosen = pic.rel == cur;
                if (chosen && appearing) ImGui::SetKeyboardFocusHere();
                if (ImGui::InvisibleButton("##pic", ImVec2(k, k))) { a.ini.set("Profile", "Icon", pic.rel); picked = true; }
                if (chosen && appearing) ImGui::SetItemDefaultFocus();
                // only the tiles on screen load their picture
                if (ImGui::IsItemVisible()) {
                    if (SDL_Texture *t = player_icon_texture(a, pic.rel))
                        dl->AddImageRounded((ImTextureID)(intptr_t)t, p, ImVec2(p.x + k, p.y + k), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
                    else
                        dl->AddRectFilled(p, ImVec2(p.x + k, p.y + k), U(col::steel), S(4));
                }
                frame(p, chosen);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    if (picked) a.ini.save();
    return picked;
}
