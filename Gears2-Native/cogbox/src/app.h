// COGBox - the launcher's state, shared by the front screen (front.cpp), the game pages (page.cpp), adding a copy
// (install.cpp), starting a game (play.cpp) and the window (main.cpp).
#pragma once
#include <SDL3/SDL.h>
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "report.h"   // GpuCard (0.6.2)
#include "games.h"
#include "gears_settings.h"
#include "util.h"
#include "proton.h"   // proton049

#define COGBOX_VERSION "0.6.7"
#define COGBOX_NAME "COGBox"

namespace gs = cogbox::gears;
// The page's tabs after gs::Tab's (GitHub #425: named here, so a new settings tab moves them all - page.cpp's k_tabs)
constexpr int kTabSocial = (int)gs::Tab::Achievements + 1, kTabMods = kTabSocial + 1, kTabAbout = kTabMods + 1;
constexpr int kTabCount = kTabAbout + 1;

struct Cover {
    SDL_Texture *tex = nullptr;   // the player's own scan (covers\gears2.png ...), nullptr = drawn by COGBox
    int w = 0, h = 0;
};

// Adding a copy runs on a worker thread; the UI reads this.
struct InstallJob {
    std::atomic<int> state{0};          // 0 idle, 1 working, 2 done, 3 failed, 4 cancelled
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::mutex m;
    std::string what, current, error, result;   // what: "Unpacking the disc image"; result: the line shown when done
    int game = -1;
    std::thread th;
};

struct CrashDumpStatus;   // crash_dump.h

// A game that is running.
struct Running {
    HANDLE proc = nullptr;
    int game = -1;
    int64_t started = 0;
    std::string logDir;
    std::string exe;        // GitHub #194: the port program started (play_poll's missing-DLL check)
    bool prepare = false;   // job 641: a "Prepare all shaders" run (the game builds the pack, then exits)
    std::shared_ptr<CrashDumpStatus> crashDump;   // GitHub #286: the crash minidump watcher (crash_dump.h)
};

enum Screen { SCR_FRONT, SCR_PAGE, SCR_FABRICATOR };   // SCR_FABRICATOR: the local mod manager (fabricator_ui.cpp)

struct Modal {                       // one message box at a time, drawn by main.cpp
    bool open = false;
    std::string title, text;
    std::vector<std::string> buttons;   // first = default
    int answer = -1;                    // set when a button is pressed
    std::string tag;                    // who asked ("crash", "replace" ...)
    int game = -1;
    std::string data;                   // anything the asker needs back (a path)
    std::string check;                  // 0.2.0: a checkbox under the text when not empty ("Don't show this again")
    bool checked = false;
    std::vector<std::string> list;      // 0.2.0: a list to choose from (the language prompt); listSel = the choice
    int listSel = 0;
};

struct App {
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    bool quit = false;
    bool closeAfterPlay = false;   // 0.6.6 (GitHub #374): started with --play - close once that game ends normally
    Ini ini;
    GameState st[G_ALL];
    Cover cover[G_ALL];
    Cover backdrop[G_ALL];          // covers\<key>-background.jpg (downloaded with Get box art)
    gs::Store store;
    gs::Settings cur[G_COUNT];        // the values each page shows (Store::view), saved with Store::commit
    gs::Environment env[G_COUNT];
    bool dirty[G_COUNT] = {};

    // front screen
    float carPos = 0;                 // the carousel: which box is in front (fractional while turning)
    int front = 0;                    // the box it is turning to
    int frontSel = 0;                 // the controller's choice under the box: 0 Play / Add your copy, 1 Settings (LB / RB), 2 Achievements, 3 Social, 4 Fabricator
    double restSince = 0;             // when the carousel stopped turning
    // pages
    Screen screen = SCR_FRONT;
    int page = 0;                     // the game whose page is shown
    int tab = 0;                      // the page's tab: gs::Tab's first (DISPLAY ... ACHIEVEMENTS), then kTabSocial ...
    int fabTab = 0;                   // the FABRICATOR screen's tab: 0 Library, 1 Store, 2 Creator
    int barSel = -1;                  // a page's bottom bar chosen with the controller: -1 none (in the rows), 0 Library, 1 Save, 2 Play
    double zoomStart = -10;           // the front <-> page transition
    Screen zoomTo = SCR_FRONT;
    // the message line at the bottom
    std::string message; bool messageBad = false; double messageAt = -100;

    Modal modal;
    InstallJob job;
    Running run;
    std::vector<std::string> gpus;    // graphics cards (DXGI)
    std::vector<GpuCard> gpuCards;    // 0.6.2 (#304): the cards the Graphics card option offers (name + video memory)
    std::vector<std::string> audioDevices;   // GitHub #425: the sound output devices the Output device option offers
    bool hasAvx2 = true;
    bool padConnected = false;
    bool lastPad = true;              // 0.2.0: the last input came from a controller (hints follow it), else keyboard / mouse
    std::string joinLobby[G_COUNT];   // social step 3: the next start of this game joins that room code (Join friend)
    bool coldPrepOk[G_COUNT] = {};      // 0.6.5: "Play now" chosen at the cold-start question (until the launcher closes)
    bool prepThenPlay[G_COUNT] = {};    // 0.6.5: a Prepare all shaders run started from that question - play when it ends
    bool misplacedAsked[G_COUNT] = {};  // 0.6.4: the hand-copied saves question was asked for this Play (play.cpp)
    bool tuMismatchOk[G_COUNT] = {};  // TU preflight: "Play anyway" chosen for this game (until the launcher closes)
    bool padPs = false;               // job 523: the controller is a PlayStation one (button names: Cross, Circle...)
    // 0.6.3: the Custom controller layout's "press a button" (page.cpp pad_layout_section). padPress = the last controller
    // button pressed (a gs::PadInput, from SDL's gamepad events; LT / RT past half way), padPressSeq counts the presses.
    // padCaptureFrame = the last frame the list waited for a press: then and the frame after, the controller's own uses in
    // the launcher (menu navigation, LB / RB tabs, Start = Play, LT + RT = full screen) are off, so the press only sets.
    int padPress = -1;
    unsigned padPressSeq = 0;
    int padCaptureFrame = -100;
    // r48: padCaptureActive = a row is waiting now (a mouse click then only cancels: main.cpp takes the click, and
    // padCaptureClickFrame says which frame); padCaptureHold = after a press is set or the wait ends, padCaptureFrame is
    // kept fresh while any controller button, trigger or stick is still held, so a held LB / D-pad doesn't work the page
    bool padCaptureActive = false, padCaptureHold = false;
    double padCaptureHoldAt = 0;   // r49: when the hold began - it lasts 2.5 s at most (a drifting stick can't keep it on)
    int padCaptureClickFrame = -100;
    unsigned saves = 0;   // settings_save calls (the controller layout's Undo lasts until a save)
    bool borderless = false;          // job 523: no title bar (COGBox draws its own minimise / maximise / close)
    bool fullscreen = false;          // job 523: COGBox fills the screen (F11, or LT + RT together)
    bool tuTold[G_COUNT] = {};            // 0.4.6: "Title update needed" said for this game this session (front.cpp, install.cpp)
    uint64_t raiseUntil = 0;
    unsigned long raisePid = 0;       // the game that just closed (play_poll)          // to-do 20: keep bringing the launcher to the front until then (SDL ticks, ms)
    bool playWasMax = false;          // job 526: the window as it was when Play was pressed, put back afterwards
    bool wine = false;                // job 523: running under Wine / Proton (Linux, Steam Deck)
    bool setupFolderChecked = false;  // 0.6.1: the unpack-folder check (main.cpp startup_notices) ran this start
    bool adminChecked = false;        // 0.6.1: the "running as administrator - drag and drop is off" check ran this start
    bool mac = false;                 // job 643: ... on a Mac (CrossOver)
    proton::Info proton;              // proton049: Proton / SteamOS / Steam Deck details (all empty on Windows)
    std::vector<std::string> dropped; // files dropped on the window, handled next frame
};

void set_message(App &a, const std::string &m, bool bad = false);
void dlss_custom_answer(App &a, int g, const std::string &path);   // page.cpp, 0.4.9: "Use it" in the Custom DLSS file question
void report_now(App &a, int g, const std::string &session, bool versionOk = false);   // page.cpp: the About tab problem report
// page.cpp: the game version or title update isn't one COGBox expects (and the player hasn't said that file works)
bool untested_version(const App &a, int g);
// page.cpp: after a report: its folder opens with the zip selected and a pre-filled GitHub issue opens (Shagg 2 Nov)
void report_share(App &a, int g, const std::string &zip, const std::string &what);
// Job 523: a controller button's name for the connected pad - "A" on Xbox, "Cross" on PlayStation (A B X Y LB RB Back Start).
const char *pad_name(const App &a, const char *xbox);
bool pad_capturing(const App &a);   // 0.6.3: the controller layout list is waiting for a button (see App::padCaptureFrame)
void set_borderless(App &a, bool on);   // job 523: saved in CogBox.ini [Window] Borderless
void set_fullscreen(App &a, bool on);   // job 523: saved in CogBox.ini [Window] Fullscreen
void ask(App &a, const std::string &tag, int game, const std::string &title, const std::string &text,
         std::vector<std::string> buttons, const std::string &data = "");

// front.cpp
void front_draw(App &a, ImVec2 size);
void draw_box(App &a, int g, ImVec2 center, float height, float yawDeg, float light, bool reflection);
void covers_load(App &a);
bool cover_set_from_file(App &a, int g, const std::string &path);   // GitHub #241: a dropped picture as the game's cover
void backdrop_draw(App &a, ImDrawList *dl, ImVec2 size, int g, float alpha);
// Shagg 1 Nov: the games in the library carousel (added ones only, release order) and a game's place in it (-1: not there)
std::vector<int> library_games(const App &a);
int library_slot(const App &a, int g);
void art_download_start();
int art_download_poll();
// page.cpp
void page_draw(App &a, ImVec2 size);
// fabricator_ui.cpp: the FABRICATOR screen (LIBRARY, STORE, CREATOR), and a file or folder dropped on it (the CREATOR
// tab takes a folder as the mod to package; anything else is a mod to add)
void fabricator_draw(App &a, ImVec2 size);
void fabricator_drop(App &a, const std::string &path);
// achievements_tab.cpp (achievements part 4): the ACHIEVEMENTS tab below its option rows (the rows' label / control
// widths and right edge, as page.cpp lays them out); read again after a game closed; the front button's "open on All";
// the reveal question answered (ask tag "achreveal", 1 = yes).
void achievements_tab(App &a, int g, float labelW, float ctrlW, float rowX1);
void achievements_tab_reload();
void achievements_tab_open_all();
// social_tab.cpp (social step 3): the SOCIAL tab, and its worker (re)started from CogBox.ini [Online] and the profile
void social_tab(App &a, int g, float labelW, float ctrlW, float rowX1);
void social_restart(App &a);
bool social_delete_busy();   // 0.6.0: DELETE ONLINE ACCOUNT is still waiting for the server
void social_send_icon(App &a);   // the profile picture friends see (at sign-in and when it changes)
void social_remove_answer(int answer);
void social_delete_answer(App &a, int answer);   // DELETE ONLINE ACCOUNT, after the player confirmed
void social_delete_poll(App &a);              // each frame: finishes DELETE ONLINE ACCOUNT once the server answered
void social_rename(App &a, const std::string &name);   // SAVE NAME: the online account takes the new name
void achievements_reveal_answer(App &a, int answer);
void env_refresh(App &a, int g);
bool settings_save(App &a, int prefer = -1);   // prefer: the game whose page is saved (default: the open page)
std::vector<std::string> monitor_choices();   // page.cpp: "Default (main screen)", then each screen
// install.cpp
void install_start(App &a, const std::string &path, int forGame);
void import_saves_from(App &a, int game, const std::string &from);   // 0.6.1: page.cpp (Import saves, or a dropped save package)
// 0.6.4 (Shagg 16 Nov): saves copied into the game's saves folder by hand that the game can't use as they are
// (save_import::find_misplaced). misplaced_saves: what is there now (cached a few seconds); import_misplaced_saves: brings
// them in (a message says how it went).
std::vector<std::filesystem::path> misplaced_saves(App &a, int game);
bool import_misplaced_saves(App &a, int game);   // false: not done (a message says why)
// the CogBox.ini [Saves] key that remembers "Not now" for this game and launcher profile
std::string found_not_now_key(int game);
bool backup_running();   // main.cpp: "Back up saves" is zipping
void install_confirmed(App &a);      // the "replace" question answered yes
bool drop_is_game_or_update(const std::string &folder);          // 0.2.0: a dropped folder COGBox adds as a copy / update
std::vector<std::string> dlc_packages_in(const std::string &folder);   // 0.2.0: DLC packages in a dropped folder (3 levels)
// 0.2.0: downloadable content COGBox added (user\0000000000000000\<title>\00000002\<name>)
struct DlcItem {
    std::string name, title, dir, header;
    std::string dupOf, dupOfName;   // GitHub #283: set when another installed package already has all of this one's files
};
const std::vector<DlcItem> &dlc_list(int game);          // cached; refreshed after an add or remove
void dlc_remove_ask(App &a, int game, const DlcItem &item);
void dlc_remove_confirmed(App &a);   // the "dlcremove" question answered yes
void dlc_remove_dupes_ask(App &a, int game);   // GitHub #283: "Remove duplicates"
void dlc_remove_dupes_confirmed(App &a);       // the "dlcdupes" question answered yes (checked again in full on the job thread)
bool dlc_dupes_running();                        // GitHub #283 (r25): a duplicate check is reading package files now
bool dlc_dupes_checking(int game);             // GitHub #283 (r22 R1): the duplicate check for the listed folders is still running
// 0.2.0: mods (mods.cpp) - <Game>\mods\<mod>\, laid out like the game's data folder
struct ModItem {
    std::string folder, path, name, version, author, description;
    bool enabled = false;
    // Gears 3 (the game's own rules, modcore): why the game will leave it out, its campaign's place in Select Chapter,
    // and what in its mod.ini the game does not understand
    std::string problem, campaign;
    std::vector<std::string> notes;
    bool fab = false;         // installed by the Fabricator: its folder has COGBox's fabricator.json
    std::string fab_source;   // ... and the record's source kind ("file", "nexus"); "" when not
    uint32_t textures = 0;   // KiltedKernel: texture pack - the DDS files named by texture hash in its folder
};
std::string mods_dir(int game);
std::vector<ModItem> mods_list(App &a, int game);         // in load order
void mods_set_enabled(App &a, int game, const std::string &folder, bool on);
void mods_move(App &a, int game, const std::string &folder, int dir);   // -1 up (earlier), +1 down (later, wins)
// "Close the game first." while it runs; left_out (may be null) gets the program files left out (mods_unpack_zip)
bool mods_add_zip(App &a, int game, const std::string &zipPath, std::string &err, std::vector<std::string> *left_out = nullptr);
// Unpacks a mod zip into folder (made if missing): the zip's paths as Windows reads them, never outside folder, the
// real inflated bytes counted against the limits (8 GiB, 50 000 files); stripTop takes off a single top folder that
// isn't one of the game's own. Program files (mod_file_rules.h) are left out, their paths in folder added to left_out
// (may be null); a fabricator.json at any depth is left out too (only COGBox writes one). false + err (no file paths in
// it) on a problem; files already written stay.
bool mods_unpack_zip(App &a, int game, const std::string &zipPath, const std::string &folder, bool stripTop, std::string &err,
                     std::vector<std::string> *left_out = nullptr);
// The same without App, for a worker thread: dataDir is the game's data folder (App::st[game].dataDir, read on the UI
// thread), which decides whether a single top folder is the game's own.
bool mods_unpack_zip_in(const std::string &dataDir, const std::string &zipPath, const std::string &folder, bool stripTop, std::string &err,
                        std::vector<std::string> *left_out = nullptr);
void mods_store(App &a, int game, const std::vector<ModItem> &v);   // the order and on/off state, as listed
void mods_turn_on_last(App &a, int game, const std::string &folder);   // a new mod: on, last in the order
void mods_remove_tree(const std::wstring &dir);   // only a folder COGBox made itself (a .partial unpacking, a backup)
void cogbox_remove_tree(const std::wstring &dir);   // a folder of COGBox's own outside the mods (fabricator\work): never through a link
// (real_path / is_plain_child, review r369 F1: util.h)
void install_poll(App &a);
void install_draw(App &a, ImVec2 size);
// play.cpp
void play_start(App &a, int g, bool prepare = false);   // prepare: job 641's Prepare all shaders
void play_poll(App &a);
void play_answer(App &a, const Modal &m);
// linux_ui.cpp (proton049): Linux / Steam Deck through Proton - nothing happens on Windows
void linux_start(App &a, bool hadSettings);   // first start under Proton: the recommended settings (only without settings.ini)
void linux_poll(App &a);                      // "Look for my games" finished: the list to choose from
void linux_answer(App &a, const Modal &m);    // the "protonrec" / "protonfound" questions
float linux_copy_buttons_width(const App &a);
void linux_copy_buttons(App &a);              // Choose a file... / Choose a folder... / Paste a path (Linux: + Look for my games)
void linux_recommended_ask(App &a, int g);    // "Use recommended Steam Deck / Proton settings"
std::string shown_path(const App &a, const std::string &p);   // under Wine Z:\home\... shown as /home/...
// 0.6.2 review R7: shell_open(dir), and under Wine also "Folder: /home/..." as the message (Game Mode has no file
// manager, and inside Steam's container xdg-open may open nothing)
void open_folder(App &a, const std::string &dir);
void auto_settings_apply(App &a, int g);    // Shagg 29 Oct: the "Detect best settings" answer (page.cpp)   // the "crash" question answered
bool make_report(App &a, int g, const std::string &sessionDir, const std::string &what, std::string &zip, std::string &err);
std::string newest_session(int g);
// main.cpp
void refresh_games(App &a, bool hash);
// music.cpp (Shagg 1 Nov, the Athelas design): the chosen game's own menu music in the library, read from the player's copy
bool music_has_source(const App &a, int g);   // the game has a copy to read its music from
bool music_any_source(const App &a);          // ... a game in the library has
bool music_on(const App &a);                  // CogBox.ini [Launcher] Music
void music_set_on(App &a, bool on);
void music_update(App &a);                    // once a frame: what to play, on / off, paused while a game runs
void music_stop();                            // at exit
int music_test(const std::string &key, const std::string &copy, const std::string &report);   // --musictest
int music_scan(const std::string &folder, const std::string &report);                         // --musicscan
int music_probe(const std::string &file, const std::string &report);                          // --musicprobe
std::vector<std::string> audio_output_devices();   // GitHub #425: SDL's sound output devices' names (empty: none / no audio)
void audio_devices_refresh(App &a, bool force = false);   // ... into a.audioDevices and every game's env (start, Audio tab,
                                                          // Play); read at most every 3 s unless forced
