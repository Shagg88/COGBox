// COGBox - the Gears of War settings (Gears 2, Gears 3, Judgment): data model, presets, per-game memory, ini load/save
// and the game's command line. A port of the game-settings part of Gears2-Native's tools\launcher.ps1 (WinForms);
// "PS1 nnn" comments cite that file's line numbers. No UI code here: the Dear ImGui side reads options() and calls
// available() / build_args(); everything that depends on the PC comes in through Environment / Capability.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cogbox {
namespace gears {

enum class Game { Gears2 = 0, Gears3 = 1, Judgment = 2 };
enum class Version { TitleUpdate = 0, Disc = 1 };   // PS1 125 $verNames (index 0 = the title update)
enum class Tab { Display, Graphics, Controls, Gameplay };
enum class Kind { Bool, Choice, Int, Float };
// Where a value is kept (PS1 103-123, 418-441, 446-464, 759-774):
//   Shared  - one value for all games (PS1 top-level $s keys outside $perKeys)
//   PerGame - remembered per game in PerGame (PS1 120 $perKeys); a game without its own set starts from the last
//             game's values (the top-level ones)
//   GameOwn - its own key per game from the start (PS1 StartG2/G3/GJ, Version/G3Version, Aniso/G3Aniso/GJAniso,
//             FpsMode/G3FpsMode/GJFpsMode), with per-game defaults
enum class Scope { Shared, PerGame, GameOwn };
enum Preset { PresetOriginal = 0, PresetHigh = 1, PresetUltra = 2, PresetCustom = 3 };   // PS1 160

// What the game's exe can do. The caller implements it for the exe that Play would start (see exe_for_update()).
struct Capability {
    // PS1 216 Exe-Has: the exe contains this text (simple, case-insensitive match as Select-String -SimpleMatch).
    std::function<bool(const std::string& needle)> exe_has;
    // PS1 213 Runtime-For + Exe-Has: the SDK runtime DLL next to the exe (rexruntimerd.dll, else rexruntime.dll)
    // exists and contains this text. False when there is no runtime DLL.
    std::function<bool(const std::string& needle)> runtime_has;
    // A file of this name is next to the exe (PS1 280, 852-854).
    std::function<bool(const std::string& name)> file_next_to_exe;
};

// Everything about this PC / install for ONE game (the one being shown or started).
struct Environment {
    // Data roots (PS1 802-821: fixed project paths there, parameters here). Strings are UTF-8, put in "..." as given.
    std::string game_data_root;     // --game_data_root (the game's extracted disc)
    std::string update_data_root;   // --update_data_root (the extracted title update package)
    std::string user_data_root;     // --user_data_root (saves)
    std::string shader_cache_dir;   // --gow2_shader_cache_dir (Gears 3 / Judgment only; PS1 813, 820)
    std::string log_dir;            // this play session's folder (--log_file, --gow2_crash_dir, {LOG} in extra args)
    char path_sep = '\\';           // used to join log_dir with the log file's name
    // PS1 15 / 24 / 821: the title update build and its package are there (Gears 2 / 3: the TU6 exe and default.xexp;
    // Judgment: its TU4 default.xexp - Judgment always runs with it when present).
    bool update_ready = false;
    // PS1 225 $g2Edits0127: Gears 2's DISC exe has the ini-edits replace form ("'file|section|~=old>new'"). The PS1
    // checks the disc exe even when the TU6 exe runs, so it is its own field.
    bool g2_disc_ini_edits = false;
    // Graphics cards (PS1 151-153): every card's name; gpu_name = the main one (main_gpu(gpus) gives the PS1's pick;
    // if gpu_name is empty and gpus is not, main_gpu(gpus) is used).
    std::vector<std::string> gpus;
    std::string gpu_name;
    int screen_count = 1;           // PS1 146 (Monitor is greyed with fewer than 2)
    // PS1 947-948: the screen the game uses - Monitor 1..N if within screen_count, else the main screen - its refresh
    // rate now (0 = unknown) and its device name (only for the note).
    int refresh_hz = 0;
    std::string monitor_device;
    bool has_avx2 = true;           // PS1 207-210 GearsCpu.HasAvx2
    bool wine = false;              // job 523: Wine / Proton (Linux, Steam Deck)
    std::map<std::string, std::string> keys;   // 0.2.0: the player's own keyboard keys, cvar -> key (CogBox.ini [Keys])
    // PS1 960-961: the lines of out\extra-args.txt (raw; empty = no file). Trimmed, '#' lines skipped, {LOG} = log_dir.
    std::vector<std::string> extra_args_lines;
    Capability cap;
};

// One game's values, keyed by option id (bool 0/1, choice index, number).
class Settings {
public:
    std::map<std::string, double> v;
    bool has(const std::string& id) const { return v.count(id) != 0; }
    double f(const std::string& id) const { auto it = v.find(id); return it == v.end() ? 0.0 : it->second; }
    int i(const std::string& id) const { return static_cast<int>(f(id)); }
    bool b(const std::string& id) const { return f(id) != 0.0; }
    void set(const std::string& id, double x) { v[id] = x; }
};

struct Availability {
    bool ok = true;
    std::string why;   // the PS1's "Not yet in <game> (...)." text; may be empty when greyed for a UI reason
};

struct Option {
    std::string id;                    // stable; the ini key (the PS1's $s key, without the G3/GJ prefix)
    std::string label;                 // the launcher's text
    Tab tab = Tab::Gameplay;
    Kind kind = Kind::Bool;
    Scope scope = Scope::Shared;
    std::vector<std::string> choices;  // Kind::Choice
    double def = 0, min = 0, max = 1, step = 1;
    int decimals = 0;                  // Kind::Float display
    std::string tooltip;               // the PS1's tooltip = the plain-English description
    std::string feature;               // the PS1's Can feature name ("" = always available)
    std::string depends_on;            // another option whose state enables this one ("" = none)
    bool in_preset = false;            // part of the quality preset: changing it makes the preset Custom (PS1 588-591)
    int ps1_line = 0;                  // where its control is made in the PS1
};

// The options for a game (the same list for every game - Shagg's rule, PS1 4-5 - with that game's chapter list and
// defaults). 39 options; see options_count_by_tab() in the test.
const std::vector<Option>& options(Game g);
const Option* find_option(Game g, const std::string& id);
const char* tab_name(Tab t);

// PS1 263 Can: the reason a feature is greyed for the game (ok = available). Uses env.cap for the exe of `ver`.
Availability can(Game g, const std::string& feature, Version ver, const Environment& env);
// can() for the option's feature plus the UI dependencies (fullscreen -> exclusive, HDR -> nits, AA -> scene
// resolution, at least 2 screens -> monitor).
Availability available(const Option& o, Game g, Version ver, const Settings& s, const Environment& env);
// A single choice greyed: the FPS counter's "Game's own (column)" in Judgment (PS1 267, 475).
Availability choice_available(const Option& o, Game g, int index);
// The tooltip to show: the reason in front of the description (PS1 412-416, 476-477), the GPU line for AA (PS1 345).
std::string tooltip_for(const Option& o, Game g, const Availability& a, const Environment& env);

// The version that actually runs (PS1 459: TU only when its build is there; Judgment always its TU).
Version effective_version(Game g, const Settings& s, const Environment& env);
// Play starts the title update exe (Gears 2 / 3: TU6 build; Judgment: its only exe, with TU4 when present).
bool exe_for_update(Game g, Version ver, const Environment& env);

// Presets (PS1 573-592). apply_preset sets the 13 preset boxes and Preset; Custom changes nothing.
void apply_preset(Settings& s, int preset);
// Set a value as the UI would: an in_preset option that really changes makes the preset Custom (PS1 588).
// Setting "Preset" itself applies the preset.
void set_value(Game g, Settings& s, const std::string& id, double value);

// Monitor list entries (PS1 147).
std::string monitor_label(int index1, int width, int height, bool primary);
// PS1 152: the main graphics card of a list.
std::string main_gpu(const std::vector<std::string>& gpus);
// PS1 141: the start URL for a chapter (0-based).
std::string launch_url(Game g, int chapter);

// The remembered settings of all three games (PS1 $s + PerGame).
class Store {
public:
    Store();                                         // the PS1 $defaults (PS1 103-116, 154, 157-158)
    int last_game = 0;                               // PS1 $s.Game
    Settings view(Game g, const Environment& env) const;     // PS1 Update-ForGame + Load-Per (446-464, 428-441)
    void remember(Game g, const Settings& s);                // leaving a game in the UI (PS1 450-455)
    void commit(Game g, const Settings& s, const Environment& env);   // Play / save (PS1 759-774)
    bool load_ini(const std::string& text);          // unknown keys ignored, missing keys = defaults
    std::string save_ini() const;
    void reset();                                    // PS1 737-741: everything back to the defaults
    // The raw stores (exposed for tests / migration).
    std::map<std::string, double> top;               // Shared + PerGame keys (the last game's), and the legacy flags
    std::map<std::string, double> per[3];            // PerGame sets; empty = the game has none yet
    std::map<std::string, double> own[3];            // GameOwn values
private:
    void migrate();
};

// 0.2.0: the keyboard keys for keyboard-and-mouse play - each controller input, what it does in Gears, its default.
struct KeyAction { const char *cvar; const char *pad; const char *what; const char *def; };
const std::vector<KeyAction> &key_actions();

// PS1 788-961 (Play): the game's command-line arguments, in the PS1's order. `ver` is the chosen version (see
// effective_version); `notes`, if given, gets the "launcher: ..." lines the PS1 adds to args.txt.
std::vector<std::string> build_args(Game g, Version ver, const Settings& s, const Environment& env,
                                    std::vector<std::string>* notes = nullptr);
// PS1 966-972: true = do not start (the exe is an AVX2 build and this CPU has no AVX2); `message` = what to say.
bool blocked_by_cpu(Game g, const Environment& env, std::string* message = nullptr);

}  // namespace gears
}  // namespace cogbox
