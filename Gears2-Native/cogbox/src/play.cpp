// COGBox - starting a game, waiting for it, and saying in plain words what happened when it ends badly; the problem
// report. The exit-code meanings and the report's contents follow Gears2-Native's launcher (tools\launcher.ps1,
// patch 0131).
#include "app.h"
#include "report.h"
#include "theme.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>

static std::string hex32(DWORD c) { char b[16]; snprintf(b, sizeof b, "0x%08lX", (unsigned long)c); return b; }

static const char *exit_meaning(DWORD code)
{
    switch (code) {
    case 0xC0000005: return "it crashed (a memory access error, 0xC0000005)";
    case 0xC0000409: return "it stopped itself after a fatal error (0xC0000409)";
    case 0xC0000374: return "its memory got corrupted (0xC0000374)";
    case 0xC000001D: return "this CPU lacks an instruction the build needs (0xC000001D) - the ports are built for CPUs with AVX2 (2013 and newer)";
    case 0xC0000135: return "a file it needs next to it is missing (0xC0000135) - unzip the COGBox release again, keeping every file";
    case 0xC0000142: return "a DLL it needs failed to start (0xC0000142)";
    case 0x887A0005: return "the graphics card stopped responding or its driver was reset (0x887A0005)";
    case 0x887A0006: return "the graphics card stopped responding (0x887A0006)";
    case 0x887A0007: return "the graphics driver was reset (0x887A0007)";
    case 1: return "it ended with an error, or was ended from outside (Task Manager)";
    }
    return nullptr;
}

static std::vector<std::string> crash_files(const std::string &dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(dir, "crash-*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(join(dir, narrow(fd.cFileName))); } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

static void append_line(const std::string &file, const std::string &line)
{
    std::string have;
    read_file(file, have, 4 << 20);
    write_file(file, have + line + "\r\n");
}

// ---- the in-game menu (patch 0163 in the ports): Back + Start or F1 in the game shows these settings ----------------
// The menu file lists every setting of this game as the settings page shows it now; the game writes the ones the player
// changed to menu-changes.txt and, for "Restart to apply", menu-restart.flag, all in this play's log folder. When the
// game has closed, play_poll applies the changes to the settings and starts the game again if the flag is there.
static std::string one_line(std::string s)
{
    for (char &c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

static void write_menu_file(App &a, int g, gs::Version ver, const std::string &logDir)
{
    gs::Game G = (gs::Game)g;
    std::string t = "; COGBox's in-game menu for this play (written by COGBox " COGBOX_VERSION ")\r\n[menu]\r\n";
    t += std::string("game=") + game_def(g).name + "\r\n";
    t += "changes=" + join(logDir, "menu-changes.txt") + "\r\nrestart=" + join(logDir, "menu-restart.flag") + "\r\n";
    for (const gs::Option &o : gs::options(G)) {
        gs::Availability av = gs::available(o, G, ver, a.cur[g], a.env[g]);
        double v = a.cur[g].has(o.id) ? a.cur[g].f(o.id) : o.def;
        std::vector<std::string> choices = o.id == "Monitor" ? monitor_choices() : o.choices;
        const char *kind = o.kind == gs::Kind::Bool ? "bool" : o.kind == gs::Kind::Choice ? "choice" : o.kind == gs::Kind::Int ? "int" : "float";
        char nums[160];
        snprintf(nums, sizeof nums, "value=%.6g\r\nmin=%.6g\r\nmax=%.6g\r\nstep=%.6g\r\ndecimals=%d\r\n", v, o.min, o.max, o.step, o.decimals);
        t += "[option]\r\nid=" + o.id + "\r\ntab=" + gs::tab_name(o.tab) + "\r\nlabel=" + one_line(o.label) + "\r\nkind=" + kind + "\r\n" + nums;
        if (!choices.empty()) {
            t += "choices=";
            for (size_t i = 0; i < choices.size(); i++) t += (i ? "|" : "") + one_line(choices[i]);
            t += "\r\n";
        }
        t += std::string("enabled=") + (av.ok ? "1" : "0") + "\r\nwhy=" + one_line(av.why) + "\r\ntip=" + one_line(gs::tooltip_for(o, G, av, a.env[g])) + "\r\n";
    }
    write_file(join(logDir, "menu.txt"), t);
}

// After a play: the in-game menu's changes into the settings; true when it asked for a restart.
static bool apply_menu_changes(App &a, int g, const std::string &logDir)
{
    std::string text;
    if (read_file(join(logDir, "menu-changes.txt"), text, 1 << 20)) {
        int n = 0;
        size_t at = 0;
        while (at < text.size()) {
            size_t nl = text.find('\n', at);
            std::string l = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t eq = l.find('=');
            if (eq == std::string::npos || !gs::find_option((gs::Game)g, l.substr(0, eq))) continue;
            gs::set_value((gs::Game)g, a.cur[g], l.substr(0, eq), atof(l.c_str() + eq + 1));
            n++;
        }
        if (n) {
            a.dirty[g] = true;
            settings_save(a);
            append_line(join(logDir, "args.txt"), "launcher: the in-game menu changed " + std::to_string(n) + " setting(s)");
        }
    }
    return file_exists(join(logDir, "menu-restart.flag"));
}

void play_start(App &a, int g)
{
    if (a.run.proc) return;
    const GameDef &d = game_def(g);
    refresh_games(a, false);
    GameState &st = a.st[g];
    if (st.ready != R_READY) { set_message(a, std::string(d.name) + " isn't ready to play yet - add your copy first.", true); return; }
    env_refresh(a, g);
    gs::Game G = (gs::Game)g;
    gs::Version ver = gs::effective_version(G, a.cur[g], a.env[g]);
    std::string exe = gs::exe_for_update(G, ver, a.env[g]) ? st.portExeTu : st.portExeDisc;
    if (exe.empty()) exe = st.portExeDisc.empty() ? st.portExeTu : st.portExeDisc;
    if (exe.empty() || !file_exists(exe)) { set_message(a, "The port program is missing - unzip the COGBox release again.", true); return; }
    std::string why;
    if (gs::blocked_by_cpu(G, a.env[g], &why)) {
        ask(a, "info", g, "CPU too old for this build", why, { "OK" });
        return;
    }
    // this play's own folder: the log, the arguments, any crash report
    std::string logDir = join(game_logs_dir(g), now_stamp());
    make_dirs(logDir);
    make_dirs(game_user_dir(g));
    make_dirs(game_shader_dir(g));
    a.env[g].log_dir = logDir;
    std::vector<std::string> notes;
    std::vector<std::string> args = gs::build_args(G, ver, a.cur[g], a.env[g], &notes);
    std::string shared = join(exe_dir(), "shared");
    bool useShared = dir_exists(shared);
    if (useShared) args.push_back("--gow2_dxc_dir=\"" + shared + "\"");
    if (a.env[g].cap.exe_has && a.env[g].cap.exe_has("cogbox_menu")) {   // ports with the in-game menu (patch 0163)
        write_menu_file(a, g, ver, logDir);
        args.push_back("--cogbox_menu=\"" + join(logDir, "menu.txt") + "\"");
    }
    std::string argText;
    for (auto &s : args) argText += s + "\r\n";
    argText += "launcher: COGBox " COGBOX_VERSION ", game " + std::string(d.name) + ", exe " + exe + "\r\n";
    argText += "launcher: graphics card(s): ";
    for (size_t i = 0; i < a.gpus.size(); i++) argText += (i ? " / " : "") + a.gpus[i];
    argText += "\r\n";
    for (auto &n : notes) argText += n + "\r\n";
    write_file(join(logDir, "args.txt"), argText);

    // The upscaler and shader-compiler DLLs every game uses are kept once, in <COGBox>\shared (Shagg 20:27, option 1):
    // the game gets that folder at the front of its PATH (FSR's loader and XeSS are loaded by name) and --gow2_dxc_dir
    // (the renderer loads dxcompiler.dll from there). DLSS (NGX looks only beside the exe), dxil.dll and the runtime
    // stay in each game folder. Checked by starting every game with FSR and XeSS from that layout (job 487).
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    for (auto &s : args) cmd += L" " + widen(s);
    std::wstring envBlock;   // this process's environment with PATH = shared;PATH (empty = inherit as is)
    if (useShared) {
        wchar_t *e = GetEnvironmentStringsW();
        bool hadPath = false;
        for (wchar_t *v = e; v && *v; v += wcslen(v) + 1) {
            std::wstring kv = v;
            if (_wcsnicmp(kv.c_str(), L"PATH=", 5) == 0) { kv = L"Path=" + widen(shared) + L";" + kv.substr(5); hadPath = true; }
            envBlock += kv;
            envBlock.push_back(L'\0');
        }
        if (e) FreeEnvironmentStringsW(e);
        if (!hadPath) { envBlock += L"Path=" + widen(shared); envBlock.push_back(L'\0'); }
        envBlock.push_back(L'\0');
    }
    std::wstring dir = widen(exe.substr(0, exe.find_last_of('\\')));
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(widen(exe).c_str(), &cmd[0], nullptr, nullptr, FALSE, useShared ? CREATE_UNICODE_ENVIRONMENT : 0,
                        useShared ? (LPVOID)envBlock.data() : nullptr, dir.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        append_line(join(logDir, "args.txt"), "launcher: not started, Windows error " + std::to_string(e));
        set_message(a, std::string(d.name) + " couldn't be started (Windows error " + std::to_string(e) + ").", true);
        return;
    }
    CloseHandle(pi.hThread);
    a.run.proc = pi.hProcess;
    a.run.game = g;
    a.run.started = unix_now();
    a.run.logDir = logDir;
    a.ini.seti("LastPlayed", d.key, a.run.started);
    a.ini.save();
    SDL_MinimizeWindow(a.win);
}

void play_poll(App &a)
{
    if (!a.run.proc || WaitForSingleObject(a.run.proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    bool known = GetExitCodeProcess(a.run.proc, &code) != 0;
    CloseHandle(a.run.proc);
    a.run.proc = nullptr;
    int g = a.run.game;
    const GameDef &d = game_def(g);
    int64_t secs = std::max<int64_t>(0, unix_now() - a.run.started);
    // play time: only plays that got going (a start that fails at once isn't play)
    if (secs >= 20) a.ini.seti("PlayTime", d.key, a.ini.geti64("PlayTime", d.key, 0) + secs);
    a.ini.save();
    refresh_games(a, false);
    SDL_RestoreWindow(a.win);
    SDL_RaiseWindow(a.win);

    std::vector<std::string> crash = crash_files(a.run.logDir);
    append_line(join(a.run.logDir, "args.txt"), "launcher: the game ended after " + std::to_string(secs) + " s, exit code " +
                                                    (known ? hex32(code) : std::string("unknown")) + ", crash reports " + std::to_string(crash.size()));
    bool restart = apply_menu_changes(a, g, a.run.logDir);
    if (known && code == 0 && crash.empty()) {
        if (restart) { append_line(join(a.run.logDir, "args.txt"), "launcher: restarting the game (in-game menu)"); play_start(a, g); }
        return;
    }
    std::string what;
    if (const char *m = known ? exit_meaning(code) : nullptr) what = m;
    else if (known && code != 0) what = "it ended with exit code " + hex32(code);
    else what = "it crashed";
    std::string log;
    if (read_file(join(a.run.logDir, d.logName), log, 64u << 20) &&
        (log.find("DEVICE_REMOVED") != std::string::npos || log.find("DEVICE_HUNG") != std::string::npos || log.find("DXGI_ERROR_DEVICE") != std::string::npos))
        what += "; its log mentions the graphics card being lost (a driver reset, or an overheating / overclocked card)";
    if (!crash.empty()) what += "; it wrote a crash report";
    ask(a, "crash", g, std::string(d.name) + " closed",
        std::string(d.name) + " closed unexpectedly: " + what + ".\n\nMake a problem report (a zip on your desktop) for it? "
        "It holds the log, the settings, a description of this PC and any crash report - no saves, no game files, and your "
        "Windows user name is taken out. Nothing is sent by itself.",
        { "Make a report", "Open the log folder", "Close" }, a.run.logDir + "\n" + what);
}

void play_answer(App &a, const Modal &m)
{
    std::string dir = m.data.substr(0, m.data.find('\n'));
    std::string what = m.data.find('\n') == std::string::npos ? "" : m.data.substr(m.data.find('\n') + 1);
    if (m.answer == 0) {
        std::string zip, err;
        if (make_report(a, m.game, dir, what, zip, err)) {
            set_message(a, "Saved on your desktop: " + zip.substr(zip.find_last_of('\\') + 1) + " - attach it to a GitHub issue.", false);
            shell_open(desktop_dir());
        } else set_message(a, "Couldn't make the report: " + err, true);
    } else if (m.answer == 1) shell_open(dir);
}

std::string newest_session(int g)
{
    std::string best;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(game_logs_dir(g), "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return "";
    do {
        std::string n = narrow(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n.size() == 15 && n[8] == '-' && n > best) best = n;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return best.empty() ? "" : join(game_logs_dir(g), best);
}

static std::string displays_text()
{
    std::string s;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    for (int i = 0; ids && i < n; i++) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(ids[i]);
        char b[160];
        snprintf(b, sizeof b, "Display %d: %dx%d at %.0f Hz%s\r\n", i + 1, m ? m->w : 0, m ? m->h : 0, m ? m->refresh_rate : 0.0f,
                 ids[i] == SDL_GetPrimaryDisplay() ? " (main)" : "");
        s += b;
    }
    if (ids) SDL_free(ids);
    return s;
}

bool make_report(App &a, int g, const std::string &session, const std::string &what, std::string &zip, std::string &err)
{
    const GameDef &d = game_def(g);
    GameState &st = a.st[g];
    std::vector<ReportFile> files;
    // the session's own files (log, args.txt, crash reports and dumps)
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(session, "*")).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;   // no links (review 15)
            std::string n = narrow(fd.cFileName), lo = n;
            for (char &c : lo) c = (char)tolower((unsigned char)c);
            bool text = lo.size() > 4 && (lo.rfind(".txt") == lo.size() - 4 || lo.rfind(".log") == lo.size() - 4 || lo.rfind(".toml") == lo.size() - 5);
            files.push_back({ "session/" + n, join(session, n), text });
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    files.push_back({ "settings.ini", join(exe_dir(), "settings.ini"), true });
    std::string sys = "Game: " + std::string(d.name) + "\r\nSession: " + session.substr(session.find_last_of('\\') + 1) + "\r\n";
    if (!what.empty()) sys += "What happened (the launcher's reading): " + what + "\r\n";
    sys += "COGBox: " COGBOX_VERSION "\r\nPort: " + st.exe() + "\r\n\r\n" + report_system_text(displays_text()) + "\r\n";
    // the game files' fingerprints (never the files)
    sys += std::string("Game files: ") + (st.dataDir.empty() ? "none" : st.xexMatch ? "the expected release" : "a different release (default.xex " +
           cached_sha1(a.ini, join(st.dataDir, "default.xex")) + ", " + std::to_string(file_size(join(st.dataDir, "default.xex"))) + " bytes)") + "\r\n";
    sys += std::string(d.updateName) + ": " + (st.updateDir.empty() ? "none" : st.xexpMatch ? "the expected update" : "a different update (default.xexp " +
           cached_sha1(a.ini, join(st.updateDir, "default.xexp")) + ")") + "\r\n";
    sys += "\r\nMade by COGBox at " + now_stamp() + ". Your Windows user name and PC name are replaced (x's in crash dumps).\r\n";
    std::string name = std::string("CogBox-report-") + d.dir + "-" + now_stamp() + ".zip";
    zip = join(desktop_dir(), name);
    return report_write(files, sys, zip, err);
}
