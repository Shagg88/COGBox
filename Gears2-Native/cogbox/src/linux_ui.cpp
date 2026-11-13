// COGBox - Linux and Steam Deck through Proton / Wine, the launcher's side (proton049, 0.4.9): the recommended settings
// (first start, or the About tab's button), and adding a copy without drag and drop (the Steam Deck's Game Mode has no
// file manager to drag from): a folder picker, a pasted Linux path, and a search of the usual Linux folders through
// Wine's Z: drive. Detection and path conversion are in proton.cpp. Nothing here runs on Windows proper.
#include "app.h"
#include "i18n.h"
#include "proton.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

void ui_tip(const std::string &text_en);    // page.cpp
bool drop_is_game_or_update(const std::string &folder);   // install.cpp

std::string shown_path(const App &a, const std::string &p) { return proton::display_path(p, a.wine); }

// ---- the recommended settings -----------------------------------------------------------------------------------------
static std::string value_text(const gs::Option &o, double v)
{
    if (o.kind == gs::Kind::Choice) {
        const int k = (int)v;
        return k >= 0 && k < (int)o.choices.size() ? o.choices[(size_t)k] : std::to_string(k);
    }
    if (o.kind == gs::Kind::Bool) return v != 0 ? "on" : "off";
    char b[32];
    snprintf(b, sizeof b, "%g", v);
    return b;
}

static std::string recommended_text(App &a, int g)
{
    std::string t = std::string(tr(a.proton.steamDeck ? "These settings work best on the Steam Deck:" : "These settings work best under Proton / Wine:")) + "\n\n";
    for (const auto &kv : gs::proton_recommended((gs::Game)g, a.env[g])) {
        const gs::Option *o = gs::find_option((gs::Game)g, kv.first);
        if (!o || kv.first == "WineApi" || kv.first == "SceneMode") continue;   // (the details behind Vulkan / FSR)
        t += std::string(tr(o->label)) + ": " + tr(value_text(*o, kv.second)) + "\n";
    }
    if (a.proton.steamDeck) t += "\n" + std::string(tr("For 40 fps: set the frame rate limit to 40 and the screen's refresh rate to 40 Hz in Steam's Performance menu."));
    t += "\n\n" + std::string(tr("For every game. Nothing else is changed. Apply?"));
    return t;
}

static void recommended_apply(App &a)
{
    for (int g = 0; g < G_COUNT; g++) {
        for (const auto &kv : gs::proton_recommended((gs::Game)g, a.env[g]))
            if (gs::find_option((gs::Game)g, kv.first)) gs::set_value((gs::Game)g, a.cur[g], kv.first, kv.second);
        a.dirty[g] = true;
    }
    settings_save(a);
}

void linux_recommended_ask(App &a, int g)
{
    ask(a, "protonrec", g, a.proton.steamDeck ? "Recommended Steam Deck settings" : "Recommended Proton settings", recommended_text(a, g), { "Apply", "Cancel" });
}

void linux_start(App &a, bool hadSettings)
{
    if (!a.wine || a.mac) return;
    if (!a.proton.proton && !a.proton.steamDeck) return;   // plain Wine on a desktop: left as it is
    if (a.ini.geti("Linux", "RecommendedDone", 0)) return;
    a.ini.seti("Linux", "RecommendedDone", 1);
    a.ini.save();
    if (hadSettings) {   // the player's own settings (from Windows, or an earlier COGBox): never changed silently
        set_message(a, "Running under Proton. Recommended settings: About tab, \"Use recommended Steam Deck / Proton settings\".", false);
        return;
    }
    recommended_apply(a);
    set_message(a, a.proton.steamDeck ? "First start on the Steam Deck: the recommended settings are on. Change any of them as you like."
                                      : "First start under Proton: the recommended settings are on. Change any of them as you like.", false);
}

// ---- adding a copy without drag and drop ------------------------------------------------------------------------------
static std::atomic<int> g_scan{0};   // 0 idle, 1 searching, 2 done
static std::mutex g_scanMx;
static std::vector<std::string> g_found;

static void scan_thread(std::vector<std::string> roots)
{
    std::vector<std::filesystem::path> r;
    for (const std::string &x : roots) r.push_back(std::filesystem::path(widen(x)));
    auto isGame = [](const std::filesystem::path &p, bool dir) { return proton::quick_candidate(p, dir) && drop_is_game_or_update(narrow(p.wstring())); };
    std::vector<std::filesystem::path> f = proton::find_copies(r, isGame, 3, 6000);
    std::lock_guard<std::mutex> l(g_scanMx);
    g_found.clear();
    for (const auto &p : f) g_found.push_back(narrow(p.wstring()));
    g_scan = 2;
}

static void scan_start(App &a)
{
    if (g_scan == 1) return;
    std::vector<std::string> roots = proton::roots_here(a.proton);
    if (roots.empty()) { set_message(a, "The Linux folders can't be reached from here (no Z: drive).", true); return; }
    g_scan = 1;
    set_message(a, "Looking for your games in the usual Linux folders...", false);
    std::thread(scan_thread, std::move(roots)).detach();
}

void linux_poll(App &a)
{
    if (g_scan != 2 || a.modal.open) return;
    g_scan = 0;
    std::vector<std::string> found;
    {
        std::lock_guard<std::mutex> l(g_scanMx);
        found.swap(g_found);
    }
    if (found.empty()) {
        set_message(a, "No game copies found in the usual Linux folders. Use \"Choose a folder...\" or \"Paste a Linux path\".", true);
        return;
    }
    std::string data;
    for (const std::string &f : found) data += f + "\n";
    ask(a, "protonfound", -1, "Games found", "Choose the copy to add:", { "Add this copy", "Cancel" }, data);
    for (const std::string &f : found) a.modal.list.push_back(shown_path(a, f));
}

static void add_path(App &a, const std::string &winPath)
{
    if (winPath.empty()) return;
    if (!dir_exists(winPath) && !file_exists(winPath)) { set_message(a, "Not found: " + shown_path(a, winPath), true); return; }
    a.dropped.push_back(winPath);   // the same way as a dropped folder / file (main.cpp)
}

void linux_answer(App &a, const Modal &m)
{
    if (m.tag == "protonrec" && m.answer == 0) {
        recommended_apply(a);
        set_message(a, "Recommended settings applied to every game. Change any of them as you like.", false);
    } else if (m.tag == "protonfound" && m.answer == 0) {
        std::vector<std::string> paths;
        size_t at = 0, nl;
        while ((nl = m.data.find('\n', at)) != std::string::npos) { paths.push_back(m.data.substr(at, nl - at)); at = nl + 1; }
        if (m.listSel >= 0 && m.listSel < (int)paths.size()) add_path(a, paths[(size_t)m.listSel]);
    }
}

// 0.6.2 (Shagg 13 Nov: Steam Deck players can't drag and drop - "add the proper buttons necessary to avoid the issue",
// for anyone): every player gets a file picker, a folder picker and a pasted path - the same import as a drop
// (a.dropped). Under Proton / Wine the pasted path may be a Linux one, and the search of the Linux folders is offered.
float linux_copy_buttons_width(const App &a)   // for centring them
{
    float w = 0;
    int n = 0;
    for (const char *en : { "Choose a file...", "Choose a folder...", a.wine ? "Paste a Linux path" : "Paste a path", a.wine ? "Look for my games" : nullptr }) {
        if (!en) continue;
        w += std::max(T(90), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, tr(en)).x + S(28));
        ++n;
    }
    return w + ImGui::GetStyle().ItemSpacing.x * float(n - 1);
}

void linux_copy_buttons(App &a)
{
    const bool busy = a.job.state.load() == 1 || a.run.proc != nullptr;
    auto button = [&](const char *en) {
        const char *l = tr(en);
        const float w = std::max(T(90), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, l).x + S(28));
        return th_button(l, ImVec2(w, T(26)), !busy, F_BODY);
    };
    if (button("Choose a file...")) {
        // (title updates and DLC packages have no file extension: "All files" first)
        add_path(a, pick_file(L"Choose your disc image, Games on Demand file, title update or DLC package",
                              L"All files\0*.*\0Disc images (*.iso)\0*.iso\0Game executables (default.xex)\0*.xex\0"));
    }
    ui_tip(a.wine ? "A disc image (.iso), a Games on Demand file, a title update or a DLC package. Your Linux folders are under Z: (Z:\\home\\... is /home/...)."
                  : "A disc image (.iso), a Games on Demand file, a title update or a DLC package (those two have no file extension).");
    ImGui::SameLine();
    if (button("Choose a folder...")) add_path(a, pick_folder(L"Choose the folder of your game copy, title update or DLC"));
    ui_tip(a.wine ? "A folder with your game (default.xex), a title update or DLC. Your Linux folders are under Z: (Z:\\home\\... is /home/...)."
                  : "A folder with your game (default.xex), a title update or DLC.");
    ImGui::SameLine();
    if (!a.wine) {
        if (button("Paste a path")) {
            char *c = SDL_GetClipboardText();
            std::string text = c ? c : "";
            if (c) SDL_free(c);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
            while (!text.empty() && (text.front() == ' ')) text.erase(text.begin());
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
            if (text.empty()) set_message(a, "Copy a file's or folder's path first (in File Explorer: Shift + right-click > Copy as path).", true);
            else add_path(a, text);
        }
        ui_tip("Copy the path of your disc image, game folder, title update or DLC (File Explorer: Shift + right-click > Copy as path), then press this.");
        return;
    }
    if (button("Paste a Linux path")) {
        char *c = SDL_GetClipboardText();
        const std::string text = c ? c : "";
        if (c) SDL_free(c);
        const std::string w = proton::to_wine(text, a.proton.home);
        if (proton::clean_pasted(text).empty()) set_message(a, "Copy a folder's or disc image's path first (for example /home/deck/Games/Gears of War 2).", true);
        else if (w.empty()) set_message(a, "That isn't a folder path: " + proton::clean_pasted(text), true);
        else add_path(a, w);
    }
    ui_tip("Copy a path such as /home/deck/Games/Gears of War 2 or ~/Games/gears2.iso (from Dolphin: right-click > Copy Location), then press this.");
    ImGui::SameLine();
    if (button(g_scan == 1 ? "Looking..." : "Look for my games") && g_scan != 1) scan_start(a);
    ui_tip("Searches ~/Games, ~/Downloads, ~/Desktop, your Steam library folders, SD cards and USB drives (/run/media) and ~/.local/share for game copies, disc images and title updates.");
}
