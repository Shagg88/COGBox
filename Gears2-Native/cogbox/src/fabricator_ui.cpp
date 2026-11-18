// COGBox - the FABRICATOR screen: COGBox's local mod manager. Three tabs (LB / RB switch them):
//   LIBRARY  each game's mods in load order - on / off, up / down, open the folder, update and uninstall what COGBox
//            installed - and Add a mod (a .zip or a folder, chosen or dropped on the window);
//   STORE    a placeholder: mods from Nexus Mods come later;
//   CREATOR  a mod folder checked, its mod.ini edited in a form, a clean zip made, and how to publish it on Nexus Mods.
// It looks like a game's settings page (page.cpp's panel, tabs, sections and buttons). The long parts run on
// fabricator_state.cpp's worker; this file only draws and starts them. Nothing here goes on the network.
#include "app.h"
#include "theme.h"
#include "i18n.h"
#include "fabricator_state.h"
#include "fabricator_pack.h"
#include "mod_ini_edit.h"
#include "imgui_internal.h"   // ImGui::FindRenderedTextEnd

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>

// page.cpp's building blocks, so this screen looks like the settings pages
void ui_section(const char *title_en);
void ui_tip(const std::string &text_en);
void ui_note(const std::string &text_en, ImVec4 c = col::dim);
void ui_columns(const std::function<void(int)> &body);
float ui_label_w();
float ui_row_x1();
void ui_rows_begin();

namespace {

const int kFilterGames[] = { G_GEARS1, G_GEARS2, G_GEARS3, G_JUDGMENT };   // release order, as the front screen
const char *const kGameKeys[] = { "gears1", "gears2", "gears3", "judgment" };
const char *const kGameNames[] = { "Gears 1", "Gears 2", "Gears 3", "Judgment" };
const char *const kSideKeys[] = { "everyone", "host", "client" };
const char *const kSideNames[] = { "Everyone (every player needs it)", "Host only", "Client only" };
const char *const kTabs[] = { "LIBRARY", "STORE", "CREATOR" };
const float kRowText = 1.1f;   // page.cpp's k_rowText

ImVec4 hexc(unsigned v, float a = 1) { return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
std::string lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim(const std::string &s)
{
    size_t x = s.find_first_not_of(" \t\r\n"), y = s.find_last_not_of(" \t\r\n");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}
std::string base_name(const std::string &p)
{
    std::string s = p;
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const size_t at = s.find_last_of("\\/");
    return at == std::string::npos ? s : s.substr(at + 1);
}
std::string dir_of(const std::string &p)
{
    const size_t at = p.find_last_of("\\/");
    return at == std::string::npos ? std::string() : p.substr(0, at);
}
int key_index(const std::string &k)
{
    for (int i = 0; i < 4; i++) if (k == kGameKeys[i]) return i;
    return -1;
}
// the text cut with "..." to fit w (never inside a UTF-8 character)
std::string fit(ImFont *f, float fs, const std::string &t, float w)
{
    if (f->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x <= w) return t;
    std::string s = t;
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();     // a cut character's tail
        if (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0xC0) s.pop_back();        // ... and its lead byte
        if (f->CalcTextSizeA(fs, FLT_MAX, 0, (s + "...").c_str()).x <= w) return s + "...";
    }
    return "";
}

// ---- small widgets (page.cpp's sizes) --------------------------------------------------------------------------------
float btn_w(const char *label_en)
{
    const char *l = tr(label_en);
    return std::max(T(90), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, l, ImGui::FindRenderedTextEnd(l)).x + S(28));
}
bool btn(const char *label_en, bool enabled = true)   // page.cpp's small_button, with an enabled state
{
    return th_button(label_en, ImVec2(btn_w(label_en), T(26)), enabled, F_BODY);
}
float row_btn_w(const char *label_en)   // a LIBRARY row's buttons: narrower, so the name keeps its room
{
    const char *l = tr(label_en);
    return std::max(T(60), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, l, ImGui::FindRenderedTextEnd(l)).x + S(22));
}
int str_cb(ImGuiInputTextCallbackData *d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string *s = (std::string *)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = &(*s)[0];
    }
    return 0;
}
bool input(const char *id, std::string &s, const char *hint, float w)
{
    ImGui::SetNextItemWidth(w);
    if (s.capacity() < 16) s.reserve(16);
    return ImGui::InputTextWithHint(id, tr(hint), &s[0], s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, str_cb, &s);
}
// mod.ini keeps a value on one line: the box wraps long text, Enter and pasted line breaks never go in
int one_line_cb(ImGuiInputTextCallbackData *d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) return d->EventChar == '\n' || d->EventChar == '\r';
    return str_cb(d);
}
bool input_multi(const char *id, std::string &s, ImVec2 size)
{
    if (s.capacity() < 16) s.reserve(16);
    const bool changed = ImGui::InputTextMultiline(id, &s[0], s.capacity() + 1, size,
                                                   ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_WordWrap,
                                                   one_line_cb, &s);
    for (char &c : s) if (c == '\n' || c == '\r') c = ' ';   // (whatever got past the filter)
    return changed;
}
// a form row: the label, then the control at the column's label width (as page.cpp's option rows)
void form_label(const char *label_en)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label_en));
    ImGui::SameLine(std::min(ui_label_w(), S(220)));
}
float form_ctrl_w() { return std::max(S(160), ui_row_x1() - ImGui::GetCursorScreenPos().x); }

// a word in a rounded box; kind: 0 grey, 1 green, 2 amber, 4 blue. Returns its width.
float pill(ImDrawList *dl, ImVec2 p, const std::string &t, int kind)
{
    static const unsigned bg[] = { 0x2a2d32, 0x2c3d24, 0x3f3018, 0x3d1f22, 0x1f2d3d }, fg[] = { 0x968f88, 0x7cb262, 0xe2a034, 0xe0686c, 0x7fa8d0 };
    const float fs = FS(F_SMALL) * 0.9f;
    const ImVec2 ts = F_SMALL->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str());
    const ImVec2 b(p.x + ts.x + S(16), p.y + ts.y + S(4));
    dl->AddRectFilled(p, b, U(hexc(bg[kind])), (b.y - p.y) * 0.5f);
    dl->AddText(F_SMALL, fs, ImVec2(p.x + S(8), p.y + S(2)), U(hexc(fg[kind])), t.c_str());
    return b.x - p.x;
}

void open_game_mods(App &a, int g) { make_dirs(mods_dir(g)); open_folder(a, mods_dir(g)); }

// ---- LIBRARY ----------------------------------------------------------------------------------------------------------
const wchar_t *const kZipFilter = L"Zip files\0*.zip\0All files\0*.*\0";

std::string phase_text(const FabJob &j)
{
    const int64_t total = j.progress.total, done = j.progress.done;
    switch (j.phase.load()) {
    case FabJob::Queued: return tr("Waiting");
    case FabJob::Packing: return total > 0 ? trf("Packing the folder {1}%", { std::to_string((int)std::min<int64_t>(100, done * 100 / total)) }) : tr("Packing the folder");
    case FabJob::Checking: return tr("Checking the zip");
    case FabJob::Checked: return tr("Checked");
    case FabJob::Asking: return tr("Waiting for your answer");
    case FabJob::Waiting: return tr("Waiting for the game to close");
    case FabJob::Unpacking: return tr("Unpacking");
    case FabJob::Unpacked: return tr("Waiting for the game to close");
    default: return "";
    }
}

void jobs_section(App &a)
{
    const auto jobs = fab_jobs();
    if (jobs.empty()) return;
    ui_section("WORKING");
    for (size_t i = 0; i < jobs.size(); i++) {
        const auto &j = jobs[i];
        ImGui::PushID(j->id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(j->name.c_str());
        ImGui::SameLine(0, S(14));
        ImGui::PushStyleColor(ImGuiCol_Text, col::amber);
        ImGui::TextUnformatted(phase_text(*j).c_str());
        ImGui::PopStyleColor();
        const int64_t total = j->progress.total, done = j->progress.done;
        const float bx1 = ui_row_x1() - (fab_can_cancel(*j) ? btn_w("Cancel") + S(10) : 0);
        const float bx0 = std::max(ImGui::GetItemRectMax().x + S(16), bx1 - S(260));
        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        const float y = (r0.y + r1.y) * 0.5f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (bx1 - bx0 > S(40)) {
            dl->AddRectFilled(ImVec2(bx0, y - S(3)), ImVec2(bx1, y + S(3)), U(col::faint, 0.5f), S(3));
            const int ph = j->phase;
            const bool working = ph == FabJob::Packing || ph == FabJob::Checking || ph == FabJob::Unpacking;
            if (ph == FabJob::Packing && total > 0) {
                const float f = (float)std::min<int64_t>(total, done) / (float)total;
                dl->AddRectFilled(ImVec2(bx0, y - S(3)), ImVec2(bx0 + (bx1 - bx0) * f, y + S(3)), U(col::amber), S(3));
            } else if (working) {   // no figure: a band going along
                const float t = (float)fmod(ImGui::GetTime() * 0.6, 1.0), w = (bx1 - bx0) * 0.25f, x = bx0 + (bx1 - bx0 - w) * t;
                dl->AddRectFilled(ImVec2(x, y - S(3)), ImVec2(x + w, y + S(3)), U(col::amber), S(3));
            }
        }
        if (fab_can_cancel(*j)) {
            ImGui::SameLine(ui_row_x1() - ImGui::GetWindowPos().x + ImGui::GetScrollX() - btn_w("Cancel"));
            if (btn("Cancel")) fab_cancel(j);
        }
        ImGui::PopID();
    }
    (void)a;
}

void library_row(App &a, int g, const FabLibRow &r, size_t i, size_t n)
{
    const ModItem &m = r.m;
    ImGui::PushID((int)i + 9000);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float rw = ui_row_x1() - r0.x;
    // the lines under the name: what the game makes of it (mods.cpp apply_mod_rules), what mod.ini has it doesn't know
    std::vector<std::pair<std::string, ImVec4>> lines;
    if (m.enabled && !m.problem.empty()) lines.push_back({ trf("Left out by the game: {1}", { m.problem }), col::bad });
    for (const std::string &x : m.notes) lines.push_back({ "mod.ini: " + x, col::amber });
    const float lineH = FS(F_SMALL) * kRowText + S(3);
    const float top = T(54), rh = top + (lines.empty() ? 0 : lines.size() * lineH + S(6));
    const ImVec2 r1(r0.x + rw, r0.y + rh);
    dl->AddRectFilled(r0, r1, U(hexc(0x16181b), 0.8f), S(4));
    dl->AddRect(r0, r1, U(hexc(0x2f3338)), S(4));
    const bool gameRuns = a.run.proc != nullptr || any_game_exe_running();   // review r369: started outside COGBox too

    // the buttons, right to left: Uninstall and Update (COGBox's installs only), Open folder, Down, Up
    struct B { const char *label; bool enabled; int what; };
    std::vector<B> bs;
    if (r.rec) { bs.push_back({ "Uninstall", !gameRuns, 4 }); bs.push_back({ "Update...", !gameRuns, 3 }); }
    bs.push_back({ "Open folder", true, 2 });
    bs.push_back({ "Down", !gameRuns && i + 1 < n, 1 });
    bs.push_back({ "Up", !gameRuns && i > 0, 0 });
    float bx = r1.x - S(10);
    std::vector<float> xs(bs.size());
    for (size_t k = 0; k < bs.size(); k++) { bx -= row_btn_w(bs[k].label); xs[k] = bx; bx -= S(6); }

    // the switch, the name, the line under it
    ImGui::SetCursorScreenPos(ImVec2(r0.x + S(10), r0.y + (top - ImGui::GetFrameHeight()) * 0.5f));
    if (gameRuns) ImGui::BeginDisabled();
    bool on = m.enabled;
    if (ImGui::Checkbox("##on", &on)) {
        mods_set_enabled(a, g, m.folder, on);
        env_refresh(a, g);
        fab_library_changed();
    }
    if (gameRuns) ImGui::EndDisabled();
    ui_tip(gameRuns ? "Close the game first." : "On: the game uses it. Changes apply the next time the game starts.");
    const float tx = ImGui::GetItemRectMax().x + S(12), textW = bx - S(10) - tx;
    std::string head = m.name + (m.version.empty() ? "" : "  " + m.version);
    const float nfs = FS(F_BOLD) * 0.95f;
    dl->AddText(F_BOLD, nfs, ImVec2(tx, r0.y + S(7)), U(m.enabled ? col::text : col::dim), fit(F_BOLD, nfs, head, textW).c_str());
    float x = tx;
    const float sy = r0.y + S(11) + nfs, sfs = FS(F_SMALL) * 0.95f;
    auto sub = [&](const std::string &t, ImVec4 c) {
        if (t.empty() || x > tx + textW) return;
        const std::string f = fit(F_SMALL, sfs, t, tx + textW - x);
        dl->AddText(F_SMALL, sfs, ImVec2(x, sy), U(c), f.c_str());
        x += F_SMALL->CalcTextSizeA(sfs, FLT_MAX, 0, f.c_str()).x + S(12);
    };
    if (!m.author.empty()) sub(trf("by {1}", { m.author }), col::dim);
    std::string type = r.type.empty() && !m.campaign.empty() ? std::string(tr("Campaign")) : r.type;
    if (!type.empty() && type[0] >= 'a' && type[0] <= 'z') type[0] = (char)(type[0] - 'a' + 'A');   // "gameplay" -> "Gameplay"
    if (!type.empty()) sub(type, col::dim);
    if (m.textures && x < tx + textW - S(80)) {
        const std::string t = m.textures == 1 ? std::string(tr("Texture pack: 1 texture")) : trf("Texture pack: {1} textures", { std::to_string(m.textures) });
        x += pill(dl, ImVec2(x, sy - S(1)), t, 4) + S(12);
    }
    const std::string src = !r.rec ? tr("Hand-added") : r.rec->source.kind == "nexus" ? tr("From Nexus Mods") : tr("Added from a file");
    const float srcX = x;
    sub(src, r.rec ? col::good : col::faint);
    if (ImGui::IsMouseHoveringRect(ImVec2(srcX, sy), ImVec2(x, sy + sfs)) && !r.rec) {
        ImGui::SetTooltip("%s", tr("Put in the mods folder by hand, not added by COGBox - COGBox never deletes it. To remove it, delete its folder yourself (Open folder)."));
    } else if (ImGui::IsMouseHoveringRect(ImVec2(r0.x, r0.y), ImVec2(tx + textW, r0.y + top)) && !m.description.empty()) {
        ImGui::SetTooltip("%s", m.description.c_str());
    }

    for (size_t k = 0; k < bs.size(); k++) {
        ImGui::SetCursorScreenPos(ImVec2(xs[k], r0.y + (top - T(26)) * 0.5f));
        if (th_button(bs[k].label, ImVec2(row_btn_w(bs[k].label), T(26)), bs[k].enabled, F_BODY)) {
            switch (bs[k].what) {
            case 0: case 1:
                mods_move(a, g, m.folder, bs[k].what == 0 ? -1 : 1);
                env_refresh(a, g);
                fab_library_changed();
                break;
            case 2: open_folder(a, m.path); break;
            case 3: {
                const std::string zip = pick_file(L"Choose the mod's new .zip", kZipFilter);
                if (!zip.empty()) fab_update_from(a, g, m.folder, zip);
                break;
            }
            case 4: fab_uninstall_ask(a, g, m); break;
            }
        }
        if (!bs[k].enabled && gameRuns) ui_tip("Close the game first.");
        else if (bs[k].what == 3) ui_tip("Choose the new version's .zip. Your on / off choice and the order stay as they are; the old version is kept in the mods folder's .fabricator-backup.");
        else if (bs[k].what == 4) ui_tip("Moves the mod to the mods folder's .fabricator-backup (it can be put back by hand).");
        else if (bs[k].what <= 1) ui_tip("The lower mod wins where two change the same file.");
    }
    float ly = r0.y + top;
    for (auto &l : lines) {
        const std::string f = fit(F_SMALL, FS(F_SMALL) * kRowText, l.first, rw - S(20) - (tx - r0.x));
        dl->AddText(F_SMALL, FS(F_SMALL) * kRowText, ImVec2(tx, ly), U(l.second), f.c_str());
        ly += lineH;
    }
    ImGui::SetCursorScreenPos(r0);
    ImGui::Dummy(ImVec2(rw, rh));
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PopID();
}

void library_tab(App &a)
{
    const int g = fab_library_game(a);
    // the game filter, and Add a mod
    {
        int sel = 0;
        for (int i = 0; i < 4; i++) if (kFilterGames[i] == g) sel = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Game"));
        ImGui::SameLine(0, S(12));
        ImGui::SetNextItemWidth(T(200));
        const char *names[4];
        for (int i = 0; i < 4; i++) names[i] = tr(kGameNames[i]);
        if (ImGui::Combo("##fabgame", &sel, names, 4)) fab_library_set_game(kFilterGames[sel]);
        ImGui::SameLine(0, S(24));
        if (btn("Add a mod (.zip)...")) {
            const std::string p = pick_file(L"Choose a mod's .zip", kZipFilter);
            if (!p.empty()) fab_add_path(a, p);
        }
        ui_tip("The mod is checked first, then added, turned on and put last in the order. A mod already added (the same mod.ini Name) is offered as an update.");
        ImGui::SameLine();
        if (btn("Add a mod folder...")) {
            const std::string p = pick_folder(L"Choose a mod's folder (laid out like the game's files, with its mod.ini)");
            if (!p.empty()) fab_add_path(a, p);
        }
        ui_tip("The folder is packed and checked like a zip, then added. The folder you chose stays as it is.");
        ImGui::SameLine();
        if (btn("Mods folder")) open_game_mods(a, g);
        ui_tip(trf("Open {1}'s mods folder.", { game_def(g).shortName }));
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    ui_note("Or drop a mod's .zip or folder on this window. A mod.ini for another game adds it to that game.");
    jobs_section(a);

    ui_section("LOAD ORDER");
    const std::vector<FabLibRow> rows = fab_library(a, g);   // (a copy: a row's action reads the folder again)
    if (rows.empty()) ui_note(trf("No mods for {1} yet - add one above.", { game_def(g).shortName }));
    else ui_note("Top to bottom: where two mods change the same file, the lower one wins. Changes apply the next time the game starts.");
    ImGui::Dummy(ImVec2(0, S(4)));
    for (size_t i = 0; i < rows.size(); i++) library_row(a, g, rows[i], i, rows.size());
}

// ---- STORE ------------------------------------------------------------------------------------------------------------
void store_tab(App &a)
{
    ui_section("MODS FROM NEXUS MODS - COMING SOON");
    ui_note("Soon you'll browse and get mods for every Gears game from Nexus Mods here. Until then, add a mod you downloaded with Add a mod on the LIBRARY tab.", col::text);
    ImGui::Dummy(ImVec2(0, S(8)));
    ui_note("Making a mod? Publish it on Nexus Mods with the COGBox tag so players can find it.");
#ifdef COGBOX_FAB_NEXUS
    ui_section("CHECK FOR UPDATES");
    ImGui::Dummy(ImVec2(0, S(40)));
#endif
    (void)a;
}

// ---- CREATOR ----------------------------------------------------------------------------------------------------------
std::string g_crFolder;
bool g_crHadIni = false;
ModIniFields g_crSaved, g_crForm;   // as mod.ini has it now, and the form
std::string g_crNeeds;              // the form's Requires, as typed
std::shared_ptr<FabJob> g_crCheck, g_crPack;

std::string needs_text(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}
std::vector<std::string> needs_list(const std::string &s)
{
    std::vector<std::string> v;
    size_t at = 0;
    while (at <= s.size()) {
        size_t c = s.find(',', at);
        if (c == std::string::npos) c = s.size();
        const std::string t = trim(s.substr(at, c - at));
        if (!t.empty()) v.push_back(t);
        at = c + 1;
    }
    return v;
}
bool same_fields(const ModIniFields &x, const ModIniFields &y)
{
    return x.name == y.name && x.game == y.game && x.version == y.version && x.author == y.author && x.description == y.description &&
           x.side == y.side && x.needs == y.needs;
}
std::string check_game(const App &a)   // the game the check is for: mod.ini's (modcore: none = gears3)
{
    (void)a;
    return g_crSaved.game.empty() ? std::string("gears3") : lower(g_crSaved.game);
}
void creator_check(const App &a)
{
    if (!g_crFolder.empty()) g_crCheck = fab_check_start(g_crFolder, check_game(a));
}
void creator_choose(App &a, const std::string &folder)
{
    g_crFolder = folder;
    g_crPack.reset();
    auto f = mod_ini_read(folder);
    g_crHadIni = f.has_value();
    g_crSaved = f ? *f : ModIniFields();
    if (!f) {   // a new mod.ini: the LIBRARY's game, and the folder's name
        g_crSaved.game = game_def(fab_library_game(a)).key;
        g_crSaved.name = base_name(folder);
        g_crSaved.version = "1.0";
    }
    g_crForm = g_crSaved;
    g_crNeeds = needs_text(g_crForm.needs);
    if (!f) g_crSaved = ModIniFields();   // (nothing saved yet: the form is "changed" until Save)
    // a mod.ini without Name or Game: the form suggests them (the check lists them until they are saved)
    if (trim(g_crForm.name).empty()) g_crForm.name = base_name(folder);
    if (key_index(lower(trim(g_crForm.game))) < 0) g_crForm.game = game_def(fab_library_game(a)).key;
    creator_check(a);
}

void creator_folder_section(App &a)
{
    ui_section("YOUR MOD'S FOLDER");
    if (btn("Choose your mod's folder...")) {
        const std::string p = pick_folder(L"Choose your mod's folder (laid out like the game's files)");
        if (!p.empty()) creator_choose(a, p);
    }
    ui_tip("The folder you work on: laid out like the game's own files (GearGame\\...), with its mod.ini. Or drop it on this window.");
    if (!g_crFolder.empty()) {
        ImGui::SameLine();
        if (btn("Open folder")) open_folder(a, g_crFolder);
        ui_note(base_name(g_crFolder), col::text);
        ui_tip(g_crFolder);
    } else ui_note("No folder chosen yet. Choose one, or drop it on this window.");
}

void creator_check_section(App &a)
{
    if (g_crFolder.empty()) return;
    ui_section("CHECK");
    if (!g_crCheck || fab_job_running(*g_crCheck)) ui_note("Checking the folder...");
    else if (g_crCheck->problems.empty()) ui_note("Ready to package", col::good);
    else {
        ui_note(g_crCheck->problems.size() == 1 ? std::string("1 thing to fix before packaging:") : trf("{1} things to fix before packaging:", { std::to_string(g_crCheck->problems.size()) }), col::amber);
        for (size_t i = 0; i < g_crCheck->problems.size() && i < 40; i++) ui_note("- " + g_crCheck->problems[i], col::bad);
        if (g_crCheck->problems.size() > 40) ui_note(trf("... and {1} more", { std::to_string(g_crCheck->problems.size() - 40) }), col::bad);
    }
    const bool busy = g_crCheck && fab_job_running(*g_crCheck);
    if (btn("Check again", !busy)) creator_check(a);
    ui_tip("What a clean package needs: a mod.ini with Name and Game, no program files, safe file names (no links), and the limits. The zip's 4 GB limit is checked when the zip is made.");
}

void creator_form_section(App &a)
{
    if (g_crFolder.empty()) return;
    ui_section("MOD.INI");
    ImGui::PushID("crform");
    form_label("Name");
    input("##name", g_crForm.name, "The mod's name players see", form_ctrl_w());
    form_label("Game");
    {
        int gi = std::max(0, key_index(lower(g_crForm.game)));
        const char *names[4];
        for (int i = 0; i < 4; i++) names[i] = tr(kGameNames[i]);
        ImGui::SetNextItemWidth(form_ctrl_w());
        if (ImGui::Combo("##game", &gi, names, 4)) g_crForm.game = kGameKeys[gi];
    }
    form_label("Version");
    input("##version", g_crForm.version, "1.0", form_ctrl_w());
    form_label("Author");
    input("##author", g_crForm.author, "Your name", form_ctrl_w());
    form_label("Description");
    input_multi("##desc", g_crForm.description, ImVec2(form_ctrl_w(), T(60)));
    form_label("Side");
    {
        int si = 0;
        for (int i = 0; i < 3; i++) if (lower(g_crForm.side) == kSideKeys[i]) si = i;
        const char *names[3];
        for (int i = 0; i < 3; i++) names[i] = tr(kSideNames[i]);
        ImGui::SetNextItemWidth(form_ctrl_w());
        if (ImGui::Combo("##side", &si, names, 3)) g_crForm.side = kSideKeys[si];
        ui_tip("Who needs the mod in multiplayer: everyone, only the host, or only this player (menus, HUD, text).");
    }
    form_label("Requires");
    input("##needs", g_crNeeds, "Other Mod, Another Mod", form_ctrl_w());
    ui_tip("The mods that must be on too, by name, comma-separated. The game leaves this mod out without them.");
    g_crForm.needs = needs_list(g_crNeeds);
    ImGui::PopID();
    const bool dirty = !same_fields(g_crForm, g_crSaved) || !g_crHadIni;
    ImGui::Dummy(ImVec2(0, S(2)));
    if (btn("Save mod.ini", dirty)) {
        if (mod_ini_write(g_crFolder, g_crForm)) {
            auto f = mod_ini_read(g_crFolder);
            g_crHadIni = f.has_value();
            g_crSaved = f ? *f : g_crForm;
            g_crForm = g_crSaved;
            g_crNeeds = needs_text(g_crForm.needs);
            set_message(a, "mod.ini is saved.", false);
            creator_check(a);
        } else set_message(a, "mod.ini couldn't be saved (is the folder read-only, or is mod.ini saved as UTF-16 or with old Mac line endings? Save it as UTF-8 first).", true);
    }
    ui_tip("Only these keys change; everything else in mod.ini (other sections, comments) stays as it is.");
    if (dirty && g_crHadIni) { ImGui::SameLine(); ui_note("Not saved yet", col::amber); }
}

void creator_zip_section(App &a)
{
    ui_section("MAKE A ZIP");
    ui_note("A clean zip of the folder: no fabricator.json, no backups, no program files.");
    const bool dirty = !same_fields(g_crForm, g_crSaved) || !g_crHadIni;
    const bool checked = g_crCheck && !fab_job_running(*g_crCheck);
    const bool clean = checked && g_crCheck->problems.empty();
    const bool packing = g_crPack && fab_job_running(*g_crPack);
    const bool can = !g_crFolder.empty() && !dirty && clean && !packing;
    if (btn("Make a zip...", can)) {
        const std::string dest = pick_save_file(L"Save the mod's zip", L"Zip files\0*.zip\0", fab_package_name(g_crSaved), L"zip");
        if (!dest.empty()) g_crPack = fab_pack_start(g_crFolder, dest, base_name(dest));
    }
    ui_tip(g_crFolder.empty() ? "Choose your mod's folder first." : dirty ? "Save mod.ini first." : !checked ? "Wait for the check." :
           !clean ? "Fix what the check lists first." : packing ? "A zip is being made." : "Choose where to save it. Its name is the mod's name and version.");
    if (g_crPack) {
        const int ph = g_crPack->phase;
        if (packing) {
            const int64_t t = g_crPack->progress.total, d = g_crPack->progress.done;
            ui_note(t > 0 ? trf("Packing {1}%", { std::to_string((int)std::min<int64_t>(100, d * 100 / t)) }) : std::string(tr("Packing...")), col::amber);
            if (btn("Cancel##pack")) fab_cancel(g_crPack);
        } else if (ph == FabJob::Done) {
            ui_note(trf("Made {1}.", { g_crPack->name }), col::good);
            if (btn("Open folder##zip")) { if (a.wine) open_folder(a, dir_of(g_crPack->zip)); else shell_select(g_crPack->zip); }
        } else if (ph == FabJob::Failed) ui_note(trf("The zip couldn't be made: {1}.", { g_crPack->problems.empty() ? std::string("?") : g_crPack->problems[0] }), col::bad);
        else if (ph == FabJob::Cancelled) ui_note("Cancelled - no zip was made.");
    }
}

void creator_publish_section()
{
    ui_section("PUBLISH ON NEXUS MODS");
    ui_note("1. Make a mod page for your game on Nexus Mods.", col::text);
    ui_note("2. Upload this zip as the main file.", col::text);
    ui_note("3. Add the COGBox tag.", col::text);
    ui_note("4. Say which COGBox version it needs.", col::text);
    ui_note(trf("This is COGBox {1}.", { COGBOX_VERSION }));
}

void creator_tab(App &a)
{
    // left: the folder, its check, the zip and the steps after it; right: the mod.ini form (the tallest part)
    ui_columns([&](int side) {
        if (side <= 0) {
            creator_folder_section(a);
            creator_check_section(a);
            creator_zip_section(a);
            creator_publish_section();
        }
        if (side != 0) creator_form_section(a);
    });
}

}  // namespace

void fabricator_drop(App &a, const std::string &path)
{
    if (a.fabTab == 2 && dir_exists(path)) { creator_choose(a, path); return; }   // the mod to package
    a.fabTab = 0;   // a mod to add: the LIBRARY shows it working
    fab_add_path(a, path);
}

void fabricator_draw(App &a, ImVec2 size)
{
    const double now = ImGui::GetTime();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // page.cpp's panel (#366's bigger window)
    const float pw = std::min(size.x - S(100), S(1540)), ph = std::min(size.y - S(40), S(940));
    const ImVec2 p0((size.x - pw) * 0.5f, (size.y - ph) * 0.5f), p1(p0.x + pw, p0.y + ph);
    th_plate(dl, p0, p1, S(18), false, 0.45f);
    th_plate_fill(dl, ImVec2(p0.x + S(6), p0.y + S(6)), ImVec2(p1.x - S(6), p1.y - S(6)), S(14), U(ImVec4(0.02f, 0.025f, 0.03f, 0.30f)));

    // the header: a cog, the name, what it is
    const float hh = std::max(S(108), S(22) + FS(F_TITLE) + FS(F_BODY) + S(16));
    th_cog(dl, ImVec2(p0.x + S(60), p0.y + S(58)), S(30), 10, (float)now * 0.12f, U(col::redHi, 0.9f), U(ImVec4(0.03f, 0.03f, 0.035f, 1)));
    dl->AddText(F_TITLE, FS(F_TITLE), ImVec2(p0.x + S(112), p0.y + S(22)), U(col::text), tr("FABRICATOR"));
    dl->AddText(F_BODY, FS(F_BODY), ImVec2(p0.x + S(114), p0.y + S(22) + FS(F_TITLE) + S(2)), U(col::dim),
                tr("Your mods for every Gears game, on this PC"));

    // the tabs (as page.cpp's: the same width)
    const float tx = p0.x + S(24), ty = p0.y + hh, tw = (pw - S(48)) / 8, tabH = T(30);
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(ImVec2(tx + i * tw, ty));
        if (th_tab(kTabs[i], a.fabTab == i, ImVec2(tw - S(4), tabH))) a.fabTab = i;
    }
    {   // a running job, right of the tabs
        for (const auto &j : fab_jobs()) {
            if (!fab_job_running(*j)) continue;
            const std::string s = j->name + "  " + phase_text(*j);
            const float maxW = p1.x - S(24) - (tx + 3 * tw + S(16));
            const std::string f = fit(F_SMALL, FS(F_SMALL), s, maxW);
            const float w = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, f.c_str()).x;
            dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(p1.x - S(24) - w, ty + (tabH - FS(F_SMALL)) * 0.5f), U(col::amber), f.c_str());
            break;
        }
    }
    const bool padFree = !a.modal.open && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !pad_capturing(a) && a.zoomTo == SCR_FABRICATOR;
    if (padFree && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) a.fabTab = (a.fabTab + 2) % 3;
    if (padFree && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) a.fabTab = (a.fabTab + 1) % 3;
    a.fabTab = std::max(0, std::min(2, a.fabTab));
    dl->AddLine(ImVec2(p0.x + S(24), ty + tabH), ImVec2(p1.x - S(24), ty + tabH), U(col::redHi, 0.7f), S(2));

    // the body
    const float barH = S(64);
    const ImVec2 c0(p0.x + S(30), ty + tabH + S(10)), c1(p1.x - S(24), p1.y - barH - S(8));
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("fabrows", ImVec2(c1.x - c0.x, c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushFont(F_BODY, FS(F_BODY) * kRowText);
    ui_rows_begin();
    if (a.fabTab == 0) library_tab(a);
    else if (a.fabTab == 1) store_tab(a);
    else creator_tab(a);
    ImGui::PopFont();
    ImGui::EndChild();

    // the bar: BACK (Esc / B do the same: main.cpp)
    const float by = p1.y - barH;
    dl->AddLine(ImVec2(p0.x + S(24), by), ImVec2(p1.x - S(24), by), U(col::edge, 0.35f), S(1));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(24), by + S(12)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (th_button("BACK", ImVec2(S(130), S(40)), true, F_HEAD)) { a.zoomTo = SCR_FRONT; a.zoomStart = now; }
    ImGui::PopItemFlag();
    ui_tip(a.padConnected ? std::string("Back to the library (") + pad_name(a, "B") + ")" : "Back to the library (Esc)");
    {
        const std::string hint = a.fabTab == 2 ? tr("Drop your mod's folder here to package it.") : tr("Drop a mod's .zip or folder here to add it.");
        const ImVec2 ss = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, hint.c_str());
        const float midL = p0.x + S(170), midR = p1.x - S(24);
        if (ss.x < midR - midL) dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(midL + (midR - midL - ss.x) * 0.5f, by + S(24)), U(col::dim), hint.c_str());
    }
}
