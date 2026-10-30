// COGBox - the three games, where their ports and the player's copies are, and whether each is ready to play.
//
// The release folder (what the zip unpacks to):
//   CogBox.exe, SDL3.dll, README.txt, licenses (folder)
//   Gears2\port-tu6\gow2.exe   Gears2\port-disc\gow2.exe     (each with its runtime DLLs)
//   Gears3\port-tu6\gow3.exe   Gears3\port-disc\gow3.exe
//   Judgment\port\gowj.exe
// What COGBox makes there for the player (never in the zip, so an update keeps it):
//   <Game>\game\extracted      the disc's files (default.xex ...), unless the player points at a folder of their own
//   <Game>\game\update         the title update's files (default.xexp ...)
//   <Game>\user                saves and the port's own files      <Game>\logs\<date-time>   one folder per play
//   <Game>\shadercache         shaders and pipelines, kept between plays
#pragma once
#include <string>
#include <cstdint>
#include "util.h"

enum GameId { G_GEARS2, G_GEARS3, G_JUDGMENT, G_COUNT };

// Job 647 (GitHub #45, region discs): code = SHA-1 of the XEX from its image (PE data) offset on. The European and
// Japanese Judgment discs and the European Gears 2 disc differ from the US ones only in the XEX header (region, media
// ID, signature) - the same program - so they count as the tested version. "" = no code fingerprint.
struct Fingerprint { uint64_t size; const char *sha1; const char *code = ""; };

struct GameDef {
    GameId id;
    const char *name;          // "Gears of War 2"
    const char *shortName;     // "Gears 2"
    const char *key;           // "gears2": CogBox.ini keys, covers\gears2.png
    const char *dir;           // "Gears2": its folder in the release
    uint32_t titleId;          // the Xbox 360 title ID (from the game's XEX / packages)
    const char *exeName;       // "gow2.exe"
    const char *logName;       // "gow2.log"
    const char *updateName;    // "Title Update 6"
    bool updateNeeded;         // true: without the update the disc build runs (it has one); false: one build, update optional
    const char *portTu;        // the port folder for the updated game ("port-tu6"); "" = none
    const char *portDisc;      // the port folder for the disc game ("port-disc", or "port" when there is one build)
    Fingerprint xex;           // the disc's default.xex the ports are made for
    Fingerprint xexp;          // the title update's default.xexp
    const char *year;
    const char *tagline;       // a line under the box when the game hasn't been added
};

const GameDef &game_def(int g);
int game_by_title_id(uint32_t tid);        // -1 if none of ours

enum Readiness {
    R_NO_PORT,         // this release has no port program for the game
    R_NO_COPY,         // the player hasn't added their copy yet
    R_COPY_MISSING,    // a folder was added but it's gone (a drive unplugged?)
    R_NEED_UPDATE,     // only the title update build is in this release and the update hasn't been added
    R_READY,           // can play
};

// Everything the launcher knows about one game on this PC (refreshed by game_refresh).
struct GameState {
    std::string dataDir;       // the folder with default.xex ("" if none)
    std::string updateDir;     // the folder with default.xexp ("" if none)
    bool dataOwn = false;      // the player's own folder (pointed at), not one COGBox made
    bool xexKnown = false, xexMatch = false;   // checked against the fingerprint (cached by size + time)
    bool xexpKnown = false, xexpMatch = false;
    // Shagg 29 Oct: an untested version the player said works ("This version works") - remembered by its fingerprint
    bool xexConfirmed = false, xexpConfirmed = false;
    std::string portExeTu, portExeDisc;        // full paths when present
    Readiness ready = R_NO_PORT;
    bool useUpdate = false;    // Play starts the updated game
    int64_t playSeconds = 0, lastPlayed = 0;
    std::string versionText() const;           // "Title Update 6" / "Disc version" / ""
    // the exe Play starts: the update build when the update is used and there is one (Judgment has one build for both)
    std::string exe() const { return useUpdate && !portExeTu.empty() ? portExeTu : (portExeDisc.empty() ? portExeTu : portExeDisc); }
};

std::string game_root(int g);              // <exe dir>\Gears2
std::string game_user_dir(int g);
std::string game_logs_dir(int g);
std::string game_shader_dir(int g);
std::string game_default_data_dir(int g);  // <root>\game\extracted
std::string game_default_update_dir(int g);

// Reads CogBox.ini and the folders; the fingerprints are only hashed when 'hash' is set (slow: ~20 MB of reading).
void game_refresh(int g, Ini &ini, GameState &st, bool hash);
// Stores where the game's files are.
void game_set_data(Ini &ini, int g, const std::string &dataDir, bool own);
void game_set_update(Ini &ini, int g, const std::string &updateDir);
// The SHA-1 of a file, cached in CogBox.ini by path + size + time ([Hashes]).
std::string cached_sha1(Ini &ini, const std::string &path);
std::string cached_code_sha1(Ini &ini, const std::string &path);   // job 647
bool xex_is_tested(const GameDef &d, const std::string &full, const std::string &code);
