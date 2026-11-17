// COGBox - the FABRICATOR screen's state (Task 12): what the store, the player's library and the creator's pages last
// answered, the Gets and publishes running, and the store's pictures as textures. Every network call and transfer runs
// on a worker thread (one for calls, one for pictures, one for downloads, one for uploads), so the UI thread never
// waits on the network; it reads a copy (fab_view) each frame. An install is split (fabricator_install.h): the UI thread
// picks the folder (fab_state_poll), an install worker checks and unpacks the zip into <folder>.partial (the long part:
// gigabytes for a texture pack), then the UI thread does the short steps that touch CogBox.ini and the mods folders
// (moves, backup, the order) - no lock, and one install at a time. Nothing here shows or logs a uuid, a token or a
// download address.
#pragma once
#include "fabricator_install.h"
#include "fabricator_model.h"
#include "fabricator_net.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct App;

// GET /creator/mods: one of the creator's mods, every version with its state
struct FabCreatorVersion {
    int64_t id = 0, created_at = 0, size = 0;
    std::string version, state;
};
struct FabCreatorMod {
    int64_t id = 0;
    std::string name, game, state;
    std::vector<FabCreatorVersion> versions;
    int downloads = 0, ratings = 0, reports_open = 0;
    double rating = 0;
};
// GET /creator/limits
struct FabLimits {
    bool known = false;
    std::string can_publish;   // "" may publish, else the server's reason ("account too new", "daily limit" ...)
    int64_t bytes_used = 0, quota = 0, max_file_bytes = 0;
    int uploads_today = 0, max_uploads_per_day = 0;
};

// The store listing asked for (GET /mods): search text, game / type ("" all), sort, page (from 1)
struct FabQuery {
    std::string search, game, type, sort = "popular";
    int page = 1;
    bool operator==(const FabQuery &o) const { return search == o.search && game == o.game && type == o.type && sort == o.sort && page == o.page; }
    bool operator!=(const FabQuery &o) const { return !(*this == o); }
};
constexpr int kFabPerPage = 12;

struct FabPublish;

// One Get (a download, then the install on the UI thread) or one publish. phase is read by the UI each frame.
struct FabTransfer {
    enum Kind { Download, Upload };
    enum Phase {
        Queued, Address, Downloading, Downloaded, Installing,   // a Get (Installing: checked and unpacked on a worker)
        Unpacked,                                               // a Get: unpacked, the UI thread moves it into place
        Packing, Checking, Uploading,                           // a publish (Uploading: the server checks once all is sent)
        Done, Failed, Cancelled
    };
    Kind kind = Download;
    std::atomic<int> phase{Queued};
    FabProgress progress;
    int64_t mod_id = 0;        // a Get: the mod; a publish: the mod a new version is for (0 a new mod), then the one published
    std::string name;          // the mod's name, for the lines on screen
    int game = -1;             // GameId
    bool update = false;       // a Get of a newer version over the installed one
    bool told_close = false;   // "Close the game first." said for this install (UI thread)
    FabLibraryItem item;       // a Get: what fab_install gets (version, version_id, sha256 from the download answer)
    std::string zip;           // a Get: the downloaded zip; a publish: the zip sent
    std::atomic<bool> zip_made{false};   // a publish: COGBox packed the zip from a folder (kept while it can be resumed)
    std::atomic<bool> resumable{false};  // a publish that stopped (cancelled, no answer): the server session and the packed
                                         // zip are kept for the next publish of the same folder or zip (fab_discard drops them)
    std::atomic<bool> discarding{false}; // fab_discard is asking the server
    std::shared_ptr<FabPublish> request; // a publish: what was asked (fab_retry sends it again)
    FabInstallPlan plan;                 // a Get: written by the UI thread before Installing, read by the install worker
    std::atomic<int> failed_phase{Queued};   // the phase it failed in (a publish: Packing, Checking = local checks, Uploading)
    std::vector<std::string> problems_;   // why it failed: written under the state's lock, read with fab_transfer_problems
};

struct FabView {
    bool online = false;          // CogBox.ini [Online] Enabled = 1 (off: the Fabricator makes no network call at all)
    bool configured = false;      // a server address is set
    bool signed_in = false;       // the profile's online account is there (Online play on, identity.json)
    bool account_checked = false;
    std::string tag;              // Name#1234
    // the store
    FabQuery query;
    bool store_asked = false, store_loading = false;
    std::vector<FabCard> cards;
    int total = 0;
    std::string store_error;
    // a mod's page
    int64_t page_id = 0;
    bool page_loading = false;
    std::optional<FabPage> page;
    std::string page_error;
    // the player's library (GET /library)
    bool library_loaded = false, library_loading = false;
    std::vector<FabLibraryItem> library;
    std::string library_error;
    // the creator's mods and limits
    bool creator_loaded = false, creator_loading = false;
    std::vector<FabCreatorMod> creator;
    FabLimits limits;
    std::string creator_error;
    // Gets and publishes (newest last); finished ones stay until fab_dismiss
    std::vector<std::shared_ptr<FabTransfer>> transfers;
    std::map<int64_t, int> my_ratings;   // stars this player gave (remembered in <state dir>\ratings.json)
};

// The server (CogBox.ini [Online] Server), the profile folder whose identity.json is the account, and whether online
// play is on ([Online] Enabled, as the SOCIAL tab reads it): off, no call or transfer goes out (running ones are
// cancelled). Called by the screen each frame; a change asks the account again.
void fab_configure(const std::string &server_url, const std::string &identity_dir, bool online);
FabView fab_view();
void fab_refresh_account();
void fab_store_query(const FabQuery &q);
void fab_open_page(int64_t id);   // 0 closes the page
void fab_load_library();
void fab_load_creator();
void fab_rate(int64_t id, int stars);
void fab_report(int64_t id, const std::string &reason, const std::string &text);
void fab_remove_own(int64_t id);
// Get: a download address (which also puts the mod in the library), the download to <state dir>\downloads, then the
// install (prepare and commit on the UI thread, waiting while a game runs; the unpack on a worker), the zip deleted after.
void fab_get(const FabLibraryItem &item, int game, bool update);
// Publish: a folder is zipped first, to <state dir>\uploads\<16 hex of the SHA-256 of its full path>.zip with a manifest
// (every file's path, size and time) beside it: publishing the same unchanged folder again reuses that zip, so the upload
// goes on where it stopped; a changed folder is packed again and its old server session deleted. The zip is checked
// here (fab_check_zip), then fab_upload. The packed zip is deleted once published, refused, or discarded.
struct FabPublish {
    std::string source;        // a folder or a .zip
    bool folder = false;
    std::string game_key;      // gears1 .. judgment
    std::string name;
    std::string meta_json;
    int64_t mod_id = 0;        // 0: a new mod
    std::vector<std::string> screens;
};
void fab_publish(const FabPublish &p);
void fab_cancel(const std::shared_ptr<FabTransfer> &t);
// A stopped publish given up for good: its server session is deleted (it stops counting toward the daily limit and the
// quota), the zip COGBox packed is deleted and the transfer leaves the list. A session the server is still finishing
// (409), or a server that can't be reached, keeps both: the player is told to try again in a moment.
void fab_discard(const std::shared_ptr<FabTransfer> &t);
// A stopped publish sent again as it was asked (its kept session goes on from where it stopped).
void fab_retry(const std::shared_ptr<FabTransfer> &t);
void fab_dismiss(const std::shared_ptr<FabTransfer> &t);   // a finished one leaves the list
std::vector<std::string> fab_transfer_problems(const FabTransfer &t);
// The first running Get or publish's percent (0-100), -1 when none runs (the front screen's FABRICATOR button).
int fab_transfer_percent();

// Goes up each time a Get installed something (or the screen changed a mod): the screen reads the mods folders again.
uint64_t fab_local_changes();
void fab_mark_local_change();

// A store picture as a texture: only "/api/v1/fabricator/images/<64 hex>.png" (anything else the server sends is
// ignored). nullptr while it loads (on the picture worker), when it can't be read, or over 3840 x 2160. Cached by key,
// at most 120 (least used first out, never one drawn this frame); freed when the screen closes.
SDL_Texture *fab_image(App &a, const std::string &path, int *w = nullptr, int *h = nullptr);
// A picture the player chose for an upload (a local file): the upload form's previews only, never store data.
SDL_Texture *fab_local_image(App &a, const std::string &file, int *w = nullptr, int *h = nullptr);
void fab_images_free();

// Once a frame on the UI thread (main.cpp): finished downloads installed, messages shown, pictures freed when the
// screen is not open.
void fab_state_poll(App &a);
// At exit: transfers cancelled, the workers given a moment to stop, the textures freed.
void fab_state_shutdown();

// mod.ini of a folder or a zip (the upload form's prefill): Name, Game, Version, Description, Side, Type.
struct FabModIni {
    bool found = false;
    std::string name, game, version, description, side, type;
};
FabModIni fab_read_mod_ini(const std::string &source, bool folder);
