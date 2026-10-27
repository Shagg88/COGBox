// COGBox - a launcher for the Gears of War native PC ports (Gears of War 2, Gears of War 3, Gears of War: Judgment).
// It finds the player's own copies, sets them up, shows the games as boxes on a carousel, keeps each game's settings,
// starts the right port with them, explains in plain words when a game can't start, keeps itself up to date from
// GitHub and puts the logs into one zip for a bug report. Built after Athelas (the same author's launcher for the
// Lord of the Rings games): C++17, Dear ImGui on SDL3, cross-compiled with MinGW.
#include "app.h"
#include "theme.h"
#include "update.h"
#include "report.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"
#include "../third_party/miniz/miniz.h"

#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

void set_message(App &a, const std::string &m, bool bad)
{
    a.message = m;
    a.messageBad = bad;
    a.messageAt = ImGui::GetTime();
}

void ask(App &a, const std::string &tag, int game, const std::string &title, const std::string &text, std::vector<std::string> buttons,
         const std::string &data)
{
    a.modal.open = true;
    a.modal.tag = tag;
    a.modal.game = game;
    a.modal.title = title;
    a.modal.text = text;
    a.modal.buttons = std::move(buttons);
    a.modal.answer = -1;
    a.modal.data = data;
}

void refresh_games(App &a, bool hash)
{
    for (int g = 0; g < G_COUNT; g++) {
        game_refresh(g, a.ini, a.st[g], hash);
        env_refresh(a, g);
    }
    if (hash) a.ini.save();
}

// ---- answers to the questions ----------------------------------------------------------------------------------------
static bool zip_folder(const std::string &dir, const std::string &zipPath, std::string &err)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 1 << 20)) { err = "couldn't start the zip"; return false; }
    bool ok = true;
    std::vector<std::string> stack = { "" };
    while (!stack.empty() && ok) {
        std::string rel = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(widen(join(join(dir, rel), "*")).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = narrow(fd.cFileName);
            if (n == "." || n == "..") continue;
            std::string r = rel.empty() ? n : rel + "\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // links aren't followed into the backup (review 11)
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(r); continue; }
            std::string data;
            if (!read_file(join(dir, r), data, 1u << 30)) { ok = false; err = "couldn't read " + r; break; }
            std::string zn = r;
            std::replace(zn.begin(), zn.end(), '\\', '/');
            ok = mz_zip_writer_add_mem(&z, zn.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION) != 0;
        } while (ok && FindNextFileW(f, &fd));
        FindClose(f);
    }
    void *buf = nullptr;
    size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &buf, &size) != 0;
    if (ok) ok = write_file(zipPath, std::string((const char *)buf, size));
    if (buf) mz_free(buf);
    mz_zip_writer_end(&z);
    if (!ok && err.empty()) err = "couldn't write the zip (is the disk full?)";
    return ok;
}

static void answered(App &a)
{
    Modal m = a.modal;      // a copy: the answer may ask a new question
    a.modal.open = false;
    int g = m.game;
    const GameDef &d = game_def(g < 0 ? 0 : g);
    if (m.tag == "crash") play_answer(a, m);
    else if (m.tag == "replace" && m.answer == 0) install_confirmed(a);
    else if (m.tag == "backup" && m.answer == 0) {
        std::string zip = join(desktop_dir(), std::string("CogBox-saves-") + d.dir + "-" + now_stamp() + ".zip"), err;
        if (zip_folder(game_user_dir(g), zip, err))
            ask(a, "info", g, "Back up saves", "Saved on your desktop:\n" + zip + "\n\nTo restore: close the game and unzip it into\n" + game_user_dir(g), { "OK" });
        else set_message(a, "Couldn't back up the saves: " + err, true);
    } else if (m.tag == "clearshaders" && m.answer == 0) {
        std::string dir = game_shader_dir(g);
        if (MoveFileExW(widen(dir).c_str(), widen(dir + ".cleared-" + now_stamp()).c_str(), 0)) set_message(a, std::string("Cleared the shaders of ") + d.name + ".", false);
        else set_message(a, "Couldn't clear them - is the game running?", true);
    } else if (m.tag == "reset" && m.answer == 0) {
        std::string p = join(exe_dir(), "settings.ini");
        if (file_exists(p)) MoveFileExW(widen(p).c_str(), widen(p + ".before-reset-" + now_stamp()).c_str(), MOVEFILE_REPLACE_EXISTING);
        a.store.reset();
        for (int i = 0; i < G_COUNT; i++) { a.cur[i] = a.store.view((gs::Game)i, a.env[i]); a.dirty[i] = false; }
        settings_save(a);
        set_message(a, "Every setting is back to its default.", false);
    }
}

static void draw_modal(App &a, ImVec2 size)
{
    Modal &m = a.modal;
    if (!m.open) return;
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    (void)fg;
    float w = std::min(size.x - S(60), S(620));
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (!ImGui::IsPopupOpen("##modal")) ImGui::OpenPopup("##modal");
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(24)));
    if (ImGui::BeginPopupModal("##modal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImVec2 p0 = ImGui::GetWindowPos(), p1(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        th_plate(ImGui::GetWindowDrawList(), p0, p1, S(14), false);
        th_heading(m.title.c_str(), p1.x - S(28));
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PushTextWrapPos(w - S(28));
        ImGui::TextUnformatted(m.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(10)));
        float bw = 0;
        for (auto &b : m.buttons) bw += ImGui::CalcTextSize(b.c_str()).x + S(44) + S(12);
        ImGui::SetCursorPosX(std::max(S(28), w - S(28) - bw + S(12)));
        for (size_t i = 0; i < m.buttons.size(); i++) {
            if (i) ImGui::SameLine(0, S(12));
            const std::string &b = m.buttons[i];
            ImVec2 bs(ImGui::CalcTextSize(b.c_str()).x + S(44), S(38));
            bool hit = i == 0 ? th_button_red(b.c_str(), bs, true, F_BOLD) : th_button(b.c_str(), bs, true, F_BOLD);
            if (i == 0 && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
            if (hit) m.answer = (int)i;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) m.answer = (int)m.buttons.size() - 1;
        if (m.answer >= 0) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (m.answer >= 0) answered(a);
}

// ---- the whole window ------------------------------------------------------------------------------------------------
// ---- job 523: the window - zoom with its size, and borderless (our own title strip) ------------------------------
static float g_ui0 = 1.0f;          // g_ui at the starting window size
static int g_w0 = 1, g_h0 = 1;      // the starting window size
static App *g_app = nullptr;
static bool g_dragTop = false;      // last frame: the mouse is over the title strip's empty part

static SDL_HitTestResult window_hit(SDL_Window *w, const SDL_Point *p, void *)
{
    if (!g_app || !g_app->borderless) return SDL_HITTEST_NORMAL;
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED) return g_dragTop && p->y < (int)S(20) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
    int W = 0, H = 0;
    SDL_GetWindowSize(w, &W, &H);
    const int e = std::max(4, (int)(6 * g_ui0));
    const bool l = p->x < e, r = p->x >= W - e, t = p->y < e, b = p->y >= H - e;
    if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (t) return SDL_HITTEST_RESIZE_TOP;
    if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    if (l) return SDL_HITTEST_RESIZE_LEFT;
    if (r) return SDL_HITTEST_RESIZE_RIGHT;
    return g_dragTop && p->y < (int)S(20) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

void set_borderless(App &a, bool on)
{
    a.borderless = on;
    SDL_SetWindowBordered(a.win, !on);
    a.ini.seti("Window", "Borderless", on ? 1 : 0);
    a.ini.save();
}

void set_fullscreen(App &a, bool on)
{
    a.fullscreen = on;
    SDL_SetWindowFullscreen(a.win, on);   // SDL3: borderless, the size of the desktop
    a.ini.seti("Window", "Fullscreen", on ? 1 : 0);
    a.ini.save();
}

// Minimise / maximise / close in the top-right corner when there is no title bar; the strip left of them moves the window.
static void draw_window_buttons(App &a, ImVec2 size)
{
    if (!a.borderless || a.fullscreen) { g_dragTop = false; return; }
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const float bw = S(34), bh = S(18), y = S(1);
    float x = size.x - S(4) - 3 * bw - 2 * S(3);
    const bool maxed = (SDL_GetWindowFlags(a.win) & SDL_WINDOW_MAXIMIZED) != 0;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (th_button("_##wmin", ImVec2(bw, bh), true, F_SMALL)) SDL_MinimizeWindow(a.win);
    ImGui::SetCursorScreenPos(ImVec2(x + bw + S(3), y));
    if (th_button(maxed ? "[-]##wmax" : "[ ]##wmax", ImVec2(bw, bh), true, F_SMALL)) { if (maxed) SDL_RestoreWindow(a.win); else SDL_MaximizeWindow(a.win); }
    ImGui::SetCursorScreenPos(ImVec2(x + 2 * (bw + S(3)), y));
    if (th_button_red("X##wclose", ImVec2(bw, bh), true, F_SMALL)) a.quit = true;
    ImGui::PopItemFlag();
    const ImVec2 m = ImGui::GetIO().MousePos;
    g_dragTop = m.y >= 0 && m.y < S(20) && m.x < x - S(4) && !ImGui::IsAnyItemHovered() && !a.modal.open;
}

static void draw_ui(App &a, ImVec2 size)
{
    double now = ImGui::GetTime();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    static const ImVec4 tints[G_COUNT] = { ImVec4(0.30f, 0.45f, 0.70f, 1), ImVec4(0.75f, 0.50f, 0.22f, 1), ImVec4(0.70f, 0.22f, 0.16f, 1) };
    // the tint follows the box in front (blended while turning)
    int g0 = (int)floorf(a.carPos) % G_COUNT, g1 = (g0 + 1) % G_COUNT;
    float f = a.carPos - floorf(a.carPos);
    int tg = a.screen == SCR_PAGE ? a.page : -1;
    ImVec4 tint = tg >= 0 ? tints[tg] : ImVec4(tints[g0].x * (1 - f) + tints[g1].x * f, tints[g0].y * (1 - f) + tints[g1].y * f, tints[g0].z * (1 - f) + tints[g1].z * f, 1);
    th_background(bg, size, now, tint);
    // the game's own background art (when downloaded), crossfading with the box in front, darker on a page
    if (tg >= 0) backdrop_draw(a, bg, size, tg, 0.85f);   // the settings page too shows the game's art (Shagg 18:41)
    else { backdrop_draw(a, bg, size, g0, 0.85f * (1 - f)); backdrop_draw(a, bg, size, g1, 0.85f * f); }
    if (tg < 0) bg->AddRectFilledMultiColor(ImVec2(0, size.y * 0.45f), size, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200));

    // the zoom between the carousel and a page: darkens, switches half way, lightens (0.5 s)
    double zt = now - a.zoomStart;
    const double zoomLen = getenv("COGBOX_SLOW") ? 4.0 : 0.5;
    if (zt >= 0 && zt < zoomLen && a.zoomTo != a.screen && zt >= zoomLen * 0.5) a.screen = a.zoomTo;
    if (a.screen == SCR_FRONT) front_draw(a, size);
    else page_draw(a, size);
    if (zt >= 0 && zt < zoomLen) {
        float k = (float)(1.0 - fabs(zt / zoomLen * 2 - 1));
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, (int)(255 * k)));
    } else if (a.zoomTo != a.screen) a.screen = a.zoomTo;

    // the message line
    if (!a.message.empty() && now - a.messageAt < 12) {
        float al = (float)std::min(1.0, (12 - (now - a.messageAt)) / 1.5);
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        ImVec2 ts = F_BODY->CalcTextSizeA(FS(F_BODY), size.x - S(120), size.x - S(120), a.message.c_str());
        float top = a.screen == SCR_FRONT ? S(84) : S(4);   // above the boxes / over the page's frame, never over buttons
        ImVec2 p0((size.x - ts.x) * 0.5f - S(18), top), p1(p0.x + ts.x + S(36), p0.y + ts.y + S(18));
        th_plate_fill(fg, p0, p1, S(8), U(ImVec4(0.05f, 0.05f, 0.06f, 0.94f), al));
        fg->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + S(4), p1.y), U(a.messageBad ? col::bad : col::good, al));
        fg->AddText(F_BODY, FS(F_BODY), ImVec2(p0.x + S(18), p0.y + S(9)), U(col::text, al), a.message.c_str(), nullptr, size.x - S(120));
    }
    // a newer COGBox: one line in the corner (installed from the About tab)
    std::string newer = update_newer();
    if (!newer.empty() && a.screen == SCR_FRONT) {
        std::string t = "COGBox " + newer + " is out - see any game's About tab";
        ImVec2 ts = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, t.c_str());
        ImGui::GetWindowDrawList()->AddText(F_SMALL, FS(F_SMALL), ImVec2(size.x - ts.x - S(24), S(24)), U(col::amber), t.c_str());
    }
    install_draw(a, size);
    draw_modal(a, size);
    draw_window_buttons(a, size);   // job 523
    ImGui::End();
}

const char *pad_name(const App &a, const char *x)
{
    if (!a.padPs) return x;
    static const char *const map[][2] = { { "A", "Cross" }, { "B", "Circle" }, { "X", "Square" }, { "Y", "Triangle" },
                                          { "LB", "L1" }, { "RB", "R1" }, { "LT", "L2" }, { "RT", "R2" }, { "Back", "Share" }, { "Start", "Options" } };
    for (auto &m : map) if (strcmp(m[0], x) == 0) return m[1];
    return x;
}

static void refresh_pads(App &a)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    a.padConnected = n > 0;
    if (getenv("COGBOX_PADPS")) { a.padConnected = true; }   // test switch: shows the PlayStation names
    a.padPs = false;   // job 523: the first PlayStation pad switches the button names
    for (int i = 0; i < n; i++) {
        SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[i]);
        if (t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5) { a.padPs = true; break; }
    }
    if (getenv("COGBOX_PADPS")) a.padPs = true;
    if (ids) SDL_free(ids);
}

// Test switches (all off for players): COGBOX_SHOT=<file.bmp> saves the window after COGBOX_SHOT_FRAMES frames (90)
// and exits; COGBOX_PAGE=<game>[:<tab>] opens on a page; COGBOX_FRONT=<game> turns the carousel there.
static void test_shot(App &a)
{
    const char *shot = getenv("COGBOX_SHOT");
    if (!shot) return;
    const char *fr = getenv("COGBOX_SHOT_FRAMES");
    if (ImGui::GetFrameCount() != (fr ? atoi(fr) : 90)) return;
    SDL_Surface *s = SDL_RenderReadPixels(a.ren, nullptr);
    if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
    a.quit = true;
}

// Test switch: CogBox.exe --x360test <source> <out folder or -> <report.txt> - what x360fs makes of a copy: its kind, header,
// listing, then (with an out folder) the extraction and the SHA-1 of every file written. For checking the reader on real
// discs and packages; players never see it.
#include "x360fs.h"
static int x360test(int argc, wchar_t **argv)
{
    if (argc < 5) return 2;
    std::wstring src = argv[2], out = argv[3], rep = argv[4];
    std::string r;
    auto line = [&](const std::string &s) { r += s + "\r\n"; };
    std::filesystem::path res;
    x360fs::SourceKind k = x360fs::detect(src, &res);
    line(std::string("kind: ") + x360fs::source_kind_name(k));
    line("resolved: " + narrow(res.wstring()));
    std::string err;
    std::vector<x360fs::Entry> list;
    bool listed = false;
    char b[256];
    if (k == x360fs::SourceKind::DiscImage) {
        snprintf(b, sizeof b, "partition offset: 0x%llX", (unsigned long long)x360fs::disc_partition_offset(res)); line(b);
        x360fs::XexInfo xi;
        if (x360fs::disc_xex_info(res, xi)) { snprintf(b, sizeof b, "xex title id: %08X media %08X version %s disc %u/%u", xi.title_id, xi.media_id, x360fs::xbox_version_string(xi.version).c_str(), xi.disc_number, xi.disc_count); line(b); }
        listed = x360fs::list_disc(res, list, err);
    } else if (k == x360fs::SourceKind::StfsPackage || k == x360fs::SourceKind::GodContainer) {
        x360fs::PackageInfo pi;
        if (x360fs::read_package_info(res, pi, err)) {
            snprintf(b, sizeof b, "package: %s type %08X title %08X version %s base %s", pi.magic.c_str(), pi.content_type, pi.title_id,
                     x360fs::xbox_version_string(pi.version).c_str(), x360fs::xbox_version_string(pi.base_version).c_str());
            line(b);
            line("name: " + pi.display_name + " | " + pi.title_name);
        } else line("header error: " + err);
        listed = k == x360fs::SourceKind::StfsPackage ? x360fs::list_stfs(res, list, err) : x360fs::list_god(res, list, err);
    }
    uint64_t total = 0;
    size_t files = 0;
    for (auto &e : list) if (!e.is_dir) { total += e.size; files++; }
    snprintf(b, sizeof b, "listing: %s, %zu files, %llu bytes", listed ? "ok" : ("FAILED " + err).c_str(), files, (unsigned long long)total);
    line(b);
    bool ok = listed;
    if (out != L"-" && listed) {
        DWORD t0 = GetTickCount();
        err.clear();
        if (k == x360fs::SourceKind::DiscImage) ok = x360fs::extract_disc(res, out, nullptr, nullptr, err);
        else if (k == x360fs::SourceKind::StfsPackage) ok = x360fs::extract_stfs(res, out, nullptr, nullptr, err, true);
        else ok = x360fs::extract_god(res, out, nullptr, nullptr, err);
        snprintf(b, sizeof b, "extract: %s in %lu s", ok ? "ok" : ("FAILED " + err).c_str(), (unsigned long)((GetTickCount() - t0) / 1000)); line(b);
        if (ok)
            for (auto &e : list) {
                if (e.is_dir) continue;
                std::filesystem::path p = std::filesystem::path(out) / std::filesystem::u8path(e.path);
                line(x360fs::sha1_file(p) + "  " + std::to_string(e.size) + "  " + e.path);
            }
    }
    line(ok ? "RESULT: ok" : "RESULT: FAILED");
    write_file(narrow(rep), r);
    return ok ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2 && wcscmp(argv[1], L"--x360test") == 0) return x360test(argc, argv);
        if (argv) LocalFree(argv);
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        MessageBoxA(nullptr, SDL_GetError(), COGBOX_NAME, MB_ICONERROR);
        return 1;
    }
    g_ui = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_ui <= 0.0f) g_ui = 1.0f;
    g_ui *= 0.78f;   // Shagg 18:31: about 25% smaller than the first build
    int ww = (int)(1280 * g_ui), wh = (int)(800 * g_ui);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm && (ww > dm->w * 0.92f || wh > dm->h * 0.88f)) {
        float k = std::min(dm->w * 0.92f / ww, dm->h * 0.88f / wh);
        g_ui *= k; ww = (int)(ww * k); wh = (int)(wh * k);
    }
    SDL_Window *win = SDL_CreateWindow(COGBOX_NAME, ww, wh, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) { MessageBoxA(nullptr, SDL_GetError(), COGBOX_NAME, MB_ICONERROR); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_SetWindowMinimumSize(win, (int)(1100 * g_ui), (int)(700 * g_ui));
    g_ui0 = g_ui; g_w0 = std::max(1, ww); g_h0 = std::max(1, wh);   // job 523: the zoom's starting point

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    theme_fonts(io);
    theme_style();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    App a;
    a.win = win;
    a.ren = ren;
    a.ini.load(join(exe_dir(), "CogBox.ini"));
    a.ini.path = join(exe_dir(), "CogBox.ini");
    // job 523: Wine / Proton (Linux, Steam Deck) - ntdll has wine_get_version there
    a.wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    {
        // a small screen (Steam Deck 1280x800) starts borderless and maximised the first time; then the player's choice
        const bool small = dm && dm->w <= 1280 && dm->h <= 800;
        g_app = &a;
        SDL_SetWindowHitTest(win, window_hit, nullptr);
        if (a.ini.geti("Window", "Borderless", small ? 1 : 0)) { a.borderless = true; SDL_SetWindowBordered(win, false); }
        if (small) SDL_MaximizeWindow(win);
        if (a.ini.geti("Window", "Fullscreen", 0)) { a.fullscreen = true; SDL_SetWindowFullscreen(win, true); }
    }
    if (const char *t = getenv("COGBOX_WINDOW")) { int w = 0, h = 0; if (sscanf(t, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) SDL_SetWindowSize(win, w, h); }   // test switch
    if (getenv("COGBOX_BORDERLESS")) { a.borderless = true; SDL_SetWindowBordered(win, false); }   // test switch (not saved)
    a.gpus = report_gpu_names();
    a.hasAvx2 = cpu_has_avx2();
    refresh_games(a, true);
    {
        std::string text;
        bool had = read_file(join(exe_dir(), "settings.ini"), text, 4 << 20) && a.store.load_ini(text);
        for (int g = 0; g < G_COUNT; g++) { a.cur[g] = a.store.view((gs::Game)g, a.env[g]); env_refresh(a, g); }
        bool anyReady = false;
        for (int g = 0; g < G_COUNT; g++) anyReady |= a.st[g].ready == R_READY;
        if (!had && !anyReady) set_message(a, "Welcome! Add your copy of a game to start - a disc image, a Games on Demand copy or an extracted folder.", false);
    }
    a.front = std::max(0, std::min(G_COUNT - 1, a.ini.geti("Front", "Box", 0)));
    // the first game the player has, if the remembered one isn't ready
    if (a.st[a.front].ready != R_READY)
        for (int g = 0; g < G_COUNT; g++) if (a.st[g].ready == R_READY) { a.front = g; break; }
    if (const char *t = getenv("COGBOX_FRONT")) a.front = std::max(0, std::min(G_COUNT - 1, atoi(t)));
    a.carPos = (float)a.front;
    if (const char *t = getenv("COGBOX_PAGE")) {
        a.page = std::max(0, std::min(G_COUNT - 1, atoi(t)));
        const char *c = strchr(t, ':');
        a.tab = c ? std::max(0, std::min(5, atoi(c + 1))) : 0;
        a.screen = a.zoomTo = SCR_PAGE;
    }
    covers_load(a);
    if (getenv("COGBOX_GETART")) art_download_start();   // test switch
    refresh_pads(a);
    update_tidy();
    if (a.ini.geti("Updates", "Check", 1) && !getenv("COGBOX_SHOT")) update_check_start(COGBOX_VERSION);

    while (!a.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) a.quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)) a.quit = true;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) refresh_pads(a);
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) a.dropped.push_back(e.drop.data);
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED && !a.run.proc) { refresh_games(a, false); covers_load(a); }
        }
        play_poll(a);
        switch (art_download_poll()) {
        case 2: covers_load(a); set_message(a, "Box art and backgrounds are in the covers folder.", false); break;
        case 3: set_message(a, "The box art couldn't be downloaded - check the internet connection.", true); break;
        }
        install_poll(a);
        if (!a.dropped.empty() && (a.modal.open || a.job.state.load() == 1)) { a.dropped.clear(); set_message(a, "Finish the question or the unpacking first, then drop it again.", true); }
        if (!a.dropped.empty()) {
            std::string p = a.dropped.front();
            a.dropped.clear();
            install_start(a, p, -1);
        }
        if (a.run.proc && (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) { SDL_Delay(100); continue; }   // a game is playing
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
        // the controller works the launcher only while it is the window in front
        // On the front screen the D-pad / arrows turn the carousel and LB / RB choose Play or Settings (front.cpp keeps
        // that choice itself), so Dear ImGui's own navigation is off there unless a question box is open.
        bool frontOwn = (a.screen == SCR_FRONT && a.zoomTo == SCR_FRONT && !a.modal.open) ||
                        (a.screen == SCR_PAGE && a.barSel >= 0 && !a.modal.open);   // also a page's bottom bar (page.cpp)
        if (!SDL_GetKeyboardFocus() || a.run.proc || frontOwn) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        if (frontOwn) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        {   // job 523: everything grows with the window (1x at the starting size, at most 2x)
            int W = 0, H = 0;
            SDL_GetWindowSize(win, &W, &H);
            float z = std::max(1.0f, std::min(2.0f, std::min((float)W / g_w0, (float)H / g_h0)));
            if (fabsf(z - g_zoom) > 0.005f) {
                g_zoom = z;
                g_ui = g_ui0 * z;
                theme_style();
                ImGui::GetStyle().FontSizeBase = F_BODY->LegacySize * z;
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        {   // job 523 (Shagg 20:29): F11, or both triggers together, switch fullscreen from any screen
            static bool triggersWere = false;
            const bool triggers = ImGui::IsKeyDown(ImGuiKey_GamepadL2) && ImGui::IsKeyDown(ImGuiKey_GamepadR2);
            if (!a.run.proc && SDL_GetKeyboardFocus() &&
                (ImGui::IsKeyPressed(ImGuiKey_F11, false) || (triggers && !triggersWere))) set_fullscreen(a, !a.fullscreen);
            triggersWere = triggers;
        }
        draw_ui(a, io.DisplaySize);
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 8, 8, 10, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        test_shot(a);
        SDL_RenderPresent(ren);
    }
    if (std::any_of(a.dirty, a.dirty + G_COUNT, [](bool x) { return x; })) settings_save(a);
    a.ini.save();
    if (a.job.state.load() == 1) { a.job.cancel = true; }
    if (a.job.th.joinable()) a.job.th.join();
    for (auto &c : a.cover) if (c.tex) SDL_DestroyTexture(c.tex);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
