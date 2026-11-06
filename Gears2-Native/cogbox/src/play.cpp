// COGBox - starting a game, waiting for it, and saying in plain words what happened when it ends badly; the problem
// report. The exit-code meanings and the report's contents follow Gears2-Native's launcher (tools\launcher.ps1,
// patch 0131).
#include "app.h"
#include "report.h"
#include "theme.h"
#include "i18n.h"
#include "x360fs.h"

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
    case 0xE0C0B009: return "its renderer couldn't start (the game said why) - try the other Graphics API (Vulkan / DirectX 12) on the Graphics tab, and update Windows and the graphics driver";   // 0.2.0 (#9)
    }
    return nullptr;
}

// GitHub #194: 0xC0000135 (STATUS_DLL_NOT_FOUND) doesn't say which DLL. The exe's import table does: each DLL it
// names is looked for where Windows looks (the exe's folder, COGBox's shared folder that goes first on the game's PATH,
// System32, Windows, PATH), and the imports of the DLLs found outside Windows (ours, the runtime's) are checked too.
// The file is mapped, not read, so only the header and import pages are touched. Delay-loaded DLLs don't stop a start
// and are left out; api-ms-win-* / ext-ms-* names are API sets Windows resolves itself.
static std::vector<std::string> pe_imports(const std::string &path)
{
    std::vector<std::string> out;
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz = {};
    HANDLE m = GetFileSizeEx(f, &sz) && sz.QuadPart >= 4096 ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    const uint8_t *b = m ? (const uint8_t *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
    const uint64_t n = (uint64_t)sz.QuadPart;
    if (b) {
        {   // every read below is checked against the file's size: a damaged file gives no names, not a crash
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)b;
            const uint64_t pe = dos->e_magic == IMAGE_DOS_SIGNATURE ? (uint32_t)dos->e_lfanew : n;
            if (pe + sizeof(IMAGE_NT_HEADERS64) <= n && *(const DWORD *)(b + pe) == IMAGE_NT_SIGNATURE) {
                const IMAGE_FILE_HEADER *fh = (const IMAGE_FILE_HEADER *)(b + pe + 4);
                const uint8_t *opt = b + pe + 4 + sizeof(IMAGE_FILE_HEADER);
                const WORD magic = *(const WORD *)opt;
                const IMAGE_DATA_DIRECTORY *dd = nullptr;
                DWORD ndd = 0;
                if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                    const IMAGE_OPTIONAL_HEADER64 *o = (const IMAGE_OPTIONAL_HEADER64 *)opt; dd = o->DataDirectory; ndd = o->NumberOfRvaAndSizes;
                } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
                    const IMAGE_OPTIONAL_HEADER32 *o = (const IMAGE_OPTIONAL_HEADER32 *)opt; dd = o->DataDirectory; ndd = o->NumberOfRvaAndSizes;
                }
                const IMAGE_SECTION_HEADER *sec = (const IMAGE_SECTION_HEADER *)(opt + fh->SizeOfOptionalHeader);
                const uint64_t secEnd = (uint64_t)((const uint8_t *)(sec + fh->NumberOfSections) - b);
                auto off = [&](DWORD rva) -> uint64_t {   // file offset of an RVA, n when it's in no section
                    for (WORD i = 0; i < fh->NumberOfSections; i++) {
                        const DWORD va = sec[i].VirtualAddress, len = std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
                        if (rva >= va && rva - va < len) {
                            const uint64_t o = (uint64_t)sec[i].PointerToRawData + (rva - va);
                            return o < n ? o : n;
                        }
                    }
                    return n;
                };
                if (dd && ndd > IMAGE_DIRECTORY_ENTRY_IMPORT && secEnd <= n && dd[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
                    uint64_t at = off(dd[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
                    for (int k = 0; k < 512 && at + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= n; k++, at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                        const IMAGE_IMPORT_DESCRIPTOR *d = (const IMAGE_IMPORT_DESCRIPTOR *)(b + at);
                        if (!d->Name) break;
                        const uint64_t s = off(d->Name);
                        std::string name;
                        for (uint64_t i = s; i < n && i < s + 260 && b[i]; i++) name += (char)b[i];
                        if (!name.empty()) out.push_back(name);
                    }
                }
            }
        }
        UnmapViewOfFile(b);
    }
    if (m) CloseHandle(m);
    CloseHandle(f);
    return out;
}

static std::string lower_ascii(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }

// The folders Windows looks in for the game's DLLs, in its order; [0] is the exe's own folder.
static std::vector<std::string> dll_dirs(const std::string &exe)
{
    std::vector<std::string> v = { exe.substr(0, exe.find_last_of("\\/")) };
    const std::string shared = join(exe_dir(), "shared");
    if (dir_exists(shared)) v.push_back(shared);
    wchar_t w[MAX_PATH];
    UINT k = GetSystemDirectoryW(w, MAX_PATH);
    if (k && k < MAX_PATH) v.push_back(narrow(w));
    k = GetWindowsDirectoryW(w, MAX_PATH);
    if (k && k < MAX_PATH) v.push_back(narrow(w));
    DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (len > 1 && len < (1u << 16)) {
        std::wstring p(len, L'\0');
        p.resize(GetEnvironmentVariableW(L"PATH", &p[0], len));
        size_t at = 0;
        while (at <= p.size()) {
            size_t e = p.find(L';', at);
            if (e == std::wstring::npos) e = p.size();
            std::wstring d = p.substr(at, e - at);
            d.erase(std::remove(d.begin(), d.end(), L'"'), d.end());
            if (!d.empty()) v.push_back(narrow(d));
            at = e + 1;
        }
    }
    return v;
}

// The DLLs the game needs at start that are in none of those folders (lower case, each once). Empty when all are there
// or the exe couldn't be read.
static std::vector<std::string> missing_dlls(const std::string &exe)
{
    const std::vector<std::string> dirs = dll_dirs(exe);
    const size_t own = dir_exists(join(exe_dir(), "shared")) ? 2 : 1;   // the exe's folder and shared: our files
    std::vector<std::string> missing, seen, todo = { exe };
    for (size_t t = 0; t < todo.size() && t < 24; t++) {   // the exe, then the DLLs of ours it pulls in (a few)
        for (const std::string &raw : pe_imports(todo[t])) {
            const std::string name = lower_ascii(raw);
            if (name.rfind("api-ms-", 0) == 0 || name.rfind("ext-ms-", 0) == 0) continue;
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            seen.push_back(name);
            size_t i = 0;
            while (i < dirs.size() && !file_exists(join(dirs[i], raw))) i++;
            if (i == dirs.size()) missing.push_back(name);
            else if (i < own) todo.push_back(join(dirs[i], raw));
        }
    }
    return missing;
}

// What usually brings a missing DLL back (English: appended after the translated message, like the DLL names).
static std::string dll_fix(const std::string &name)
{
    if (name.rfind("vcruntime140", 0) == 0 || name.rfind("msvcp140", 0) == 0 || name.rfind("concrt140", 0) == 0 ||
        name.rfind("vcomp140", 0) == 0 || name.rfind("ucrtbase", 0) == 0)
        return "install the Microsoft Visual C++ 2015-2022 Redistributable (x64) from Microsoft (aka.ms/vs/17/release/vc_redist.x64.exe)";
    if (name == "d3d12core.dll" || name == "d3d12sdklayers.dll" || name == "dxcompiler.dll" || name == "dxil.dll")
        return "it ships in the COGBox zip - unzip the release again keeping every file (and check the antivirus didn't remove it)";
    if (name == "d3d12.dll" || name == "dxgi.dll" || name == "vulkan-1.dll" || name.rfind("d3dcompiler_", 0) == 0 || name.rfind("xinput", 0) == 0)
        return "part of Windows / the graphics driver - update Windows and the graphics driver";
    return "unzip the COGBox release again keeping every file, and check the antivirus didn't remove it";
}

static std::string missing_dlls_text(const std::vector<std::string> &m)
{
    std::string s;
    for (size_t i = 0; i < m.size() && i < 8; i++) s += (i ? "; " : "") + m[i] + " (" + dll_fix(m[i]) + ")";
    return s;
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
        // the texts in the launcher's language (the game shows them as given); the tab stays English - the game matches
        // "Achievements" by it and translates its button itself (--cogbox_ui_lang)
        t += "[option]\r\nid=" + o.id + "\r\ntab=" + gs::tab_name(o.tab) + "\r\nlabel=" + one_line(tr(o.label)) + "\r\nkind=" + kind + "\r\n" + nums;
        if (!choices.empty()) {
            t += "choices=";
            for (size_t i = 0; i < choices.size(); i++) t += (i ? "|" : "") + one_line(o.id == "Monitor" ? choices[i] : tr(choices[i]));
            t += "\r\n";
        }
        t += std::string("enabled=") + (av.ok ? "1" : "0") + "\r\nwhy=" + one_line(tr(av.why)) + "\r\ntip=" + one_line(tr(gs::tooltip_for(o, G, av, a.env[g]))) + "\r\n";
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

void play_start(App &a, int g, bool prepare)
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
    // Title-update preflight (#183 / #184, 0xC0000409 class): a title update made for another disc release than these
    // game files - its execution info's base version (the version it patches) or title isn't the default.xex's - makes
    // the game stop itself at start. Asked before the launch instead of letting it crash.
    if (gs::exe_for_update(G, ver, a.env[g]) && !st.updateDir.empty() && !a.tuMismatchOk[g]) {
        x360fs::XexInfo xi, pi;
        if (x360fs::read_xex_info(widen(join(st.dataDir, "default.xex")), xi) &&
            x360fs::read_xex_info(widen(join(st.updateDir, "default.xexp")), pi) && pi.base_version &&
            (pi.title_id != xi.title_id || pi.base_version != xi.version)) {
            ask(a, "tumismatch", g, tr("The title update doesn't match your game files"),
                std::string(tr("This title update is for another release of the game, and the game would most likely stop at start.")) +
                    " " + tr("Add the title update that matches your copy.") +
                    "\n\n" + tr("Game files") + ": " + x360fs::xbox_version_string(xi.version) + "   " + tr("Title update for") +
                    ": " + x360fs::xbox_version_string(pi.base_version),
                { tr("Play anyway"), tr("Cancel") });   // Shagg: no disc-version button
            return;
        }
    }
    // this play's own folder: the log, the arguments, any crash report
    std::string logDir = join(game_logs_dir(g), now_stamp());
    make_dirs(logDir);
    make_dirs(game_user_dir(g));
    make_dirs(game_shader_dir(g));
    if (g == G_GEARS2) {   // GitHub #58: shaders Gears 2 built before 0.3.0 sit next to the port; move them once
        const std::string to = game_shader_dir(g);
        const std::string from = join(exe.substr(0, exe.find_last_of("\\/")), "out\\shader-cache");
        if (dir_exists(from) && !file_exists(join(to, "pipelines-d3d12.bin"))) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(widen(join(from, "*")).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    MoveFileExW(widen(join(from, narrow(fd.cFileName))).c_str(), widen(join(to, narrow(fd.cFileName))).c_str(),
                                MOVEFILE_COPY_ALLOWED);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
    a.env[g].log_dir = logDir;
    std::vector<std::string> notes;
    std::vector<std::string> args = gs::build_args(G, ver, a.cur[g], a.env[g], &notes);
    std::string shared = join(exe_dir(), "shared");
    bool useShared = dir_exists(shared);
    if (useShared) args.push_back("--gow2_dxc_dir=\"" + shared + "\"");
    if (prepare) {   // job 641: build every shipped recipe, then the game saves its pipeline library and exits
        args.push_back("--gow2_renderer_prepare_all=true");
        DeleteFileW(widen(join(game_shader_dir(g), "prepare-progress.txt")).c_str());
    }
    if (a.env[g].cap.exe_has && a.env[g].cap.exe_has("cogbox_menu")) {   // ports with the in-game menu (patch 0163)
        write_menu_file(a, g, ver, logDir);
        args.push_back("--cogbox_menu=\"" + join(logDir, "menu.txt") + "\"");
        if (a.padPs && a.env[g].cap.exe_has("Circle: back to the game")) args.push_back("--cogbox_menu_ps=true");   // job 523: PlayStation names in the in-game menu
    }
    // Achievements (part 1): the game caches its achievement list in <COGBox>\achievements. The capability text is the
    // setting's description: a short name like "cogbox_dir" is built in code, not stored as text in the exe.
    // Part 4: the popups' text stands for both (every exe with the popups has the folder setting, and the launcher looks
    // for that text anyway), so the 82 MB exe is searched once for achievements instead of twice (~0.3 s each).
    if (a.env[g].cap.exe_has && a.env[g].cap.exe_has("COGBox achievement popups"))
        args.push_back("--cogbox_dir=\"" + exe_dir() + "\"");
    // The in-game text (menu, Achievements page, popups, shader notes) in the launcher's language, from lang\<code>.txt
    // beside CogBox.exe - only an exe that has the option (an older one would refuse it)
    if (a.env[g].cap.exe_has && i18n_current() != "en" && a.env[g].cap.exe_has("COGBox in-game language"))
        args.push_back("--cogbox_ui_lang=" + i18n_current());
    std::string argText;
    for (auto &s : args) argText += s + "\r\n";
    argText += "launcher: COGBox " COGBOX_VERSION ", game " + std::string(d.name) + ", exe " + exe + "\r\n";
    argText += "launcher: graphics card(s): ";
    for (size_t i = 0; i < a.gpus.size(); i++) argText += (i ? " / " : "") + a.gpus[i];
    argText += "\r\n";
    for (auto &n : notes) argText += n + "\r\n";
    {   // GitHub #194: a DLL the game can't start without, said before it fails with 0xC0000135
        const std::vector<std::string> miss = missing_dlls(exe);
        if (!miss.empty()) argText += "launcher: DLL(s) the game imports that aren't in its folder, shared, System32 or PATH: " + missing_dlls_text(miss) + "\r\n";
    }
    write_file(join(logDir, "args.txt"), argText);

    // The upscaler and shader-compiler DLLs every game uses are kept once, in <COGBox>\shared (Shagg 20:27, option 1):
    // the game gets that folder at the front of its PATH (FSR's loader and XeSS are loaded by name) and --gow2_dxc_dir
    // (the renderer loads dxcompiler.dll from there). DLSS (NGX looks only beside the exe), dxil.dll and the runtime
    // stay in each game folder. Checked by starting every game with FSR and XeSS from that layout (job 487).
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    for (auto &s : args) cmd += L" " + widen(s);
    std::wstring envBlock;   // this process's environment with PATH = shared;PATH (empty = inherit as is)
    // Proton / Wine: vkd3d-proton (DirectX 12 there) writes its own errors to vkd3d.log in the session folder, so a
    // problem report carries them (a player's own VKD3D_LOG_FILE wins).
    const bool vkd3dLog = a.env[g].wine;
    const bool newEnv = useShared || vkd3dLog;
    if (newEnv) {
        wchar_t *e = GetEnvironmentStringsW();
        bool hadPath = false, hadVkd3d = false;
        for (wchar_t *v = e; v && *v; v += wcslen(v) + 1) {
            std::wstring kv = v;
            if (useShared && _wcsnicmp(kv.c_str(), L"PATH=", 5) == 0) { kv = L"Path=" + widen(shared) + L";" + kv.substr(5); hadPath = true; }
            if (_wcsnicmp(kv.c_str(), L"VKD3D_LOG_FILE=", 15) == 0) hadVkd3d = true;
            envBlock += kv;
            envBlock.push_back(L'\0');
        }
        if (e) FreeEnvironmentStringsW(e);
        if (useShared && !hadPath) { envBlock += L"Path=" + widen(shared); envBlock.push_back(L'\0'); }
        if (vkd3dLog && !hadVkd3d) { envBlock += L"VKD3D_LOG_FILE=" + widen(join(logDir, "vkd3d.log")); envBlock.push_back(L'\0'); }
        envBlock.push_back(L'\0');
    }
    std::wstring dir = widen(exe.substr(0, exe.find_last_of('\\')));
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(widen(exe).c_str(), &cmd[0], nullptr, nullptr, FALSE, newEnv ? CREATE_UNICODE_ENVIRONMENT : 0,
                        newEnv ? (LPVOID)envBlock.data() : nullptr, dir.c_str(), &si, &pi)) {
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
    a.run.exe = exe;
    a.run.prepare = prepare;
    if (prepare) return;   // job 641: the launcher stays up and shows the progress
    a.ini.seti("LastPlayed", d.key, a.run.started);
    a.ini.save();
    a.playWasMax = (SDL_GetWindowFlags(a.win) & SDL_WINDOW_MAXIMIZED) != 0;   // job 526
    SDL_MinimizeWindow(a.win);
}

// job 528 / to-do 20: bring the launcher's window to the front after a game closed. Windows won't let a background
// program take the front (it only flashes its taskbar button). A tap of Alt counts as this program's own input,
// joining the foreground thread's input for a moment lets SetForegroundWindow through, and a moment as a topmost
// window lifts it over anything left behind.
// true when the launcher is already in front (then the 3-second retry stops, so a click elsewhere is left alone)
static bool launcher_to_front(App &a)
{
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(a.win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!h || GetForegroundWindow() == h) return true;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    const DWORD me = GetCurrentThreadId();
    const HWND fgw = GetForegroundWindow();
    const DWORD fg = fgw ? GetWindowThreadProcessId(fgw, nullptr) : 0;
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetFocus(h);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
    return GetForegroundWindow() == h;
}

void play_poll(App &a)
{
    if (a.raiseUntil) {   // to-do 20
        // review 0.4.5: at most every 250 ms, and given up once another program has the front (the player chose it)
        static uint64_t next = 0;
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        const bool other = fg && pid != GetCurrentProcessId() && pid != a.raisePid;
        if (a.run.proc || SDL_GetTicks() >= a.raiseUntil || other) a.raiseUntil = 0;
        else if (SDL_GetTicks() >= next) { next = SDL_GetTicks() + 250; if (launcher_to_front(a)) a.raiseUntil = 0; }
    }
    if (!a.run.proc || WaitForSingleObject(a.run.proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    bool known = GetExitCodeProcess(a.run.proc, &code) != 0;
    a.raisePid = GetProcessId(a.run.proc);   // review 0.4.5: the closing game's windows don't count as "another program"
    CloseHandle(a.run.proc);
    a.run.proc = nullptr;
    achievements_tab_reload();   // achievements part 4: its new unlocks
    int g = a.run.game;
    const GameDef &d = game_def(g);
    int64_t secs = std::max<int64_t>(0, unix_now() - a.run.started);
    if (a.run.prepare) {   // job 641: no play time, no window juggling - say how it went
        a.run.prepare = false;
        std::string prog;
        read_file(join(game_shader_dir(g), "prepare-progress.txt"), prog, 4096);
        const bool done = prog.find(" done ") != std::string::npos;
        append_line(join(a.run.logDir, "args.txt"), "launcher: prepare all shaders ended after " + std::to_string(secs) + " s, exit code " +
                                                        (known ? hex32(code) : std::string("unknown")) + ", progress " + prog);
        if (done && known && code == 0)
            ask(a, "info", g, "Shaders prepared",
                std::string(d.name) + "'s shaders are ready (" + std::to_string(secs / 60) + " min " + std::to_string(secs % 60) +
                " s). Chapters will now start without building them. Do this again after a graphics driver update or a new "
                "COGBox release.", { "OK" });
        else if ((known && code == 0) || (known && code == 0xC0B05709))   // job 666: 0xC0B05709 = the Stop button
            set_message(a, "Preparing shaders stopped before the end - what was built so far is kept.", false);
        else
            set_message(a, "Preparing shaders stopped (" + (known ? "exit code " + hex32(code) : std::string("unknown exit")) +
                           ") - what was built so far is kept. A problem report from the Help tab helps.", true);
        refresh_games(a, false);
        return;
    }
    // play time: only plays that got going (a start that fails at once isn't play)
    if (secs >= 20) a.ini.seti("PlayTime", d.key, a.ini.geti64("PlayTime", d.key, 0) + secs);
    a.ini.save();
    refresh_games(a, false);
    // job 526 (Shagg 20:46): back to the window as it was - fullscreen, maximised (borderless) or its normal size
    SDL_RestoreWindow(a.win);
    if (a.fullscreen) SDL_SetWindowFullscreen(a.win, true);
    else if (a.playWasMax) SDL_MaximizeWindow(a.win);
    SDL_RaiseWindow(a.win);
    // job 528 (Shagg 20:57): Windows won't let a background program take the front (it only flashes its taskbar
    // button), and the game that was in front has just closed. A tap of Alt counts as this program's own input, and
    // joining the foreground thread's input for a moment lets SetForegroundWindow through - the usual workaround.
    // to-do 20 (Shagg 3 Nov: the launcher didn't come back after the game closed): the game's window may still be
    // closing in this frame, so the launcher keeps asking for the front for 3 seconds (play_poll, launcher_to_front)
    a.raiseUntil = launcher_to_front(a) ? 0 : SDL_GetTicks() + 3000;

    std::vector<std::string> crash = crash_files(a.run.logDir);
    append_line(join(a.run.logDir, "args.txt"), "launcher: the game ended after " + std::to_string(secs) + " s, exit code " +
                                                    (known ? hex32(code) : std::string("unknown")) + ", crash reports " + std::to_string(crash.size()));
    bool restart = apply_menu_changes(a, g, a.run.logDir);
    if (known && code == 0 && crash.empty()) {
        if (restart) { append_line(join(a.run.logDir, "args.txt"), "launcher: restarting the game (in-game menu)"); play_start(a, g); }
        return;
    }
    // 0.2.0 (GitHub #6): the game closed because FSR crashed inside the graphics driver - FSR off for this game (SMAA)
    if (known && code == 0xE0C0B00Au) {
        a.cur[g].set("AAMode", 1);
        a.dirty[g] = true;
        settings_save(a);
        ask(a, "info", g, "FSR turned off",
            std::string(d.name) + " closed because AMD FSR crashed inside the graphics driver. COGBox has switched its "
            "anti-aliasing to SMAA. A newer graphics driver may fix FSR - you can turn it back on in the Graphics tab.", { "OK" });
        return;
    }
    std::string what;
    if (const char *m = known ? exit_meaning(code) : nullptr) what = m;
    else if (known && code != 0) what = "it ended with exit code " + hex32(code);
    else what = "it crashed";
    if (known && code == 0xC0000135) {   // GitHub #194: which DLL (the names and fixes stay in English)
        const std::vector<std::string> miss = missing_dlls(a.run.exe);
        what += miss.empty() ? std::string(". Missing DLL: none found by COGBox's check of the game's imports")
                             : ". Missing DLL: " + missing_dlls_text(miss);
    }
    std::string log;
    if (read_file(join(a.run.logDir, d.logName), log, 64u << 20) &&
        (log.find("DEVICE_REMOVED") != std::string::npos || log.find("DEVICE_HUNG") != std::string::npos || log.find("DXGI_ERROR_DEVICE") != std::string::npos))
        what += "; its log mentions the graphics card being lost (a driver reset, or an overheating / overclocked card)";
    // 0.4.6 (GitHub #183): a crash early on, right after the runtime warned that the title update is for another
    // version of the game (its base signature doesn't match the game's default.xex), most likely comes from that
    // update - say so plainly. (The warning alone is harmless on some known-good copies, so only with a crash.)
    if (code != 0 && secs <= 90 && log.find("XEX patch signature hash doesn't match") != std::string::npos)
        what += ". Its log says the title update doesn't match this copy of the game (it was made for another version of "
                "the game) - add the title update that belongs to your copy (same console and disc) by dropping it onto this window";
    if (!crash.empty()) what += "; it wrote a crash report";
    ask(a, "crash", g, std::string(d.name) + " closed",
        std::string(d.name) + " closed unexpectedly: " + what + ".\n\n" + (untested_version(a, g) ?
        "Your game version or title update is an untested one, so this may be why. Please don't report problems you get with it - "
        "try the expected version first.\n\n" : "") + "Make a problem report (a zip on your desktop) for it? "
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
        if (make_report(a, m.game, dir, what, zip, err)) report_share(a, m.game, zip, what);
        else set_message(a, "Couldn't make the report: " + err, true);
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
    {   // Shagg 31 Oct: name the experimental settings that were on, so a report shows them at once
        const gs::Settings &cs = a.cur[g];
        std::string exp;
        // Vulkan: experimental again in Gears 3 and Judgment (Shagg 3 Nov), not in Gears 1 / 2
        if (cs.i("Api") == 1 && (g == G_GEARS3 || g == G_JUDGMENT) && !a.wine) exp += "Vulkan, ";   // (the Proton path is Vulkan)
        // DLSS experimental again (Shagg 3 Nov: flickering in Gears 1 #147 and Judgment, smearing in Gears 3 #150); SMAA T2x is not
        if (cs.i("AAMode") >= 2 && cs.i("AAMode") <= 4) exp += std::string(cs.i("AAMode") == 2 ? "FSR" : cs.i("AAMode") == 3 ? "DLSS" : "XeSS") + ", ";
        if (cs.i("AO") >= 2) exp += "XeGTAO, ";   // experimental again (Shagg 3 Nov: about 5 ms a frame)
        if (cs.i("FrameGen") > 0) exp += "frame generation, ";
        if (cs.b("SystemLink")) exp += "System Link, ";
        if (cs.b("HDR")) exp += "HDR, ";   // 0.4.6: experimental in all four games
        sys += "Experimental settings on: " + (exp.empty() ? std::string("none") : exp.substr(0, exp.size() - 2)) + "\r\n\r\n";
    }
    // the game files' fingerprints (never the files)
    sys += std::string("Game files: ") + (st.dataDir.empty() ? "none" : st.xexMatch ? "the expected release" : "a different release (default.xex " +
           cached_sha1(a.ini, join(st.dataDir, "default.xex")) + ", " + std::to_string(file_size(join(st.dataDir, "default.xex"))) + " bytes)") + "\r\n";
    // GitHub #138: always the update's SHA-1 and size, and "expected" only for the port's own update - a file from the
    // reported-working list or one the player confirmed is named as such (#138's TU1 file was listed as "expected").
    const std::string xexpPath = st.updateDir.empty() ? "" : join(st.updateDir, "default.xexp");
    const std::string xexpSha = xexpPath.empty() ? "" : cached_sha1(a.ini, xexpPath);
    sys += std::string(d.updateName) + ": " + (st.updateDir.empty() ? "none" :
           std::string(xexpSha == d.xexp.sha1 ? "the expected update" : st.xexpMatch ? "a reported-working update" :
                       st.xexpConfirmed ? "a different update the player confirmed works" : "a different update") +
           " (default.xexp " + xexpSha + ", " + std::to_string(file_size(xexpPath)) + " bytes)") + "\r\n";
    sys += "\r\nMade by COGBox at " + now_stamp() + ". Your Windows user name and PC name are replaced (x's in crash dumps).\r\n";
    std::string name = std::string("CogBox-report-") + d.dir + "-" + now_stamp() + ".zip";
    // 0.2.1: under Proton / Wine the "desktop" is hidden inside the game's prefix - use COGBox's own reports folder
    std::string dir = a.wine ? join(exe_dir(), "reports") : desktop_dir();
    if (a.wine) make_dirs(dir);
    zip = join(dir, name);
    return report_write(files, sys, zip, err);
}
