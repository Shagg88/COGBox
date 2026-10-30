// COGBox - the launcher's state, shared by the front screen (front.cpp), the game pages (page.cpp), adding a copy
// (install.cpp), starting a game (play.cpp) and the window (main.cpp).
#pragma once
#include <SDL3/SDL.h>
#include <windows.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "games.h"
#include "gears_settings.h"
#include "util.h"

#define COGBOX_VERSION "0.2.2"
#define COGBOX_NAME "COGBox"

namespace gs = cogbox::gears;

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

// A game that is running.
struct Running {
    HANDLE proc = nullptr;
    int game = -1;
    int64_t started = 0;
    std::string logDir;
    bool prepare = false;   // job 641: a "Prepare all shaders" run (the game builds the pack, then exits)
};

enum Screen { SCR_FRONT, SCR_PAGE };

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
    Ini ini;
    GameState st[G_COUNT];
    Cover cover[G_COUNT];
    Cover backdrop[G_COUNT];          // covers\<key>-background.jpg (downloaded with Get box art)
    gs::Store store;
    gs::Settings cur[G_COUNT];        // the values each page shows (Store::view), saved with Store::commit
    gs::Environment env[G_COUNT];
    bool dirty[G_COUNT] = {};

    // front screen
    float carPos = 0;                 // the carousel: which box is in front (fractional while turning)
    int front = 0;                    // the box it is turning to
    int frontSel = 0;                 // the controller's choice under the box: 0 Play / Add your copy, 1 Settings (LB / RB)
    double restSince = 0;             // when the carousel stopped turning
    // pages
    Screen screen = SCR_FRONT;
    int page = 0;                     // the game whose page is shown
    int tab = 0;
    int barSel = -1;                  // a page's bottom bar chosen with the controller: -1 none (in the rows), 0 Library, 1 Save, 2 Play
    double zoomStart = -10;           // the front <-> page transition
    Screen zoomTo = SCR_FRONT;
    // the message line at the bottom
    std::string message; bool messageBad = false; double messageAt = -100;

    Modal modal;
    InstallJob job;
    Running run;
    std::vector<std::string> gpus;    // graphics cards (DXGI)
    bool hasAvx2 = true;
    bool padConnected = false;
    bool lastPad = true;              // 0.2.0: the last input came from a controller (hints follow it), else keyboard / mouse
    bool padPs = false;               // job 523: the controller is a PlayStation one (button names: Cross, Circle...)
    bool borderless = false;          // job 523: no title bar (COGBox draws its own minimise / maximise / close)
    bool fullscreen = false;          // job 523: COGBox fills the screen (F11, or LT + RT together)
    bool playWasMax = false;          // job 526: the window as it was when Play was pressed, put back afterwards
    bool wine = false;                // job 523: running under Wine / Proton (Linux, Steam Deck)
    std::vector<std::string> dropped; // files dropped on the window, handled next frame
};

void set_message(App &a, const std::string &m, bool bad = false);
// Job 523: a controller button's name for the connected pad - "A" on Xbox, "Cross" on PlayStation (A B X Y LB RB Back Start).
const char *pad_name(const App &a, const char *xbox);
void set_borderless(App &a, bool on);   // job 523: saved in CogBox.ini [Window] Borderless
void set_fullscreen(App &a, bool on);   // job 523: saved in CogBox.ini [Window] Fullscreen
void ask(App &a, const std::string &tag, int game, const std::string &title, const std::string &text,
         std::vector<std::string> buttons, const std::string &data = "");

// front.cpp
void front_draw(App &a, ImVec2 size);
void draw_box(App &a, int g, ImVec2 center, float height, float yawDeg, float light, bool reflection);
void covers_load(App &a);
void backdrop_draw(App &a, ImDrawList *dl, ImVec2 size, int g, float alpha);
void art_download_start();
int art_download_poll();
// page.cpp
void page_draw(App &a, ImVec2 size);
void env_refresh(App &a, int g);
bool settings_save(App &a);
std::vector<std::string> monitor_choices();   // page.cpp: "Default (main screen)", then each screen
// install.cpp
void install_start(App &a, const std::string &path, int forGame);
void install_confirmed(App &a);      // the "replace" question answered yes
bool drop_is_game_or_update(const std::string &folder);          // 0.2.0: a dropped folder COGBox adds as a copy / update
std::vector<std::string> dlc_packages_in(const std::string &folder);   // 0.2.0: DLC packages in a dropped folder (3 levels)
// 0.2.0: downloadable content COGBox added (user\0000000000000000\<title>\00000002\<name>)
struct DlcItem { std::string name, title, dir, header; };
const std::vector<DlcItem> &dlc_list(int game);          // cached; refreshed after an add or remove
void dlc_remove_ask(App &a, int game, const DlcItem &item);
void dlc_remove_confirmed(App &a);   // the "dlcremove" question answered yes
// 0.2.0: mods (mods.cpp) - <Game>\mods\<mod>\, laid out like the game's data folder
struct ModItem { std::string folder, path, name, version, author, description; bool enabled = false; };
std::string mods_dir(int game);
std::vector<ModItem> mods_list(App &a, int game);         // in load order
void mods_set_enabled(App &a, int game, const std::string &folder, bool on);
void mods_move(App &a, int game, const std::string &folder, int dir);   // -1 up (earlier), +1 down (later, wins)
std::vector<std::string> mods_enabled_paths(App &a, int game);
bool mods_add_zip(App &a, int game, const std::string &zipPath, std::string &err);
void install_poll(App &a);
void install_draw(App &a, ImVec2 size);
// play.cpp
void play_start(App &a, int g, bool prepare = false);   // prepare: job 641's Prepare all shaders
void play_poll(App &a);
void play_answer(App &a, const Modal &m);
void auto_settings_apply(App &a, int g);    // Shagg 29 Oct: the "Detect best settings" answer (page.cpp)   // the "crash" question answered
bool make_report(App &a, int g, const std::string &sessionDir, const std::string &what, std::string &zip, std::string &err);
std::string newest_session(int g);
// main.cpp
void refresh_games(App &a, bool hash);
