// COGBox - one page per game: its box and name, status lamps, the settings in tabs (built from gears_settings'
// option list, so a new option there shows up here), the About tab, and a bar with Library, a summary, Save and Play.
#include "app.h"
#include "theme.h"
#include "update.h"
#include "imgui_internal.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

// ---- the PC, for the settings ---------------------------------------------------------------------------------------
// Searching a 100 MB exe for a word takes a moment, so each (file, word) answer is kept.
static bool file_has(const std::string &path, const std::string &needle)
{
    static std::map<std::string, bool> cache;
    std::string key = path + "|" + needle + "|" + std::to_string(file_size(path));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    bool found = false;
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h != INVALID_HANDLE_VALUE && !needle.empty()) {
        std::string lo = needle;
        for (char &c : lo) c = (char)tolower((unsigned char)c);
        std::vector<char> buf(1 << 22);
        std::string carry;
        DWORD got = 0;
        while (!found && ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
            std::string chunk = carry + std::string(buf.data(), got);
            for (char &c : chunk) c = (char)tolower((unsigned char)c);
            found = chunk.find(lo) != std::string::npos;
            carry = chunk.size() >= lo.size() ? chunk.substr(chunk.size() - lo.size() + 1) : chunk;
        }
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    cache[key] = found;
    return found;
}

static std::string runtime_dll(const std::string &exe)
{
    std::string dir = exe.substr(0, exe.find_last_of('\\'));
    for (const char *n : { "rexruntimerd.dll", "rexruntime.dll" })
        if (file_exists(join(dir, n))) return join(dir, n);
    return "";
}

void env_refresh(App &a, int g)
{
    GameState &st = a.st[g];
    gs::Environment &e = a.env[g];
    // a folder path ending in a backslash ("E:\") would end its quoted argument as \" - which Windows reads as a quote
    auto safe = [](std::string p) { if (!p.empty() && (p.back() == '\\' || p.back() == '/')) p += "."; return p; };
    e.game_data_root = safe(st.dataDir);
    e.update_data_root = safe(st.updateDir);
    e.user_data_root = game_user_dir(g);
    e.shader_cache_dir = game_shader_dir(g);
    e.update_ready = !st.updateDir.empty() && (game_def(g).updateNeeded ? !st.portExeTu.empty() : true);
    e.g2_disc_ini_edits = g == G_GEARS2 && !st.portExeDisc.empty() && file_has(st.portExeDisc, "~=");
    e.gpus = a.gpus;
    e.gpu_name = gs::main_gpu(a.gpus);
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    e.screen_count = std::max(1, n);
    // the screen the game will use: Monitor 1..N when set and present, else the main one
    int mon = a.cur[g].i("Monitor");
    SDL_DisplayID use = SDL_GetPrimaryDisplay();
    if (ids && mon >= 1 && mon <= n) use = ids[mon - 1];
    const SDL_DisplayMode *dm = use ? SDL_GetDesktopDisplayMode(use) : nullptr;
    e.refresh_hz = dm && dm->refresh_rate > 0 ? (int)lroundf(dm->refresh_rate) : 0;
    const char *dn = use ? SDL_GetDisplayName(use) : nullptr;
    e.monitor_device = dn ? dn : "";
    if (ids) SDL_free(ids);
    e.has_avx2 = a.hasAvx2;
    e.wine = a.wine;   // job 523
    e.extra_args_lines.clear();
    std::string extra;
    if (read_file(join(exe_dir(), "extra-args.txt"), extra, 1 << 20)) {
        size_t at = 0;
        while (at < extra.size()) {
            size_t nl = extra.find('\n', at);
            std::string l = extra.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            e.extra_args_lines.push_back(l);
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
    }
    // what the exe Play would start can do
    gs::Version ver = gs::effective_version((gs::Game)g, a.cur[g], e);
    std::string exe = gs::exe_for_update((gs::Game)g, ver, e) ? st.portExeTu : st.portExeDisc;
    if (exe.empty()) exe = st.portExeDisc.empty() ? st.portExeTu : st.portExeDisc;
    st.useUpdate = !st.portExeTu.empty() && exe == st.portExeTu && !st.updateDir.empty();
    if (game_def(g).id == G_JUDGMENT) st.useUpdate = !st.updateDir.empty();
    e.cap.exe_has = [exe](const std::string &s) { return !exe.empty() && file_has(exe, s); };
    e.cap.runtime_has = [exe](const std::string &s) { std::string r = runtime_dll(exe); return !r.empty() && file_has(r, s); };
    // beside the exe, or in the release's shared folder (<COGBox>\shared), which play.cpp puts on the game's PATH: the
    // FSR / XeSS / frame generation DLLs live there in the release zip
    e.cap.file_next_to_exe = [exe](const std::string &name) {
        if (exe.empty()) return false;
        if (file_exists(join(exe.substr(0, exe.find_last_of('\\')), name))) return true;
        std::string shared = join(exe_dir(), "shared");
        return dir_exists(shared) && file_exists(join(shared, name));
    };
}

bool settings_save(App &a)
{
    for (int g = 0; g < G_COUNT; g++)
        if (a.dirty[g]) { a.store.commit((gs::Game)g, a.cur[g], a.env[g]); a.dirty[g] = false; }
    a.store.last_game = a.page;
    bool ok = write_file(join(exe_dir(), "settings.ini"), a.store.save_ini());
    if (!ok) set_message(a, "The settings couldn't be saved - is the COGBox folder read-only?", true);
    return ok;
}

// ---- rows ------------------------------------------------------------------------------------------------------------
static float g_labelW, g_ctrlW, g_rowX1;
static const float k_rowText = 1.1f;    // the rows' text, bigger than the body font (Shagg 21:44: hard to read)

static void row_label(const char *label, const std::string &desc, bool enabled)
{
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? col::text : col::faint);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (!desc.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", desc.c_str());
    ImGui::SameLine(g_labelW);
}

static void row_desc(const std::string &desc)
{
    if (desc.empty()) return;
    ImGui::PushFont(F_SMALL, FS(F_SMALL) * k_rowText);
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(g_rowX1);
    ImGui::TextUnformatted(desc.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

std::vector<std::string> monitor_choices()
{
    std::vector<std::string> v = { "Default (main screen)" };
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    for (int i = 0; ids && i < n; i++) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(ids[i]);
        v.push_back(gs::monitor_label(i + 1, m ? m->w : 0, m ? m->h : 0, ids[i] == SDL_GetPrimaryDisplay()));
    }
    if (ids) SDL_free(ids);
    return v;
}

// One option: its label, its control, its description (the reason first when greyed).
static void option_row(App &a, int g, const gs::Option &o)
{
    gs::Game G = (gs::Game)g;
    gs::Version ver = gs::effective_version(G, a.cur[g], a.env[g]);
    gs::Availability av = gs::available(o, G, ver, a.cur[g], a.env[g]);
    std::string tip = gs::tooltip_for(o, G, av, a.env[g]);
    ImGui::PushID(o.id.c_str());
    ImVec2 rowA = ImGui::GetCursorScreenPos();
    row_label(o.label.c_str(), "", av.ok);
    if (!av.ok) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(g_ctrlW);
    double v = a.cur[g].has(o.id) ? a.cur[g].f(o.id) : o.def;
    bool changed = false;
    double nv = v;
    switch (o.kind) {
    case gs::Kind::Bool: {
        bool b = v != 0;
        if (ImGui::Checkbox("##v", &b)) { nv = b ? 1 : 0; changed = true; }
        break;
    }
    case gs::Kind::Choice: {
        std::vector<std::string> items = o.id == "Monitor" ? monitor_choices() : o.choices;
        int idx = std::max(0, std::min((int)items.size() - 1, (int)v));
        if (ImGui::BeginCombo("##v", items.empty() ? "" : items[idx].c_str())) {
            for (int i = 0; i < (int)items.size(); i++) {
                gs::Availability ca = gs::choice_available(o, G, i);
                if (!ca.ok) ImGui::BeginDisabled();
                if (ImGui::Selectable(items[i].c_str(), i == idx)) { nv = i; changed = true; }
                if (!ca.ok) { ImGui::EndDisabled(); if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !ca.why.empty()) ImGui::SetTooltip("%s", ca.why.c_str()); }
                if (i == idx) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case gs::Kind::Int: {
        int iv = (int)lround(v);
        if (ImGui::SliderInt("##v", &iv, (int)o.min, (int)o.max)) {
            int st = std::max(1, (int)o.step);
            iv = (int)o.min + (int)lround((iv - o.min) / st) * st;
            nv = iv; changed = true;
        }
        break;
    }
    case gs::Kind::Float: {
        float fv = (float)v;
        char fmt[16];
        snprintf(fmt, sizeof fmt, "%%.%df", std::max(0, o.decimals));
        if (ImGui::SliderFloat("##v", &fv, (float)o.min, (float)o.max, fmt)) {
            double st = o.step > 0 ? o.step : 0.1;
            nv = o.min + std::round((fv - o.min) / st) * st; changed = true;
        }
        break;
    }
    }
    if (!av.ok) ImGui::EndDisabled();
    if (changed && nv != v) {
        gs::set_value(G, a.cur[g], o.id, nv);
        a.dirty[g] = true;
        if (o.id == "Version" || o.id == "Monitor") env_refresh(a, g);
    }
    // the description as a tooltip: the mouse resting on the row (label or control) for a second (greyed rows lead with why)
    ImVec2 rowB(g_rowX1, ImGui::GetItemRectMax().y);
    static ImGuiID tipRow = 0;
    static double tipSince = 0;
    ImGuiID id = ImGui::GetID("row");
    bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(rowA, rowB) &&
                !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (over) {
        if (tipRow != id || ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) { if (tipRow != id) tipSince = ImGui::GetTime(); tipRow = id; }
        if (ImGui::GetTime() - tipSince > 1.0 && !tip.empty()) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(S(460), FLT_MAX));
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(S(440));
            ImGui::TextUnformatted(tip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    } else if (tipRow == id) tipRow = 0;
    ImGui::PopID();
}

// ---- the About tab ---------------------------------------------------------------------------------------------------
static void lamp_line(int state, const std::string &text)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    th_lamp(ImGui::GetWindowDrawList(), ImVec2(p.x + S(6), p.y + h * 0.55f), S(4.5f), state);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(20), p.y));
    ImGui::PushTextWrapPos(g_rowX1);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

static bool small_button(const char *label) { return th_button(label, ImVec2(F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, label).x + S(28), T(26)), true, F_BODY); }

static bool any_files(const std::string &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(dir, "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do { std::wstring n = fd.cFileName; if (n != L"." && n != L"..") any = true; } while (!any && FindNextFileW(f, &fd));
    FindClose(f);
    return any;
}

static void about_tab(App &a, int g)
{
    const GameDef &d = game_def(g);
    GameState &st = a.st[g];
    th_heading("YOUR COPY", g_rowX1);
    if (st.dataDir.empty()) lamp_line(3, "Not added yet. Use Add your copy below (a disc image, a Games on Demand copy, or an extracted folder).");
    else {
        lamp_line(st.xexMatch ? 1 : st.xexKnown ? 2 : 0,
                  std::string("Game files: ") + st.dataDir + (st.dataOwn ? "  (your own folder)" : "") +
                  (st.xexMatch ? "  -  the release the ports are made for" : st.xexKnown ? "  -  a different release: it may not start or may misbehave" : "  -  not checked yet"));
    }
    if (st.updateDir.empty())
        lamp_line(d.updateNeeded || g == G_JUDGMENT ? 2 : 0, std::string(d.updateName) + ": not added" +
                  (g == G_JUDGMENT ? " - required for now (the disc version crashes at start)"   // 0.2.0 (#1)
                   : d.updateNeeded ? " - the disc version runs instead" : " (optional)"));
    else
        lamp_line(st.xexpMatch ? 1 : 2, std::string(d.updateName) + ": " + st.updateDir + (st.xexpMatch ? "" : "  -  a different update than the ports expect"));
    std::string exe = st.exe();
    // 0.2.0 (#3 / #7): name the file that is missing, so a screenshot says what's wrong
    lamp_line(exe.empty() ? 3 : 1, exe.empty() ? std::string("The port program is missing: expected ") + join(join(game_root(g), *d.portTu ? d.portTu : d.portDisc), d.exeName) +
                                                     " - unzip the release again, keeping its folders." : "Port: " + exe);
    ImGui::Dummy(ImVec2(0, S(4)));
    bool busy = a.job.state.load() == 1, running = a.run.proc != nullptr;
    if (busy || running) ImGui::BeginDisabled();
    if (small_button(st.dataDir.empty() ? "Add your copy..." : "Replace your copy...")) {
        std::string p = pick_file(L"Choose your copy: a disc image (.iso), a Games on Demand file, or default.xex in a folder",
                                  L"Xbox 360 games\0*.iso;*.xex;*.*\0All files\0*.*\0");
        if (!p.empty()) install_start(a, p, g);
    }
    ImGui::SameLine();
    if (small_button("Use a folder...")) {
        std::string p = pick_folder(L"Choose the folder with your game's default.xex");
        if (!p.empty()) install_start(a, p, g);
    }
    ImGui::SameLine();
    if (small_button(st.updateDir.empty() ? "Add the title update..." : "Replace the title update...")) {
        std::string p = pick_file(L"Choose the title update package (from your console's Content\\0000000000000000\\<title ID>\\000B0000 folder)",
                                  L"All files\0*.*\0");
        if (!p.empty()) install_start(a, p, g);
    }
    if (busy || running) ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, S(8)));
    th_heading("FOLDERS", g_rowX1);
    if (small_button("Saves")) { make_dirs(game_user_dir(g)); shell_open(game_user_dir(g)); }
    ImGui::SameLine();
    if (small_button("Logs")) { make_dirs(game_logs_dir(g)); shell_open(game_logs_dir(g)); }
    ImGui::SameLine();
    if (small_button("Back up saves")) {
        if (running) set_message(a, std::string(d.name) + " is running - close it first, so the save is complete.", true);
        else if (!any_files(game_user_dir(g))) set_message(a, std::string(d.name) + " has no saves here yet.", false);
        else ask(a, "backup", g, "Back up saves", std::string("Put a copy of ") + d.name + "'s saves in a zip on your desktop?", { "Back up", "Cancel" });
    }
    ImGui::SameLine();
    if (small_button("Clear shaders...")) {
        if (!dir_exists(game_shader_dir(g))) set_message(a, std::string(d.name) + " has no saved shaders to clear.", false);
        else ask(a, "clearshaders", g, "Clear shaders",
                 std::string("Clear the saved shaders and pipelines of ") + d.name + "?\n\nThe next start builds them again, so expect "
                 "stutter and pop-in the first time, as on a new PC. The folder is set aside (renamed), not deleted.", { "Clear", "Cancel" });
    }

    ImGui::Dummy(ImVec2(0, S(8)));
    th_heading("PROBLEMS", g_rowX1);
    row_desc("A problem report is one zip on your desktop with the last play's log, the settings and a description of this PC "
             "(no saves, no game files; your Windows user name is taken out). Nothing is sent - attach it to a GitHub issue.");
    if (small_button("Make a problem report")) {
        std::string s = newest_session(g);
        if (s.empty()) set_message(a, std::string("No play of ") + d.name + " yet - play once, then make the report.", false);
        else {
            std::string zip, err;
            if (make_report(a, g, s, "", zip, err)) { set_message(a, "Saved on your desktop: " + zip.substr(zip.find_last_of('\\') + 1), false); }
            else set_message(a, "Couldn't make the report: " + err, true);
        }
    }
    ImGui::SameLine();
    if (small_button("Reset settings to defaults...")) ask(a, "reset", g, "Reset to defaults", "Put every setting of every game back to its default? Your saves are not touched.", { "Reset", "Cancel" });

    ImGui::Dummy(ImVec2(0, S(8)));
    th_heading("COGBOX", g_rowX1);
    std::string newer = update_newer();
    if (!newer.empty()) {
        lamp_line(2, "COGBox " + newer + " is out (this is " COGBOX_VERSION ").");
        float p = 0; std::string err;
        int s = update_install_state(&p, &err);
        if (s == 1 || s == 2) ImGui::ProgressBar(p, ImVec2(g_rowX1 - ImGui::GetCursorScreenPos().x, 0), s == 1 ? "Downloading..." : "Unpacking...");
        else if (s == 3) { if (small_button("Restart COGBox")) { if (update_restart()) a.quit = true; } }
        else {
            if (s == -1) lamp_line(3, err);
            if (small_button("Update")) {
                if (running) set_message(a, "Close the game first.", true); else update_install_start(newer);
            }
            ImGui::SameLine();
            if (small_button("Release page")) shell_open(update_page());
        }
    } else if (!a.ini.geti("Updates", "Check", 1)) lamp_line(0, "COGBox " COGBOX_VERSION " - checking for updates is off.");
    else lamp_line(update_checked() ? 1 : 0, update_checked() ? "COGBox " COGBOX_VERSION " - the latest (or GitHub couldn't be reached)." : "COGBox " COGBOX_VERSION " - checking for updates...");
    bool fs = a.fullscreen;   // job 523
    if (ImGui::Checkbox("Fullscreen (F11, or LT + RT together)", &fs)) set_fullscreen(a, fs);
    bool bl = a.borderless;   // job 523
    if (ImGui::Checkbox("Borderless window (no title bar)", &bl)) set_borderless(a, bl);
    row_desc("COGBox draws its own minimise, maximise and close buttons in the top-right corner; drag the strip beside them to move the window.");
    bool chk = a.ini.geti("Updates", "Check", 1) != 0;
    if (ImGui::Checkbox("Check GitHub for a newer COGBox at start", &chk)) { a.ini.seti("Updates", "Check", chk ? 1 : 0); a.ini.save(); }
    row_desc("Only the release page's address is asked for; nothing about you or this PC is sent.");
}

// ---- the page --------------------------------------------------------------------------------------------------------
static const char *k_tabs[] = { "DISPLAY", "GRAPHICS", "CONTROLS", "GAMEPLAY", "ABOUT" };   // the one sound option is in Gameplay
static const int k_tabCount = 5;

static std::string summary(App &a, int g)
{
    gs::Game G = (gs::Game)g;
    auto choice_text = [&](const char *id) -> std::string {
        const gs::Option *o = gs::find_option(G, id);
        if (!o || o->choices.empty()) return "";
        int i = std::max(0, std::min((int)o->choices.size() - 1, a.cur[g].has(id) ? a.cur[g].i(id) : (int)o->def));
        std::string s = o->choices[i];
        size_t par = s.find(" (");
        return par == std::string::npos ? s : s.substr(0, par);
    };
    std::string s = choice_text("Resolution");
    std::string fps = choice_text("Fps"), pr = choice_text("Preset"), aa = choice_text("AAMode");
    if (!fps.empty()) s += "   |   " + fps + (fps == "Unlimited" ? "" : " fps");
    if (!aa.empty() && aa != "Off") s += "   |   " + aa;
    if (!pr.empty()) s += "   |   " + pr;
    return s;
}

void page_draw(App &a, ImVec2 size)
{
    int g = a.page;
    const GameDef &d = game_def(g);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pw = std::min(size.x - S(40), S(980)), ph = std::min(size.y - S(40), S(760));
    ImVec2 p0((size.x - pw) * 0.5f, (size.y - ph) * 0.5f), p1(p0.x + pw, p0.y + ph);
    // see-through so the game's art shows, dark enough behind the text to read it (Shagg 18:41)
    th_plate(dl, p0, p1, S(18), false, 0.45f);
    th_plate_fill(dl, ImVec2(p0.x + S(6), p0.y + S(6)), ImVec2(p1.x - S(6), p1.y - S(6)), S(14), U(ImVec4(0.02f, 0.025f, 0.03f, 0.30f)));

    // header: the box, the name, the lamps
    float hh = std::max(S(108), S(22) + FS(F_TITLE) + FS(F_BODY) + S(16));
    draw_box(a, g, ImVec2(p0.x + S(60), p0.y + S(64)), S(92), -12, 1.0f, false);
    dl->AddText(F_TITLE, FS(F_TITLE), ImVec2(p0.x + S(112), p0.y + S(22)), U(col::text), d.name);
    std::string sub = a.st[g].ready == R_READY ? (a.st[g].useUpdate ? std::string(d.updateName) : "Disc version") : "Not ready to play";
    if (a.st[g].playSeconds > 0) sub += "   |   " + human_duration(a.st[g].playSeconds) + " played";
    dl->AddText(F_BODY, FS(F_BODY), ImVec2(p0.x + S(114), p0.y + S(22) + FS(F_TITLE) + S(2)), U(col::dim), sub.c_str());
    {
        struct L { int s; const char *t; } lamps[3] = {
            { a.st[g].dataDir.empty() ? 3 : a.st[g].xexMatch ? 1 : 2, "Game" },
            { a.st[g].updateDir.empty() ? (d.updateNeeded ? 2 : 0) : a.st[g].xexpMatch ? 1 : 2, "Update" },
            { a.padConnected ? 1 : 0, "Controller" } };
        float x = p1.x - S(24);
        for (int i = 2; i >= 0; i--) {
            float tw = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, lamps[i].t).x;
            x -= tw;
            dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(x, p0.y + S(28)), U(col::dim), lamps[i].t);
            th_lamp(dl, ImVec2(x - S(10), p0.y + S(28) + FS(F_SMALL) * 0.55f), S(4.5f), lamps[i].s);
            x -= S(30);
        }
    }

    // tabs
    float tx = p0.x + S(24), ty = p0.y + hh, tw = (pw - S(48)) / k_tabCount, tabH = T(30);
    for (int i = 0; i < k_tabCount; i++) {
        ImGui::SetCursorScreenPos(ImVec2(tx + i * tw, ty));
        if (th_tab(k_tabs[i], a.tab == i, ImVec2(tw - S(4), tabH))) a.tab = i;
    }
    // The controller and the bottom bar (Shagg 22:23): D-pad down past the last row goes to the bar (Play chosen); there
    // D-pad left / right or LB / RB choose Library, Save or Play, A presses it, D-pad up goes back to the rows. Dear
    // ImGui's own navigation is off while the bar is chosen (main.cpp) and the bar's buttons are not in it.
    bool padNav = a.padConnected && !a.modal.open && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && a.zoomTo == SCR_PAGE;
    if (!padNav || ImGui::IsMouseClicked(0)) a.barSel = -1;
    // B goes to the bar's Library button (Shagg 22:26), not straight back to the library: A there goes back, or move to
    // Save / Play. (A B that just closed a drop-down list was for the list, so it is left alone.)
    static bool popupLastFrame = false;
    bool padB = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (padNav && padB && !popupLastFrame) { a.barSel = 0; ImGui::SetNavCursorVisible(false); }
    popupLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || a.modal.open;
    {
        static ImGuiID downFrom = 0;
        static int downAt = -1;
        bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown);
        if (padNav && a.barSel < 0) {
            // a down press that left the focus where it was: the last row - the next stop is the bar
            if (down) { downFrom = ImGui::GetFocusID(); downAt = ImGui::GetFrameCount(); }
            else if (downAt >= 0 && ImGui::GetFrameCount() >= downAt + 3) {
                if (ImGui::GetFocusID() == downFrom) { a.barSel = 2; ImGui::SetNavCursorVisible(false); }
                downAt = -1;
            }
        } else if (padNav) {
            downAt = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) a.barSel = std::max(0, a.barSel - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) a.barSel = std::min(2, a.barSel + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp)) { a.barSel = -1; ImGui::SetNavCursorVisible(true); }
        }
    }
    // LB / RB switch tabs while the bar is not chosen
    if (a.barSel < 0 && ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) a.tab = (a.tab + k_tabCount - 1) % k_tabCount;
    if (a.barSel < 0 && ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) a.tab = (a.tab + 1) % k_tabCount;
    dl->AddLine(ImVec2(p0.x + S(24), ty + tabH), ImVec2(p1.x - S(24), ty + tabH), U(col::redHi, 0.7f), S(2));

    // the rows
    float barH = S(64);
    ImVec2 c0(p0.x + S(30), ty + tabH + S(10)), c1(p1.x - S(24), p1.y - barH - S(8));
    ImGui::SetCursorScreenPos(c0);
    ImGui::BeginChild("rows", ImVec2(c1.x - c0.x, c1.y - c0.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushFont(F_BODY, FS(F_BODY) * k_rowText);
    g_ctrlW = T(300);
    g_labelW = std::min(S(500), (c1.x - c0.x) - g_ctrlW - S(40));
    g_rowX1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - S(16);
    if (a.tab < k_tabCount - 1) {
        gs::Tab want = (gs::Tab)a.tab;
        int shown = 0;
        for (const gs::Option &o : gs::options((gs::Game)g)) {
            if (o.tab != want) continue;
            option_row(a, g, o);
            shown++;
        }
        if (!shown) row_desc("Nothing to set here yet.");
    } else about_tab(a, g);
    ImGui::PopFont();
    ImGui::EndChild();

    // the bar
    float by = p1.y - barH;
    dl->AddLine(ImVec2(p0.x + S(24), by), ImVec2(p1.x - S(24), by), U(col::edge, 0.35f), S(1));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(24), by + S(12)));
    bool barA = a.barSel >= 0 && padNav && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // the controller reaches the bar through a.barSel, not Dear ImGui
    if (th_button("LIBRARY", ImVec2(S(130), S(40)), true, F_HEAD, a.barSel == 0) || (barA && a.barSel == 0) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !a.modal.open)) {
        a.barSel = -1;
        if (std::any_of(a.dirty, a.dirty + G_COUNT, [](bool x) { return x; })) settings_save(a);
        a.zoomTo = SCR_FRONT; a.zoomStart = ImGui::GetTime();
    }
    std::string sm = summary(a, g);
    ImVec2 ss = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, sm.c_str());
    float midL = p0.x + S(170), midR = p1.x - S(310);
    if (ss.x < midR - midL) dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(midL + (midR - midL - ss.x) * 0.5f, by + S(24)), U(col::dim), sm.c_str());
    bool anyDirty = std::any_of(a.dirty, a.dirty + G_COUNT, [](bool x) { return x; });
    ImGui::SetCursorScreenPos(ImVec2(p1.x - S(24) - S(280), by + S(12)));
    if (th_button(anyDirty ? "SAVE *" : "SAVE", ImVec2(S(130), S(40)), true, F_HEAD, a.barSel == 1) || (barA && a.barSel == 1)) { if (settings_save(a)) set_message(a, "Saved.", false); }
    ImGui::SameLine(0, S(20));
    bool ready = a.st[g].ready == R_READY && !a.run.proc && a.job.state.load() != 1;
    if (th_button_red("PLAY", ImVec2(S(130), S(40)), ready, F_HEAD, a.barSel == 2) || (ready && ((barA && a.barSel == 2) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))) {
        settings_save(a);
        play_start(a, g);
    }
    ImGui::PopItemFlag();
}
