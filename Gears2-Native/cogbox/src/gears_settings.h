// COGBox - the Gears of War settings (Gears 2, Gears 3, Judgment, Gears 1): data model, presets, per-game memory, ini load/save
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

// Gears1 is the last index (added after the others) so saved settings keep their meaning.
enum class Game { Gears2 = 0, Gears3 = 1, Judgment = 2, Gears1 = 3 };
constexpr int kGameCount = 4;
// 0.6.2 (Shagg 11-14 17:18): the first-person camera ships as a hidden tester option - no launcher option; testers turn it
// on with --gow3_first_person=true / --gow2_ / --gowj_ in extra-args.txt. true = the Gameplay tab offers it again.
constexpr bool kOfferFirstPerson = false;
enum class Version { TitleUpdate = 0, Disc = 1 };   // PS1 125 $verNames (index 0 = the title update)
// = the page's tab index (SOCIAL, MODS, ABOUT are after). GitHub #425: Audio before Achievements - tabs are not saved by
// index (options by id, the in-game menu by tab_name()), and page.cpp / front.cpp / main.cpp name these indexes.
enum class Tab { Display, Graphics, Controls, Gameplay, Audio, Achievements };
// Text (0.4.8, GitHub #229): a short line of text (Settings::t), always Scope::Shared, kept in [General] of the settings
// file; every value goes through sanitize_text() on the way in and out, so only its allowed characters ever reach the
// file or the command line.
enum class Kind { Bool, Choice, Int, Float, Text };
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
    // A file of this name is next to the exe (as above) and its version resource's ProductName or FileDescription
    // contains this text (case-insensitive). False when the file has no version resource.
    std::function<bool(const std::string& name, const std::string& text)> file_product_has;
};

// Everything about this PC / install for ONE game (the one being shown or started).
struct Environment {
    // Data roots (PS1 802-821: fixed project paths there, parameters here). Strings are UTF-8, put in "..." as given.
    std::string game_data_root;     // --game_data_root (the game's extracted disc)
    std::string update_data_root;   // --update_data_root (the extracted title update package)
    std::string user_data_root;     // --user_data_root (saves)
    std::string identity_dir;       // --net_identity_dir (the profile's online account, shared by all games)
    std::string online_region;      // the region chosen in the SOCIAL tab ("" = Any region)
    std::string online_server;      // social step 3: the multiplayer server when enabled ("" = offline profiles)
    std::string online_stun;        // its STUN address (host:3478)
    std::string shader_cache_dir;   // --gow2_shader_cache_dir (every game; Gears 2 since #58)
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
    // 0.6.2 (GitHub #304): the graphics cards the game can choose from - the DXGI hardware adapters' names, as the game
    // reads them (no software adapters, no Microsoft Basic Render Driver). The Graphics card option is sent only when its
    // saved name is one of these. Empty = not worked out (the option sends nothing: Automatic).
    std::vector<std::string> graphics_cards;
    // GitHub #425: the sound output devices the game can choose from - SDL's playback devices' names, as the runtime's SDL
    // lists them (SDK patch 117). The Output device option is sent only when its saved name is one of these. Empty = not
    // worked out (the option sends nothing: System default).
    std::vector<std::string> audio_devices;
    int screen_count = 1;           // PS1 146 (Monitor is greyed with fewer than 2)
    // PS1 947-948: the screen the game uses - Monitor 1..N if within screen_count, else the main screen - its refresh
    // rate now (0 = unknown) and its device name (only for the note).
    int refresh_hz = 0;
    std::string monitor_device;
    // 0.4.9 (4:3): that screen's desktop size in pixels (0 = unknown) - Aspect ratio Auto picks 4:3 from it
    int monitor_w = 0, monitor_h = 0;
    bool has_avx2 = true;           // PS1 207-210 GearsCpu.HasAvx2
    bool wine = false;              // job 523: Wine / Proton (Linux, Steam Deck)
    bool mac = false;               // job 643: Wine on a Mac (CrossOver) - DirectX 12 (D3DMetal) by default
    // proton049: a Steam Deck under Proton (proton.h) and its LCD model (no HDR screen). Both false on Windows.
    bool steam_deck = false;
    int low_port_start = -1;        // 0.6.2 (R4): Linux's first port for normal programs (-1 unknown / not Linux)
    bool deck_lcd = false;
    std::vector<std::string> mods;              // 0.2.0: the enabled mod folders, in load order (--ue3_mods)
    // texture packs (KiltedKernel; r95 L1): those of `mods` that hold texture-pack files, in load order
    // (--gow2_texture_pack_dirs - only these, so the command line stays short and the renderer walks no other mod)
    std::vector<std::string> texture_pack_mods;
    unsigned vram_mb = 0;                      // 0.6.4 (GitHub #341): the main graphics card's video memory, 0 = unknown
    std::map<std::string, std::string> keys;   // 0.2.0: the player's own keyboard keys, cvar -> key (CogBox.ini [Keys])
    std::string recipe_pack;                   // job 630: <COGBox>\recipes\recipes-<exe>.rcp, "" = none
    std::string prompt_dir;                    // job 629: <COGBox>\prompts\keyboard (key-cap overlays), "" = none
    // PS1 960-961: the lines of out\extra-args.txt (raw; empty = no file). Trimmed, '#' lines skipped, {LOG} = log_dir.
    std::vector<std::string> extra_args_lines;
    std::string pad_remap;  // GitHub #97: CogBox.ini [Pad] Remap= (the Custom controller layout, SDK patch 55; PadMap below)
    std::string gamertags[3];   // 0.4.9 split-screen: [Profile] Gamertag2..4 (--user_gamertag2..4, SDK patch 71); "" = "Player N"
    std::string gamertag;   // GitHub #119: CogBox.ini [Profile] Gamertag= (--user_gamertag, SDK patch 54); "" = the runtime's "User"
    // GitHub #159: the Lang / VoiceLang choices (by index) whose files are not on the player's copy, found by the
    // launcher from game_data_root (empty: not checked, every choice allowed). A choice saved before, or copied from
    // another game's set, is then not passed on: the game keeps its own default instead of finding nothing.
    std::vector<bool> lang_missing, voice_missing;
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
    std::map<std::string, std::string> t;   // Kind::Text values (already sanitised)
    std::string str(const std::string& id) const { auto it = t.find(id); return it == t.end() ? std::string() : it->second; }
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
    int max_len = 0;                   // Kind::Text: the most characters kept
    std::string tooltip;               // the PS1's tooltip = the plain-English description
    std::string feature;               // the PS1's Can feature name ("" = always available)
    std::string depends_on;            // another option whose state enables this one ("" = none)
    bool in_preset = false;            // part of the quality preset: changing it makes the preset Custom (PS1 588-591)
    int ps1_line = 0;                  // where its control is made in the PS1
};

// The options for a game (the same list for every game - Shagg's rule, PS1 4-5 - with that game's chapter list and
// defaults). 40 options (PR #43: RealOcclusion); see options_count_by_tab() in the test.
const std::vector<Option>& options(Game g);
const Option* find_option(Game g, const std::string& id);
const char* tab_name(Tab t);

// PS1 263 Can: the reason a feature is greyed for the game (ok = available). Uses env.cap for the exe of `ver`.
Availability can(Game g, const std::string& feature, Version ver, const Environment& env);
// can() for the option's feature plus the UI dependencies (fullscreen -> exclusive, HDR -> nits, AA -> scene
// resolution, at least 2 screens -> monitor).
Availability available(const Option& o, Game g, Version ver, const Settings& s, const Environment& env);
// A single choice greyed: the FPS counter's "Game's own (column)" in Judgment / Gears 1 (PS1 267, 475), and the game
// languages Gears 1 has no text for.
Availability choice_available(const Option& o, Game g, int index);
// Frame generation choices (FrameGen): 2-5 (NVIDIA DLSS) are not listed at all without the exe's support and NVIDIA's
// Streamline DLLs next to it; greyed without an RTX 40 (2x) / RTX 50 (3x, 4x, dynamic) card. 1 (AMD) greyed without its DLL.
bool dlss_fg_present(const Environment& env);
// 7 = a frame-generation choice dropped in 0.6.6: never listed; a saved 7 becomes AMD's (Store::view).
constexpr int k_fgDropped = 7;
inline bool framegen_is_dlss(int index) { return index >= 2 && index <= 6; }
bool framegen_choice_hidden(int index, const Environment& env);
// Notes/83: on Vulkan only 1 (AMD, experimental) - with AMD's Vulkan DLLs, the exe's support and SDK patch 69.
Availability framegen_choice_available(int index, const Environment& env, bool vulkan = false);
// Notes/83: the game will run on Vulkan (Graphics API, or Proton / Wine's choice).
bool runs_vulkan(const Settings& s, const Environment& env);
// vkfreeze049 (jobs 1454 / 1455 / 1463 / 1466): AMD FSR frame generation on Vulkan can freeze on NVIDIA cards in 0.4.9 -
// kept available, with this warning under the Frame generation row when Vulkan + an NVIDIA card + AMD's are chosen.
extern const char* const k_fgVkNvidiaWarning;
bool fg_vk_nvidia_warning(const Settings& s, const Environment& env);
bool fsr_vulkan(const Environment& env);   // fsr049: FSR upscaling available on Vulkan
bool fsr_vulkan_on(const Settings& s, const Environment& env);   // ...and "FSR on Vulkan (experimental)" is on
// #345: the render scale the game will use (1-4): the chosen one, 1 for 720p / 1080p, and for "Match my monitor" what the
// game picks - the smallest scale covering the monitor (by width with Ultrawide on); 2 when the monitor size is unknown
int expected_render_scale(Game g, const Settings& s, const Environment& env);
// #345 / GitHub #98: Scene resolution mode m (1 Quality, 2 Performance, 3 Ultra performance) has a whole-number scene size
// at render scale `out` - otherwise the upscaler only anti-aliases (DLAA / native AA)
// perf1080 (performance_1080_base): at 3x (4K) Performance fits too - the scene is then the game's own 1920x1080
// (true 1080p), drawn at 1x under a 2x output, a whole-number pair.
inline bool scene_mode_fits(int m, int out, bool perf1080 = false) {
    return (m == 2 && (out == 2 || out == 4 || (out == 3 && perf1080))) || ((m == 1 || m == 3) && out == 3);
}
// Performance at 4K from the true-1080p base: this game's exe can draw 1920x1080 itself (the same conditions as the
// 1080p render resolution - its menus, and the wide picture with Ultrawide on). The launcher then sends 2x of 1080
// lines instead of 3x of 720 for "4K + Performance": the same 3840x2160 output, with a 1920x1080 scene for the upscaler.
bool performance_1080_base(Game g, const Settings& s, const Environment& env);
// 0.6.6 (Shagg 20 Nov; job 2137b: DLSS "Quality" at 1440p cost 1.1 ms of GPU time and lowered nothing): a Scene
// resolution that the render resolution can't divide is greyed in the list, with what would make it work.
inline Availability scene_mode_choice_available(int m, int out, bool perf1080 = false) {
    Availability a;
    if (m < 1 || m > 3 || scene_mode_fits(m, out, perf1080)) return a;
    a.ok = false;
    const std::string at = out == 1 ? "720p / 1080p (1x)" : out == 2 ? "1440p (2x)" : out == 3 ? "4K (3x)" : "5K (4x)";
    a.why = m == 2 ? "Performance draws the scene at half size, which needs render resolution 2x (1440p) or 4x. At " + at +
                         " it would only anti-alias and cost frames."
                   : std::string(m == 1 ? "Quality draws the scene at 2/3 size" : "Ultra performance draws the scene at 1/3 size") +
                         ", which needs render resolution 3x (4K). At " + at + " it would only anti-alias and cost frames." +
                         (out == 2 || out == 4 ? " Performance works here." : "");
    return a;
}
// proton049: under Proton / Wine, a single choice that can't work there, with its one-line reason (always ok on
// Windows): exclusive fullscreen, NVIDIA DLSS without an NVIDIA RTX card, DLSS frame generation.
Availability wine_choice_available(const Option& o, int index, const Environment& env);
// proton049: a line about the row on Linux / Steam Deck (DLSS and NVAPI, HDR, Reflex, the Deck's 40 / 60 fps), shown
// under its description under Proton / Wine; "" on Windows.
std::string wine_note(const Option& o, const Environment& env);
// proton049: the recommended Steam Deck / Proton settings for a game, as (option id, value) - empty on Windows. Vulkan,
// FSR (with "FSR on Vulkan" when this build has it, else SMAA), frame generation off; on a Steam Deck also borderless,
// 1x render resolution (the Deck's 1280x800 screen), a 60 fps limit, Steam Deck button prompts and no keyboard / mouse.
std::vector<std::pair<std::string, double>> proton_recommended(Game g, const Environment& env);
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
// Kind::Text (0.4.8): the value as it may be stored and passed on. PeerIps (GitHub #229): IPv4 addresses only (what the
// runtime's --net_peer_ip reads), separated by ", "; anything else is dropped, duplicates and 0.0.0.0 /
// 255.255.255.255 too, at most 16 addresses. `dropped` (optional) gets how many entries were left out.
// GraphicsCard (0.6.2, GitHub #304) and AudioDevice (GitHub #425): a device's name - control characters and quotes dropped,
// trimmed, at most max_len bytes.
std::string sanitize_text(const Option& o, const std::string& in, int* dropped = nullptr);
// The characters a Kind::Text field accepts while typing (the rest are not entered at all).
bool text_char_ok(const Option& o, unsigned c);

// Monitor list entries (PS1 147).
std::string monitor_label(int index1, int width, int height, bool primary);
// 0.6.2 (GitHub #304): the Graphics card option's saved name is one of env.graphics_cards (same letters, any case).
bool graphics_card_present(const Environment& env, const std::string& name);
// GitHub #425: the Output device option's saved name is one of env.audio_devices (same letters, any case, both as
// sanitize_text keeps them).
bool audio_device_present(const Environment& env, const std::string& name);
// PS1 152: the main graphics card of a list.
std::string main_gpu(const std::vector<std::string>& gpus);

// The remembered settings of all the games (PS1 $s + PerGame).
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
    std::map<std::string, double> per[kGameCount];   // PerGame sets; empty = the game has none yet
    std::map<std::string, double> own[kGameCount];   // GameOwn values
    std::map<std::string, std::string> texts;         // Kind::Text values (Shared only, sanitised)
private:
    void migrate();
};

// 0.2.0: the keyboard keys for keyboard-and-mouse play - each controller input, what it does in Gears, its default.
struct KeyAction { const char *cvar; const char *pad; const char *what; const char *def; };
// GitHub #208: per game - Judgment throws grenades with LB and opens the tactical display with D-pad down (the other
// three the other way round), so its two rows are named and given their default keys (Tab / 4) by what they do there.
const std::vector<KeyAction> &key_actions(Game g = Game::Gears2);
// 0.6.3 (GitHub #316): the key that switches the game window between windowed and borderless full screen - the same in
// all four games (the games' bind_cogbox_fullscreen). Not F11: the cheat keys (and the AO debug keys) have it. Shown on the
// Controls tab; window_key() is what a --bind_cogbox_fullscreen=<key> line in extra-args.txt changes it to (the last line
// wins; "None" = off), else the default. The key text is the games' own: [Alt+][Ctrl+][Shift+]<key name>.
constexpr const char *kWindowKeyDefault = "Alt+Return";
std::string window_key(const Environment &env);
// 0.6.3 (reviews r49 / r50): one value for all games - the Shared options and the text ones (kept once, Store::texts).
// Before a save, each unsaved page's Shared / text values that differ from what the store holds (its edits; compared after
// that game's clamp, as view() shows them - r51) go to every page (clamped for each game), so a change on one page shows
// on the others and committing another page can't put the old value back - and two unsaved pages' edits to different
// options are both kept. On the same option the `prefer` page (the one being saved)
// wins, else the later game. GameOwn / PerGame values are left alone. changed[k] = page k's values changed.
void sync_shared(const Store& st, const Environment* envs, Settings* pages, const bool* dirty, int prefer,
                 bool* changed);   // (all arrays kGameCount long)

// 0.6.3: the Custom controller layout (Controller layout = Custom; CogBox.ini [Pad] Remap=, shared by all four games),
// edited on the Controls tab. The runtime's pad_remap (SDK patch 55, input_system.cpp) reads "src=dst" pairs, comma
// separated, spaces dropped, any case: the controller's src button presses the game's dst button. A button that is only
// a source no longer presses its own; one never named as a source stays itself; two sources may press one dst. There is
// no "nothing": every controller button presses some game button. "sticks=swap" swaps the two sticks. Anything else (an
// unknown name, "a=", "a=b=c") is ignored by the runtime; the editor keeps such text as it was and says so.
enum PadInput { PadA, PadB, PadX, PadY, PadLB, PadRB, PadLT, PadRT, PadLS, PadRS, PadBack, PadStart,
                PadUp, PadDown, PadLeft, PadRight, kPadInputs };
const char* pad_input_token(int i);   // the runtime's name: "a", "lb", "ls", "back", "up" ... (nullptr out of range)
const char* pad_input_label(int i);   // the launcher's: "A", "LB", "Left stick click", "Back", "D-pad up" ...
int pad_input_from_token(const std::string& token);   // any case; -1 = not a name the runtime knows
struct PadMap {
    int to[kPadInputs];                 // to[controller button] = the game button it presses (to[i] == i: unchanged)
    bool sticks_swapped = false;
    std::vector<std::string> unknown;   // parts of the text the runtime ignores, kept as written
    PadMap() { for (int i = 0; i < kPadInputs; ++i) to[i] = i; }
    bool same_layout(const PadMap& o) const;   // same buttons and sticks (unknown parts not compared)
};
// What the launcher hands the runtime (build_args): only letters, digits, '=' and ','.
std::string pad_remap_clean(const std::string& spec);
PadMap parse_pad_remap(const std::string& spec);    // as the runtime reads it (the last pair for a button wins)
std::string format_pad_remap(const PadMap& m);      // only the moved buttons, in pad_input order; "" = the game's own
const char* pad_layout_preset(int layout);          // Controller layout 0-3's text ("" "sticks=swap" "a=b,b=a" ...)
std::vector<int> pad_sources(const PadMap& m, int game_button);   // the controller buttons that press it
// The controller button `pad` now presses game button `game`. If that leaves the game button it pressed before with no
// controller button, the buttons swap: a button that pressed `game` until now takes it over (as the keyboard keys'
// "was taken from"). Returned: what changed besides `pad` itself, for the note under the list.
struct PadAssign {
    bool changed = false;
    int took_from = -1;     // the game button `pad` pressed before
    int swapped = -1;       // the controller button that now presses took_from instead (-1: none)
    bool left_empty = false;   // took_from has no controller button now (no button could take it over)
};
PadAssign pad_assign(PadMap& m, int game, int pad);

// Job 661: draws the keyboard button prompts (key caps over each game's controller-button sheet, 4x the sheet's size) for
// these keys (cvar -> "Key[,Key2]") into <promptDir>\hires-<game>; returns that folder or "" (keycaps.cpp sets it).
using PromptComposer = std::string (*)(const std::string &promptDir, const std::string &game,
                                       const std::map<std::string, std::string> &keys);
extern PromptComposer g_prompt_composer;

// PS1 788-961 (Play): the game's command-line arguments, in the PS1's order. `ver` is the chosen version (see
// effective_version); `notes`, if given, gets the "launcher: ..." lines the PS1 adds to args.txt.
std::vector<std::string> build_args(Game g, Version ver, const Settings& s, const Environment& env,
                                    std::vector<std::string>* notes = nullptr);
// 0.4.9 live settings: for the in-game menu (menu.txt "live=" lines), each option that can change while the game runs,
// mapped to its values that need NO restart, as "<value>:<cvar>=<v>;<cvar>=<v>" - this game's own cvar names. Worked
// out from build_args itself: a value is live when, with only that option changed, the command line differs from this
// play's in the option's live cvars alone; those cvars (or their defaults when build_args leaves one out) are what the
// menu sets. So a live change and the next start always give the cvars the same values.
std::map<std::string, std::vector<std::string>> live_lines(Game g, Version ver, const Settings& s, const Environment& env);
// PS1 966-972: true = do not start (the exe is an AVX2 build and this CPU has no AVX2); `message` = what to say.
bool blocked_by_cpu(Game g, const Environment& env, std::string* message = nullptr);

}  // namespace gears
}  // namespace cogbox
