// COGBox - one page per game: its box and name, status lamps, the settings in tabs (built from gears_settings'
// option list, so a new option there shows up here), the About tab, and a bar with Library, a summary, Save and Play.
#include <cctype>
#include "app.h"
#include "profiles.h"
#include "social_net.h"   // social step 3: the multiplayer server
#include "theme.h"
#include "i18n.h"   // 0.2.0
#include "update.h"
#include "report.h"   // report_gpu_names (report_share)
#include "dlss_custom.h"   // 0.4.9: Custom DLSS file
#include "imgui_internal.h"

#include <windows.h>
#include <shlobj.h>     // 0.2.0: desktop shortcuts
#include <shobjidl.h>
#include <objbase.h>
#include <dxgi.h>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <algorithm>

// ---- the PC, for the settings ---------------------------------------------------------------------------------------
// Searching a 100 MB exe for a word takes a moment, so each (file, word) answer is kept.
// Shagg 31 Oct: the About tab's HELP US TEST list - what this release needs players to confirm. Update it with every
// release (games: bit per GameId - 1 Gears 2, 2 Gears 3, 4 Judgment, 8 Gears 1; 15 = all).
struct HelpTest { unsigned games; const char *text; };
static const HelpTest k_helpTest[] = {
    // Shagg 6 Nov: never ask players to do extra work to find data for us - at most "press Report a problem".
    { 2,  "Sudden death / \"cursed\" save, or a teammate killing you (#55, #68, #79, #80): if it happens to you, press Report a problem afterwards - the report has what we need." },
    { 2,  "Raam's Shadow DLC not found (#79): if the game doesn't see your DLC, press Report a problem after that start." },
    { 15, "System Link - LAN play (experimental, #72): if you try it, tell us in a ticket on GitLab whether it worked." },
    { 15, "Other versions of the game (regional discs, other title updates): if yours isn't the one COGBox expects, tell us whether it works." },
};


// legal2-050: issues live in GitLab's tickets now, which can't be pre-filled from a link - the details go on the
// clipboard and the new-ticket page opens; the player pastes them.
static const char *const kTicketsUrl = "https://gitlab.com/shagg88/cogbox/-/issues";   // 0.5.0: GitLab
static void open_ticket(const std::string &title, const std::string &body)
{
    SDL_SetClipboardText((title + "\n\n" + body).c_str());
    shell_open(std::string(kTicketsUrl) + "/new");   // GitLab: .../-/issues/new
}

// Shagg 2 Nov: "Report a problem" in one go - the ticket page can't take a file from COGBox (that needs the player's
// sign-in), so the zip's folder opens with it selected and a new issue opens pre-filled; the player drags the zip in.
void report_share(App &a, int g, const std::string &zip, const std::string &what)
{
    const GameDef &d = game_def(g);
    std::string gpus;
    for (const std::string &n : report_gpu_names()) gpus += (gpus.empty() ? "" : ", ") + n;
    const std::string title = std::string(d.name) + ": " + (what.empty() ? "describe the problem here" : what.substr(0, 80));
    const std::string body = std::string("**What happened:** \n\n**Where in the game (chapter / menu / loading):** \n\n**Does it happen every time?** \n\n---\n") +
        "Game: " + d.name + "\nCOGBox: " COGBOX_VERSION "\nGraphics: " + (gpus.empty() ? "unknown" : gpus) +
        (a.wine ? "\nLinux / Steam Deck: " + proton::summary(a.proton) : std::string()) +   // proton049
        (what.empty() ? "" : "\nThe launcher's reading: " + what) +
        "\n\nPlease attach the report zip (" + zip.substr(zip.find_last_of('\\') + 1) + ") before you submit.\n";
    shell_select(zip);
    open_ticket(title, body);
    if (a.wine) set_message(a, "Report saved in COGBox's reports folder: " + shown_path(a, zip) + ". " + tr("A GitLab ticket page opened and the details are on the clipboard - paste them, attach the zip and submit."), false);   // proton049 path + legal2-050 GitLab
    else set_message(a, "Report saved: " + zip.substr(zip.find_last_of('\\') + 1) + ". " + tr("A GitLab ticket page opened and the details are on the clipboard - paste them, attach the zip and submit."), false);
}

// Shagg 31 Oct: the About tab's problem report (also after the experimental-settings prompt, main.cpp "reportexp")
bool untested_version(const App &a, int g)
{
    const GameState &st = a.st[g];
    return (st.xexKnown && !st.xexMatch && !st.xexConfirmed) || (st.useUpdate && st.xexpKnown && !st.xexpMatch && !st.xexpConfirmed);
}

void report_now(App &a, int g, const std::string &session, bool versionOk)
{
    if (!versionOk && untested_version(a, g)) {   // Shagg 2 Nov: allowed to play, but no bug reports from the wrong update
        const GameDef &d = game_def(g);
        ask(a, "reportver", g, "Untested game version or title update",
            std::string("Your copy of ") + d.name + " or its " + d.updateName + " isn't the one COGBox is made for, so problems with it "
            "are expected and usually can't be fixed on our side.\n\nPlease don't report a problem you get with it. Use the "
            "expected version first (the About tab shows which file is different); if the problem happens there too, report it.",
            { "Make the report anyway", "Cancel" });
        a.modal.data = session;
        return;
    }
    std::string zip, err;
    if (make_report(a, g, session, "", zip, err)) report_share(a, g, zip, "");
    else set_message(a, "Couldn't make the report: " + err, true);
}

// ProductName or FileDescription in the file's version resource contains text (case-insensitive). 0.4.8 review: tells
// the MFG Unlocker's dxgi.dll from ReShade / Special K / DXVK, which use the same file name.
static bool file_product_has(const std::string &path, const std::string &text)
{
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(widen(path).c_str(), &dummy);
    if (!size || text.empty()) return false;
    std::vector<char> info(size);
    if (!GetFileVersionInfoW(widen(path).c_str(), 0, size, info.data())) return false;
    struct Lang { WORD lang, cp; } *langs = nullptr;
    UINT n = 0;
    std::vector<std::wstring> blocks;
    if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &n) && langs)
        for (UINT i = 0; i < n / sizeof(Lang); i++) {
            wchar_t b[16];
            swprintf(b, 16, L"%04x%04x", langs[i].lang, langs[i].cp);
            blocks.push_back(b);
        }
    blocks.push_back(L"040904b0");
    blocks.push_back(L"040904e4");
    std::wstring want = widen(text);
    for (wchar_t &c : want) c = (wchar_t)towlower(c);
    for (const std::wstring &b : blocks)
        for (const wchar_t *key : { L"ProductName", L"FileDescription" }) {
            std::wstring q = L"\\StringFileInfo\\" + b + L"\\" + key;
            wchar_t *val = nullptr;
            UINT len = 0;
            if (!VerQueryValueW(info.data(), q.c_str(), (void **)&val, &len) || !val || !len) continue;
            std::wstring v(val, wcsnlen(val, len));
            for (wchar_t &c : v) c = (wchar_t)towlower(c);
            if (v.find(want) != std::wstring::npos) return true;
        }
    return false;
}

// 0.5.0 review (Gears 3 Play hang): every capability question used to re-read and lowercase the whole exe (77 MB for
// Gears 3) - dozens of misses at Play froze COGBox for many seconds ("Not Responding"). Now the exe is read and
// lowercased once (one file kept, keyed by path + size + time) and each question is a search in memory.
static bool file_has(const std::string &path, const std::string &needle)
{
    static std::map<std::string, bool> cache;
    struct Held { std::string key, data; };
    static Held slots[2];               // the exe and the runtime DLL, kept in memory lowercased (review: not one slot)
    static int nextSlot = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool st = GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fa) != 0;
    const std::string fkey = path + "|" + std::to_string(fa.nFileSizeLow) + "|" + std::to_string(fa.nFileSizeHigh) + "|" +
                             std::to_string(fa.ftLastWriteTime.dwLowDateTime) + "|" + std::to_string(fa.ftLastWriteTime.dwHighDateTime);
    const std::string key = fkey + "|" + needle;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (!st || needle.empty()) return cache[key] = false;
    Held *hs = nullptr;
    for (Held &x : slots) if (x.key == fkey) hs = &x;
    if (!hs) {
        hs = &slots[nextSlot]; nextSlot = (nextSlot + 1) % 2;
        std::string &held = hs->data;
        held.clear(); held.shrink_to_fit(); hs->key.clear();
        HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return cache[key] = false;
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1LL << 30)) {
            held.resize((size_t)sz.QuadPart);
            size_t off = 0;
            DWORD got = 0;
            while (off < held.size() && ReadFile(h, &held[off], (DWORD)std::min<size_t>(held.size() - off, 1u << 24), &got, nullptr) && got) off += got;
            held.resize(off);
            for (char &c : held) c = (char)tolower((unsigned char)c);
            hs->key = fkey;
        }
        CloseHandle(h);
    }
    std::string lo = needle;
    for (char &c : lo) c = (char)tolower((unsigned char)c);
    const bool found = hs->key == fkey && hs->data.find(lo) != std::string::npos;
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

// GitHub #154: keyboard keys per game - CogBox.ini [Keys.<game>] (e.g. [Keys.judgment]). A key the player never set for
// a game falls back to the old shared [Keys] (what 0.2.0 - 0.4.3 saved for all the games), then to the Gears layout.
static std::string keys_section_name(int g) { return std::string("Keys.") + game_def(g).key; }
static std::string key_get(App &a, int g, const char *cvar)
{
    std::string v = a.ini.get(keys_section_name(g), cvar);
    return v.empty() ? a.ini.get("Keys", cvar) : v;
}

static bool lang_on_copy(const std::string &root, int g, int idx);
static bool voice_on_copy(const std::string &root, int g, int idx);

void env_refresh(App &a, int g)
{
    GameState &st = a.st[g];
    gs::Environment &e = a.env[g];
    // a folder path ending in a backslash ("E:\") would end its quoted argument as \" - which Windows reads as a quote
    auto safe = [](std::string p) { if (!p.empty() && (p.back() == '\\' || p.back() == '/')) p += "."; return p; };
    e.game_data_root = safe(st.dataDir);
    e.update_data_root = safe(st.updateDir);
    e.user_data_root = game_user_dir(g);
    e.identity_dir = profile_dir(exe_dir(), std::to_string(current_profile()));
    // social step 3: online only when the player enabled the multiplayer server in the SOCIAL tab
    e.online_server = a.ini.get("Online", "Enabled") == "1" ? a.ini.get("Online", "Server", kCogboxServer) : std::string();
    e.online_stun = stun_address(e.online_server);
    e.online_region = e.online_server.empty() ? std::string() : region_or_default(a.ini.get("Online", "Region"), local_utc_minutes());
    e.shader_cache_dir = game_shader_dir(g);
    e.mods = mods_enabled_paths(a, g);   // 0.2.0
    {   // job 620 (#45), GitHub #159: the Lang / VoiceLang choices not on this copy (greyed, and not passed at launch)
        const gs::Option *lo = gs::find_option((gs::Game)g, "Lang"), *vo = gs::find_option((gs::Game)g, "VoiceLang");
        e.lang_missing.assign(lo ? lo->choices.size() : 0, false);
        e.voice_missing.assign(vo ? vo->choices.size() : 0, false);
        for (size_t i = 0; i < e.lang_missing.size(); i++) e.lang_missing[i] = !lang_on_copy(e.game_data_root, g, (int)i);
        for (size_t i = 0; i < e.voice_missing.size(); i++) e.voice_missing[i] = !voice_on_copy(e.game_data_root, g, (int)i);
    }
    {   // job 630: the shipped chapter recipes for this game (built from the player's own files at start-up)
        std::string exe = game_def(g).exeName;
        const std::string rcp = join(exe_dir(), "recipes\\recipes-" + exe.substr(0, exe.size() - 4) + ".rcp");
        e.recipe_pack = file_exists(rcp) ? rcp : "";
    }
    e.prompt_dir = dir_exists(join(exe_dir(), "prompts\\keyboard")) ? join(exe_dir(), "prompts\\keyboard") : "";   // job 629
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
    e.monitor_w = dm ? dm->w : 0;   // 0.4.9: Aspect ratio Auto
    e.monitor_h = dm ? dm->h : 0;
    const char *dn = use ? SDL_GetDisplayName(use) : nullptr;
    e.monitor_device = dn ? dn : "";
    if (ids) SDL_free(ids);
    e.has_avx2 = a.hasAvx2;
    e.wine = a.wine;   // job 523
    e.mac = a.mac;     // job 643
    e.steam_deck = a.proton.steamDeck;   // proton049
    e.deck_lcd = a.proton.deckLcd;
    e.keys.clear();    // 0.2.0: the player's own keyboard keys (Controls tab); GitHub #154: this game's own
    for (const gs::KeyAction &k : gs::key_actions((gs::Game)g)) {
        std::string v = key_get(a, g, k.cvar);
        if (!v.empty()) e.keys[k.cvar] = v;
    }
    e.gamertag = a.ini.get(profile_section(), "Gamertag");   // 0.5.0: the current profile's name
    for (int k = 0; k < 3; k++) e.gamertags[k] = a.ini.get("Profile", "Gamertag" + std::to_string(k + 2));   // 0.4.9
    e.pad_remap = a.ini.get("Pad", "Remap");   // GitHub #97 (SDK patch 55)   // GitHub #119 (SDK patch 54)
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
    if (game_update_required(g)) st.useUpdate = !st.updateDir.empty() && (!game_def(g).updateNeeded || !st.portExeTu.empty());   // title updates required
    if (use_normal_code(a, g) && file_exists(gs::normal_code_exe(exe))) exe = gs::normal_code_exe(exe);   // cg050
    // 0.5.0 (Play took 2+ minutes in Gears 2 / 3 / Judgment): writing the in-game menu file asks these ~10,000-12,000
    // times (live_lines runs build_args for every value of every live option). Each answer is now worked out once per
    // env (the files are checked when the env is built) and then read from memory - no file-system call per question.
    auto memo = [](std::string path) {
        auto m = std::make_shared<std::unordered_map<std::string, bool>>();
        return [path, m](const std::string &s) {
            if (path.empty()) return false;
            auto it = m->find(s);
            if (it != m->end()) return it->second;
            return (*m)[s] = file_has(path, s);
        };
    };
    e.cap.exe_has = memo(exe);
    e.cap.runtime_has = memo(runtime_dll(exe));
    // beside the exe, or in the release's shared folder (<COGBox>\shared), which play.cpp puts on the game's PATH: the
    // FSR / XeSS / frame generation DLLs live there in the release zip
    e.cap.file_next_to_exe = [exe, m = std::make_shared<std::unordered_map<std::string, bool>>()](const std::string &name) {
        if (exe.empty()) return false;
        auto it = m->find(name);
        if (it != m->end()) return it->second;
        bool r = file_exists(join(exe.substr(0, exe.find_last_of('\\')), name));
        if (!r) { std::string shared = join(exe_dir(), "shared"); r = dir_exists(shared) && file_exists(join(shared, name)); }
        return (*m)[name] = r;
    };
    e.cap.file_product_has = [exe](const std::string &name, const std::string &text) {
        if (exe.empty()) return false;
        std::string beside = join(exe.substr(0, exe.find_last_of('\\')), name);
        if (file_exists(beside) && file_product_has(beside, text)) return true;
        std::string shared = join(join(exe_dir(), "shared"), name);
        return file_exists(shared) && file_product_has(shared, text);
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
// GitHub #123: g_rowX1 is a screen x, but PushTextWrapPos takes a window-local x (Dear ImGui adds the window's
// position), so the rows' descriptions wrapped that far past the right edge and were cut off. This converts it.
static float row_wrap_x() { return g_rowX1 - ImGui::GetWindowPos().x + ImGui::GetScrollX(); }
static const float k_rowText = 1.1f;    // the rows' text, bigger than the body font (Shagg 21:44: hard to read)

static void row_label(const char *label, const std::string &desc, bool enabled)
{
    label = tr(label);   // 0.2.0
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? col::text : col::faint);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (!desc.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", desc.c_str());
    ImGui::SameLine(g_labelW);
}

// ---- 0.4.6 (Shagg 5 Nov 18:07: "the whole launcher aesthetically streamlined and simple") - one visual system for
// every tab: a section is a gap and one heading style; explanations are hover tooltips (ui_tip), never paragraphs;
// buttons share one height (and a minimum width, so a row of them lines up).
void ui_note(const std::string &text_en, ImVec4 c = col::dim);
static int g_sections;   // sections drawn so far in this tab (no gap above the first)

void ui_tip(const std::string &text_en)   // (also achievements_tab.cpp) a hover / controller-focus tooltip for the last item
{
    if (text_en.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(S(460), FLT_MAX));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(440));
    ImGui::TextUnformatted(tr(text_en).c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static void ui_gap() { ImGui::Dummy(ImVec2(0, S(10))); }

void ui_section(const char *title_en)   // (also achievements_tab.cpp)
{
    if (g_sections++) ui_gap();
    th_heading(title_en, g_rowX1);
    ImGui::Dummy(ImVec2(0, S(2)));
}

static bool small_button(const char *label_en)
{
    const char *label = tr(label_en);
    const float w = std::max(T(90), F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, label).x + S(28));
    return th_button(label, ImVec2(w, T(26)), true, F_BODY);
}

// one short line of feedback or status, small and dim (instead of a paragraph)
void ui_note(const std::string &text_en, ImVec4 c)   // (also achievements_tab.cpp)
{
    ImGui::PushFont(F_SMALL, FS(F_SMALL) * k_rowText);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(row_wrap_x());
    ImGui::TextUnformatted(tr(text_en).c_str());
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
// ---- 0.2.0: keyboard keys (Controls tab) -----------------------------------------------------------------------------
// The SDK's key names (rexglue ui/keybinds.cpp) for the keys Dear ImGui reports.
static const char *sdk_key_name(ImGuiKey k)
{
    static char one[2];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) { one[0] = char('A' + (k - ImGuiKey_A)); one[1] = 0; return one; }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) { one[0] = char('0' + (k - ImGuiKey_0)); one[1] = 0; return one; }
    static char fk[8];
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) { snprintf(fk, sizeof fk, "F%d", 1 + (k - ImGuiKey_F1)); return fk; }
    static char np[10];
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) { snprintf(np, sizeof np, "Numpad%d", k - ImGuiKey_Keypad0); return np; }
    switch (k) {
    case ImGuiKey_Space: return "Space"; case ImGuiKey_Enter: return "Return"; case ImGuiKey_KeypadEnter: return "NumpadEnter";
    case ImGuiKey_Tab: return "Tab"; case ImGuiKey_Backspace: return "Backspace"; case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Insert: return "Insert"; case ImGuiKey_Home: return "Home"; case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp"; case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_LeftArrow: return "Left"; case ImGuiKey_RightArrow: return "Right"; case ImGuiKey_UpArrow: return "Up"; case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftShift: case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl: return "Control";
    case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_Apostrophe: return "Quote"; case ImGuiKey_Comma: return "Comma"; case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Period: return "Period"; case ImGuiKey_Slash: return "Slash"; case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Equal: return "Plus"; case ImGuiKey_LeftBracket: return "LBracket"; case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_RightBracket: return "RBracket"; case ImGuiKey_GraveAccent: return "Backtick"; case ImGuiKey_CapsLock: return "CapsLock";
    case ImGuiKey_KeypadAdd: return "NumpadPlus"; case ImGuiKey_KeypadSubtract: return "NumpadMinus";
    case ImGuiKey_KeypadMultiply: return "NumpadStar"; case ImGuiKey_KeypadDivide: return "NumpadSlash";
    default: return nullptr;
    }
}

// The game's keyboard driver (rexglue ui/sdl_virtual_key.cpp) identifies keys by SDL scancode, i.e. by their position on
// a US layout, while Dear ImGui names them by the character they type (SDL keycode). On AZERTY / QWERTZ / other layouts
// those differ (AZERTY "A" is the US "Q" position, QWERTZ Y/Z swap, AZERTY ";" sits at the US "," position), so the
// pressed key is named here from its scancode, the same way the driver will see it.
int g_lastKeyScancode = 0;
static const char *sdk_scancode_name(int sc)
{
    static char one[2];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { one[0] = char('A' + (sc - SDL_SCANCODE_A)); one[1] = 0; return one; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { one[0] = char('1' + (sc - SDL_SCANCODE_1)); one[1] = 0; return one; }
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon"; case SDL_SCANCODE_EQUALS: return "Plus"; case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_MINUS: return "Minus"; case SDL_SCANCODE_PERIOD: return "Period"; case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_GRAVE: return "Backtick"; case SDL_SCANCODE_LEFTBRACKET: return "LBracket"; case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_RIGHTBRACKET: return "RBracket"; case SDL_SCANCODE_APOSTROPHE: return "Quote";
    default: return nullptr;   // everything else: ImGui's name already matches the driver's
    }
}

static std::string key_shown(const std::string &v)   // "MMB,Z" -> "Middle mouse / Z"
{
    std::string out, tok;
    auto add = [&](std::string t) {
        if (t == "LMB") t = "Left mouse"; else if (t == "RMB") t = "Right mouse"; else if (t == "MMB") t = "Middle mouse";
        else if (t == "MB4") t = "Mouse button 4"; else if (t == "MB5") t = "Mouse button 5";   // GitHub #208 (SDK patch 65)
        else if (t == "Return") t = "Enter";
        if (!t.empty()) out += (out.empty() ? "" : " / ") + t;
    };
    for (char c : v) { if (c == ',') { add(tok); tok.clear(); } else tok += c; }
    add(tok);
    return out;
}

// Job 641 (Shagg 29 Oct: "pre-compile every single Shader before they jump into gameplay even if it costs users 8 to
// 12 minutes"): the game builds every shipped chapter recipe (--gow2_renderer_prepare_all), saves its pipeline library
// and exits; its <shadercache>\prepare-progress.txt ("<done> <total> <state> <seconds>") drives the bar here.

// 0.4.9: Custom DLSS file (dlss_custom.h) - advanced, at the player's own risk; COGBox only checks and copies the file
static void dlss_file_section(App &a, int g)
{
    if (a.env[g].wine) return;   // DLSS is DirectX 12 on Windows only
    const std::string exe = a.st[g].exe();
    if (exe.empty()) return;
    const cogbox::DlssState st = cogbox::dlss_state(exe);
    if (!st.present && st.backupVersion.empty()) return;   // a build without DLSS
    ui_section("DLSS FILE (ADVANCED)");
    std::string line = !st.present ? "No DLSS file beside the game."
                       : st.custom ? "In use: your own DLSS file " + st.version + " (COGBox's " + st.backupVersion + " is backed up)."
                                   : "In use: COGBox's DLSS file " + st.version + ".";
    ui_note(line, st.custom ? col::amber : col::dim);
    const bool busy = a.run.proc && a.run.game == g;
    if (small_button("Custom DLSS file...")) {
        if (busy) set_message(a, "Close the game first.", true);
        else {
            std::string p = pick_file(L"Choose an official NVIDIA nvngx_dlss.dll", L"NVIDIA DLSS (nvngx_dlss.dll)\0nvngx_dlss*.dll\0DLL files\0*.dll\0");
            if (!p.empty()) {
                const cogbox::DlssFileInfo fi = cogbox::dlss_check_file(p);
                if (!fi.ok) set_message(a, "Not used: " + fi.why + ".", true);
                else
                    ask(a, "dlsscustom", g, "Use your own DLSS file?",
                        "NVIDIA DLSS " + fi.version + ", signed by NVIDIA.\n\nAdvanced, at your own risk: COGBox is tested with its own "
                        "DLSS file (" + (st.custom ? st.backupVersion : st.version) + "). Another version can look different, run "
                        "slower or not start DLSS - then the game uses no upscaler. COGBox's file is backed up (not deleted) "
                        "and \"Restore COGBox's DLSS\" puts it back.",
                        { "Use it", "Cancel" }, p);
            }
        }
    }
    if (!st.backupVersion.empty()) {
        ImGui::SameLine();
        if (small_button("Restore COGBox's DLSS")) {
            std::string msg;
            if (busy) set_message(a, "Close the game first.", true);
            else { const bool ok = cogbox::dlss_restore(exe, msg); set_message(a, msg, !ok); }
        }
    }
    ui_note("For the \"DLSS\" anti-aliasing (Super Resolution) of this game only. Use an official NVIDIA file - from NVIDIA's "
            "DLSS SDK release or a game you own. COGBox checks NVIDIA's signature and copies it; nothing is downloaded.");
}

void dlss_custom_answer(App &a, int g, const std::string &path)
{
    if (g < 0 || g >= G_COUNT) return;
    std::string msg;
    const bool ok = cogbox::dlss_install_custom(a.st[g].exe(), path, msg);
    set_message(a, msg, !ok);
}

static void prepare_section(App &a, int g)
{
    const bool supported = a.env[g].cap.exe_has && a.env[g].cap.exe_has("gow2_renderer_prepare_all") && !a.env[g].recipe_pack.empty();
    ui_section("PREPARE ALL SHADERS");
    static const char *k_prepTip = "Builds the shaders of every chapter before you play, so chapters start without stutter while they are "
             "built. Takes a few minutes depending on your PC (about 1.5 minutes on a fast one), once - again only after a "
             "graphics driver update or a new COGBox release. "
             "The game window opens and shows its intro while it works, then closes by itself.";
    const bool mine = a.run.proc && a.run.prepare && a.run.game == g;
    if (mine) {
        std::string prog;
        uint64_t done = 0, total = 0, secs = 0;
        char state[32] = {};
        if (read_file(join(game_shader_dir(g), "prepare-progress.txt"), prog, 4096))
            sscanf(prog.c_str(), "%llu %llu %31s %llu", (unsigned long long *)&done, (unsigned long long *)&total, state, (unsigned long long *)&secs);
        const float f = total ? std::min(1.0f, float(double(done) / double(total))) : 0.0f;
        const std::string what = !strcmp(state, "reading") ? "reading the game's packages and building"
                                 : !strcmp(state, "saving") ? "saving" : !strcmp(state, "done") ? "done" : "building";
        char line[200];
        snprintf(line, sizeof line, "%s - %llu of %llu (%d%%), %llu min %02llu s", what.c_str(), (unsigned long long)done,
                 (unsigned long long)total, int(f * 100.0f), (unsigned long long)(secs / 60), (unsigned long long)(secs % 60));
        ImGui::ProgressBar(f, ImVec2(g_rowX1 - ImGui::GetCursorScreenPos().x, T(26)), total ? line : "starting the game...");
        ImGui::Dummy(ImVec2(0, S(4)));
        if (small_button("Stop preparing")) TerminateProcess(a.run.proc, 0xC0B05709);   // job 666: stopped on purpose (play_poll)
    } else {
        const bool ok = supported && a.st[g].ready == R_READY && !a.run.proc && a.job.state.load() != 1;
        if (!ok) ImGui::BeginDisabled();
        if (small_button("Prepare all shaders now")) {
            settings_save(a);
            play_start(a, g, true);
        }
        if (!ok) ImGui::EndDisabled();
        ui_tip(supported ? k_prepTip : "Needs this game's next build and its recipe pack (recipes folder).");
    }
}

static void keys_section(App &a, int g)
{
    static int capture = -1, captureFrame = 0;
    static bool captureAdd = false;   // right-click: the key is added as a second key, not instead of the keys
    static std::string keyNote;   // what the last key press changed besides its own action
    static int keysGame = -1;     // review 1186: a key chosen for one game's row never lands on another game's
    if (keysGame != g) { keysGame = g; capture = -1; keyNote.clear(); }
    const auto &acts = gs::key_actions((gs::Game)g);
    ui_section("KEYBOARD KEYS");
    const std::string keysTip = tr(std::string("Used when Keyboard and mouse is on, in ") + game_def(g).shortName +
              " only - each game keeps its own keys. Choose an action, then press its new key or mouse button (Esc cancels).") +
              "\n\n" + tr("Click: set the key. Right-click: add a second key.");
    const std::string sec = keys_section_name(g);
    auto set_all = [&](const char *cvar, const std::string &v) {   // (this game's keys only, GitHub #154)
        a.ini.set(sec, cvar, v);
        a.ini.save();
        env_refresh(a, g);
    };
    if (!keyNote.empty()) ui_note(keyNote, col::amber);
    const bool capturing = capture >= 0;
    if (capturing) ImGui::BeginDisabled();   // while waiting for a key, Space / Enter mustn't press a button
    for (int i = 0; i < (int)acts.size(); i++) {
        const gs::KeyAction &k = acts[i];
        // review 0.4.5: the Walk key only with a runtime that has it (SDK patch 60) - binding it would take the key from
        // another action and send nothing
        if (!strcmp(k.cvar, "keybind_walk") && !(a.env[g].cap.runtime_has && a.env[g].cap.runtime_has("keybind_walk"))) continue;
        ImGui::PushID(k.cvar);
        std::string label = std::string(pad_name(a, k.pad)) + "  -  " + k.what;
        row_label(label.c_str(), "", true);
        std::string cur = key_get(a, g, k.cvar);
        if (cur.empty()) cur = k.def;
        std::string shown = capture == i ? std::string("Press a key...") : key_shown(cur);
        if (th_button((shown + "##k").c_str(), ImVec2(g_ctrlW, T(26)), true, F_BODY, capture == i)) { capture = i; captureAdd = false; captureFrame = ImGui::GetFrameCount(); }
        // Two keys per action: right-click adds one beside the current key (the SDK takes a comma list, "MMB,Z").
        if (!capturing && ImGui::IsItemClicked(ImGuiMouseButton_Right)) { capture = i; captureAdd = true; captureFrame = ImGui::GetFrameCount(); }
        if (!capturing) ui_tip(keysTip);
        ImGui::PopID();
    }
    if (capturing) ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (small_button("Reset keys to the Gears layout")) {
        for (const gs::KeyAction &k : acts) a.ini.set(sec, k.cvar, k.def);   // the layout itself, not the shared [Keys]
        set_all(acts[0].cvar, acts[0].def);
        capture = -1;
    }
    // the press that sets the key: a frame after the click that chose the row, so that click isn't taken as the key
    if (capture >= 0 && ImGui::GetFrameCount() > captureFrame + 1) {
        const char *name = nullptr;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) { capture = -1; return; }
        if (ImGui::IsMouseClicked(0)) name = "LMB";
        else if (ImGui::IsMouseClicked(1)) name = "RMB";
        else if (ImGui::IsMouseClicked(2)) name = "MMB";
        else if (ImGui::IsMouseClicked(3) || ImGui::IsMouseClicked(4)) {   // GitHub #208: the side buttons, SDK patch 65
            if (a.env[g].cap.runtime_has && a.env[g].cap.runtime_has("mouse side buttons bindable (MB4")) name = ImGui::IsMouseClicked(3) ? "MB4" : "MB5";
            else { keyNote = "Mouse buttons 4 and 5 need the next COGBox runtime - choose another key."; capture = -1; return; }
        }
        for (int k = ImGuiKey_NamedKey_BEGIN; !name && k < ImGuiKey_NamedKey_END; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) name = sdk_key_name((ImGuiKey)k);
        if (name && strcmp(name, "LMB") && strcmp(name, "RMB") && strcmp(name, "MMB") && strcmp(name, "MB4") && strcmp(name, "MB5"))
            if (const char *pos = sdk_scancode_name(g_lastKeyScancode)) name = pos;   // by position, as the driver sees it
        if (name) {
            // Players reported mixed-up binds: one key bound to two actions pressed both. A key now belongs to one action:
            // taken from any other action that had it (an action left with no key says so). WASD and the arrows move and
            // can't be bound to an action.
            const std::string nm = name;
            keyNote.clear();
            for (const char *mv : { "W", "A", "S", "D", "Up", "Down", "Left", "Right" })
                if (nm == mv) { keyNote = nm + " moves (W A S D and the arrow keys) - choose another key."; capture = -1; return; }
            if (nm == "Backslash") { keyNote = "\\ takes a screenshot in every game - choose another key."; capture = -1; return; }
            for (int j = 0; j < (int)acts.size(); j++) {
                if (j == capture) continue;
                std::string cur = key_get(a, g, acts[j].cvar);
                if (cur.empty()) cur = acts[j].def;
                std::string kept, tok;
                bool removed = false;
                for (size_t p0 = 0; p0 <= cur.size();) {
                    size_t p1 = cur.find(',', p0);
                    if (p1 == std::string::npos) p1 = cur.size();
                    tok = cur.substr(p0, p1 - p0);
                    p0 = p1 + 1;
                    if (tok.empty()) continue;
                    if (tok == nm) { removed = true; continue; }
                    kept += (kept.empty() ? "" : ",") + tok;
                }
                if (!removed) continue;
                a.ini.set(sec, acts[j].cvar, kept.empty() ? std::string("None") : kept);
                keyNote += std::string(keyNote.empty() ? "" : " ") + key_shown(nm) + " was taken from \"" + acts[j].what + "\"" +
                           (kept.empty() ? " - that action has no key now." : ".");
            }
            std::string value = name;
            if (captureAdd) {   // keep the action's first key that isn't this one: at most two keys
                std::string cur = key_get(a, g, acts[capture].cvar);
                if (cur.empty()) cur = acts[capture].def;
                const std::string first = cur.substr(0, cur.find(','));
                if (!first.empty() && first != "None" && first != nm) value = first + "," + nm;
            }
            set_all(acts[capture].cvar, value);
            capture = -1;
        }
    }
}

// Job 620 (GitHub #45): a game language whose text isn't on the player's copy can't be chosen - the game would fall
// back to English (the US Judgment disc has 11 languages, not German, Italian, Spanish (Spain), Russian, Portuguese
// or Japanese). Decided by the copy's Coalesced_<code>.bin; a code we don't know, or an unknown folder, stays allowed.
static bool lang_on_copy(const std::string &root, int g, int idx)
{
    static const char *code[22] = { "", "INT", "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "PTB", "POL", "RUS", "CZE", "HUN",
                                    "DUT", "DAN", "FIN", "NOR", "", "JPN", "KOR", "", "CHN" };
    if (idx <= 0 || idx >= 22 || !code[idx][0]) return true;
    if (root.empty()) return true;
    if (g == G_GEARS1) {   // UE3 2006: WarGame\Localization\Coalesced.<code> (chi deu esm esn fra int ita jpn kor)
        // the codes Gears 1 has (gears_settings choice_available limits the list to these); Chinese (Traditional) = chi
        static const char *g1code[22] = { "", "INT", "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "", "", "", "", "", "", "", "", "",
                                          "", "JPN", "KOR", "CHI", "" };
        if (!g1code[idx][0]) return false;
        const std::string dir = root + "\\WarGame\\Localization\\";
        if (GetFileAttributesW(widen(dir + "Coalesced.INT").c_str()) == INVALID_FILE_ATTRIBUTES) return true;   // unknown layout: allow
        return GetFileAttributesW(widen(dir + "Coalesced." + g1code[idx]).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    bool any = false;
    for (const char *sub : { "\\GearGame\\CookedXenon\\", "\\GearGame\\CookedXbox360\\" }) {
        const std::string dir = root + sub;
        if (GetFileAttributesW(widen(dir + "Coalesced_INT.bin").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        any = true;
        if (GetFileAttributesW(widen(dir + "Coalesced_" + code[idx] + ".bin").c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return !any;   // no Coalesced_INT.bin found: not a layout we know, allow
}

// GitHub #159: a voice language (VoiceLang, GitHub #87) needs that language's localised packages on the copy
// (<base>_LOC_<code>.xxx, the voices' twins the runtime swaps in); with none, the swap finds nothing. Gears 2, 3 and
// Judgment; Gears 1, a code we don't know or an unknown folder stay allowed.
static bool voice_on_copy(const std::string &root, int g, int idx)
{
    static const char *code[10] = { "", "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "POL", "RUS", "JPN" };
    if (idx <= 0 || idx >= 10 || root.empty() || g == G_GEARS1) return true;
    bool any = false;
    for (const char *sub : { "\\GearGame\\CookedXenon\\", "\\GearGame\\CookedXbox360\\" }) {
        const std::string dir = root + sub;
        if (GetFileAttributesW(widen(dir + "Coalesced_INT.bin").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        any = true;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(widen(dir + "*_LOC_" + code[idx] + ".xxx").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); return true; }
    }
    return !any;
}

static void option_row(App &a, int g, const gs::Option &o)
{
    gs::Game G = (gs::Game)g;
    gs::Version ver = gs::effective_version(G, a.cur[g], a.env[g]);
    gs::Availability av = gs::available(o, G, ver, a.cur[g], a.env[g]);
    std::string tip = tr(gs::tooltip_for(o, G, av, a.env[g]));   // 0.2.0
    if (!gs::wine_note(o, a.env[g]).empty()) tip += "\n" + tr(gs::wine_note(o, a.env[g]));   // proton049 (Proton / Wine only)
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
        // Shagg 3 Nov: on Proton / Wine Vulkan is the recommended path (DirectX 12 crashes there), so not "experimental"
        if (o.id == "Api" && a.env[g].wine && items.size() > 1 && items[1] == "Vulkan (experimental)") { items[0] = "DirectX 12"; items[1] = "Vulkan"; }
        for (std::string &it : items) it = tr(it);   // 0.2.0
        // notes/83: AMD FSR frame generation runs on Vulkan too, experimental there
        if (o.id == "FrameGen" && items.size() > 1 && gs::runs_vulkan(a.cur[g], a.env[g])) items[1] += std::string(" ") + tr("(experimental on Vulkan)");
        if (ImGui::BeginCombo("##v", items.empty() ? "" : items[idx].c_str())) {
            // 0.2.1: Render resolution lists 1080p in size order; the saved value is still the original index
            std::vector<int> ord;
            for (int i = 0; i < (int)items.size(); i++) ord.push_back(i);
            if (o.id == "Resolution" && items.size() == 6) ord = { 0, 5, 1, 2, 3, 4 };
            for (int i : ord) {
                if (o.id == "FrameGen" && i != idx && gs::framegen_choice_hidden(i, a.env[g])) continue;   // NVIDIA DLSS FG: only with its DLLs
                gs::Availability ca = gs::choice_available(o, G, i);
                if (ca.ok) ca = gs::wine_choice_available(o, i, a.env[g]);   // proton049: what can't work under Proton (ok on Windows)
                if (ca.ok && o.id == "FrameGen") ca = gs::framegen_choice_available(i, a.env[g], gs::runs_vulkan(a.cur[g], a.env[g]));
                // 0.3.0 (Shagg 14:18): FSR / DLSS / XeSS are DirectX 12 only - greyed while Vulkan is the Graphics API
                if (ca.ok && o.id == "AAMode" && i >= 2 && i <= 4 && gs::runs_vulkan(a.cur[g], a.env[g]) && !(i == 2 && gs::fsr_vulkan_on(a.cur[g], a.env[g]))) {   // 5 SMAA T2x: both APIs; fsr049: FSR on Vulkan (opt-in)
                    ca.ok = false;
                    ca.why = i == 2 ? (gs::fsr_vulkan(a.env[g]) ? "FSR on Vulkan is experimental - turn on \"FSR on Vulkan (experimental)\" to use it; otherwise the launcher uses SMAA."
                                                                : "FSR on Vulkan needs a newer game build - with Vulkan the launcher uses SMAA. Choose DirectX 12 as the Graphics API to use it.")
                                   : "DirectX 12 only - with Vulkan the launcher uses SMAA. Choose DirectX 12 as the Graphics API to use it.";
                }
                const std::vector<bool> &miss = o.id == "Lang" ? a.env[g].lang_missing : a.env[g].voice_missing;
                if (ca.ok && (o.id == "Lang" || o.id == "VoiceLang") && i < (int)miss.size() && miss[i]) {   // job 620 (#45), #159
                    ca.ok = false;
                    ca.why = o.id == "Lang" ? "Not on your copy of the game - it would show English. Copies from other regions have other languages."
                                            : "Not on your copy of the game - it has no voice files in this language. Copies from other regions have other languages.";
                }
                if (!ca.ok) ImGui::BeginDisabled();
                ImGui::PushID(i);   // translations can make two choices read the same
                if (ImGui::Selectable(items[i].c_str(), i == idx)) { nv = i; changed = true; }
                ImGui::PopID();
                if (!ca.ok) { ImGui::EndDisabled(); if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !ca.why.empty()) ImGui::SetTooltip("%s", a.wine ? tr(ca.why).c_str() : ca.why.c_str()); }
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
        // 0.2.1 (Shagg 16:41): moves in the option's own steps (0.05 for the mouse), with the mouse, keys and pad -
        // an integer slider over the steps, showing the value
        const double st = o.step > 0 ? o.step : 0.1;
        const int steps = std::max(1, (int)std::lround((o.max - o.min) / st));
        int k = std::max(0, std::min(steps, (int)std::lround((v - o.min) / st)));
        char fmt[24];
        snprintf(fmt, sizeof fmt, "%.*f", std::max(0, o.decimals), o.min + k * st);
        if (ImGui::SliderInt("##v", &k, 0, steps, fmt)) {
            nv = o.min + std::max(0, std::min(steps, k)) * st; changed = true;
        }
        break;
    }
    case gs::Kind::Text: {
        // 0.4.8 (GitHub #229): a one-line text box. Only the option's characters can be typed (gs::text_char_ok) and at
        // most max_len; the value kept is gs::sanitize_text's (what isn't valid is dropped and said so under the row).
        static char buf[512];
        static ImGuiID editing = 0;
        static int dropped = 0;
        static std::string droppedFor;
        const ImGuiID me = ImGui::GetID("##t");
        const std::string cur = a.cur[g].str(o.id);
        if (editing != me) snprintf(buf, sizeof buf, "%s", cur.c_str());
        const int cap = std::max(1, std::min((int)sizeof buf, (o.max_len > 0 ? o.max_len : 255) + 1));
        auto filter = [](ImGuiInputTextCallbackData *d) -> int {
            const gs::Option *op = (const gs::Option *)d->UserData;
            return gs::text_char_ok(*op, (unsigned)d->EventChar) ? 0 : 1;
        };
        ImGui::InputTextWithHint("##t", "e.g. 26.10.20.30, 26.10.20.31", buf, (size_t)cap, ImGuiInputTextFlags_CallbackCharFilter,
                                 filter, (void *)&o);
        if (ImGui::IsItemActive()) editing = me;
        if (ImGui::IsItemDeactivated()) {
            editing = 0;
            int drop = 0;
            const std::string clean = gs::sanitize_text(o, buf, &drop);
            dropped = drop; droppedFor = o.id;
            if (clean != cur) { a.cur[g].t[o.id] = clean; a.dirty[g] = true; }
            snprintf(buf, sizeof buf, "%s", clean.c_str());
        }
        if (droppedFor == o.id && dropped > 0)
            ui_note(std::to_string(dropped) + " entr" + (dropped == 1 ? "y was" : "ies were") +
                    " left out - only IPv4 addresses like 26.10.20.30 work (16 at most).", col::amber);
        break;
    }
    }
    if (!av.ok) ImGui::EndDisabled();
    if (o.id == "FrameGen" && av.ok && gs::fg_vk_nvidia_warning(a.cur[g], a.env[g]))   // vkfreeze049
        ui_note(gs::k_fgVkNvidiaWarning, col::amber);
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
    // GitHub #90: a language saved before (or from another copy) that this copy doesn't have - say so before the launch,
    // with what the game uses instead (the launcher doesn't pass it, gears_settings build_args)
    if (av.ok && (o.id == "Lang" || o.id == "VoiceLang")) {
        const int cur = (int)lround(a.cur[g].has(o.id) ? a.cur[g].f(o.id) : o.def);
        const std::vector<bool> &miss = o.id == "Lang" ? a.env[g].lang_missing : a.env[g].voice_missing;
        if (cur > 0 && cur < (int)miss.size() && miss[cur])
            ui_note(o.id == "Lang" ? "Not on your copy of the game - it will show English."
                                   : "Not on your copy of the game - the voices will be in the game language instead.",
                    col::amber);
    }
    ImGui::PopID();
}

// ---- the About tab ---------------------------------------------------------------------------------------------------
static void lamp_line(int state, const std::string &text_en, const std::string &tip_en = "")
{
    const std::string text = tr(text_en);   // 0.2.0
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    th_lamp(ImGui::GetWindowDrawList(), ImVec2(p.x + S(6), p.y + h * 0.55f), S(4.5f), state);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(20), p.y));
    ImGui::PushTextWrapPos(row_wrap_x());   // GitHub #123
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    if (!tip_en.empty()) {   // a tip may reuse a "  -  ..." line piece: shown without the dash
        std::string t = tr(tip_en);
        t.erase(0, t.find_first_not_of(" -"));
        ui_tip(t);
    }
}

// 0.2.0: a shortcut on the player's desktop to CogBox.exe with these arguments (an existing one of the same name is replaced)
static bool make_shortcut(const std::string &name, const std::string &args, const std::string &desc, std::string &err)
{
    wchar_t desk[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) { err = "no desktop folder"; return false; }
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl));
    if (ok) {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        sl->SetPath(exe);
        sl->SetArguments(widen(args).c_str());
        sl->SetWorkingDirectory(widen(exe_dir()).c_str());
        sl->SetDescription(widen(desc).c_str());
        sl->SetIconLocation(exe, 0);
        IPersistFile *pf = nullptr;
        ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf));
        if (ok) {
            std::string safe = name;
            for (char &c : safe) if (strchr("\\/:*?\"<>|", c)) c = '-';
            ok = SUCCEEDED(pf->Save((std::wstring(desk) + L"\\" + widen(safe) + L".lnk").c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    if (!ok) err = "Windows refused it";
    return ok;
}


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

// Shagg 29 Oct: "This version works" - the player says an untested game version or title update runs fine. Remembered
// by its fingerprint (no more warning for that exact file), and a pre-filled GitHub issue opens in the browser so the
// player can tell the developer by submitting it. Nothing is sent by COGBox itself.
[[maybe_unused]] static std::string url_encode(const std::string &s)   // (unused since legal2-050: tickets take no pre-filled link)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

static void confirm_works(App &a, int g, bool update)
{
    const GameDef &d = game_def(g);
    GameState &st = a.st[g];
    const std::string file = update ? join(st.updateDir, "default.xexp") : join(st.dataDir, "default.xex");
    const std::string h = cached_sha1(a.ini, file);
    if (h.empty()) { set_message(a, "The file couldn't be read - nothing was changed.", true); return; }
    a.ini.set(d.key, update ? "WorksXexp" : "WorksXex", h);
    a.ini.save();
    if (update) st.xexpConfirmed = true; else st.xexConfirmed = true;
    const std::string what = update ? std::string(d.updateName) + " (a different title update)" : std::string("a different game version");
    const std::string title = std::string("Version works: ") + d.name + " - " + (update ? "title update" : "game version") + " " + h.substr(0, 12);
    const std::string body = std::string("This version runs for me in COGBox ") + COGBOX_VERSION + ".\n\nGame: " + d.name + "\nWhat: " + what +
                             "\nFile: " + (update ? "default.xexp" : "default.xex") + "\nSHA-1: " + h + "\nSize: " + std::to_string(file_size(file)) +
                             " bytes\n\nWhere it came from (region, disc / Games on Demand / update), and anything that didn't work:\n";
    open_ticket(title, body);
    set_message(a, "Thanks - this version won't be flagged again. A GitLab ticket page opened and its details are on the clipboard: paste them and submit to let the developer know.", false);
}

// Shagg 29 Oct: "Detect best settings" (Graphics tab) - the main graphics card's name and video memory (DXGI), the
// screen's size and refresh rate, and Wine / Proton decide a tier; the changes are listed and applied only on Apply.
// SMAA for every tier (the anti-aliasing that works well today). Gameplay and controls are not touched.
struct AutoPlan { std::string gpu; unsigned vramMB = 0; int tier = 0; std::vector<std::pair<std::string, int>> set; std::string text; };

static void main_gpu_memory(std::string &name, unsigned &mb)
{
    name.clear(); mb = 0;
    HMODULE dx = LoadLibraryA("dxgi.dll");
    if (!dx) return;
    typedef HRESULT(WINAPI * CreateFn)(REFIID, void **);
    CreateFn create = (CreateFn)(void *)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1 *fac = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void **)&fac))) {
        IDXGIAdapter1 *ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND && i < 8; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                unsigned m = (unsigned)(d.DedicatedVideoMemory >> 20);
                if (m > mb) {   // the card with the most video memory is the one the game picks
                    mb = m;
                    int n = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nullptr, 0, nullptr, nullptr);
                    std::string s(n > 0 ? n - 1 : 0, '\0');
                    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, &s[0], n, nullptr, nullptr);
                    name = s;
                }
            }
            ad->Release();
        }
        fac->Release();
    }
    FreeLibrary(dx);
}

static AutoPlan auto_plan(App &a, int g)
{
    AutoPlan p;
    main_gpu_memory(p.gpu, p.vramMB);
    std::string n = p.gpu;
    for (char &c : n) c = (char)tolower((unsigned char)c);
    const bool integrated = n.find("uhd") != std::string::npos || n.find("iris") != std::string::npos ||
                            n.find("radeon(tm) graphics") != std::string::npos || n.find("radeon graphics") != std::string::npos ||
                            n.find("vangogh") != std::string::npos || n.find("custom gpu 0405") != std::string::npos || p.vramMB < 2048;
    const bool modern = n.find("rtx 30") != std::string::npos || n.find("rtx 40") != std::string::npos || n.find("rtx 50") != std::string::npos ||
                        n.find("rx 6") != std::string::npos || n.find("rx 7") != std::string::npos || n.find("rx 9") != std::string::npos ||
                        n.find("arc") != std::string::npos;
    const bool deck = a.env[g].wine && (integrated || n.find("amd custom") != std::string::npos);
    p.tier = integrated || deck ? 0 : (p.vramMB >= 10240 || (p.vramMB >= 8000 && modern)) ? 2 : p.vramMB >= 5800 ? 1 : 0;
    const int hz = a.env[g].refresh_hz;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // Render resolution: 0 = 1x, 1 = 2x, 4 = match my monitor, 5 = 1080p (720p upscaled)
    int res = p.tier == 2 ? 4 : p.tier == 1 ? (sh <= 1440 ? 4 : 1) : (sw >= 1920 ? 5 : 0);
    // Frame rate limit (FrameCap indices): 0 = 30, 2 = 45, 3 = 60, 5 = 120. Job 638: the Steam Deck gets 45.
    int fps = p.tier == 2 ? (hz >= 115 ? 5 : 3) : p.tier == 1 ? (hz >= 115 ? 5 : 3) : (deck ? 2 : 3);
    if (p.tier == 1 && res == 4 && sh > 1080) fps = 3;   // mid cards at native 1440p: 60
    int pool = p.vramMB >= 8000 ? 2 : 1;                   // Large (256 MB) / Standard
    p.set = { { "AAMode", 1 }, { "Resolution", res }, { "FrameCap", fps }, { "Vsync", 1 }, { "Pool", pool },
              { "Aniso", p.tier >= 1 ? 1 : 0 }, { "Trilinear", 1 }, { "FrameGen", 0 }, { "Preset", 3 } };
    if (!a.env[g].wine) p.set.push_back({ "Api", 0 });     // DirectX 12; under Wine / Proton the API is left as it is
    if (const auto *ro = gs::find_option((gs::Game)g, "RealOcclusion")) p.set.push_back({ "RealOcclusion", (int)ro->def });   // job 666: the game's default (on in Gears 3 / Judgment)
    static const char *tierName[] = { "entry level / integrated / Steam Deck", "mid range", "high end" };
    static const char *resName[] = { "1x - 720p", "2x - 1440p", "3x - 4K", "4x - 5K", "Match my monitor (native)", "1080p (720p upscaled)" };
    static const char *fpsName[] = { "30", "45", "60", "90", "120", "144", "165", "Unlimited" };
    char b[512];
    snprintf(b, sizeof b, "Found: %s (%u MB video memory), screen %dx%d at %d Hz%s - %s.\n\n", p.gpu.empty() ? "an unknown graphics card" : p.gpu.c_str(),
             p.vramMB, sw, sh, hz, a.env[g].wine ? ", Wine / Proton" : "", tierName[p.tier]);
    p.text = b;
    p.text += std::string("Render resolution: ") + resName[res] + "\nAnti-aliasing: SMAA\nFrame rate limit: " + fpsName[fps] +
              "\nVsync: on\nTexture memory: " + (pool == 2 ? "Large (256 MB)" : "Standard") + "\nSharper textures (16x anisotropic): " +
              (p.tier >= 1 ? "on" : "off") + "\nSmooth texture detail steps (trilinear): on\nFrame generation: off" +
              (a.env[g].wine ? "" : "\nGraphics API: DirectX 12") + "\n\nGameplay and controls are not changed. Apply?";
    return p;
}

void auto_settings_apply(App &a, int g)
{
    if (g < 0 || g >= G_COUNT) return;
    AutoPlan p = auto_plan(a, g);
    for (const auto &kv : p.set)
        if (gs::find_option((gs::Game)g, kv.first)) a.cur[g].set(kv.first, kv.second);
    a.dirty[g] = true;
    set_message(a, "Settings detected and applied. Change any of them as you like.", false);
}

// 0.4.6 (Shagg 5 Nov): mods have their own tab (they were a section of About)
static void mods_tab(App &a, int g)
{
    bool running = a.run.proc != nullptr;
    // 0.2.0: mods - <Game>\mods\<mod>\, laid out like the game's data folder; a later one wins
    ui_section("MODS");
    {
        static const char *k_modsTip = "Ticked mods are used, top to bottom - where two change the same file, the lower one wins. Changes apply the next time the game starts.";
        static const char *k_noModsTip = "No mods. Add a mod's .zip, or put its folder in this game's mods folder - laid out like the game's own files (GearGame\\...).";
        static std::vector<ModItem> ml[G_COUNT];
        static Uint64 mlAt[G_COUNT] = {};
        if (!mlAt[g] || SDL_GetTicks() - mlAt[g] > 2000) { ml[g] = mods_list(a, g); mlAt[g] = SDL_GetTicks(); }
        if (ml[g].empty()) { lamp_line(0, "No mods yet"); ui_tip(k_noModsTip); }
        bool changed = false;
        const float bw = T(90);   // Up / Down: one column at the right, the same size
        for (size_t i = 0; i < ml[g].size(); i++) {
            const ModItem &m = ml[g][i];
            ImGui::PushID((int)i + 8000);
            if (running) ImGui::BeginDisabled();
            bool on = m.enabled;
            if (ImGui::Checkbox("##on", &on)) { mods_set_enabled(a, g, m.folder, on); changed = true; }
            ui_tip(k_modsTip);
            ImGui::SameLine();
            std::string line = m.name + (m.version.empty() ? "" : "  " + m.version) + (m.author.empty() ? "" : "  -  " + m.author);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(line.c_str());
            if (!m.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.description.c_str());
            const float colX = g_rowX1 - ImGui::GetWindowPos().x + ImGui::GetScrollX() - bw * 2 - S(8);
            if (i > 0) { ImGui::SameLine(colX); if (th_button(tr("Up"), ImVec2(bw, T(26)), true, F_BODY)) { mods_move(a, g, m.folder, -1); changed = true; } }
            if (i + 1 < ml[g].size()) {
                ImGui::SameLine(colX + bw + S(8));
                if (th_button(tr("Down"), ImVec2(bw, T(26)), true, F_BODY)) { mods_move(a, g, m.folder, 1); changed = true; }
            }
            if (running) ImGui::EndDisabled();
            // Gears 3: what the game will make of it (mods.cpp apply_mod_rules)
            if (m.enabled && !m.problem.empty()) lamp_line(3, "Left out by the game: " + m.problem);
            else if (m.enabled && !m.campaign.empty()) lamp_line(1, m.campaign);
            for (const std::string &n : m.notes) ui_note("mod.ini: " + n, col::amber);
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        if (running) ImGui::BeginDisabled();
        if (small_button("Add a mod (.zip)...")) {
            std::string p = pick_file(L"Choose a mod's .zip", L"Zip files\0*.zip\0All files\0*.*\0");
            std::string err;
            if (!p.empty()) {
                if (mods_add_zip(a, g, p, err)) set_message(a, "The mod is added and ticked.", false);
                else set_message(a, err, true);
                changed = true;
            }
        }
        ui_tip(ml[g].empty() ? k_noModsTip : k_modsTip);
        if (running) ImGui::EndDisabled();
        ImGui::SameLine();
        if (small_button("Mods folder")) { make_dirs(mods_dir(g)); shell_open(mods_dir(g)); }
        ui_tip(k_noModsTip);
        if (changed) { mlAt[g] = 0; env_refresh(a, g); }
    }
}

static void about_tab(App &a, int g)
{
    const GameDef &d = game_def(g);
    GameState &st = a.st[g];
    bool busy = a.job.state.load() == 1, running = a.run.proc != nullptr;

    // ---- COGBOX: the version and the update
    ui_section("COGBOX");
    std::string newer = update_newer();
    if (!newer.empty()) {
        lamp_line(2, "COGBox " + newer + " is out (this is " COGBOX_VERSION ").");
        float p = 0; std::string err;
        int s = update_install_state(&p, &err);
        if (s == 1 || s == 2) ImGui::ProgressBar(p, ImVec2(g_rowX1 - ImGui::GetCursorScreenPos().x, 0), s == 1 ? "Downloading..." : "Unpacking...");
        else if (s == 3) { static bool rs = false; if (!rs) { rs = true; if (update_restart()) a.quit = true; } else if (small_button("Restart COGBox")) { if (update_restart()) a.quit = true; } }   // 0.2.2: by itself
        else {
            if (s == -1) lamp_line(3, err);
            if (small_button("Update")) {
                if (running) set_message(a, "Close the game first.", true); else update_install_start(newer);
            }
            ImGui::SameLine();
            if (small_button("Release page")) shell_open(update_page());
        }
    } else if (!a.ini.geti("Updates", "Check", 1)) lamp_line(0, "COGBox " COGBOX_VERSION " - checking for updates is off.");
    else lamp_line(update_checked() ? 1 : 0, update_checked() ? "COGBox " COGBOX_VERSION " - the latest (or the update server couldn't be reached)." : "COGBox " COGBOX_VERSION " - checking for updates...");

    // ---- proton049: LINUX / STEAM DECK - what COGBox found, and the recommended settings
    if (a.wine) {
        ui_section(a.proton.steamDeck ? "STEAM DECK" : "LINUX");
        lamp_line(a.proton.proton || a.mac ? 1 : 2, std::string(tr("Running through")) + " " + proton::summary(a.proton),
                  a.proton.proton || a.mac ? "" : "plain Wine: COGBox is tested with Steam's Proton (see the README's Linux and Steam Deck section)");
        if (!a.mac) {
            if (small_button("Use recommended Steam Deck / Proton settings")) linux_recommended_ask(a, g);
            ui_tip("Vulkan, FSR and no frame generation; on a Steam Deck also borderless on its 1280x800 screen, 60 fps, Steam Deck button prompts and the controller. You see the list before anything changes.");
        }
    }

    // ---- YOUR COPY: game files, title update, port (the folders are in the tooltips)
    ui_section("YOUR COPY");
    if (st.dataDir.empty()) lamp_line(3, "Not added yet. Use Add your copy below (a disc image, a Games on Demand copy, or an extracted folder).");
    else {
        // 0.4.6: the path and a two-word state on the line; the explanation is its tooltip
        lamp_line(st.xexMatch || st.xexConfirmed ? 1 : st.xexKnown ? 2 : 0,
                  std::string("Game files: ") + shown_path(a, st.dataDir) + (st.dataOwn ? "  (your own folder)" : "") +
                  (st.xexMatch ? "" : st.xexConfirmed ? "  -  confirmed works" : st.xexKnown ? "  -  untested version" : "  -  not checked yet"),
                  st.xexMatch ? "  -  the release the ports are made for"
                  : st.xexConfirmed ? "  -  an untested version you confirmed works"
                  : st.xexKnown ? "  -  an untested version: it may work, but you may experience problems" : "");
        if (st.xexKnown && !st.xexMatch && !st.xexConfirmed) {
            ImGui::PushID("works_xex");
            if (small_button("This version works")) confirm_works(a, g, false);
            ImGui::PopID();
        }
    }
    if (st.updateDir.empty())
        lamp_line(2, std::string(d.updateName) + ": not added - required to play");   // title updates required (0.4.0)
    else {
        lamp_line(st.xexpMatch || st.xexpConfirmed ? 1 : 2, std::string(d.updateName) + ": " + shown_path(a, st.updateDir) +
                  (st.xexpMatch ? "" : st.xexpConfirmed ? "  -  confirmed works" : "  -  untested update"),
                  st.xexpMatch ? "" : st.xexpConfirmed ? "  -  an untested update you confirmed works" : "  -  an untested update: it may work, but problems are likely. Please don't report problems you get with it - use the expected update first");
        if (!st.xexpMatch && !st.xexpConfirmed) {
            ImGui::PushID("works_xexp");
            if (small_button("This update works")) confirm_works(a, g, true);
            ImGui::PopID();
        }
    }
    std::string exe = st.exe();
    // 0.2.0 (#3 / #7): name the file that is missing, so a screenshot says what's wrong
    lamp_line(exe.empty() ? 3 : 1, exe.empty() ? std::string("The port program is missing: expected ") + shown_path(a, join(join(game_root(g), *d.portTu ? d.portTu : d.portDisc), d.exeName)) +
                                                     " - unzip the release again, keeping its folders." : "Port: " + shown_path(a, exe));
    // 0.2.0: downloadable content from the player's own console; one compact list under the copy
    const std::vector<DlcItem> &dl = dlc_list(g);
    for (size_t i = 0; i < dl.size(); i++) {
        ImGui::PushID((int)i + 7000);
        lamp_line(1, dl[i].title + (dl[i].title != dl[i].name ? "  (" + dl[i].name + ")" : ""));
        ImGui::SameLine();
        if (busy || running) ImGui::BeginDisabled();
        if (small_button("Remove")) dlc_remove_ask(a, g, dl[i]);
        if (busy || running) ImGui::EndDisabled();
        ImGui::PopID();
    }
    // 0.4.6: drag and drop is the only way to add a copy, title update or DLC
    ImGui::Dummy(ImVec2(0, S(2)));
    ui_note("To add or replace your copy or the title update, drag and drop it onto this window.", col::amber);
    ui_tip("To add downloadable content, drag and drop the package onto this window.");
    if (a.wine) {   // proton049: no drag and drop in the Steam Deck's Game Mode
        ImGui::Dummy(ImVec2(0, S(2)));
        linux_copy_buttons(a);
    }

    // ---- FOLDERS: folders, saves, shaders and desktop shortcuts
    ui_section("FOLDERS");
    if (small_button("Saves")) { make_dirs(game_user_dir(g)); shell_open(game_user_dir(g)); }
    ImGui::SameLine();
    if (small_button("Logs")) { make_dirs(game_logs_dir(g)); shell_open(game_logs_dir(g)); }
    ImGui::SameLine();
    if (small_button("Back up saves")) {
        if (running) set_message(a, std::string(d.name) + " is running - close it first, so the save is complete.", true);
        else if (!any_files(game_user_dir(g))) set_message(a, std::string(d.name) + " has no saves here yet.", false);
        else ask(a, "backup", g, "Back up saves", std::string("Put a copy of ") + d.name + (a.wine ? "'s saves in a zip in COGBox's backups folder?" : "'s saves in a zip on your desktop?"), { "Back up", "Cancel" });
    }
    ImGui::SameLine();
    if (small_button("Clear shaders...")) {
        if (!dir_exists(game_shader_dir(g))) set_message(a, std::string(d.name) + " has no saved shaders to clear.", false);
        else ask(a, "clearshaders", g, "Clear shaders",
                 std::string("Clear the saved shaders and pipelines of ") + d.name + "?\n\nThe next start builds them again, so expect "
                 "stutter and pop-in the first time, as on a new PC. The folder is set aside (renamed), not deleted.", { "Clear", "Cancel" });
    }
    // 0.2.0: desktop shortcuts - COGBox itself, and this game (it opens COGBox and starts the game)
    static const char *k_pinTip = "To pin one to the taskbar or Start, right-click the shortcut and choose Pin (Windows doesn't let programs do that themselves).";
    if (small_button("Desktop shortcut: COGBox")) {
        std::string err;
        if (make_shortcut("COGBox", "", "Gears of War native PC ports - launcher", err)) set_message(a, "A COGBox shortcut is on your desktop.", false);
        else set_message(a, "The shortcut couldn't be made: " + err, true);
    }
    ui_tip(k_pinTip);
    ImGui::SameLine();
    {
        const std::string label = std::string("Desktop shortcut: ") + d.name;
        if (small_button(label.c_str())) {
            std::string err;
            if (make_shortcut(d.name, std::string("--play ") + d.key, std::string("Play ") + d.name + " through COGBox", err))
                set_message(a, std::string("A ") + d.name + " shortcut is on your desktop.", false);
            else set_message(a, "The shortcut couldn't be made: " + err, true);
        }
        ui_tip(k_pinTip);
    }

    // ---- PROBLEMS: report, what to test, issues, reset
    ui_section("PROBLEMS");
    if (small_button("Report a problem")) {
        std::string s = newest_session(g);
        if (s.empty()) set_message(a, std::string("No play of ") + d.name + " yet - play once, then make the report.", false);
        else if (!a.ini.geti("Notices", "ReportExperimentalNever", 0)) {   // Shagg 31 Oct: experimental settings off first
            ask(a, "reportexp", g, "Before you make the report",
                "Experimental settings (FSR / DLSS / XeSS, XeGTAO, frame generation and others) are for "
                "testing, not for regular play - for the smoothest play, use settings that aren't marked experimental.\n\n"
                "Please turn every experimental setting off, play again and then make the report - unless the report is "
                "about one of those settings. The report lists the experimental settings that were on.",
                { "Make the report", "Cancel" });
            a.modal.check = "Don't show this again";
            a.modal.data = s;
        } else report_now(a, g, s);
    }
    ui_tip("A problem report is one zip on your desktop with the last play's log, the settings and a description of this PC "
              "(no saves, no game files; your Windows user name is taken out). Nothing is sent - attach it to a GitLab ticket.");
    // Shagg 31 Oct: what this release needs players to test (k_helpTest, top of this file) - now a dialog
    {
        std::string list;
        for (const HelpTest &h : k_helpTest) if (h.games & (1u << g)) list += std::string("\n  -  ") + tr(h.text);
        if (!list.empty()) {
            ImGui::SameLine();
            if (small_button("What to test..."))
                ask(a, "helptest", g, "HELP US TEST",
                    std::string(tr("We're still tracking these down. If one of them happens to you, press Report a problem (below) - that is all "
                                   "we need.")) + "\n" + list, { "Close" });
        }
    }
    ImGui::SameLine();
    if (small_button("Open the tickets on GitLab")) shell_open(kTicketsUrl);
    ImGui::SameLine();
    if (small_button("Reset settings to defaults...")) ask(a, "reset", g, "Reset to defaults", "Put every setting of every game back to its default? Your saves are not touched.", { "Reset", "Cancel" });

    // ---- SETTINGS: COGBox's own (the explanations are tooltips on the labels)
    ui_section("SETTINGS");
    {   // 0.2.0: COGBox's language
        const auto &L = languages();
        const std::string cur = a.ini.get("Launcher", "Language").empty() ? "auto" : a.ini.get("Launcher", "Language");
        std::string shown = std::string(tr("Automatic")) + " (" + tr("Windows' language") + ")";
        for (const Language &l : L) if (l.code == cur) shown = l.name;
        row_label("Language", tr("Translations are written by the project; corrections are welcome in a GitLab ticket."), true);
        ImGui::SetNextItemWidth(g_ctrlW);
        if (ImGui::BeginCombo("##lang", shown.c_str())) {
            if (ImGui::Selectable((std::string(tr("Automatic")) + " (" + tr("Windows' language") + ")").c_str(), cur == "auto")) {
                a.ini.set("Launcher", "Language", "auto"); a.ini.save(); i18n_load("auto");
            }
            for (const Language &l : L)
                if (ImGui::Selectable(l.name.c_str(), cur == l.code)) { a.ini.set("Launcher", "Language", l.code); a.ini.save(); i18n_load(l.code); }
            ImGui::EndCombo();
        }
    }
    static const char *k_winTip = "COGBox draws its own minimise, maximise and close buttons in the top-right corner; drag the strip beside them to move the window.";
    bool fs = a.fullscreen;   // job 523
    if (ImGui::Checkbox(tr("Fullscreen (F11, or LT + RT together)"), &fs)) set_fullscreen(a, fs);
    ImGui::SameLine(0, S(24));
    bool bl = a.borderless;   // job 523
    if (ImGui::Checkbox(tr("Borderless window (no title bar)"), &bl)) set_borderless(a, bl);
    ui_tip(k_winTip);
    bool chk = a.ini.geti("Updates", "Check", 1) != 0;
    if (ImGui::Checkbox(tr("Check GitLab for a newer COGBox at start"), &chk)) { a.ini.seti("Updates", "Check", chk ? 1 : 0); a.ini.save(); }
    ui_tip("Only the release page's address is asked for; nothing about you or this PC is sent.");

    // ---- LICENCES (0.4.8): the third-party terms, and NVIDIA's attribution for DLSS / DLSS Frame Generation / Reflex
    // (NVIDIA RTX SDKs License 7.1(b): attribute the SDK in the application's about box)
    ui_section("LICENCES");
    ui_note("COGBox is MIT-licensed. Libraries, fonts and vendor SDKs come under their own terms, in the licenses folder.");
    // 0.4.9: credits for the vendor upscaler / frame-generation SDKs (NVIDIA RTX SDKs License 6.1(b) / 7.1(b): text only,
    // no logos). The label is translated (lang files); the credit and trademark sentences stay English on purpose: they are
    // not in the lang files, so tr() returns them as they are.
    ui_note("Upscaling and frame generation technology:");
    ui_note("NVIDIA\u00AE DLSS \u2014 Uses NVIDIA DLSS technology (the NVIDIA DLSS SDK, the NVIDIA Streamline SDK and the NVIDIA Reflex SDK). "
            "NVIDIA, GeForce RTX, DLSS and Reflex are trademarks of NVIDIA Corporation.");
    ui_note("AMD FidelityFX\u2122 Super Resolution (FSR) \u2014 Uses AMD FidelityFX FSR technology (AMD FidelityFX SDK). "
            "AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.");
    ui_note("Intel\u00AE XeSS \u2014 Uses Intel XeSS technology (XeSS Super Resolution, XeSS Frame Generation, XeLL). "
            "Intel and XeSS are trademarks of Intel Corporation or its subsidiaries.");
    if (small_button("Licences folder")) {
        const std::string ld = join(exe_dir(), "licenses");
        if (dir_exists(ld)) shell_open(ld); else set_message(a, "The licenses folder is missing - unzip the release again.", true);
    }
}

// ---- the settings tabs' groups (presentation only: which heading an option shows under) -------------------------------
struct TabGroups { const char *heading[4]; const char *const *ids[4]; };
static const char *const k_dispScreen[] = { "Fullscreen", "WindowSize", "Monitor", "Ultrawide", "AspectRatio", "HideNotes", nullptr };
static const char *const k_dispFrames[] = { "Vsync", "FrameCap", "FpsMode", nullptr };
static const char *const k_dispPicture[] = { "Brightness", "HDR", "PaperWhite", "Peak", nullptr };
static const char *const k_gfxQuality[] = { "Api", "WineApi", "Preset", "Resolution", "AAMode", "DlssModel", "Sharpen", "SceneMode", "FrameGen", nullptr };
static const char *const k_gfxDetail[] = { "Aniso", "Trilinear", "MipFade", "Pool", "DrawDist", "AO", nullptr };
static const char *const k_gfxCamera[] = { "MotionBlur", "DoF", "DofScale", "Bloom", "FilmGrain", "Shake", "Fov", nullptr };
static const char *const k_ctlPad[] = { "PadLayout", "Vibration", "PadPrompts", "PromptStyle", "DeadZone", "RadialDZ", "LowLag", nullptr };
static const char *const k_ctlMouse[] = { "Mnk", "Sens", "AimTurn", "WalkMode", nullptr };
static const char *const k_playGame[] = { "Version", "Lang", "VoiceLang", "SkipLogos", "Mute", nullptr };
static const char *const k_playPlayers[] = { "Players", "SplitLayout", "SystemLink", "PeerIps", nullptr };
static const char *const k_playExtras[] = { "OriginalUnlocks", "Cheats", "Mutators", nullptr };
static const char *const k_achPopups[] = { "AchievementStyle", "AchievementColour", "AchievementPosition", "AchievementSound",
                                           "HideAchievementPopups", nullptr };   // PR #187
static const char *const k_none[] = { nullptr };
static const TabGroups k_groups[5] = {   // one per gs::Tab, in its order
    { { "SCREEN", "FRAME RATE", "PICTURE", nullptr }, { k_dispScreen, k_dispFrames, k_dispPicture, k_none } },
    { { "QUALITY", "DETAIL", "CAMERA AND EFFECTS", nullptr }, { k_gfxQuality, k_gfxDetail, k_gfxCamera, k_none } },
    { { "CONTROLLER", "KEYBOARD AND MOUSE", nullptr, nullptr }, { k_ctlPad, k_ctlMouse, k_none, k_none } },
    { { "GAME", "PLAYERS", "EXTRAS", nullptr }, { k_playGame, k_playPlayers, k_playExtras, k_none } },
    { { "ACHIEVEMENT POPUPS", nullptr, nullptr, nullptr }, { k_achPopups, k_none, k_none, k_none } },
};

// ---- the page --------------------------------------------------------------------------------------------------------
static const char *k_tabs[] = { "DISPLAY", "GRAPHICS", "CONTROLS", "GAMEPLAY", "ACHIEVEMENTS", "SOCIAL", "MODS", "ABOUT" };   // the one sound option is in Gameplay
static const int k_tabCount = 8;
static const int k_tabSocial = 5;   // social step 3; the tabs before it are gs::Tab's (ACHIEVEMENTS = gs::Tab::Achievements)
static const int k_tabMods = 6;     // ABOUT is the last

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
    std::string fps = choice_text("FrameCap"), pr = choice_text("Preset"), aa = choice_text("AAMode");
    if (!fps.empty()) s += "   |   " + fps + (fps == "Unlimited" ? "" : " fps");
    if (!aa.empty() && aa != "Off") s += "   |   " + aa;
    if (!pr.empty()) s += "   |   " + pr;
    return tr(s);   // 0.2.0
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
    dl->AddText(F_BODY, FS(F_BODY), ImVec2(p0.x + S(114), p0.y + S(22) + FS(F_TITLE) + S(2)), U(col::dim), tr(sub).c_str());
    {
        struct L { int s; const char *t; } lamps[3] = {
            { a.st[g].dataDir.empty() ? 3 : a.st[g].xexMatch ? 1 : 2, "Game" },
            { a.st[g].updateDir.empty() ? (d.updateNeeded ? 2 : 0) : a.st[g].xexpMatch ? 1 : 2, "Update" },
            { a.padConnected ? 1 : 0, "Controller" } };
        float x = p1.x - S(24);
        for (int i = 2; i >= 0; i--) {
            lamps[i].t = tr(lamps[i].t);   // 0.2.0
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
    g_sections = 0;
    if (a.tab < k_tabSocial) {
        gs::Tab want = (gs::Tab)a.tab;
        int shown = 0;
        if (want == gs::Tab::Graphics) {   // Shagg 29 Oct
            if (small_button("Detect best settings")) {
                AutoPlan p = auto_plan(a, g);
                ask(a, "autoset", g, "Detect best settings", p.text, { "Apply", "Cancel" });
            }
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        // grouped under a few headings (k_groups). An option not in the table shows in its tab's first group, so a
        // new option in gears_settings is never hidden.
        std::vector<const gs::Option *> mine;
        for (const gs::Option &o : gs::options((gs::Game)g)) {
            if (o.tab != want) continue;
            if (o.id == "WineApi" && !a.env[g].wine) continue;   // Shagg 13:16: Proton / Wine only
            mine.push_back(&o);
        }
        const TabGroups &tg = k_groups[(int)want];
        auto group_of = [&](const std::string &id) {
            for (int k = 0; k < 4 && tg.heading[k]; k++)
                for (const char *const *x = tg.ids[k]; *x; x++) if (id == *x) return k;
            return 0;
        };
        for (int k = 0; k < 4 && tg.heading[k]; k++) {
            bool any = false;
            for (const gs::Option *o : mine) any |= group_of(o->id) == k;
            if (!any) continue;
            ui_section(tg.heading[k]);
            for (const gs::Option *o : mine) if (group_of(o->id) == k) { option_row(a, g, *o); shown++; }
        }
        if (want == gs::Tab::Controls) keys_section(a, g);   // 0.2.0; GitHub #154: per game
        if (want == gs::Tab::Graphics) dlss_file_section(a, g);   // 0.4.9
        if (want == gs::Tab::Graphics) prepare_section(a, g);   // job 641
        if (want == gs::Tab::Achievements) achievements_tab(a, g, g_labelW, g_ctrlW, g_rowX1);   // achievements part 4
        else if (!shown) ui_note("Nothing to set here yet.");
    } else if (a.tab == k_tabSocial) social_tab(a, g, g_labelW, g_ctrlW, g_rowX1);   // social step 3
    else if (a.tab == k_tabMods) mods_tab(a, g);
    else about_tab(a, g);
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
    // 0.2.1: say why PLAY is greyed
    if (!ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const GameDef &gd = game_def(g);
        std::string why = a.run.proc ? "The game is already running." : a.job.state.load() == 1 ? "Wait until the copy is finished." :
                          a.st[g].ready == R_NEED_UPDATE ? std::string("Needs ") + gd.updateName + " - add it from your console or its package file" :
                          a.st[g].ready == R_COPY_MISSING ? "Your copy isn't where it was - is its drive connected?" :
                          a.st[g].ready == R_NO_PORT ? "Not in this release of COGBox yet" : "Add your copy of the game first (Add your copy... above).";
        ImGui::SetTooltip("%s", tr(why).c_str());
    }
    ImGui::PopItemFlag();
}
