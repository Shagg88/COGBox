// COGBox - the FABRICATOR screen (Task 12): the player-facing side of the Fabricator, COGBox's mod store. Three tabs
// (LB / RB switch them): LIBRARY (the player's store mods, installed or not, and the local mods in each game's mods
// folder), STORE (search, filters, sorts, a grid of cards, a mod's page) and CREATOR (the player's uploads and the
// upload form). Signed out (online play off, or no online account yet) the Library shows the local mods only, the Store
// can be browsed but not got from, and the Creator tab is off. Every network call and transfer runs on the workers in
// fabricator_state.cpp; this file only draws what they last answered. Esc / B: back (a mod's page to the grid, else
// to the front screen).
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "profiles.h"
#include "social_net.h"   // kCogboxServer
#include "fabricator_state.h"
#include "fabricator_install.h"
#include "imgui_internal.h"   // ImGui::FindRenderedTextEnd (a label's shown part)

#include "../third_party/nlohmann/json.hpp"
#include "../third_party/stb_image.h"   // stbi_info_from_memory: a screenshot's size before it is sent

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <set>

using json = nlohmann::json;

namespace {

// ---- words and colours ---------------------------------------------------------------------------------------------
const char *const kGameKeys[] = { "gears1", "gears2", "gears3", "judgment" };
const char *const kGameNames[] = { "Gears 1", "Gears 2", "Gears 3", "Judgment" };
const char *const kTypeKeys[] = { "texture_pack", "campaign", "gameplay", "visuals", "audio", "other" };
const char *const kTypeNames[] = { "Texture pack", "Campaign", "Gameplay", "Visuals", "Audio", "Other" };
const char *const kSortKeys[] = { "popular", "rating", "new", "updated", "name" };
const char *const kSortNames[] = { "Popular", "Rating", "New", "Updated", "Name" };
const char *const kSideKeys[] = { "everyone", "host", "client" };
const char *const kSideNames[] = { "Everyone (every player needs it)", "Host only", "Client only" };
const char *const kReasonKeys[] = { "malware", "stolen", "broken", "offensive", "other" };
const char *const kReasonNames[] = { "Malware or a harmful file", "Stolen work", "Broken - doesn't work", "Offensive", "Something else" };
const char *const kSignInTip = "Sign in on the SOCIAL tab to get mods";
const char *const kOnlineOff = "Turn on Online play on the SOCIAL tab to use the store.";   // [Online] Enabled = 0: no network at all
bool g_refreshLocal = false;   // read the mods folders again at the start of the next frame (never while rows point into them)
const int kGames = 4, kTypes = 6, kSorts = 5, kSides = 3, kReasons = 5;

ImVec4 hexc(unsigned v, float a = 1) { return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }

int key_index(const char *const *keys, int n, const std::string &k)
{
    for (int i = 0; i < n; i++) if (k == keys[i]) return i;
    return -1;
}
std::string game_name(const std::string &key)
{
    const int i = key_index(kGameKeys, kGames, key);
    return i >= 0 ? kGameNames[i] : key;
}
std::string type_name(const std::string &key)
{
    const int i = key_index(kTypeKeys, kTypes, key);
    return i >= 0 ? kTypeNames[i] : key;
}
int game_of_key(const std::string &k)   // GameId, -1 unknown
{
    for (int g = 0; g < G_COUNT; g++) if (k == game_def(g).key) return g;
    return -1;
}
std::string lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
bool ends_with(const std::string &s, const char *e)
{
    const size_t n = strlen(e);
    return s.size() >= n && lower(s.substr(s.size() - n)) == e;
}
std::string trim(const std::string &s)
{
    size_t x = s.find_first_not_of(" \t\r\n"), y = s.find_last_not_of(" \t\r\n");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}
size_t chars(const std::string &s)   // characters (UTF-8 code points), as the server's limits count them
{
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
std::string base_name(const std::string &p)
{
    const size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}
std::string date_text(int64_t t)   // the server's times are milliseconds
{
    if (t <= 0) return "";
    time_t s = (time_t)(t > 100000000000LL ? t / 1000 : t);
    struct tm lt;
    if (localtime_s(&lt, &s) != 0) return "";
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}
std::string count_text(int n)
{
    char b[32];
    if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(b, sizeof b, "%dk", n / 1000);
    else if (n >= 1000) snprintf(b, sizeof b, "%.1fk", n / 1e3);
    else snprintf(b, sizeof b, "%d", n);
    return b;
}
// the text cut with "..." to fit w
std::string fit(ImFont *f, float fs, const std::string &t, float w)
{
    if (f->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x <= w) return t;
    std::string s = t;
    while (!s.empty()) {
        do s.pop_back(); while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80);
        if (!s.empty()) s.pop_back();   // the lead byte of a cut character, or one more plain one
        if (f->CalcTextSizeA(fs, FLT_MAX, 0, (s + "...").c_str()).x <= w) return s + "...";
    }
    return "";
}

// ---- small widgets -------------------------------------------------------------------------------------------------
int str_cb(ImGuiInputTextCallbackData *d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string *s = (std::string *)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = &(*s)[0];
    }
    return 0;
}
bool input(const char *id, std::string &s, const char *hint, float w, ImGuiInputTextFlags f = 0)
{
    ImGui::SetNextItemWidth(w);
    if (s.capacity() < 16) s.reserve(16);
    return ImGui::InputTextWithHint(id, tr(hint), &s[0], s.capacity() + 1, f | ImGuiInputTextFlags_CallbackResize, str_cb, &s);
}
bool input_multi(const char *id, std::string &s, ImVec2 size)
{
    if (s.capacity() < 16) s.reserve(16);
    return ImGui::InputTextMultiline(id, &s[0], s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, str_cb, &s);
}
void tip(const std::string &text_en)   // a tooltip for the last item (also when it is disabled)
{
    if (text_en.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(S(460), FLT_MAX));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(440));
    ImGui::TextUnformatted(tr(text_en).c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
float btn_w(const char *label_en)   // th_button draws its label in the full F_BODY size; "##..." is not shown
{
    const char *l = tr(label_en);
    return std::max(T(70), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, l, ImGui::FindRenderedTextEnd(l)).x + S(26));
}
bool btn(const char *label_en, bool enabled = true, float w = 0)
{
    return th_button(label_en, ImVec2(w > 0 ? w : btn_w(label_en), T(26)), enabled, F_BODY);
}
void note(const std::string &text_en, ImVec4 c = col::dim)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(tr(text_en).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void heading(const char *text_en)
{
    ImGui::Dummy(ImVec2(0, S(2)));
    th_heading(text_en, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - S(4));
    ImGui::Dummy(ImVec2(0, S(2)));
}
// A selectable line whose text is drawn as given: a mod's name may hold "##", which in an ImGui label would hide the
// rest, so the item's label is only "##<id>" and the text is drawn over it.
bool text_selectable(const std::string &text, const std::string &id, bool selected, ImVec4 c, float w = 0)
{
    ImGui::PushID(id.c_str());
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const bool hit = ImGui::Selectable("##sel", selected, 0, ImVec2(w > 0 ? w : ts.x, ts.y));
    ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), U(c), text.c_str());
    ImGui::PopID();
    return hit;
}
bool link(const std::string &text, const char *id)   // amber text that opens something (reachable with the pad)
{
    const bool hit = text_selectable(text, id, false, col::amber);
    if (ImGui::IsItemHovered()) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, U(col::amber, 0.8f), S(1));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return hit;
}
// text cut to at most max UTF-16 units (how the server counts), never inside a character
void cut_utf16(std::string &s, size_t max)
{
    size_t units = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        const size_t u = len == 4 ? 2 : 1;
        if (units + u > max || i + len > s.size()) break;
        units += u;
        i += len;
    }
    s.resize(i);
}

// a state word in a rounded box; kind: 0 grey, 1 green (installed and on), 2 amber (update), 3 red (taken down,
// removed), 4 blue (local). Returns its width.
float pill_w(const std::string &t) { return F_SMALL->CalcTextSizeA(FS(F_SMALL) * 0.85f, FLT_MAX, 0, tr(t).c_str()).x + S(16); }
float pill(ImDrawList *dl, ImVec2 p, const std::string &t_en, int kind)
{
    static const unsigned bg[] = { 0x2a2d32, 0x2c3d24, 0x3f3018, 0x3d1f22, 0x1f2d3d }, fg[] = { 0x968f88, 0x7cb262, 0xe2a034, 0xe0686c, 0x7fa8d0 };
    const std::string t = tr(t_en);
    const float fs = FS(F_SMALL) * 0.85f;
    const ImVec2 ts = F_SMALL->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str());
    const ImVec2 b(p.x + ts.x + S(16), p.y + ts.y + S(6));
    dl->AddRectFilled(p, b, U(hexc(bg[kind])), (b.y - p.y) * 0.5f);
    dl->AddText(F_SMALL, fs, ImVec2(p.x + S(8), p.y + S(3)), U(hexc(fg[kind])), t.c_str());
    return b.x - p.x;
}

void star(ImDrawList *dl, ImVec2 c, float r, ImU32 colr, bool filled)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        const float ang = -1.5707963f + i * 0.6283185f, rr = (i & 1) ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr);
    }
    if (filled) dl->AddConcavePolyFilled(pts, 10, colr);
    else dl->AddPolyline(pts, 10, colr, ImDrawFlags_Closed, S(1.2f));
}
// five stars, value 0-5 (filled from half a star), drawn at p; returns the width
float stars(ImDrawList *dl, ImVec2 p, float r, double value)
{
    for (int i = 0; i < 5; i++) star(dl, ImVec2(p.x + r + i * r * 2.3f, p.y + r), r, U(col::amber), value >= i + 0.5);
    return r * 2.3f * 5;
}

// a picture filling p0-p1 (cropped to keep its shape), or a dark placeholder while it loads
// (local: a file the player chose for an upload - only the upload form passes true; store data is a store image path)
void picture(App &a, ImDrawList *dl, const std::string &key, ImVec2 p0, ImVec2 p1, float round = 0, bool local = false)
{
    int w = 0, h = 0;
    SDL_Texture *t = key.empty() ? nullptr : local ? fab_local_image(a, key, &w, &h) : fab_image(a, key, &w, &h);
    if (!t || w <= 0 || h <= 0) {
        dl->AddRectFilledMultiColor(p0, p1, U(hexc(0x3a1d1f)), U(hexc(0x2a2024)), U(hexc(0x1d2226)), U(hexc(0x241d20)));
        th_cog(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), std::min(p1.x - p0.x, p1.y - p0.y) * 0.2f, 8, 0,
               U(col::faint, 0.35f), U(hexc(0x1d2226)));
        return;
    }
    const float ba = (p1.x - p0.x) / std::max(1.0f, p1.y - p0.y), ia = (float)w / h;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ia > ba) { const float k = ba / ia; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
    else { const float k = ia / ba; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
    if (round > 0) dl->AddImageRounded((ImTextureID)(intptr_t)t, p0, p1, uv0, uv1, IM_COL32_WHITE, round);
    else dl->AddImage((ImTextureID)(intptr_t)t, p0, p1, uv0, uv1);
}

// ---- the mods folders as the Library sees them (read again every 3 s, and after each install) -----------------------
struct LocalMod {
    int g = -1;
    ModItem m;
    std::optional<FabInstallRecord> rec;
};
std::vector<LocalMod> g_local;
double g_localAt = -100;
uint64_t g_localGen = ~0ull;
void local_refresh(App &a, bool force)
{
    const uint64_t gen = fab_local_changes();
    const double now = ImGui::GetTime();
    if (!force && gen == g_localGen && now - g_localAt < 3) return;
    g_localGen = gen;
    g_localAt = now;
    g_local.clear();
    for (int g = 0; g < G_COUNT; g++) {
        if (!dir_exists(mods_dir(g))) continue;
        for (const ModItem &m : mods_list(a, g)) {
            LocalMod lm;
            lm.g = g;
            lm.m = m;
            if (m.fab_mod) lm.rec = fab_read_record(m.path);
            g_local.push_back(lm);
        }
    }
}
const LocalMod *installed(int g, int64_t id)
{
    for (const LocalMod &l : g_local) if (l.g == g && l.m.fab_mod == id) return &l;
    return nullptr;
}

std::shared_ptr<FabTransfer> download_of(const FabView &v, int64_t id)   // the newest Get of this mod
{
    for (auto it = v.transfers.rbegin(); it != v.transfers.rend(); ++it)
        if ((*it)->kind == FabTransfer::Download && (*it)->mod_id == id) return *it;
    return nullptr;
}
bool running(const std::shared_ptr<FabTransfer> &t)
{
    if (!t) return false;
    const int p = t->phase;
    return p != FabTransfer::Done && p != FabTransfer::Failed && p != FabTransfer::Cancelled;
}
std::string transfer_text(App &a, const FabTransfer &t)
{
    const int64_t done = t.progress.done, total = t.progress.total;
    const int pct = total > 0 ? (int)std::min<int64_t>(100, done * 100 / total) : 0;
    switch (t.phase.load()) {
    case FabTransfer::Queued: return "Waiting...";
    case FabTransfer::Address: return "Starting the download...";
    case FabTransfer::Downloading: return "Downloading " + std::to_string(pct) + "%  (" + human_bytes((uint64_t)done) + " of " + human_bytes((uint64_t)total) + ")";
    case FabTransfer::Downloaded: return a.run.proc ? "Downloaded - installs when the game closes" : "Installing...";
    case FabTransfer::Installing: return "Installing...";
    case FabTransfer::Unpacked: return a.run.proc ? "Unpacked - installs when the game closes" : "Installing...";
    default: return "";
    }
}
// a running Get's line and its Cancel button; true when one is running (the caller then shows nothing else)
bool transfer_line(App &a, const std::shared_ptr<FabTransfer> &t)
{
    if (!running(t)) return false;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, col::amber);
    ImGui::TextUnformatted(tr(transfer_text(a, *t)).c_str());
    ImGui::PopStyleColor();
    if (t->phase != FabTransfer::Installing) {
        ImGui::SameLine();
        if (btn("Cancel")) fab_cancel(t);
    }
    return true;
}

FabLibraryItem item_of(const FabCard &c)
{
    FabLibraryItem it;
    it.id = c.id; it.name = c.name; it.game = c.game; it.type = c.type; it.state = "live"; it.version = c.version;
    it.size = c.size; it.screen = c.screen;
    return it;
}

// ---- the upload form -----------------------------------------------------------------------------------------------
struct Form {
    std::string source;
    bool folder = false;
    std::string name, version, shortDesc, longDesc, tags, needs, notes;
    int game = 2, type = 5, side = 0;
    std::vector<std::string> screens;
    int64_t modId = 0;   // a new version of this mod (its game is fixed)
    std::vector<std::string> problems;   // what the local look found before sending
};
Form g_form;
int64_t g_creatorSel = 0;   // 0: + New upload, else one of the creator's mods

void form_source(App &a, const std::string &path)
{
    g_form.source = path;
    g_form.folder = dir_exists(path);
    g_form.problems.clear();
    const FabModIni ini = fab_read_mod_ini(path, g_form.folder);
    if (!ini.found) { set_message(a, "No mod.ini in it - fill in the details yourself.", false); return; }
    if (!g_form.modId && !ini.name.empty()) g_form.name = ini.name;
    if (!g_form.modId) { const int gi = key_index(kGameKeys, kGames, ini.game.empty() ? "gears3" : ini.game); if (gi >= 0) g_form.game = gi; }
    if (!ini.version.empty()) g_form.version = ini.version;
    if (g_form.shortDesc.empty() && !ini.description.empty()) g_form.shortDesc = ini.description.substr(0, 120);
    { const int si = key_index(kSideKeys, kSides, ini.side == "server" ? "host" : ini.side); if (si >= 0) g_form.side = si; }
    { const int ti = key_index(kTypeKeys, kTypes, ini.type == "texture" || ini.type == "textures" ? "texture_pack" : ini.type); if (ti >= 0) g_form.type = ti; }
}
bool add_screen(App &a, const std::string &path)
{
    if (std::find(g_form.screens.begin(), g_form.screens.end(), path) != g_form.screens.end()) return false;
    if (g_form.screens.size() >= 6) { set_message(a, "Up to 6 screenshots.", true); return false; }
    g_form.screens.push_back(path);
    return true;
}
void form_reset(int64_t modId, const std::string &name, const std::string &game)
{
    g_form = Form();
    g_form.modId = modId;
    g_form.name = name;
    const int gi = key_index(kGameKeys, kGames, game);
    if (gi >= 0) g_form.game = gi;
}
std::vector<std::string> split_tags(const std::string &s)
{
    std::vector<std::string> v;
    size_t at = 0;
    while (at <= s.size()) {
        size_t c = s.find(',', at);
        if (c == std::string::npos) c = s.size();
        const std::string t = trim(s.substr(at, c - at));
        if (!t.empty() && std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
        at = c + 1;
    }
    return v;
}
// The same rules as the server's checkText and checkScreenshot, before anything is sent
std::vector<std::string> form_problems()
{
    std::vector<std::string> p;
    const Form &f = g_form;
    if (f.source.empty()) p.push_back("Choose or drop your mod's folder or .zip.");
    else if (!f.folder && !ends_with(f.source, ".zip")) p.push_back("The mod must be a folder or a .zip.");
    else if (!f.folder && !file_exists(f.source)) p.push_back("The .zip isn't there any more.");
    const size_t n = chars(trim(f.name));
    if (n < 3 || n > 60) p.push_back("The name must be 3 to 60 characters.");
    if (chars(f.version) > 24) p.push_back("The version is over 24 characters.");
    if (chars(f.shortDesc) > 120) p.push_back("The short description is over 120 characters.");
    if (chars(f.longDesc) > 8000) p.push_back("The description is over 8000 characters.");
    if (chars(f.notes) > 2000) p.push_back("\"What's new\" is over 2000 characters.");
    const std::vector<std::string> tags = split_tags(f.tags);
    if (tags.size() > 8) p.push_back("Up to 8 tags.");
    for (const std::string &t : tags) if (chars(t) > 24) { p.push_back("Tag \"" + t + "\" is over 24 characters."); break; }
    for (const std::string &t : split_tags(f.needs))
        if (t.find_first_not_of("0123456789") != std::string::npos || t.size() > 9) { p.push_back("\"Needs other mods\" takes store numbers (like 12, 40)."); break; }
    if (f.screens.empty()) p.push_back("Add at least one screenshot (PNG or JPEG).");
    for (const std::string &s : f.screens) {
        const std::string nm = base_name(s);
        if (!ends_with(s, ".png") && !ends_with(s, ".jpg") && !ends_with(s, ".jpeg")) { p.push_back(nm + ": a screenshot must be PNG or JPEG."); continue; }
        std::string data;
        if (!read_file(s, data, (4u << 20) + 1)) { p.push_back(nm + ": can't be read."); continue; }
        if (data.size() > (4u << 20)) { p.push_back(nm + ": over 4 MB."); continue; }
        int w = 0, h = 0, c = 0;
        if (!stbi_info_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &c)) p.push_back(nm + ": not a picture COGBox can read.");
        else if (w > 3840 || h > 2160) p.push_back(nm + ": over 3840 x 2160.");
    }
    return p;
}
std::string form_meta()
{
    const Form &f = g_form;
    json m = { { "name", trim(f.name) }, { "game", kGameKeys[f.game] }, { "type", kTypeKeys[f.type] }, { "version", trim(f.version) },
               { "short_desc", f.shortDesc }, { "long_desc", f.longDesc }, { "tags", split_tags(f.tags) }, { "side", kSideKeys[f.side] } };
    json req = json::array();
    for (const std::string &t : split_tags(f.needs)) req.push_back(atoll(t.c_str()));
    m["requires"] = req;
    if (!f.notes.empty()) m["notes"] = f.notes;
    return m.dump();
}

// ---- popups (opened by a flag: the tabs draw inside child windows, the popups at the screen's own level) -----------
int64_t g_reportMod = 0;
bool g_reportOpen = false;
int g_reportReason = 0;
std::string g_reportText;
int64_t g_removeMod = 0;
std::string g_removeName;
bool g_removeOpen = false;

// ---- the LIBRARY tab -----------------------------------------------------------------------------------------------
int g_libGame = -1;     // -1 all games, else an index into kGameKeys
int g_libState = 0;     // 0 all, 1 installed, 2 not installed, 3 updates

struct LibRow {
    bool local = false;
    FabLibraryItem item;
    bool inLibrary = false;
    int g = -1;
    const LocalMod *inst = nullptr;
};
enum { P_ON, P_OFF, P_NOT, P_UPDATE, P_DOWN, P_REMOVED, P_LOCAL };
int row_state(const LibRow &r)
{
    if (r.local) return P_LOCAL;
    if (r.item.state == "removed") return P_REMOVED;
    if (r.item.state == "taken_down") return P_DOWN;
    if (r.inst && r.inLibrary && r.item.version_id && r.inst->rec && r.inst->rec->version != r.item.version_id && r.item.state == "live") return P_UPDATE;
    if (r.inst) return r.inst->m.enabled ? P_ON : P_OFF;
    return P_NOT;
}
const char *const kPillText[] = { "Installed and on", "Installed and off", "Not installed", "Update available", "Taken down", "Removed", "Local" };
const int kPillKind[] = { 1, 0, 0, 2, 3, 3, 4 };

void open_page(App &a, int64_t id)
{
    a.fabTab = 1;
    fab_open_page(id);
}

void library_row(App &a, const FabView &v, const LibRow &r, int idx)
{
    ImGui::PushID(idx);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float rw = ImGui::GetContentRegionAvail().x, rh = T(58);
    const ImVec2 r1(r0.x + rw, r0.y + rh);
    dl->AddRectFilled(r0, r1, U(hexc(0x16181b), 0.85f), S(4));
    dl->AddRect(r0, r1, U(hexc(0x2f3338)), S(4));
    const float th = rh - S(12), tw = th * 16 / 9;
    picture(a, dl, r.local ? "" : r.item.screen, ImVec2(r0.x + S(6), r0.y + S(6)), ImVec2(r0.x + S(6) + tw, r0.y + S(6) + th), S(3));
    const int st = row_state(r);
    const std::shared_ptr<FabTransfer> t = r.local ? nullptr : download_of(v, r.item.id);
    const bool busy = running(t);
    const bool gameRuns = a.run.proc != nullptr;

    // the buttons, right to left
    std::vector<const char *> btns;
    if (!busy) {
        if (!r.local) btns.push_back("Page");
        if (r.inst && !r.local) btns.insert(btns.begin(), "Uninstall");
        if (r.inst) btns.insert(btns.begin(), r.inst->m.enabled ? "Turn off" : "Turn on");
        if (st == P_UPDATE) btns.insert(btns.begin(), "Update");
        if (st == P_NOT && r.item.state == "live") btns.insert(btns.begin(), "Install");
    } else btns.push_back("Cancel");
    float bx = r1.x - S(8);
    std::vector<float> xs(btns.size());
    for (size_t i = btns.size(); i-- > 0;) { bx -= btn_w(btns[i]); xs[i] = bx; bx -= S(6); }
    const float pw = pill_w(kPillText[st]);
    const float pillX = bx - S(6) - pw;
    pill(dl, ImVec2(pillX, r0.y + (rh - FS(F_SMALL) * 0.85f - S(6)) * 0.5f), kPillText[st], kPillKind[st]);

    // the name and its line
    const float tx = r0.x + S(6) + tw + S(12), textW = pillX - S(10) - tx;
    const std::string name = r.local ? r.inst->m.name : r.item.name;
    dl->AddText(F_BOLD, FS(F_BOLD) * 0.92f, ImVec2(tx, r0.y + S(8)), U(col::text), fit(F_BOLD, FS(F_BOLD) * 0.92f, name, textW).c_str());
    std::string sub = game_name(r.g >= 0 ? game_def(r.g).key : r.item.game);
    if (r.local) sub += std::string(" \xC2\xB7 ") + tr("a folder in the mods folder") + (r.inst->m.version.empty() ? "" : " \xC2\xB7 v" + r.inst->m.version);
    else {
        if (!r.item.type.empty()) sub += " \xC2\xB7 " + std::string(tr(type_name(r.item.type).c_str()));
        if (st == P_UPDATE) sub += " \xC2\xB7 v" + r.inst->rec->version_text + " \xE2\x86\x92 v" + r.item.version;
        else if (r.inst && r.inst->rec && !r.inst->rec->version_text.empty()) sub += " \xC2\xB7 v" + r.inst->rec->version_text;
        else if (!r.item.version.empty()) sub += " \xC2\xB7 v" + r.item.version;
        if (r.item.size > 0) sub += " \xC2\xB7 " + human_bytes((uint64_t)r.item.size);
    }
    if (busy) sub = tr(transfer_text(a, *t));
    dl->AddText(F_SMALL, FS(F_SMALL) * 0.9f, ImVec2(tx, r0.y + S(10) + FS(F_BOLD) * 0.92f), U(busy ? col::amber : col::dim),
                fit(F_SMALL, FS(F_SMALL) * 0.9f, sub, textW).c_str());

    for (size_t i = 0; i < btns.size(); i++) {
        ImGui::SetCursorScreenPos(ImVec2(xs[i], r0.y + (rh - T(26)) * 0.5f));
        const std::string b = btns[i];
        const bool needsGameClosed = b == "Install" || b == "Update" || b == "Turn on" || b == "Turn off" || b == "Uninstall";
        const bool needsSignIn = b == "Install" || b == "Update";
        const bool needsOnline = needsSignIn || b == "Page";
        const bool en = !(needsOnline && !v.online) && !(needsSignIn && !v.signed_in) && !(needsGameClosed && gameRuns && b != "Install" && b != "Update");
        if (btn(btns[i], en)) {
            if (b == "Install" || b == "Update") fab_get(r.item, r.g, b == "Update");
            else if (b == "Cancel") fab_cancel(t);
            else if (b == "Page") open_page(a, r.item.id);
            else if (b == "Turn on" || b == "Turn off") {
                mods_set_enabled(a, r.g, r.inst->m.folder, b == "Turn on");
                env_refresh(a, r.g);
                fab_mark_local_change();
            } else if (b == "Uninstall") {
                const std::string e = fab_uninstall(a, r.g, r.item.id);
                if (e.empty()) {
                    env_refresh(a, r.g);
                    set_message(a, name + " is uninstalled (kept in the mods folder's .fabricator-backup).", false);
                } else set_message(a, e, true);
                fab_mark_local_change();
            }
        }
        if (!en) tip(needsOnline && !v.online ? kOnlineOff : needsSignIn && !v.signed_in ? kSignInTip : "Close the game first.");
        else if (b == "Install" && gameRuns) tip("It downloads now and installs when the game closes.");
    }
    ImGui::SetCursorScreenPos(r0);
    ImGui::Dummy(ImVec2(rw, rh));
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PopID();
}

void library_tab(App &a, const FabView &v, ImVec2 c0, ImVec2 c1)
{
    // the rows: the library's mods, then installed store mods it doesn't list (signed out), then local folders
    std::vector<LibRow> rows;
    std::set<const LocalMod *> used;
    for (const FabLibraryItem &it : v.library) {
        LibRow r;
        r.item = it;
        r.inLibrary = true;
        r.g = game_of_key(it.game);
        r.inst = installed(r.g, it.id);
        if (r.inst) used.insert(r.inst);
        rows.push_back(r);
    }
    for (const LocalMod &l : g_local) {
        if (used.count(&l)) continue;
        LibRow r;
        r.g = l.g;
        r.inst = &l;
        if (l.m.fab_mod) {
            r.item.id = l.m.fab_mod;
            r.item.name = l.m.name;
            r.item.game = game_def(l.g).key;
            if (l.rec) { r.item.version = l.rec->version_text; r.item.version_id = l.rec->version; }
        } else r.local = true;
        rows.push_back(r);
    }
    int nInst = 0, nNot = 0, nUpd = 0;
    std::vector<const LibRow *> shown;
    for (const LibRow &r : rows) {
        if (g_libGame >= 0 && r.g != game_of_key(kGameKeys[g_libGame])) continue;
        const int st = row_state(r);
        const bool inst = r.inst != nullptr;
        nInst += inst; nNot += !inst; nUpd += st == P_UPDATE;
        if ((g_libState == 1 && !inst) || (g_libState == 2 && inst) || (g_libState == 3 && st != P_UPDATE)) continue;
        shown.push_back(&r);
    }

    const float sideW = S(200);
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("fabLibSide", ImVec2(sideW, c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    if (ImGui::Selectable(tr("All games"), g_libGame < 0)) g_libGame = -1;
    for (int i = 0; i < kGames; i++) if (ImGui::Selectable(tr(kGameNames[i]), g_libGame == i)) g_libGame = i;
    ImGui::Dummy(ImVec2(0, S(12)));
    const std::string l1 = std::string(tr("Installed")) + " (" + std::to_string(nInst) + ")##s1";
    const std::string l2 = std::string(tr("Not installed")) + " (" + std::to_string(nNot) + ")##s2";
    const std::string l3 = std::string(tr("Updates")) + " (" + std::to_string(nUpd) + ")##s3";
    if (ImGui::Selectable(l1.c_str(), g_libState == 1)) g_libState = g_libState == 1 ? 0 : 1;
    if (ImGui::Selectable(l2.c_str(), g_libState == 2)) g_libState = g_libState == 2 ? 0 : 2;
    if (ImGui::Selectable(l3.c_str(), g_libState == 3)) g_libState = g_libState == 3 ? 0 : 3;
    ImGui::Dummy(ImVec2(0, S(16)));
    if (btn("Refresh")) { fab_load_library(); g_refreshLocal = true; }   // (next frame: this one's rows point into g_local)
    tip("Ask the store again (a mod taken down or updated shows here).");
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(c0.x + sideW + S(14), c0.y));
    ImGui::BeginChild("fabLibRows", ImVec2(c1.x - c0.x - sideW - S(14), c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    if (!v.online) note(std::string(kOnlineOff) + " Your local mods are listed below.", col::amber);
    else if (!v.signed_in && v.account_checked) note(std::string(kSignInTip) + " - your local mods are listed below.", col::amber);
    if (!v.library_error.empty()) note(v.library_error, col::bad);
    if (v.library_loading && !v.library_loaded) note("Asking the store for your library...");
    if (shown.empty() && (v.library_loaded || !v.signed_in))
        note(rows.empty() ? "No mods yet - find some in the STORE tab." : "Nothing matches this filter.");
    for (size_t i = 0; i < shown.size(); i++) library_row(a, v, *shown[i], (int)i);
    ImGui::EndChild();
}

// ---- the STORE tab -------------------------------------------------------------------------------------------------
std::string g_search;
double g_searchEdited = -1;
int g_pageShot = 0;          // the screenshot shown big on a mod's page
int64_t g_pageShotFor = 0;

bool in_library(const FabView &v, int64_t id)
{
    for (const FabLibraryItem &i : v.library) if (i.id == id) return true;
    return false;
}

bool combo(const char *id, int &sel, const char *const *names, int n, const char *allName, float w)
{
    const char *cur = sel < 0 || sel >= n ? (allName ? tr(allName) : "") : tr(names[sel]);
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, cur)) {
        if (allName && ImGui::Selectable(tr(allName), sel < 0)) { sel = -1; changed = true; }
        for (int i = 0; i < n; i++) if (ImGui::Selectable(tr(names[i]), sel == i)) { sel = i; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

void store_card(App &a, const FabView &v, const FabCard &c, ImVec2 o, float cw, float thH, float textH)
{
    ImGui::PushID((int)c.id);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float ch = thH + textH + T(26) + S(12);
    ImGui::SetCursorScreenPos(o);
    const bool open = ImGui::InvisibleButton("##card", ImVec2(cw, thH + textH), ImGuiButtonFlags_EnableNav);
    const bool hov = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavActive);
    dl->AddRectFilled(o, ImVec2(o.x + cw, o.y + ch), U(hexc(0x16181b), 0.9f), S(4));
    picture(a, dl, c.screen, ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + cw - 1, o.y + thH));
    dl->AddRect(o, ImVec2(o.x + cw, o.y + ch), U(hov ? col::amber : hexc(0x2f3338)), S(4), 0, hov ? S(1.5f) : S(1));
    float y = o.y + thH + S(6);
    const float tw = cw - S(16);
    dl->AddText(F_BOLD, FS(F_BOLD) * 0.9f, ImVec2(o.x + S(8), y), U(col::text), fit(F_BOLD, FS(F_BOLD) * 0.9f, c.name, tw).c_str());
    y += FS(F_BOLD) * 0.9f + S(2);
    const float fs = FS(F_SMALL) * 0.85f;
    const std::string by = std::string(tr("by")) + " " + c.author + " \xC2\xB7 " + game_name(c.game);
    dl->AddText(F_SMALL, fs, ImVec2(o.x + S(8), y), U(col::dim), fit(F_SMALL, fs, by, tw).c_str());
    y += fs + S(3);
    const float sr = fs * 0.36f;
    const float sw = stars(dl, ImVec2(o.x + S(8), y + fs * 0.5f - sr), sr, c.ratings > 0 ? c.rating : 0);
    char rt[64];
    snprintf(rt, sizeof rt, " %.1f \xC2\xB7 %s %s", c.ratings > 0 ? c.rating : 0.0, count_text(c.downloads).c_str(), tr("downloads"));
    dl->AddText(F_SMALL, fs, ImVec2(o.x + S(8) + sw, y), U(col::dim), c.ratings > 0 ? rt : (std::string(" \xC2\xB7 ") + count_text(c.downloads) + " " + tr("downloads")).c_str());
    if (open) open_page(a, c.id);
    ImGui::SetCursorScreenPos(ImVec2(o.x + S(8), o.y + thH + textH + S(2)));
    const std::shared_ptr<FabTransfer> t = download_of(v, c.id);
    const int g = game_of_key(c.game);
    if (running(t)) {
        const int64_t total = t->progress.total, done = t->progress.done;
        pill(dl, ImGui::GetCursorScreenPos(), total > 0 && t->phase == FabTransfer::Downloading ? std::to_string((int)(done * 100 / total)) + "%" : "...", 2);
    } else if (installed(g, c.id)) pill(dl, ImVec2(o.x + S(8), o.y + thH + textH + S(6)), "Installed", 1);
    else if (in_library(v, c.id)) pill(dl, ImVec2(o.x + S(8), o.y + thH + textH + S(6)), "In library", 0);
    else {
        if (btn("Get", v.signed_in && g >= 0)) fab_get(item_of(c), g, false);
        if (!v.signed_in) tip(kSignInTip);
    }
    ImGui::PopID();
}

void report_button(const FabView &v, int64_t id)
{
    if (btn("Report", v.signed_in)) {
        g_reportMod = id;
        g_reportOpen = true;
        g_reportReason = 0;
        g_reportText.clear();
    }
    tip(v.signed_in ? "Tell the moderators about a problem with this mod." : kSignInTip);
}

void mod_page(App &a, const FabView &v, ImVec2 c0, ImVec2 c1)
{
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("fabPage", ImVec2(c1.x - c0.x, c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    if (btn("< Back to the store")) { fab_open_page(0); ImGui::EndChild(); return; }
    if (v.page_loading && !v.page) { note("Loading..."); ImGui::EndChild(); return; }
    if (!v.page_error.empty()) {
        note(v.page_error, col::bad);
        if (btn("Try again")) fab_open_page(v.page_id);
        ImGui::EndChild();
        return;
    }
    if (!v.page) { ImGui::EndChild(); return; }
    const FabPage &p = *v.page;
    if (p.state == "taken_down") {
        ImGui::PushFont(F_HEAD, FS(F_HEAD));
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::PopFont();
        note("This mod was taken down by the moderators. A copy you installed stays in your mods folder (the Library shows it).", col::amber);
        ImGui::EndChild();
        return;
    }
    if (g_pageShotFor != p.id) { g_pageShotFor = p.id; g_pageShot = 0; }
    if (g_pageShot >= (int)p.screens.size()) g_pageShot = 0;
    ImGui::PushFont(F_HEAD, FS(F_HEAD));
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::PopFont();
    note(std::string(tr("by")) + " " + p.author + " \xC2\xB7 " + game_name(p.game) + " \xC2\xB7 " + tr(type_name(p.type)) + " \xC2\xB7 " +
         tr("store number") + " " + std::to_string(p.id));
    ImGui::Dummy(ImVec2(0, S(4)));

    const float W = ImGui::GetContentRegionAvail().x, leftW = std::floor(W * 0.6f), rightW = W - leftW - S(16);
    const float top = ImGui::GetCursorScreenPos().y;
    // left: the screenshots, the description, what's new
    ImGui::BeginChild("fabPageL", ImVec2(leftW, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
    {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x, bh = std::min(bw * 9 / 16, S(380));
        picture(a, ImGui::GetWindowDrawList(), p.screens.empty() ? p.screen : p.screens[g_pageShot], o, ImVec2(o.x + bw, o.y + bh), S(4));
        ImGui::Dummy(ImVec2(bw, bh + S(6)));
        if (p.screens.size() > 1) {
            const float sw = S(96), shh = S(54);
            for (size_t i = 0; i < p.screens.size(); i++) {
                if (i) ImGui::SameLine(0, S(6));
                ImGui::PushID((int)i);
                const ImVec2 q = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##shot", ImVec2(sw, shh), ImGuiButtonFlags_EnableNav)) g_pageShot = (int)i;
                picture(a, ImGui::GetWindowDrawList(), p.screens[i], q, ImVec2(q.x + sw, q.y + shh), S(3));
                ImGui::GetWindowDrawList()->AddRect(q, ImVec2(q.x + sw, q.y + shh), U((int)i == g_pageShot ? col::amber : col::faint), S(3), 0,
                                                    (int)i == g_pageShot ? S(2) : S(1));
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        if (!p.short_desc.empty()) { ImGui::PushFont(F_BOLD, FS(F_BOLD) * 0.92f); note(p.short_desc, col::text); ImGui::PopFont(); }
        if (!p.long_desc.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(p.long_desc.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!p.versions.empty() && !p.versions[0].notes.empty()) {
            heading("WHAT'S NEW");
            note(p.versions[0].notes, col::text);
        }
    }
    ImGui::EndChild();
    // right: Get / Install / Update, the facts, rating, tags, needs, more from the author, Report
    ImGui::SetCursorScreenPos(ImVec2(c0.x + leftW + S(16), top));
    ImGui::BeginChild("fabPageR", ImVec2(rightW, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
    {
        const int g = game_of_key(p.game);
        const LocalMod *inst = installed(g, p.id);
        const int64_t latest = p.versions.empty() ? 0 : p.versions[0].id;
        const bool inLib = in_library(v, p.id);
        FabLibraryItem it = item_of(p);
        it.version_id = latest;
        if (!p.versions.empty()) { it.sha256 = p.versions[0].sha256; it.size = p.versions[0].size; }
        const std::shared_ptr<FabTransfer> t = download_of(v, p.id);
        const ImVec2 bs(std::min(rightW, T(220)), T(40));
        if (!transfer_line(a, t)) {
            if (!v.signed_in) { th_button_red("GET", bs, false, F_HEAD); tip(kSignInTip); }
            else if (inst && latest && inst->rec && inst->rec->version != latest) {
                if (th_button_red("UPDATE", bs, g >= 0, F_HEAD)) fab_get(it, g, true);
                tip("v" + (inst->rec->version_text.empty() ? std::string("?") : inst->rec->version_text) + " is installed; v" + p.version + " is the newest.");
            } else if (inst) { th_button("INSTALLED", bs, false, F_HEAD); tip("It's in this game's mods folder - turn it on or off in the LIBRARY tab."); }
            else if (th_button_red(inLib ? "INSTALL" : "GET", bs, g >= 0, F_HEAD)) fab_get(it, g, false);
            if (running(t) == false && t && t->phase == FabTransfer::Failed)
                for (const std::string &e : fab_transfer_problems(*t)) note(e, col::bad);
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        auto fact = [&](const char *k, const std::string &val) {
            if (val.empty()) return;
            ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
            ImGui::TextUnformatted(tr(k));
            ImGui::PopStyleColor();
            ImGui::SameLine(S(110));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(val.c_str());
            ImGui::PopTextWrapPos();
        };
        fact("Version", p.version.empty() ? "" : "v" + p.version);
        fact("Updated", date_text(p.versions.empty() ? p.updated_at : p.versions[0].created_at));
        fact("Size", p.size > 0 ? human_bytes((uint64_t)p.size) : "");
        fact("Downloads", count_text(p.downloads));
        fact("Side", p.side == "host" ? tr("Host only") : p.side == "client" ? tr("Client only") : tr("Everyone"));
        {   // the average
            ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
            ImGui::TextUnformatted(tr("Rating"));
            ImGui::PopStyleColor();
            ImGui::SameLine(S(110));
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float r = ImGui::GetTextLineHeight() * 0.38f;
            const float sw = stars(ImGui::GetWindowDrawList(), ImVec2(q.x, q.y + ImGui::GetTextLineHeight() * 0.5f - r), r, p.ratings ? p.rating : 0);
            ImGui::Dummy(ImVec2(sw + S(6), ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            char b[64];
            snprintf(b, sizeof b, "%.1f  (%d)", p.ratings ? p.rating : 0.0, p.ratings);
            ImGui::TextUnformatted(p.ratings ? b : tr("not rated yet"));
        }
        {   // the player's own stars, once the mod is in the library
            ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
            ImGui::TextUnformatted(tr("Your rating"));
            ImGui::PopStyleColor();
            ImGui::SameLine(S(110));
            if (v.signed_in && inLib) {
                auto mine = v.my_ratings.find(p.id);
                const int have = mine == v.my_ratings.end() ? 0 : mine->second;
                const float r = ImGui::GetTextLineHeight() * 0.45f;
                int hover = 0;
                std::vector<ImVec2> cs;
                for (int i = 0; i < 5; i++) {
                    if (i) ImGui::SameLine(0, S(2));
                    ImGui::PushID(i + 100);
                    const ImVec2 q = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##star", ImVec2(r * 2.3f, r * 2.3f), ImGuiButtonFlags_EnableNav)) fab_rate(p.id, i + 1);
                    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) hover = i + 1;
                    cs.push_back(ImVec2(q.x + r * 1.15f, q.y + r * 1.15f));
                    ImGui::PopID();
                }
                const int shown = hover ? hover : have;
                for (int i = 0; i < 5; i++) star(ImGui::GetWindowDrawList(), cs[i], r, U(hover ? col::text : col::amber), i < shown);
            } else {
                ImGui::TextUnformatted(tr(v.signed_in ? "Get the mod to rate it." : "Sign in to rate it."));
            }
        }
        if (!p.tags.empty()) {
            ImGui::Dummy(ImVec2(0, S(4)));
            ImDrawList *rdl = ImGui::GetWindowDrawList();
            ImVec2 q = ImGui::GetCursorScreenPos();
            const float x0 = q.x, x1 = q.x + ImGui::GetContentRegionAvail().x;
            const float ph = FS(F_SMALL) * 0.85f + S(6);
            for (const std::string &tg : p.tags) {
                const float w = F_SMALL->CalcTextSizeA(FS(F_SMALL) * 0.85f, FLT_MAX, 0, tg.c_str()).x + S(16);
                if (q.x + w > x1 && q.x > x0) { q.x = x0; q.y += ph + S(4); }
                q.x += pill(rdl, q, tg, 0) + S(4);
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, q.y + ph + S(4)));
            ImGui::Dummy(ImVec2(0, 0));
        }
        if (!p.requires.empty()) {
            heading("NEEDS");
            for (const auto &rq : p.requires) {
                const bool have = g >= 0 && installed(g, rq.first);
                if (link(rq.second, ("rq" + std::to_string(rq.first)).c_str())) open_page(a, rq.first);
                ImGui::SameLine();
                ImGui::TextUnformatted(tr(have ? "(installed)" : "(not installed)"));
            }
        }
        if (!p.others.empty()) {
            heading("MORE FROM THIS CREATOR");
            for (const FabCard &o : p.others)
                if (link(o.name + " (" + game_name(o.game) + ")", ("ot" + std::to_string(o.id)).c_str())) open_page(a, o.id);
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        report_button(v, p.id);
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void store_tab(App &a, const FabView &v, ImVec2 c0, ImVec2 c1)
{
    if (v.page_id) { mod_page(a, v, c0, c1); return; }
    FabQuery q = v.query;
    // the tools: search (sent 400 ms after the last key), Game, Type, Sort
    ImGui::SetCursorScreenPos(c0);
    const float W = c1.x - c0.x, cw = T(150), gap = S(8);
    if (input("##fabsearch", g_search, "Search mods, authors, tags...", W - 3 * (cw + gap), ImGuiInputTextFlags_EnterReturnsTrue)) g_searchEdited = -2;
    if (ImGui::IsItemEdited()) g_searchEdited = ImGui::GetTime();
    const double now = ImGui::GetTime();
    if (g_searchEdited == -2 || (g_searchEdited >= 0 && now - g_searchEdited >= 0.4)) {
        g_searchEdited = -1;
        if (trim(g_search) != q.search) { q.search = trim(g_search); q.page = 1; fab_store_query(q); }
    }
    int gi = key_index(kGameKeys, kGames, q.game), ti = key_index(kTypeKeys, kTypes, q.type), si = std::max(0, key_index(kSortKeys, kSorts, q.sort));
    ImGui::SameLine(0, gap);
    if (combo("##fabgame", gi, kGameNames, kGames, "All games", cw)) { q.game = gi < 0 ? "" : kGameKeys[gi]; q.page = 1; fab_store_query(q); }
    ImGui::SameLine(0, gap);
    if (combo("##fabtype", ti, kTypeNames, kTypes, "All types", cw)) { q.type = ti < 0 ? "" : kTypeKeys[ti]; q.page = 1; fab_store_query(q); }
    ImGui::SameLine(0, gap);
    if (combo("##fabsort", si, kSortNames, kSorts, nullptr, cw)) { q.sort = kSortKeys[si]; q.page = 1; fab_store_query(q); }
    const float y0 = ImGui::GetCursorScreenPos().y + S(4);
    const int pages = std::max(1, (v.total + kFabPerPage - 1) / kFabPerPage);
    const float barH = T(26) + S(10);

    ImGui::SetCursorScreenPos(ImVec2(c0.x, y0));
    ImGui::BeginChild("fabGrid", ImVec2(W, c1.y - y0 - barH), ImGuiChildFlags_NavFlattened);
    if (!v.signed_in && v.account_checked) note(std::string(kSignInTip) + ".", col::amber);
    if (!v.store_error.empty()) {
        note(v.store_error, col::bad);
        if (btn("Try again")) fab_store_query(q);
    } else if (v.store_loading && v.cards.empty()) note("Loading...");
    else if (v.cards.empty() && v.store_asked && !v.store_loading) note(q.search.empty() && q.game.empty() && q.type.empty() ? "The store is empty so far." : "No mods match.");
    {
        const float aw = ImGui::GetContentRegionAvail().x, g2 = S(12);
        const int cols = std::max(2, std::min(4, (int)((aw + g2) / (T(230) + g2))));
        const float cardW = (aw - g2 * (cols - 1)) / cols, thH = cardW * 0.45f;
        const float textH = FS(F_BOLD) * 0.9f + FS(F_SMALL) * 0.85f * 2 + S(14);
        const float ch = thH + textH + T(26) + S(12);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (size_t i = 0; i < v.cards.size(); i++) {
            const int col = (int)i % cols, row = (int)i / cols;
            store_card(a, v, v.cards[i], ImVec2(start.x + col * (cardW + g2), start.y + row * (ch + g2)), cardW, thH, textH);
        }
        const int rows = ((int)v.cards.size() + cols - 1) / cols;
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(aw, rows * (ch + g2)));
    }
    ImGui::EndChild();
    // paging
    ImGui::SetCursorScreenPos(ImVec2(c0.x, c1.y - barH + S(8)));
    if (btn("< Previous", q.page > 1)) { q.page--; fab_store_query(q); }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    char pg[96];
    snprintf(pg, sizeof pg, "%s %d / %d  \xC2\xB7  %d %s", tr("Page"), q.page, pages, v.total, tr(v.total == 1 ? "mod" : "mods"));
    ImGui::TextUnformatted(pg);
    ImGui::SameLine();
    if (btn("Next >", q.page < pages)) { q.page++; fab_store_query(q); }
    if (v.store_loading && !v.cards.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Loading...")); }
}

// ---- the CREATOR tab -----------------------------------------------------------------------------------------------
std::string publish_reason(const std::string &r)
{
    if (r == "account too new") return "Your online account is too new to publish yet - try again in a few days.";
    if (r == "publishing stopped") return "Publishing is stopped for your account (the moderators).";
    if (r == "quota") return "Your store space is full - remove a mod or an old version first.";
    if (r == "daily limit") return "You've reached today's upload limit - try again tomorrow.";
    if (r == "server daily limit") return "The store has reached today's upload limit - try again tomorrow.";
    if (r == "server full") return "The store's disk is full right now - try again later.";
    return r.empty() ? "" : "Publishing isn't possible right now (" + r + ").";
}
std::string state_word(const std::string &s)
{
    if (s == "live") return "live";
    if (s == "taken_down") return "taken down";
    if (s == "removed") return "removed";
    if (s == "files_deleted") return "files deleted";
    return s;
}

std::shared_ptr<FabTransfer> latest_upload(const FabView &v)
{
    for (auto it = v.transfers.rbegin(); it != v.transfers.rend(); ++it) if ((*it)->kind == FabTransfer::Upload) return *it;
    return nullptr;
}

// A stopped publish whose server session (and packed zip) is kept for a resume: "Discard the upload" gives it up for
// good, so it stops counting toward the day's uploads and the quota, and the packed zip's space comes back.
void discard_button(const std::shared_ptr<FabTransfer> &t)
{
    if (!t->resumable) return;
    ImGui::SameLine();
    const bool made = t->zip_made;
    if (btn("Discard the upload")) { fab_discard(t); fab_dismiss(t); }
    tip(made ? "OK keeps what was sent: publishing the same folder again goes on from there. Discard deletes it from the server "
               "(it stops counting toward today's uploads and your space) and deletes the zip COGBox packed."
             : "OK keeps what was sent: publishing the same .zip again goes on from there. Discard deletes it from the server "
               "(it stops counting toward today's uploads and your space).");
}

void upload_progress(App &a, const std::shared_ptr<FabTransfer> &t)
{
    if (!t) return;
    heading("PUBLISHING");
    {   // a new publish, and its result, scroll the form down to these lines
        static const FabTransfer *seen = nullptr;
        static int seenPhase = -1;
        const int p = t->phase;
        const bool end = p == FabTransfer::Done || p == FabTransfer::Failed || p == FabTransfer::Cancelled;
        if (t.get() != seen || (end && seenPhase != p)) ImGui::SetScrollHereY(0.0f);
        seen = t.get();
        seenPhase = end ? p : -1;
    }
    const int ph = t->phase, fail = t->phase == FabTransfer::Failed ? t->failed_phase.load() : -1;
    const int64_t done = t->progress.done, total = t->progress.total;
    const int pct = total > 0 ? (int)std::min<int64_t>(100, done * 100 / total) : 0;
    // steps: 0 waiting, 1 running, 2 done, 3 failed
    auto step = [&](const std::string &text, int s) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        th_lamp(ImGui::GetWindowDrawList(), ImVec2(q.x + S(6), q.y + h * 0.55f), S(4.5f), s == 2 ? 1 : s == 1 ? 2 : s == 3 ? 3 : 0);
        ImGui::SetCursorScreenPos(ImVec2(q.x + S(20), q.y));
        ImGui::PushStyleColor(ImGuiCol_Text, s == 0 ? col::dim : s == 3 ? col::bad : col::text);
        ImGui::TextUnformatted(tr(text).c_str());
        ImGui::PopStyleColor();
    };
    const int curr = ph == FabTransfer::Failed ? fail : ph == FabTransfer::Done ? 99 : ph;
    auto state_of = [&](int myPhase) {
        if (ph == FabTransfer::Failed && fail == myPhase) return 3;
        if (curr > myPhase) return 2;
        return curr == myPhase ? 1 : 0;
    };
    if (t->zip_made || ph == FabTransfer::Packing || (fail == FabTransfer::Packing))
        step(ph == FabTransfer::Packing ? "Packing the folder into a zip " + std::to_string(pct) + "%" : "Packing the folder into a zip", state_of(FabTransfer::Packing));
    step("Local checks", state_of(FabTransfer::Checking));
    const bool sent = ph == FabTransfer::Uploading && total > 0 && done >= total;
    const int up = state_of(FabTransfer::Uploading);
    step(ph == FabTransfer::Uploading && !sent ? (done == 0 ? std::string("Upload (reading the zip first)") : "Upload " + std::to_string(pct) + "%") : "Upload",
         ph == FabTransfer::Uploading ? (sent ? 2 : 1) : up == 3 && total > 0 && done >= total ? 2 : up);
    step("Server checks", ph == FabTransfer::Uploading ? (sent ? 1 : 0) : ph == FabTransfer::Done ? 2 : (up == 3 && total > 0 && done >= total) ? 3 : 0);
    if (running(t)) {
        if (btn("Cancel##up")) fab_cancel(t);
        tip("What is already sent is kept: publishing the same folder or .zip again goes on from there.");
    } else if (ph == FabTransfer::Done) {
        note(t->name + " " + tr("is live in the store."), col::good);
        if (btn("Open its page")) open_page(a, t->mod_id);
        ImGui::SameLine();
        if (btn("OK##updone")) fab_dismiss(t);
    } else if (ph == FabTransfer::Cancelled) {
        note("Cancelled.", col::dim);
        if (btn("OK##upcancel")) fab_dismiss(t);
        discard_button(t);
    } else {
        note("Not published:", col::bad);
        for (const std::string &e : fab_transfer_problems(*t)) note("  \xE2\x80\xA2 " + e, col::bad);
        if (btn("OK##upfail")) fab_dismiss(t);
        discard_button(t);
    }
}

void upload_form(App &a, const FabView &v)
{
    Form &f = g_form;
    const std::shared_ptr<FabTransfer> t = latest_upload(v);
    const bool busy = running(t);
    const float W = ImGui::GetContentRegionAvail().x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // the drop zone
    {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float h = T(64);
        const ImU32 c = U(f.source.empty() ? hexc(0x555b63) : col::good, 0.9f);
        for (float x = o.x; x < o.x + W; x += S(12)) {   // dashed edge
            dl->AddLine(ImVec2(x, o.y), ImVec2(std::min(x + S(6), o.x + W), o.y), c, S(2));
            dl->AddLine(ImVec2(x, o.y + h), ImVec2(std::min(x + S(6), o.x + W), o.y + h), c, S(2));
        }
        for (float y = o.y; y < o.y + h; y += S(12)) {
            dl->AddLine(ImVec2(o.x, y), ImVec2(o.x, std::min(y + S(6), o.y + h)), c, S(2));
            dl->AddLine(ImVec2(o.x + W, y), ImVec2(o.x + W, std::min(y + S(6), o.y + h)), c, S(2));
        }
        const std::string l1 = f.source.empty() ? tr("Drop your mod's folder or .zip here (checked before upload)")
                                                : base_name(f.source) + (f.folder ? std::string("  ") + tr("(a folder - zipped before upload)") : "");
        const ImVec2 ts = ImGui::CalcTextSize(l1.c_str());
        dl->AddText(ImVec2(o.x + (W - ts.x) * 0.5f, o.y + S(10)), U(f.source.empty() ? col::dim : col::text), l1.c_str());
        ImGui::SetCursorScreenPos(ImVec2(o.x + (W - btn_w("Choose a folder...") - btn_w("Choose a .zip...") - S(8)) * 0.5f, o.y + h - T(26) - S(8)));
        if (btn("Choose a folder...", !busy)) { const std::string p = pick_folder(L"Choose your mod's folder"); if (!p.empty()) form_source(a, p); }
        ImGui::SameLine(0, S(8));
        if (btn("Choose a .zip...", !busy)) { const std::string p = pick_file(L"Choose your mod's .zip", L"Zip files\0*.zip\0All files\0*.*\0"); if (!p.empty()) form_source(a, p); }
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + S(8)));
        ImGui::Dummy(ImVec2(W, 0));
    }
    const float lw = S(150), fw = W - lw - S(8), rowX0 = ImGui::GetCursorScreenPos().x;
    auto label = [&](const char *t_en, const char *tip_en = "") {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::TextUnformatted(tr(t_en));
        ImGui::PopStyleColor();
        tip(tip_en);
        ImGui::SameLine(lw);
    };
    if (busy) ImGui::BeginDisabled();
    label("Name", "3 to 60 characters. A new version keeps its mod's name.");
    if (f.modId) ImGui::BeginDisabled();
    input("##fname", f.name, "Your mod's name", fw);
    label("Game");
    {
        int gi = f.game;
        if (combo("##fgame", gi, kGameNames, kGames, nullptr, T(160))) f.game = std::max(0, gi);
    }
    if (f.modId) ImGui::EndDisabled();
    ImGui::SameLine(0, S(12));
    ImGui::TextDisabled("%s", tr("Type"));
    ImGui::SameLine();
    { int ti = f.type; if (combo("##ftype", ti, kTypeNames, kTypes, nullptr, T(160))) f.type = std::max(0, ti); }
    ImGui::SameLine(0, S(12));
    ImGui::TextDisabled("%s", tr("Version"));
    ImGui::SameLine();
    input("##fver", f.version, "1.0", std::max(T(80), rowX0 + lw + fw - ImGui::GetCursorScreenPos().x));
    label("Short description", "One line on the store's cards (up to 120 characters).");
    input("##fshort", f.shortDesc, "What it does, in one line", fw);
    label("Description", "The mod's page (up to 8000 characters).");
    input_multi("##flong", f.longDesc, ImVec2(fw, T(90)));
    label("Screenshots", "1 to 6 PNG or JPEG pictures, up to 4 MB and 3840 x 2160 each. Drop them on the window or add them here.");
    {
        const float sw = T(80), shh = T(45);
        for (size_t i = 0; i < f.screens.size(); i++) {
            ImGui::PushID((int)i + 300);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##sc", ImVec2(sw, shh));
            const bool hov = ImGui::IsItemHovered();
            picture(a, dl, f.screens[i], q, ImVec2(q.x + sw, q.y + shh), S(3), true);
            tip(base_name(f.screens[i]) + " - click to take it out");
            if (hov) dl->AddRectFilled(q, ImVec2(q.x + sw, q.y + shh), U(col::shadow, 0.5f), S(3));
            if (hov) dl->AddText(ImVec2(q.x + sw * 0.5f - S(4), q.y + shh * 0.5f - ImGui::GetTextLineHeight() * 0.5f), U(col::text), "x");
            if (ImGui::IsItemClicked()) { f.screens.erase(f.screens.begin() + (long)i); ImGui::PopID(); break; }
            ImGui::PopID();
            ImGui::SameLine(0, S(6));
        }
        if (f.screens.size() < 6) {
            if (th_button("+##addshot", ImVec2(shh, shh), true, F_HEAD)) {
                const std::string p = pick_file(L"Choose a screenshot", L"Pictures\0*.png;*.jpg;*.jpeg\0");
                if (!p.empty()) add_screen(a, p);
            }
            tip("Add a screenshot (PNG or JPEG).");
        } else ImGui::Dummy(ImVec2(0, shh));
    }
    label("Tags", "Up to 8, separated by commas, each up to 24 characters.");
    input("##ftags", f.tags, "horde, economy", fw);
    label("Side", "Who needs the mod in a multiplayer match (modcore's Side in mod.ini).");
    { int si = f.side; if (combo("##fside", si, kSideNames, kSides, nullptr, T(300))) f.side = std::max(0, si); }
    label("Needs other mods", "Store numbers of mods this one needs, separated by commas (each mod's page shows its store number).");
    input("##fneeds", f.needs, "e.g. 12, 40", fw);
    if (f.modId) {
        label("What's new", "Up to 2000 characters, shown on the mod's page.");
        input_multi("##fnotes", f.notes, ImVec2(fw, T(60)));
    }
    if (busy) ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("You are responsible for what you upload - see"));
    ImGui::SameLine(0, S(4));
    if (link("LEGAL.md", "legal")) {
        const std::string local = join(exe_dir(), "LEGAL.md");
        shell_open(file_exists(local) ? local : std::string("https://github.com/Shagg88/COGBox/blob/main/LEGAL.md"));
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    const std::string reason = publish_reason(v.limits.can_publish);
    const bool can = !busy && reason.empty();
    if (th_button_red("PUBLISH", ImVec2(T(170), T(40)), can, F_HEAD)) {
        f.problems = form_problems();
        if (f.problems.empty()) {
            FabPublish p;
            p.source = f.source;
            p.folder = f.folder;
            p.game_key = kGameKeys[f.game];
            p.name = trim(f.name);
            p.meta_json = form_meta();
            p.mod_id = f.modId;
            p.screens = f.screens;
            if (t && !running(t)) fab_dismiss(t);
            fab_publish(p);
        }
    }
    if (!can) tip(busy ? "A publish is running." : reason);
    if (!reason.empty()) note(reason, col::amber);
    for (const std::string &e : f.problems) note("  \xE2\x80\xA2 " + e, col::bad);
    upload_progress(a, t);
}

void creator_tab(App &a, const FabView &v, ImVec2 c0, ImVec2 c1)
{
    if (!v.signed_in) {
        ImGui::SetCursorScreenPos(c0);
        note(std::string(kSignInTip) + ".", col::amber);
        return;
    }
    const float sideW = S(250);
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("fabCrSide", ImVec2(sideW, c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    if (ImGui::Selectable(tr("+ New upload"), g_creatorSel == 0) && g_creatorSel != 0) { g_creatorSel = 0; form_reset(0, "", "gears3"); }
    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::TextDisabled("%s (%d)", tr("My mods"), (int)v.creator.size());
    if (!v.creator_error.empty()) note(v.creator_error, col::bad);
    for (const FabCreatorMod &m : v.creator) {
        std::string line = m.name;
        for (const FabCreatorVersion &cv : m.versions) if (cv.state == "live") { line += " \xC2\xB7 v" + cv.version; break; }
        if (m.state != "live") line += " \xC2\xB7 " + std::string(tr(state_word(m.state)));
        if (text_selectable(fit(F_BODY, ImGui::GetFontSize(), line, sideW - S(24)), "cm" + std::to_string(m.id), g_creatorSel == m.id, col::text,
                            ImGui::GetContentRegionAvail().x) &&
            g_creatorSel != m.id) {
            g_creatorSel = m.id;
            form_reset(m.id, m.name, m.game);
        }
    }
    ImGui::Dummy(ImVec2(0, S(12)));
    if (v.limits.known) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::PushTextWrapPos(0);
        ImGui::Text("%s: %d / %d", tr("Uploads today"), v.limits.uploads_today, v.limits.max_uploads_per_day);
        ImGui::Text("%s: %s / %s", tr("Space"), human_bytes((uint64_t)v.limits.bytes_used).c_str(), human_bytes((uint64_t)v.limits.quota).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (btn("Refresh##cr")) fab_load_creator();
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(c0.x + sideW + S(14), c0.y));
    ImGui::BeginChild("fabCrMain", ImVec2(c1.x - c0.x - sideW - S(14), c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    const FabCreatorMod *sel = nullptr;
    for (const FabCreatorMod &m : v.creator) if (m.id == g_creatorSel) sel = &m;
    if (g_creatorSel && !sel && v.creator_loaded) { g_creatorSel = 0; form_reset(0, "", "gears3"); }
    if (sel) {
        ImGui::PushFont(F_HEAD, FS(F_HEAD));
        ImGui::TextUnformatted(sel->name.c_str());
        ImGui::PopFont();
        char b[160];
        snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 %d %s \xC2\xB7 %.1f (%d) \xC2\xB7 %d %s", game_name(sel->game).c_str(), tr(state_word(sel->state)).c_str(),
                 sel->downloads, tr("downloads"), sel->rating, sel->ratings, sel->reports_open, tr("open reports"));
        note(b);
        for (const FabCreatorVersion &cv : sel->versions)
            note("v" + cv.version + " \xC2\xB7 " + date_text(cv.created_at) + " \xC2\xB7 " + human_bytes((uint64_t)cv.size) + " \xC2\xB7 " + state_word(cv.state),
                 cv.state == "live" ? col::text : col::dim);
        if (sel->state == "live") {
            if (btn("Open its page")) open_page(a, sel->id);
            ImGui::SameLine();
        }
        if (sel->state != "removed" && btn("Remove from the store")) { g_removeMod = sel->id; g_removeName = sel->name; g_removeOpen = true; }
        if (sel->state == "live") heading("NEW VERSION");
        else { note("Only a live mod gets new versions.", col::amber); ImGui::EndChild(); return; }
    } else heading("NEW UPLOAD");
    upload_form(a, v);
    ImGui::EndChild();
}

void popups(const FabView &v)
{
    if (g_reportOpen) { ImGui::OpenPopup("##fabreport"); g_reportOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(S(520), 0));
    if (ImGui::BeginPopupModal("##fabreport", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        th_heading("REPORT THIS MOD", ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        for (int i = 0; i < kReasons; i++) ImGui::RadioButton(tr(kReasonNames[i]), &g_reportReason, i);
        ImGui::TextDisabled("%s", tr("What's wrong (up to 500 characters)"));
        input_multi("##reptext", g_reportText, ImVec2(S(490), T(70)));
        cut_utf16(g_reportText, 500);
        if (btn("Send")) {
            fab_report(g_reportMod, kReasonKeys[g_reportReason], g_reportText);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (btn("Cancel##rep") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_removeOpen) { ImGui::OpenPopup("##fabremove"); g_removeOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(S(520), 0));
    if (ImGui::BeginPopupModal("##fabremove", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        th_heading("REMOVE FROM THE STORE", ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        note(g_removeName + " " + tr("leaves the store for good: its files are deleted. Players who installed it keep their copy."), col::text);
        if (th_button_red("Remove", ImVec2(btn_w("Remove"), T(26)), true, F_BODY)) { fab_remove_own(g_removeMod); g_creatorSel = 0; form_reset(0, "", "gears3"); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (btn("Cancel##rem") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    (void)v;
}

}  // namespace

void fabricator_drop(App &a, const std::string &path)
{
    const FabView v = fab_view();
    if (!v.online) { set_message(a, kOnlineOff, true); return; }
    if (!v.signed_in) { set_message(a, "Sign in on the SOCIAL tab to publish mods.", true); return; }
    a.fabTab = 2;
    const bool folder = dir_exists(path);
    if (!folder && (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg"))) { add_screen(a, path); return; }
    if (folder || ends_with(path, ".zip")) { form_source(a, path); return; }
    set_message(a, "Drop your mod's folder or .zip, or a PNG / JPEG screenshot for it.", true);
}

void fabricator_draw(App &a, ImVec2 size)
{
    const double now = ImGui::GetTime();
    const bool online = a.ini.get("Online", "Enabled") == "1";
    // as the SOCIAL tab reads it (social_restart); off, fab_configure leaves the workers no server and no account
    fab_configure(a.ini.get("Online", "Server", kCogboxServer), profile_dir(exe_dir(), std::to_string(current_profile())), online);
    static int lastFrame = -10;
    static double accountAt = 0;
    const bool opened = ImGui::GetFrameCount() - lastFrame > 2;
    lastFrame = ImGui::GetFrameCount();
    if (opened) {
        fab_refresh_account();
        fab_load_library();
        g_refreshLocal = true;
        if (a.fabTab == 2) fab_load_creator();
        accountAt = now;
        static bool testPage = true;   // the COGBOX_FABRICATOR=<tab>:<mod id> test switch (main.cpp)
        if (testPage) {
            testPage = false;
            const char *t = getenv("COGBOX_FABRICATOR");
            const char *c = t ? strchr(t, ':') : nullptr;
            if (c && atoll(c + 1) > 0) { a.fabTab = 1; fab_open_page(atoll(c + 1)); }
        }
    }
    if (now - accountAt > 10) { accountAt = now; fab_refresh_account(); }   // the SOCIAL worker may make the account meanwhile
    local_refresh(a, g_refreshLocal);   // the mods folders (every 3 s, after an install, or when asked); before any row is built
    g_refreshLocal = false;
    FabView v = fab_view();
    {
        static bool wasSigned = false;
        if (v.account_checked && v.signed_in != wasSigned) {
            wasSigned = v.signed_in;
            fab_load_library();
            if (v.signed_in) fab_load_creator();
        }
    }
    if (online && a.fabTab == 1 && !v.store_asked) { fab_store_query(v.query); v = fab_view(); }
    if (online && a.fabTab == 2 && !v.signed_in && v.account_checked) a.fabTab = 0;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float pw = std::min(size.x - S(40), S(1200)), ph = std::min(size.y - S(40), S(820));
    const ImVec2 p0((size.x - pw) * 0.5f, (size.y - ph) * 0.5f), p1(p0.x + pw, p0.y + ph);
    th_plate(dl, p0, p1, S(18), false, 0.45f);
    th_plate_fill(dl, ImVec2(p0.x + S(6), p0.y + S(6)), ImVec2(p1.x - S(6), p1.y - S(6)), S(14), U(ImVec4(0.02f, 0.025f, 0.03f, 0.55f)));

    // the header: a cog, the name, who is signed in; BACK at the right
    th_cog(dl, ImVec2(p0.x + S(52), p0.y + S(52)), S(26), 10, (float)now * 0.12f, U(col::redHi, 0.9f), U(ImVec4(0.03f, 0.03f, 0.035f, 1)));
    dl->AddText(F_TITLE, FS(F_TITLE), ImVec2(p0.x + S(92), p0.y + S(20)), U(col::text), tr("FABRICATOR"));
    {
        const std::string sub = std::string(tr("The COGBox mod store")) + "   |   " +
                                (!online ? std::string(tr(kOnlineOff)) : v.signed_in ? std::string(tr("Signed in as")) + " " + v.tag : std::string(tr(kSignInTip)));
        dl->AddText(F_BODY, FS(F_BODY), ImVec2(p0.x + S(94), p0.y + S(22) + FS(F_TITLE)),
                    U(online && (v.signed_in || !v.account_checked) ? col::dim : col::amber), sub.c_str());
    }
    bool back = false;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - S(24) - S(130), p0.y + S(26)));
    if (th_button("BACK", ImVec2(S(130), S(40)), true, F_HEAD)) back = true;
    tip(a.padConnected ? std::string("Back to the library (") + pad_name(a, "B") + ")" : "Back to the library (Esc)");

    // the tabs (LB / RB); CREATOR needs the online account
    const float hh = S(108);
    const float tx = p0.x + S(24), ty = p0.y + hh, tw = T(150), tabH = T(30);
    static const char *const kTabs[] = { "LIBRARY", "STORE", "CREATOR" };
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(ImVec2(tx + i * (tw + S(4)), ty));
        const bool off = i == 2 && online && !v.signed_in;   // (Online play off: the tab opens and says so)
        if (th_tab(kTabs[i], a.fabTab == i, ImVec2(tw, tabH)) && !off) {
            a.fabTab = i;
            if (i == 0) { fab_load_library(); g_refreshLocal = true; }
            if (i == 2) fab_load_creator();
        }
        if (off) {
            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), U(ImVec4(0, 0, 0, 0.55f)));
            tip(kSignInTip);
        }
    }
    {   // a running Get or publish, right of the tabs
        for (auto &t : v.transfers) {
            if (!running(t)) continue;
            const int64_t total = t->progress.total, done = t->progress.done;
            const int pct = total > 0 ? (int)std::min<int64_t>(100, done * 100 / total) : 0;
            const std::string s = (t->kind == FabTransfer::Upload ? std::string(tr("Publishing")) : std::string(tr("Getting"))) + " " + t->name + "  " + std::to_string(pct) + "%";
            const std::string f = fit(F_SMALL, FS(F_SMALL), s, p1.x - S(24) - (tx + 3 * (tw + S(4)) + S(16)));
            const float w = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, f.c_str()).x;
            dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(p1.x - S(24) - w, ty + (tabH - FS(F_SMALL)) * 0.5f), U(col::amber), f.c_str());
            break;
        }
    }
    dl->AddLine(ImVec2(p0.x + S(24), ty + tabH), ImVec2(p1.x - S(24), ty + tabH), U(col::redHi, 0.7f), S(2));

    // LB / RB, Esc / B (not while a list, a question or a text box has them)
    static bool popupLast = false, textLast = false;
    const bool popupNow = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || a.modal.open;
    if (!popupNow && !a.modal.open) {
        auto step = [&](int d) {
            int n = a.fabTab;
            for (int k = 0; k < 3; k++) {
                n = (n + d + 3) % 3;
                if (n != 2 || v.signed_in || !online) break;
            }
            a.fabTab = n;
            if (n == 0) { fab_load_library(); g_refreshLocal = true; }
            if (n == 2) fab_load_creator();
        };
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) step(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) step(1);
        if (!popupLast && !textLast && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) back = true;
    }

    // the body
    const ImVec2 c0(p0.x + S(26), ty + tabH + S(12)), c1(p1.x - S(26), p1.y - S(22));
    ImGui::PushFont(F_BODY, FS(F_BODY) * 0.9f);
    if (a.fabTab == 0) library_tab(a, v, c0, c1);
    else if (!online) { ImGui::SetCursorScreenPos(c0); note(kOnlineOff, col::amber); }   // Store and Creator: nothing goes out
    else if (a.fabTab == 1) store_tab(a, v, c0, c1);
    else creator_tab(a, v, c0, c1);
    popups(v);
    ImGui::PopFont();

    if (back) {
        if (a.fabTab == 1 && v.page_id) fab_open_page(0);   // a mod's page: back to the grid first
        else { a.zoomTo = SCR_FRONT; a.zoomStart = now; }
    }
    popupLast = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || a.modal.open;
    textLast = ImGui::GetIO().WantTextInput;
}
