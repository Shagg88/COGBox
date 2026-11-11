// COGBox - the Gears of War settings. See gears_settings.h; "PS1 nnn" = a line of Gears2-Native's tools\launcher.ps1.
#include "gears_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <locale>
#include <regex>
#include <sstream>
#include <cstring>

namespace cogbox {
namespace gears {

// menu5 (0.5.0): Judgment's Brightness default (the launcher's and the in-game menu's scale, 50..150; gamma = 100 / it)
constexpr int kBrightnessDefault = 75, kBrightnessOldDefault = 65;


namespace {

// Index = Game (Gears 1 last: it was added after the others).
const char* const k_gameNames[kGameCount] = { "Gears of War 2", "Gears of War 3", "Gears of War: Judgment", "Gears of War" };   // PS1 95
const char* const k_gameShort[kGameCount] = { "Gears 2", "Gears 3", "Judgment", "Gears 1" };                               // PS1 96
const char* const k_procNames[kGameCount] = { "gow2", "gow3", "gowj", "gow1" };                                           // PS1 98
// Gears 1 (UE3 2006) has one flat Coalesced.ini; --ue3_ini_edits goes to it when gow1.exe has gow1_ini_edits (can() "iniedit").
const char* const k_engineIni[kGameCount] = { "Xe-GearEngine.ini", "Xbox360-GearEngine.ini", "Xbox360-GearEngine.ini", "Xe-WarEngine.ini" };   // PS1 101

struct Gears1Chapter { const char* map; int point; const char* name; };
const Gears1Chapter k_gears1Chapters[] = {
    { "sp_prison_p", 37, "Act 1 - Time off for good behavior" },
    { "sp_embry_p", 38, "Act 1 - Begin the mission to find Alpha Squad" },
    { "sp_embry_p", 39, "Act 1 - Taking heat in Embry Square" },
    { "sp_embry_p", 40, "Act 1 - Delta Squad divides and conquers in Embry" },
    { "sp_hos_p", 41, "Act 1 - Infiltrate the House of Sovereigns" },
    { "sp_hos_p", 42, "Act 1 - Drop the Hammer on the House of Sovereigns" },
    { "sp_hos_p", 43, "Act 1 - A bloody reunion with Alpha Squad" },
    { "sp_tomb_p", 44, "Act 1 - Berserker bait" },
    { "sp_ephyrastreets_p", 45, "Act 2 - Locust counterattack" },
    { "sp_ephyrastreets_p", 46, "Act 2 - Heavy weapons in close quarters" },
    { "sp_oldephyra_p", 48, "Act 2 - A \"helping\" hand from the Stranded" },
    { "sp_oldephyra_p", 49, "Act 2 - Stay in the light or be consumed" },
    { "sp_oldephyra_p", 50, "Act 2 - Light your way to salvation" },
    { "sp_oldephyra_p", 51, "Act 2 - Shootout at Aspho Gas Station" },
    { "sp_viaducts_p", 52, "Act 2 - Burn Kryll... while burning rubber" },
    { "sp_alamo_p", 69, "Act 2 - Fight alongside the Stranded" },
    { "sp_factory_p", 53, "Act 3 - Enter Lethia Imulsion Facility" },
    { "sp_factoryinterior_p", 54, "Act 3 - Keep your distance from Dark Wretches" },
    { "sp_factoryinterior_p", 55, "Act 3 - Mechanized death ride" },
    { "sp_caves_p", 57, "Act 3 - Traverse the caves before the Kryll come home" },
    { "sp_caves_p", 58, "Act 3 - David versus Goliath" },
    { "sp_caves_p", 59, "Act 3 - More mysteries found" },
    { "sp_eba_p", 60, "Act 4 - Bullies on the playground" },
    { "sp_eba_p", 61, "Act 4 - Enjoy the local flora and fauna" },
    { "SP_EBA_RiverWalk_P", 62, "Act 4 - Surprise encore" },
    { "SP_EBA_RiverWalk_P", 63, "Act 4 - Academy campus uprising" },
    { "sp_adams_p", 64, "Act 4 - Storm the House!" },
    { "sp_adams_p", 65, "Act 4 - Evict the Locust from Adam's House" },
    { "sp_station_p", 66, "Act 5 - Secure the platform and catch the train" },
    { "sp_tyro_p", 67, "Act 5 - All aboard" },
    { "sp_tyro_p", 68, "Act 5 - A dish best served cold" },
};
// PS1 131-140: where Play can start.
std::vector<std::string> start_names(Game g) {
    std::vector<std::string> v{ "Main menu (normal)" };
    if (g == Game::Gears2) {
        for (int i = 1; i <= 29; ++i) v.push_back("Chapter " + std::to_string(i));
    } else if (g == Game::Gears3) {
        const char* n[] = { "Act I - Nightmare (prologue)", "Act I - Raven's Nest 1", "Act I - Raven's Nest 2", "Act II - Hanover 1",
            "Act II - Hanover 2", "Act II - Hanover 3", "Act II - Bridge", "Act II - Wreckage", "Act III - Badlands 1",
            "Act III - Badlands 2", "Act III - Badlands 3", "Act III - Badlands 4", "Act III - Blimp 1", "Act III - Blimp 2",
            "Act III - Fort 1", "Act III - Fort 2", "Act IV - Convoy", "Act IV - Mercy 1", "Act IV - Mercy 2", "Act IV - Char 1",
            "Act IV - Char 2", "Act IV - Char 3", "Act IV - Shipyard 1", "Act IV - Shipyard 2", "Act V - Maelstrom",
            "Act V - Coast 1", "Act V - Coast 2", "Act V - Azura 1", "Act V - Azura 2", "Act V - Tower 1", "Act V - Tower 2" };
        v.insert(v.end(), std::begin(n), std::end(n));
    } else if (g == Game::Judgment) {
        const char* n[] = { "Prologue (museum / trial)", "Sera University", "Onyx Mansion", "Onyx Mansion (part 3)",
            "Halvo Bay military base", "Halvo Bay - to the rooftops", "Rooftops", "Rooftops (part 2)", "Courthouse",
            "Courthouse (transition)", "Courthouse - boss", "Aftermath 1", "Aftermath 2", "Aftermath 2 - oil rig", "Aftermath 3",
            "Aftermath 3 - outro", "Aftermath 4", "Aftermath 5" };
        v.insert(v.end(), std::begin(n), std::end(n));
    }
    else if (g == Game::Gears1) {   // the game's own chapter table (Coalesced.ini [*-ChapN UIDataProvider_WarfareMaps], its
        // SelectMap descriptions): 31 chapters, each a level file plus its start point (job 703b)
        for (const auto& c : k_gears1Chapters) v.push_back(c.name);
    }
    return v;
}
// PS1 135-137
const char* const k_mapsGJ[18] = { "SP_00_Museum_P", "SP_01_University_P", "SP_02_Mansion_P", "SP_02_Mansion_3_P", "SP_03_Island_P",
    "SP_03_to_04_P", "SP_04_Rooftops_P", "SP_04_Rooftops2_P", "SP_05_Courthouse_P", "SP_05_CourthouseTrans_P",
    "SP_05_CourthouseBoss_P", "SPE_E1_P", "SPE_E2_P", "SPE_E2oilrig_P", "SPE_E3_P", "SPE_E3_Outro_P", "SPE_E4_P", "SPE_E5_P" };

const char* const k_aaNames[6] = { "Off", "SMAA", "FSR", "DLSS (NVIDIA RTX)", "XeSS", "SMAA T2x" };   // PS1 150; 5: notes/80 s.7
// Job 638: saved as "FrameCap", in order; the old "Fps" (30 / 60 / 120 / Unlimited) is read once and moved over
// (Store::load_ini). Shagg: 30, 40, 45, 60, 90, 120, 144, 165, 180, Unlimited (list 2; 40 / 45 for the Steam Deck; list 1 had no 40 / 180 -
// files without "FrameCapList=2" are moved over once in Store::load_ini).
const int k_fpsCaps[10] = { 0, 40, 45, 60, 90, 120, 144, 165, 180, -1 };                               // PS1 159
const int k_fpsCapCount = 10;
const char* const k_drawScale[3] = { "1", "1.5", "2" };                                      // PS1 163
const char* const k_shakeValue[3] = { "", "*=0.5", "=0" };   // 0.2.0 (#12): the factor scales the shake - Less halves the game's own, Off is 0                                    // PS1 167
const int k_poolMb[4] = { 0, -1, 256, 320 };                                                 // PS1 171
const int k_winSizes[6][2] = { { 0, 0 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };   // PS1 174
const double k_deadValue[6] = { 0, 0.05, 0.10, 0.15, 0.20, 0.25 };                          // PS1 177

std::string not_yet(int g, const std::string& what = "") {   // PS1 262
    return std::string("Not yet in ") + k_gameShort[g] + (what.empty() ? "" : " (" + what + ")") + ".";
}

bool icase_search(const std::string& text, const char* pattern) {   // PowerShell -match (case-insensitive regex)
    return std::regex_search(text, std::regex(pattern, std::regex::ECMAScript | std::regex::icase));
}

// [double].ToString(InvariantCulture) on .NET Core: the shortest text that reads back as the same double.
std::string fmt_double(double x) {
    char buf[64];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, x);
        for (char* c = buf; *c; ++c) if (*c == ',') *c = '.';
        std::istringstream in(buf); in.imbue(std::locale::classic());
        double back = 0; in >> back;
        if (back == x) break;
    }
    return buf;
}
std::string fmt_bool(bool b) { return b ? "true" : "false"; }   // $x.ToString().ToLower()

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

Option make(const char* id, const char* label, Tab tab, Kind kind, Scope scope, double def, const char* tip, int line) {
    Option o; o.id = id; o.label = label; o.tab = tab; o.kind = kind; o.scope = scope; o.def = def; o.tooltip = tip;
    o.ps1_line = line;
    return o;
}
Option choice(const char* id, const char* label, Tab tab, Scope scope, std::vector<std::string> items, int def, const char* tip, int line) {
    Option o = make(id, label, tab, Kind::Choice, scope, def, tip, line);
    o.choices = std::move(items); o.min = 0; o.max = static_cast<double>(o.choices.size()) - 1;
    return o;
}
Option number(const char* id, const char* label, Tab tab, Kind kind, Scope scope, double mn, double mx, double step, double def,
              const char* tip, int line, int dec = 0) {
    Option o = make(id, label, tab, kind, scope, def, tip, line);
    o.min = mn; o.max = mx; o.step = step; o.decimals = dec;
    return o;
}

std::vector<Option> make_options(Game g) {
    const int gi = static_cast<int>(g);
    std::vector<Option> v;
    auto add = [&](Option o, const char* feature = "", const char* depends = "", bool preset = false) {
        o.feature = feature; o.depends_on = depends; o.in_preset = preset; v.push_back(std::move(o));
    };
    const Scope SH = Scope::Shared, PG = Scope::PerGame, OWN = Scope::GameOwn;

    // ---- Display -------------------------------------------------------------------------------------------------
    // Shagg 29 Oct 14:06: one display mode box (windowed / borderless / exclusive). The id stays "Fullscreen" so saved
    // settings keep working: 0 and 1 mean what they did; the old "Exclusive" box becomes 2 (Store::view).
    add(choice("Fullscreen", "Display mode", Tab::Display, PG,
               { "Windowed", "Borderless fullscreen", "Exclusive fullscreen" }, 1,   // Shagg 11-06: borderless by default
               "Windowed: a normal window (set its size below). Borderless fullscreen: a window without borders that covers "
               "the whole screen (quick alt-tab). Exclusive fullscreen: real fullscreen mode (lower latency on some PCs; "
               "alt-tab is slower; on Steam Deck / Linux this is borderless).", 330));
    add(choice("WindowSize", "Window size", Tab::Display, PG,
               { "Default", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" }, 0,
               "The window's size when not fullscreen. Default = the game's own.", 332), "", "!Fullscreen");
    // Monitor: no PS1 tooltip (59-how-to-play: "Which monitor fullscreen uses"). Entries 1..N: monitor_label().
    add(choice("Monitor", "Monitor", Tab::Display, SH, { "Default (main screen)" }, 0,
               "Which screen the game uses. Default = the main screen.", 334), "", "screens>=2");
    add(make("HDR", "HDR (experimental)", Tab::Display, Kind::Bool, PG, 0,
             "Experimental in all four games - colours and brightness are not checked yet. Needs HDR turned on in Windows "
             "display settings.", 363));   // 0.4.6 (Shagg 4 Nov): HDR experimental
    add(number("PaperWhite", "HDR paper white (nits)", Tab::Display, Kind::Int, PG, 80, 400, 10, 203,
               "How bright normal scenes are. 203 is the reference; raise it in a bright room.", 364), "", "HDR");
    add(number("Peak", "HDR peak (nits)", Tab::Display, Kind::Int, PG, 400, 4000, 100, 1000,
               "Your screen's peak brightness (see its specs or Windows HDR calibration).", 365), "", "HDR");
    // GitHub #185 (Shagg 7 Nov): gamma on the game's display ramp. 100 = the game's own; higher = darker shadows and
    // midtones, highlights kept (Judgment looks brighter than Xbox 360 footage to some players).
    // menu5 (Shagg, RC4-4): the default 65 -> 75 (display-ramp gamma 100 / 75 = 1.333 instead of 1.538: less dark)
    add(number("Brightness", "Brightness (100 = original)", Tab::Display, Kind::Int, PG, 50, 150, 5, kBrightnessDefault,
               "Brightness of shadows and midtones. 75 (the default) is close to the Xbox 360; 100 = the picture as the game draws it here. Lower darkens them, higher lifts them; bright areas "
               "stay about the same.", 0), "gamma");
    add(make("Vsync", "Vsync (no tearing)", Tab::Display, Kind::Bool, SH, 0,
             "Each frame waits for your screen's next refresh, so the picture never tears. With G-Sync / FreeSync the screen "
             "follows the game below its refresh rate. A frame-rate limit above your screen's refresh rate is lowered to the "
             "refresh rate (half of it with frame generation). Off = frames are shown at once (lowest lag, may tear).", 344), "vsync");
    // Frame rate limit: no PS1 tooltip; text from 59-how-to-play ("The game itself asks for 30 fps").
    add(choice("FrameCap", "Frame rate limit", Tab::Display, PG,
               { "30 (original)", "40", "45", "60", "90", "120", "144", "165", "180", "Unlimited" }, 0,
               "The game itself asks for 30 frames per second, as on the console. 40 to 180 lift the limit to that (40 / 45 are for the Steam Deck: its LCD at 40 Hz, half of the OLED's 90 Hz); Unlimited "
               "removes it. With V-Sync on, a limit above your monitor's refresh rate is lowered to the refresh rate. The quality "
               "presets leave it as it is.", 341), "framecap", "", false);   // 0.4.6: not part of the presets
    add(choice("FpsMode", "FPS counter", Tab::Display, OWN, { "Off", "Bar at the top", "Bar at the bottom", "Game's own (column)" }, 0,
               "Bar: one line (FPS, frame time, slowest frame) drawn by the port itself, nothing in the game changed. Game's own: "
               "the game's built-in FPS / frame-time column.", 336));
    // GitHub #121: the port's own notes over the game (shader building / loading) off. Achievement popups have their own
    // setting below (achievements part 2).
    add(make("HideNotes", "Hide on-screen notes", Tab::Display, Kind::Bool, OWN, 0,
             "Draws none of the port's notes over the game: no shader-building note and no shader-loading indicator. "
             "The FPS counter above and achievement popups are separate.", 0));
    // Achievements part 2: one value for every game ([General]). Part 4: on the ACHIEVEMENTS tab.
    add(choice("AchievementStyle", "Popup style", Tab::Achievements, SH, { "Xbox 360", "Xbox", "PlayStation" }, 0,
               "The look of the popup when an achievement unlocks. PlayStation shows a trophy grade worked out from the "
               "gamerpoints; the gamerpoints are always recorded.", 0), "achpopups");
    add(choice("AchievementColour", "Popup colour (Xbox style)", Tab::Achievements, SH,
               { "Dark", "Red", "Green", "Blue", "Purple", "Orange", "Pink", "Teal", "Grey", "Gold" }, 0,
               "The Xbox style popup's colour.", 0), "achpopups");
    add(choice("AchievementPosition", "Popup position", Tab::Achievements, SH,
               { "Default", "Top left", "Top centre", "Top right", "Bottom left", "Bottom centre", "Bottom right" }, 0,
               "Where the popup shows. Default: bottom centre (Xbox 360, Xbox), top right (PlayStation).", 0), "achpopups");
    add(make("AchievementSound", "Popup sound", Tab::Achievements, Kind::Bool, SH, 1,
             "Plays a sound with each popup.", 0), "achpopups");
    add(make("HideAchievementPopups", "Hide achievement popups", Tab::Achievements, Kind::Bool, SH, 0,
             "No popup and no sound when an achievement unlocks. Achievements still unlock and are saved.", 0), "achpopups");
    // GitHub #52 (Gears 3 native mouse look; Gears 2 too since sweep 21)
    // GitHub #125 (SDK patch 60): the Walk key (KEYBOARD KEYS, below) is held or pressed once to switch walking on / off
    add(choice("WalkMode", "Walk key", Tab::Controls, PG, { "Hold to walk", "Press to switch walking on / off" }, 0,
               "With Keyboard and mouse: how the Walk key works (choose the key in KEYBOARD KEYS). Walking moves at half "
               "speed. No key is set at first.", 0), "walkkey");
    if (g == Game::Gears3 || g == Game::Gears2) add(choice("AimTurn", "Mouse turn when aiming", Tab::Controls, OWN, { "Same as moving around", "Game's (slower)" }, 1,
               "Same: the mouse turns the view by the same angle per inch in every camera mode - running, roadie run, cover, "
               "aiming and zoom. Game's (default, GitHub #169): aiming and zoom keep the console game's slower turn. Native mouse look only.", 0));
    // 0.5.0 (Reddit: zoomed aiming too fast): extra mouse speed while aiming and while zoomed through a scope, in % -
    // the games that tell COGBox their camera's field of view (Gears 2 and 3 now; Gears 1 and Judgment when hooked)
    if (g == Game::Gears3 || g == Game::Gears2 || g == Game::Gears1 || g == Game::Judgment) {
        add(number("AimSens", "Aim sensitivity (%)", Tab::Controls, Kind::Int, OWN, 10, 200, 5, 100,
                   "Mouse speed while aiming, as a percentage of the normal speed. 100 = unchanged. Native mouse look only.", 0));
        add(number("ZoomSens", "Zoom sensitivity (%)", Tab::Controls, Kind::Int, OWN, 10, 200, 5, 100,
                   "Mouse speed while zoomed through a scope (Longshot, Hammer of Dawn targeting), as a percentage of the "
                   "normal speed. 100 = unchanged. Native mouse look only.", 0));
    }
    add(make("Ultrawide", "Ultrawide", Tab::Display, Kind::Bool, PG, 0,
             "A wider view that matches an ultrawide monitor (21:9 or 32:9); black bars on other shapes.", 362), "ultrawide");
    // 0.4.9: 4:3 screens (SDK patch 70 --video_mode_widescreen=false): the game is told it is on a 4:3 TV and lays
    // itself out for 4:3 (a 960x720 picture, x the resolution scale), as on the console. Not with Ultrawide.
    add(choice("AspectRatio", "Aspect ratio", Tab::Display, PG, { "Auto", "16:9", "4:3" }, 0,
               "4:3 tells the game it is on an old 4:3 TV, so it makes a 4:3 picture and HUD itself, as on the console. "
               "Auto = 16:9 for now (4:3 is untested: choose it yourself). Not with Ultrawide.", 0), "aspect43", "!Ultrawide");

    // ---- Graphics ------------------------------------------------------------------------------------------------
    add(choice("Preset", "Quality preset", Tab::Graphics, PG, { "Original (as on the console)", "High", "Ultra", "Custom" }, 3,
               "Sets the graphics boxes in one go. Original = the game as on the console. Change any box afterwards and the "
               "preset shows Custom.", 316));
    // Job 521 (Shagg 19:02): Vulkan opened for testing - the play exes have the full Vulkan renderer.
    // Shagg 3 Nov: experimental again in Gears 3 and Judgment (Gears 3: black eyes on Vulkan, fine on DirectX 12)
    // 0.4.7 (Shagg 6 Nov): not experimental any more - the Vulkan texture fix (#202 / #213 / #131) tested in both games
    const bool vkExp = false;
    static const char* const kVkTip =
               "Vulkan is a full alternative to DirectX 12, and the usual choice on Steam Deck / Linux. "
               "\n"
               "On Vulkan, DLSS / XeSS are not available and FSR is experimental (\"FSR on Vulkan (experimental)\"); otherwise SMAA is used, "
               "frame generation is AMD FSR only (experimental), "
               "and Reflex / Anti-Lag are off. If something looks wrong, make a problem report and send it to us.";
    static const char* const kVkTipExp =
               "Experimental in this game - some things still look wrong on Vulkan (in Gears 3, black eyes); DirectX 12 is "
               "the tested renderer.\n"
               "On Vulkan, DLSS / XeSS are not available and FSR is experimental (\"FSR on Vulkan (experimental)\"); otherwise SMAA is used, "
               "frame generation is AMD FSR only (experimental), "
               "and Reflex / Anti-Lag are off. If something looks wrong, make a problem report and send it to us.";
    add(choice("Api", "Graphics API", Tab::Graphics, PG, { "DirectX 12 (recommended)", vkExp ? "Vulkan (experimental)" : "Vulkan" }, 0,
               vkExp ? kVkTipExp : kVkTip, 0));
    // Shagg 13:16: Vulkan is no longer forced under Proton / Wine - this box lets Steam Deck / Linux / CrossOver
    // players run DirectX 12 there (off = Vulkan, which is known to get in game there).
    // Job 643 (Shagg: "the default for SteamOS / Mac should be the currently better API"): was the WineD3D12 box (on =
    // DirectX 12); a saved WineD3D12=1 becomes DirectX 12 here (Store::load_ini).
    add(choice("WineApi", "Graphics API on Proton / Wine", Tab::Graphics, PG,
               { "Automatic", "Vulkan", "DirectX 12 (testing)" }, 0,
               "Steam Deck, Linux and Mac (CrossOver) only. Automatic = the one that works best there now: DirectX 12 "
               "(D3DMetal) on a Mac, where Vulkan can't start, and Vulkan on Steam Deck / Linux, where DirectX 12 is still "
               "being tested. If the game closes or looks wrong, make a problem report and send it to us.", 0));
    // Render resolution: no PS1 tooltip; text from 59-how-to-play's Picture table.
    add(choice("Resolution", "Render resolution", Tab::Graphics, PG,
               { "1x - 720p (original)", "2x - 1440p", "3x - 4K", "4x - 5K (supersampling, heavy)", "Match my monitor (native)", "1080p" }, 0,
               "How much detail the game draws: 1x: 1280x720 as on the console, 2x: 2560x1440, 3x: 3840x2160 (4K), "
               "4x: 5120x2880. Real detail, not an upscale. Match my monitor: the smallest scale that covers the screen, "
               "resampled to its exact size. 1080p: drawn at 720p and upscaled to the screen with sharpening - fast, for "
               "weaker graphics cards; softer than real detail.", 340), "", "", true);
    // GitHub #59: the 1080p mode's RCAS sharpen made text and some textures look wrong under Wine / Proton (Linux,
    // NVIDIA). Automatic = on in Windows, off under Wine; On / Off override it everywhere.
    add(choice("Sharpen", "Upscale sharpening (1080p)", Tab::Graphics, PG, { "Automatic", "On", "Off" }, 0,
               "With the 1080p render resolution: sharpen the upscaled picture. Automatic = on, except on Steam Deck / "
               "Linux (Wine / Proton), where it can make text and some textures look wrong.", 0));
    add(choice("AAMode", "Anti-aliasing", Tab::Graphics, PG, { "Off", "SMAA (recommended)", "FSR (FSR 4 on RX 6000 or newer, FSR 3.1 otherwise)", "DLSS (NVIDIA RTX)", "XeSS (highly experimental)",   /* fsr049 */   /* 0.4.7: DLSS tested clean in G2/G3/GJ */
                 "SMAA T2x (temporal)" }, 1,   /* Shagg 11-06: SMAA by default */
               "SMAA: light, sharp, any GPU. FSR: AMD's temporal anti-aliasing / upscaler for the 3D scene - FSR 4 on RX 6000 or "
               "newer, FSR 3.1 otherwise (any graphics card), DirectX 12 or Vulkan. DLSS: the same on "
               "NVIDIA RTX cards. XeSS: the same, best on Intel Arc cards.\nSMAA T2x: SMAA plus a temporal pass - smoother edges and much less shimmer, at little cost, any graphics card, DirectX 12 or Vulkan. Gears 2 and Gears 3 for now.", 346), "", "", true);
    // Review (fsr049): FSR on Vulkan is opt-in while it is new - without this box, FSR on Vulkan becomes SMAA as before.
    add(make("FsrVulkan", "FSR on Vulkan (experimental)", Tab::Graphics, Kind::Bool, PG, 0,
             "With Vulkan as the Graphics API: use AMD FSR (AMD FSR SDK 2.4 Vulkan upscaler) instead of SMAA when "
             "Anti-aliasing is FSR. New - if something looks wrong, turn it off and make a problem report.", 0));
    add(choice("SceneMode", "Scene resolution", Tab::Graphics, SH,
               { "Native (same as render resolution)", "Quality (scene at 2/3)", "Performance (scene at 1/2)",
                 "Ultra performance (scene at 1/3)" }, 0,
               "With FSR, DLSS or XeSS: draw the 3D scene at a lower resolution and let the upscaler fill in the detail - more "
               "frames per second, a little softer. HUD and menus stay at the full resolution.\nPerformance needs render "
               "resolution 2x (1440p) or 4x; Quality and Ultra performance need 3x (4K).", 351), "scenescale", "AAMode>=2");
    // 0.4.9: the DLSS Super Resolution model (renderer/dlss_upscaler.cpp --gow2_dlss_preset; NGX render-preset hints,
    // the same values as Streamline's sl::DLSSPreset ePresetJ..M in sl_dlss.h 2.14.16). DLSS only; index = the letter.
    add(choice("DlssModel", "DLSS model", Tab::Graphics, PG,
               { "Automatic (NVIDIA's choice for each mode)", "J - transformer (a little less ghosting, more shimmer)",
                 "K - transformer (best all-round quality)", "L - newest transformer (sharpest, heaviest)",
                 "M - newest transformer (sharp, lighter than L)" }, 0,
               "Which NVIDIA DLSS model draws the picture when Anti-aliasing is DLSS. Automatic: NVIDIA's own choice for "
               "each mode - K for DLAA, Quality and Balanced, M for Performance, L for Ultra performance.\n"
               "J and K: the first transformer models - every RTX card (20, 30, 40 and 50 series).\n"
               "L and M: the newest models (DLSS 4.5). They also run on every RTX card, but are made for RTX 40 / 50: on "
               "RTX 20 / 30 they cost clearly more frames per second - try K there if the game gets slower.", 352), "dlssmodel");
    // NVIDIA DLSS frame generation (Streamline, renderer/dlss_framegen.cpp): choices 2-5, listed only when the exe has
    // it and NVIDIA's DLLs are next to it (framegen_choice_hidden); greyed without an RTX 40 / 50 card
    add(choice("FrameGen", g == Game::Gears1 ? "Frame generation (experimental)" : "Frame generation", Tab::Graphics, SH,   // Shagg 7 Nov: experimental only in Gears 1
               { "Off", "AMD FSR frame generation", "NVIDIA DLSS frame generation 2x (RTX 40/50, experimental)",
                 "NVIDIA DLSS frame generation 3x (RTX 50, experimental)", "NVIDIA DLSS frame generation 4x (RTX 50, experimental)",
                 "NVIDIA DLSS frame generation dynamic (RTX 50, experimental)",
                 "NVIDIA DLSS frame generation 6x (RTX 50, experimental)",   /* 0.4.9: DLSS 4.5's 6x, after the others so saved choices keep their meaning */
                 "Intel XeSS frame generation (experimental)" }, 0,   /* 0.4.9 (xefg049): 7, Intel XeSS-FG 2x + XeLL */
               "Shows a generated frame between each two the game draws: smoother motion on a fast monitor, about one frame more "
               "input lag. Works best when the game already runs at 60 fps or more. Any DirectX 12 graphics card. In Gears 1 "
               "(experimental) characters can ghost slightly in dark areas.\nNVIDIA DLSS frame generation "
               "(RTX 40/50 only, experimental): 2x, 3x, 4x or 6x the frames the game draws (3x / 4x / 6x / dynamic: RTX 50; "
               "dynamic picks the multiplier to match your monitor); needs NVIDIA's "
               "Streamline files next to the game. If it can't run, the game uses AMD's instead (or none).\nIntel XeSS frame "
               "generation (experimental, untested): 2x, with Intel's Xe Low Latency. Made for Intel Arc graphics cards; Intel "
               "also allows it on other DirectX 12 cards (NVIDIA RTX 30 / Radeon RX 6000 or newer recommended). If it can't "
               "run, the game uses AMD's instead (or none).", 356), "framegen");
    // Aniso: no PS1 tooltip; text from 59-how-to-play ("16x anisotropic filtering").
    add(make("Aniso", "Sharper textures (16x anisotropic)", Tab::Graphics, Kind::Bool, OWN, g == Game::Gears2 ? 1 : 0,
             "16x anisotropic filtering: textures seen at an angle (floors, walls) stay sharp.", 357), "aniso", "", true);
    add(make("Trilinear", "Smooth texture detail steps (trilinear)", Tab::Graphics, Kind::Bool, PG, 1,
             "Texture detail levels blend instead of showing a line where they step down (the game uses point filtering "
             "between levels).", 358), "trilinear", "", true);
    // 0.4.5 (Shagg 3 Nov, to-do 22: every graphics / display option the same in all four games): the settings Gears 1's
    // 2006 engine doesn't have are in its list too, greyed with the reason (0.3.0 left them out).
    add(make("MipFade", "Sharp textures show at once (fast mip fade)", Tab::Graphics, Kind::Bool, PG, 1,
             "A newly loaded sharper texture shows at once instead of fading in over 0.3-2 seconds.", 359), "mipfade", "", true);
    // GitHub #105: Gears 3's depth of field (its half-size 640-pitch targets) at the render scale, not the console's 720p;
    // 0.4.8: Judgment too (the same 640 targets plus a quarter-size 320 chain, 185-rdc.log)
    add(make("DofScale", "Depth of field at the render resolution", Tab::Graphics, Kind::Bool, PG, 0,
             "Gears 3 and Judgment: their depth of field (the blur in front of and behind what you look at, used all "
             "the time in Judgment, its title screen too) is drawn at the render resolution instead of the console's "
             "half or quarter of 720p enlarged. Only when the render resolution is above 720p.", 0), "dofscale");
    add(choice("Pool", "Texture memory", Tab::Graphics, PG,
               { "Game's own (smallest)", "Standard (196 MB / Gears 3 200 MB)", "Large (256 MB)", "Larger (320 MB, not long-tested)" }, 1,
               "The texture streaming pool. Bigger = fewer blurry textures waiting to sharpen. The larger sizes leave less memory "
               "for the rest of the game.", 360), "pool", "", true);
    add(choice("DrawDist", "Draw distance", Tab::Graphics, PG, { "Original", "Far (1.5x)", "Farthest (2x)" }, 0,
               "How far away detail and objects are still drawn.", 361), "drawdist", "", true);
    // GitHub #113: character / weapon mesh detail. UE3's [SystemSettings] SkeletalMeshLODBias (0 = the game's own);
    // negative keeps the more detailed versions (hair, faces, armour) to a greater distance. Not in a preset: untested.
    add(choice("MeshLod", "Character detail (LOD)", Tab::Graphics, PG, { "Original", "Higher", "Highest" }, 0,
               "Keeps characters' and weapons' detailed models (hair, faces, armour) at a greater distance, for high "
               "resolutions. Costs some GPU time in crowded scenes. Original = the game's own.", 0), "inifx");
    // Motion blur: no PS1 tooltip.
    // GitHub #179: Gears 1 through Canary's TU5 patches (illusion, boma) - gow1_no_motion_blur / gow1_no_gaussian_blur
    add(make("MotionBlur", "Motion blur", Tab::Graphics, Kind::Bool, PG, 1, "The blur of fast movement (the game's own setting).", 370),
        "blurfx", "", true);
    add(make("DoF", "Depth of field", Tab::Graphics, Kind::Bool, PG, 1,
             "The blur of distant / out-of-focus areas (cutscenes, aiming).", 371), "doffx", "", true);
    // Canary game-patches (boma): Bloom off is the same GSystemSettings field the patch zeroes (G2 TU6 +56, G3 TU6 /
    // Judgment TU4 +72 - next to MotionBlur / DepthOfField, which the [SystemSettings] edits already set), so it goes
    // through the game's own [SystemSettings] Bloom. Film grain: Judgment's post-process amount (gowj_no_film_grain).
    add(make("Bloom", "Bloom", Tab::Graphics, Kind::Bool, PG, 1,
             "The glow around bright lights and surfaces (the game's own setting). Off: no glow.", 0), "bloom");
    add(make("FilmGrain", "Film grain", Tab::Graphics, Kind::Bool, PG, 1,
             "Judgment's film grain noise over the picture. Off: a clean picture.", 0), "filmgrain");
    // GitHub #102 (Shagg): no Lens flares option any more - the launcher decides per game at launch (below): on where the
    // game's occlusion queries are answered (Real occlusion, DirectX 12, not under Wine / Proton), off where they can't
    // be, since every flare would show through the scenery there.
    // Notes/80 (Gears2-Native): XeGTAO screen-space ambient occlusion in place of the game's own (Gears 2), or added
    // where a level has none (Gears 3 s.9: opt-in there, default "Game's own").
    add(choice("AO", "Ambient occlusion", Tab::Graphics, PG, { "Off", "Game's own", "XeGTAO (high, highly experimental)", "XeGTAO (ultra, highly experimental)", "CACAO (experimental)" },
                        1,   // Game's own: XeGTAO is opt-in in every game
               "The soft shadowing in corners, creases and where things touch. XeGTAO: a modern full-resolution version "
               "(Intel's XeGTAO) in place of the game's own low-resolution one - exact contact shadows instead of blobs, "
               "characters included. Highly experimental (Shagg 3 Nov): in our tests it cost about 5 ms a frame, roughly halving a "
               "high frame rate - being worked on. Ultra looks in more directions and catches more small details, for a "
               "little more. Game's own: the original. Off: none.", 0), "ao");
    // Notes/80 s.16 (Gears2-Native): PCSS for Gears 3's dynamic shadows (--gow2_pcss), Gears 3 only, opt-in.
    add(choice("SoftShadows", "Soft shadows", Tab::Graphics, PG, { "Game's own", "PCSS High (experimental)", "PCSS Ultra (experimental)" }, 0,
               "Contact-hardening soft shadows (PCSS) for the dynamic shadows of characters and moving objects: sharp where a "
               "shadow touches what casts it, softer further away, instead of the game's same small blur everywhere. Ultra "
               "takes more samples and draws the shadow maps at the render resolution for finer edges. Experimental: about "
               "0.4 ms (High) / 0.7 ms (Ultra) a frame at 4K on an RTX 5090 in our tests, more on slower cards. "
               "Game's own: the original.", 0), "pcss");
    add(choice("Shake", "Camera shake (roadie run)", Tab::Graphics, PG, { "Original", "Less", "Off" }, 0,
               "The camera shake while roadie-running. Less halves the game's own shake; Off removes it.", 373), "iniedit", "", true);
    add(number("Fov", "Field of view (%)", Tab::Graphics, Kind::Int, PG, 80, 130, 5, 100,
               "100 = the original. Widens (or narrows) every camera view by this much.", 374), "iniedit", "", true);
    // PR #43 (legoliamneeson): strict occlusion queries (--gow2_renderer_queries=strict), off by default in every game.
    // Per game, Gears 3 and Judgment only (where it was tried: walls hiding light flares); not in the preset.
    // Job 620 (PR #43 bbb7cee: batched result copies, early submits, wake on GPU completion - Judgment Museum 93 fps
    // with it vs 94 off, Gears 3 Gridlock steadier): on by default in Gears 3 and Judgment, where light showed through
    // walls (GitHub #30, #41). Gears 2 keeps it off by default, as in the PR.
    // GitHub #75 (report 28b2db29): Gears 2 shows flares through walls for the same reason (every ZPass query answered
    // with the fake count 1000). Now offered and on by default there too, as in Gears 3 / Judgment. If distant surfaces
    // flicker (jobs 139-140, the old late-answer path), turn it off; Lens Flares off is the other workaround.
    // 0.4.5 (Shagg 3 Nov, to-do 23): Real occlusion is no longer a player setting - the launcher sends a fixed value per
    // game at launch (real_occlusion() below): on in Gears 2, Gears 3 and Judgment on DirectX 12 (not under Wine /
    // Proton), off in Gears 1 (froze its title screen) and wherever the answers can't be had.

    // ---- Controls ------------------------------------------------------------------------------------------------
    add(make("Mnk", "Keyboard and mouse (Gears layout)", Tab::Controls, Kind::Bool, SH, 0,
             "WASD move, mouse look, left click fire, right click aim, Space cover/roll, F melee, E use, Q look at, R reload, "
             "Tab Tac/Com, 1-4 weapons, middle click / Z zoom, C left-stick click, Esc / Enter pause, Backspace back. A "
             "controller still works.", 378));
    // GitHub #104: keep the controller's button prompts with Keyboard and mouse on (Steam Input gyro players)
    add(make("PadPrompts", "Controller button prompts", Tab::Controls, Kind::Bool, SH, 0,
             "With Keyboard and mouse on, keep the game's controller button prompts instead of the key caps "
             "(for example when Steam Input sends a controller as keyboard and mouse).", 0));
    // GitHub #249: a manual button-prompt style (the runtime's --gow_button_prompts; auto could pick PlayStation for an
    // Xbox pad when a Sony device was also connected).
    add(choice("PromptStyle", "Button prompt style", Tab::Controls, SH,
               { "Automatic", "Xbox", "PlayStation", "Nintendo Switch", "Steam Deck" }, 0,
               "Which controller's buttons the game shows. Automatic follows the controller in use.", 0));
    // 0.2.0 (GitHub #2): up to 20 (was 5) - the mouse drives the right stick, so slow moves sat inside the game's stick
    // dead zone at 5. True mouse look (no stick limits) is planned.
    // 0.2.1: native mouse look - the value is the multiplier on the mouse counts (Shagg 15:59: 1.0 far too fast, 0.5
    // better at 1600 DPI); the old right-stick way gets twice it (its old default 1.0 at 0.5)
    // GitHub #195: steps of 0.01 (were 0.05) for fine tuning; the slider moves one step per key / pad press
    add(number("Sens", "Mouse sensitivity", Tab::Controls, Kind::Float, SH, 0.01, 2, 0.01, 0.5,
               "How fast the mouse turns the camera. 0.5 is a good start; lower it for a high-DPI mouse, raise it for a "
               "low-DPI one.", 381, 2));
    add(choice("DeadZone", "Extra stick dead zone", Tab::Controls, SH, { "Game's own", "5%", "10%", "15%", "20%", "25%" }, 0,
               "Added on top of the game's own dead zone, for worn sticks that drift. Game's own = nothing added.", 382));
    // GitHub #95: a round (radial) dead zone on both sticks, rescaled from its edge - Gears 3's feel for Gears 1 and 2.
    // SDK patch 55 (pad_radial_deadzone). In Gears 1 and 2 the game's own per-axis dead zone on the look binding is
    // taken off at the same time (below), so the round one is the one you feel.
    add(choice("RadialDZ", "Radial stick dead zone", Tab::Controls, SH, { "Game's own", "8%", "12%", "16%", "20%", "25%" }, 0,
               "A round dead zone on both sticks (measured from the centre, not per axis), so small diagonal aim moves "
               "are not swallowed. Gears 1 and 2's own square dead zone on the camera is removed with it. Game's own = "
               "unchanged.", 0), "padlayout");
    // GitHub #97: controller layouts (SDK patch 55, pad_remap). Custom = CogBox.ini [Pad] Remap=, e.g. "a=b,b=a".
    add(choice("PadLayout", "Controller layout", Tab::Controls, SH,
               { "Game's own", "Southpaw (sticks swapped)", "A and B swapped", "Bumpers and triggers swapped",
                 "Custom (CogBox.ini [Pad] Remap=)" }, 0,
               "Moves the controller's buttons and sticks before the game sees them, so the game's own button prompts "
               "keep showing the original buttons. Custom: write src=dst pairs (a b x y lb rb lt rt ls rs back start up "
               "down left right, or sticks=swap) in CogBox.ini under [Pad] Remap=.", 0), "padlayout");
    add(make("Vibration", "Controller vibration", Tab::Controls, Kind::Bool, SH, 1, "The controller's rumble.", 383));   // no PS1 tooltip
    add(make("LowLag", "Lower input lag", Tab::Controls, Kind::Bool, SH, 1,
             "Fewer frames waiting in line between the game and your screen, so the controller feels quicker. May cost a few "
             "fps on a busy graphics card; untick to compare.", 385), "lowlag");


    // ---- Gameplay ------------------------------------------------------------------------------------------------
    // 0.3.0 (Shagg 13:43): no "Start at" chapter option in any game - a level opened by its URL skips the game's own
    // chapter start (checkpoint, scripting), so a chapter could look broken that plays fine from the game's Chapter
    // Select. A player's older saved "Start" value is ignored (below).
    add(choice("Version", "Game version", Tab::Gameplay, OWN, { "Title Update (latest)", "Disc (original release)" },
               0,   // COGBox (Shagg 19:46): the PGO build is the default - Gears 2's and Gears 3's title-update builds
                    // (the PS1 has G3Version = 1, the disc; Gears 3's chat built both its exes with PGO, job 143)
               "Title updates are now required: every game runs with its last patch from Epic (Gears 2 and 3: TU6, Judgment: "
               "TU4, Gears 1: TU5). Disc-only versions are no longer supported.", 324), "version");
    // cg050 / Shagg 10 Nov: no "Faster game code" option - the faster build always runs, and the launcher falls back to
    // the -normal one by itself after the faster one crashes twice in a row (play.cpp use_normal_code)
    add(choice("Players", "Players", Tab::Gameplay, PG, { "1", "2 (split-screen)", "3 (split-screen)", "4 (split-screen)" }, 0,
               "Local split-screen co-op, one controller each. In the game, choose Campaign > Play Split-Screen; player 2 is "
               "signed in automatically. If a controller shows up twice, turn off Steam Input or DS4Windows.", 325), "players");
    // GitHub #136 (ebercr): two-player split side by side. The engine's own viewport setting, sent as a console command
    // once player 1 exists (Gears 3's --ue3_exec); the reporter's Back-button binding showed the game honours it.
    add(choice("SplitLayout", "Two-player split", Tab::Gameplay, PG, { "Top and bottom (game's)", "Side by side" }, 0,
               "Experimental - how the screen is split for 2 players. Side by side suits ultrawide monitors. 3 and 4 players keep "
               "the game's own split. Gears 2: side by side from the start.", 325), "splitlayout");
    // 0.3.5 (GitHub #72, SDK patch 49b): System Link over a LAN or a LAN VPN (Radmin VPN, ZeroTier, Hamachi). Off = the
    // runtime behaves exactly as before ("No Network Connection").
    add(make("SystemLink", "System Link - LAN play (experimental)", Tab::Gameplay, Kind::Bool, PG, 0,
             "Experimental - lets the game's System Link find and join games on other PCs on your network (or on a LAN VPN "
             "such as Radmin VPN, ZeroTier or Hamachi; not internet VPNs like NordVPN). Title Update versions only. Gears of War "
             "and Gears 2 co-op worked in testing; other modes and the other games are less tested. Voice chat is not supported yet. Windows may ask about the firewall: "
             "allow Private networks.", 325), "syslink");
    // 0.4.8 (GitHub #229, clovis-junior): the other players' addresses, as the reporter's extra-args.txt line
    // --net_peer_ip=... did (SDK patch 49b / 50) - typed here instead. IPv4 only (Radmin VPN 26.x, ZeroTier, Hamachi).
    {
        Option o = make("PeerIps", "Friends' IP addresses", Tab::Gameplay, Kind::Text, SH, 0,
                        "Optional - for System Link over a VPN such as Radmin VPN when the other player's game doesn't show "
                        "up. Type the OTHER players' IPv4 addresses (Radmin VPN shows them, e.g. 26.10.20.30), commas between "
                        "several. With 3 or more PCs every player lists all the others. Empty = find games on the network "
                        "as usual.", 325);
        o.max_len = 255;
        add(std::move(o), "syslinkpeers");
    }
    add(make("SkipLogos", "Skip the intro logos", Tab::Gameplay, Kind::Bool, PG, 0,
             "Starts at the title screen without the publisher / studio logo films. The loading film and cutscenes still play.", 326),
        "skiplogos");
    // insane050 (Shagg, 0.5.0): Insane is unlocked from the start in all four games; this setting (off) brings back the
    // original rule. GitHub #174 (Flippage's Gears 3 recomp, 26d6448): the menus see Insane unlocked; the profile is untouched
    add(make("OriginalUnlocks", "Original unlock rules", Tab::Gameplay, Kind::Bool, OWN, 0,
             "On: Insane difficulty unlocks only after you finish the campaign, as in the original game. Off: Insane can be "
             "chosen from the start. Achievements work as usual.", 0),
        "originalunlocks");
    // GitHub #89 (francoissubke-blip's tip): bots in local Horde
    add(make("HordeBots", "Horde bots", Tab::Gameplay, Kind::Bool, OWN, 0,
             "Local Horde: press Back / View when the Horde scoreboard first shows to add two bots (press again for two more). "
             "They respawn every wave. Turn it off for campaign and versus: Back also turns on the game's cheat flag.", 0),
        "hordebots");
    add(make("Cheats", "Cheats (hotkeys F5-F11)", Tab::Gameplay, Kind::Bool, SH, 0,
             "The developers' cheat keys (Gears 3 chat, its notes/25): F5-F11 in game. Achievements and saves are your own "
             "business.", 327), "cheats");
    add(make("Mutators", "Unlock all mutators", Tab::Gameplay, Kind::Bool, SH, 0,
             "Every mutator (gameplay modifier) available from the start. Writes nothing to your profile.", 328), "mutators");
    // Shagg 31 Oct: the level select menu is no longer offered (the port's --gowj_level_select stays for testing)
    // 0.2.0 (GitHub #4): the game's own language - the runtime's user_language (Xbox language IDs, issue #4's table)
    add(choice("Lang", "Game language", Tab::Gameplay, PG,
               { "Default (English)", "English (US)", "English (UK)", "French", "German", "Italian", "Spanish (Spain)",
                 "Spanish (Latin America)", "Portuguese (Brazil)", "Polish", "Russian", "Czech", "Hungarian", "Dutch",
                 "Danish", "Finnish", "Norwegian", "Swedish", "Japanese", "Korean", "Chinese (Traditional)",
                 "Chinese (Simplified)" }, 0,
               "The language of the game's own text and voices - every language the US and European releases have. Only "
               "the languages on your copy of the game are there; the others fall back to English. European copies have "
               "the most. Takes effect when the game starts.", 0), "lang");
    // GitHub #87: voices in another language than the text (layer 2 --ue3_voice_language: the game's localised packages
    // *_LOC_<lang>.xxx are served from the voice language's twins). Only languages whose packages are on the copy work.
    add(choice("VoiceLang", "Voice language", Tab::Gameplay, PG,
               { "Same as the game language", "English", "French", "German", "Italian", "Spanish (Spain)",
                 "Spanish (Latin America)", "Polish", "Russian", "Japanese" }, 0,
               "The language of the spoken dialogue, separately from the menus and text (Game language). Needs that "
               "language's voice files on your copy (European copies have English, French, German, Italian, Spanish). "
               "Subtitles stored with the voices may follow them.", 0), "voicelang");
    add(make("Mute", "Mute when the game is not the active window", Tab::Gameplay, Kind::Bool, SH, 0,
             "Silences the game while another window is in front; the sound comes back when you click back into the game.", 386),
        "mute");   // the only sound option, so it lives here (no Audio tab)
    (void)gi;
    return v;
}

double clamp_value(const Option& o, double x, const Environment* env) {
    switch (o.kind) {
    case Kind::Bool: return x != 0 ? 1 : 0;
    case Kind::Choice: {
        double mx = o.max;
        if (o.id == "Monitor" && env) mx = std::max(0, env->screen_count);   // PS1 147, 300: 1 + N entries
        return std::max(0.0, std::min(mx, std::nearbyint(x)));
    }
    case Kind::Int: return std::max(o.min, std::min(o.max, std::nearbyint(x)));
    case Kind::Float: return std::max(o.min, std::min(o.max, x));
    case Kind::Text: return 0;   // the text is in Settings::t
    }
    return x;
}

bool cap_exe(const Environment& e, const std::string& n) {
    // 0.3.0 (Shagg 14:02, Gears 1's Graphics tab greyed): the compiler builds some short option names inside the code a
    // few letters at a time ("gow1_ani" ...), so a text search misses them. Every Gears 1 build COGBox ships has these.
    static const char* const k_gears1_has[] = { "gow1_aniso16", "gow1_trilinear", "gow1_ini_edits", "gow1_skip_logos",
                                                "gow1_texture_pool_mb", "gow1_max_fps" };
    for (const char* h : k_gears1_has)
        if (n == h && e.cap.exe_has && e.cap.exe_has("gow1_tiling_off")) return true;   // any Gears 1 exe since job 697
    // 0.3.0 (job 733): --gow2_aspect / --gowj_aspect are short enough to be split the same way (job 730's gow2.exe: no
    // "gow2_aspect" text) - their description, a long literal, is searched instead
    if ((n == "gow2_aspect" || n == "gowj_aspect") && e.cap.exe_has && e.cap.exe_has("the shape. Empty = 21:9")) return true;
    return e.cap.exe_has && e.cap.exe_has(n);
}
bool cap_rt(const Environment& e, const std::string& n) { return e.cap.runtime_has && e.cap.runtime_has(n); }
bool cap_file(const Environment& e, const std::string& n) { return e.cap.file_next_to_exe && e.cap.file_next_to_exe(n); }
}  // namespace (rel049: fsr_vulkan / fsr_vulkan_on are declared in gears_settings.h and used by page.cpp)
// fsr049: FSR upscaling on Vulkan - the exe has the AMD FSR SDK 2.4 Vulkan upscaler (its switch's description, a long
// literal, is searched: short names can be split by the compiler) and AMD's Vulkan loader + upscaler DLLs are there.
bool fsr_vulkan(const Environment& e) {
    return cap_exe(e, "AMD FSR SDK 2.4 Vulkan upscaler") && cap_file(e, "amd_fidelityfx_loader_vk.dll") &&
           cap_file(e, "amd_fidelityfx_upscaler_vk.dll");
}
// Review: and the player turned "FSR on Vulkan (experimental)" on (--gow2_fsr_vulkan defaults to false)
bool fsr_vulkan_on(const Settings& s, const Environment& e) { return s.b("FsrVulkan") && fsr_vulkan(e); }
namespace {

std::vector<std::string> gpu_list(const Environment& env) {
    if (env.gpus.empty() && !env.gpu_name.empty()) return { env.gpu_name };
    return env.gpus;
}
std::string gpu_main(const Environment& env) { return env.gpu_name.empty() ? main_gpu(env.gpus) : env.gpu_name; }

const char* section_name(int g) { return g == 0 ? "Gears2" : g == 1 ? "Gears3" : g == 2 ? "Judgment" : "Gears1"; }
int section_game(const std::string& s) {
    for (int g = 0; g < kGameCount; ++g) if (s == section_name(g)) return g;
    return -1;
}

// Gears 1's disc has Coalesced.<lang> for chi deu esm esn fra int ita jpn kor (Gears1-Native notes/01-release): the
// Lang choices it can show (indices into the Lang list; 20 = Chinese (Traditional) for "chi" - the 2006 Asian release).
bool gears1_lang(int idx) {
    switch (idx) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 18: case 19: case 20: return true;
    default: return false;
    }
}

}  // namespace

// NVIDIA DLSS frame generation: the exe has --gow2_dlss_fg_mode and NVIDIA's Streamline files are next to it (shipped
// with COGBox since 0.4.8)
bool dlss_fg_present(const Environment& env) {
    if (!cap_exe(env, "gow2_dlss_fg_mode") && !cap_exe(env, "NVIDIA DLSS frame generation multiplier")) return false;
    static const char* const k_files[] = { "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll",
                                           "sl.pcl.dll", "nvngx_dlssg.dll" };
    for (const char* f : k_files) if (!cap_file(env, f)) return false;
    return true;
}

// 0.4.9 (xefg049): Intel XeSS frame generation - the exe was built with it (a log line only GN_HAVE_XEFG builds have)
// and Intel's two DLLs are next to it (the build copies them from the XeSS 3 SDK)
bool xefg_present(const Environment& env) {
    return cap_exe(env, "XeSS-FG swap chain made") && cap_file(env, "libxess_fg.dll") && cap_file(env, "libxell.dll");
}

bool framegen_choice_hidden(int index, const Environment& env) {
    if (index == k_fgXefg) return !xefg_present(env);
    return framegen_is_dlss(index) && !dlss_fg_present(env);
}

// Notes/83: the game runs on Vulkan - chosen, or Proton / Wine's Automatic / Vulkan (as in the arguments below)
bool runs_vulkan(const Settings& s, const Environment& env) {
    const int wapi = s.i("WineApi");
    const bool wineDx = env.wine && s.i("Api") == 0 && (wapi == 2 || (wapi == 0 && env.mac));
    return s.i("Api") == 1 || (env.wine && !wineDx);
}

// vkwarn2-049: Shagg's RC2-7 (3 runs, NVIDIA overlay off) did not freeze; with it on every run froze (jobs 1463-1471).
// The launcher can't see the game's process, and the overlay's on / off setting has no documented place to read, so
// the warning shows on every Vulkan + NVIDIA + AMD frame generation choice; the game log says whether the hook loaded.
const char* const k_fgVkNvidiaWarning =
    "Can freeze on NVIDIA cards with Vulkan while the NVIDIA overlay (NVIDIA App / Alt+Z) is on. Turn the overlay off, or use DirectX 12 for frame generation.";
bool fg_vk_nvidia_warning(const Settings& s, const Environment& env) {
    return s.i("FrameGen") == 1 && runs_vulkan(s, env) && icase_search(gpu_main(env), "NVIDIA|GeForce|Quadro");
}

Availability framegen_choice_available(int index, const Environment& env, bool vulkan) {
    Availability a;
    if (vulkan) {   // notes/83: AMD FSR frame generation only (DLSS-G and XeSS-FG are DirectX 12 only); experimental
        if (index >= 2) { a.ok = false; a.why = "DirectX 12 only."; }
        else if (index == 1 && (!cap_file(env, "amd_fidelityfx_framegeneration_vk.dll") ||
                                !cap_file(env, "amd_fidelityfx_loader_vk.dll"))) {
            a.ok = false;
            a.why = "This build has no AMD frame generation files for Vulkan.";
        } else if (index == 1 && !cap_exe(env, "Vulkan: AMD frame-generation swap chain made")) {
            a.ok = false;
            a.why = "On Vulkan this needs the game's next build.";
        } else if (index == 1 && !cap_rt(env, "SDK patch 69")) {
            a.ok = false;
            a.why = "On Vulkan this needs the next runtime (SDK patch 69).";
        }
        return a;
    }
    if (index == 1 && !cap_file(env, "amd_fidelityfx_framegeneration_dx12.dll")) {
        a.ok = false;
        a.why = "This build has no AMD frame generation files.";
    } else if (index == k_fgXefg) {
        // Intel: XeSS-FG runs on Intel Arc (discrete and integrated) and, since SDK 2.1.1, on other vendors' cards with
        // Shader Model 6.4 (the runtime checks that and falls back). Older Intel graphics (UHD / Iris Xe) is not listed
        // by Intel - greyed. Untested by us on every card (no Intel GPU here): experimental.
        const std::string gpu = gpu_main(env);
        if (!xefg_present(env)) { a.ok = false; a.why = "This build has no Intel XeSS frame generation files."; }
        else if (env.wine) { a.ok = false; a.why = "Not under Proton / Wine."; }
        else if (icase_search(gpu, "Intel") && !icase_search(gpu, "Arc")) { a.ok = false; a.why = "Needs an Intel Arc graphics card (or another make's DirectX 12 card)."; }
    } else if (framegen_is_dlss(index)) {
        const std::string gpu = gpu_main(env);
        const bool rtx50 = icase_search(gpu, "RTX ?50[0-9]0|RTX PRO [0-9]+ Blackwell");
        const bool rtx40 = icase_search(gpu, "RTX ?40[0-9]0|RTX [0-9]+ Ada");
        // Shagg 7 Nov: the third-party MFG Unlocker (github.com/dashdogy/MFG-Unlocker, its DLL beside the game as
        // dxgi.dll) runs DLSS frame generation on RTX 20 / 30 - tested 2x / 3x on an RTX 3080 Ti. Unofficial.
        // 0.4.8 AMD / SteamOS review: a dxgi.dll beside the game is also ReShade, Special K or a DXVK copy, so only an
        // NVIDIA GeForce RTX name on Windows proper counts (under Wine frame generation is off anyway).
        // 0.4.8 launcher review: and only the unlocker's own DLL - its version resource says ProductName "MFG Unlocker"
        // (source/native/single_version.rc upstream, since v1.5.0); any other dxgi.dll keeps the RTX 40 / 50 rules.
        const bool unlocker = !env.wine && env.cap.file_product_has && env.cap.file_product_has("dxgi.dll", "MFG Unlocker") &&
                              icase_search(gpu, "NVIDIA|GeForce") && icase_search(gpu, "RTX");
        if (!dlss_fg_present(env)) { a.ok = false; a.why = "Needs NVIDIA's Streamline files next to the game."; }
        else if (unlocker) { /* every multiplier: the unlocker reports what the card can do */ }
        else if (index == 2 && !rtx40 && !rtx50) { a.ok = false; a.why = "Needs an NVIDIA RTX 40 or 50 series graphics card."; }
        else if (index >= 3 && !rtx50) { a.ok = false; a.why = "Needs an NVIDIA RTX 50 series graphics card."; }
    }
    return a;
}

// ---- The model ------------------------------------------------------------------------------------------------------
const std::vector<Option>& options(Game g) {
    static const std::vector<Option> all[kGameCount] = { make_options(Game::Gears2), make_options(Game::Gears3), make_options(Game::Judgment),
                                                         make_options(Game::Gears1) };
    return all[static_cast<int>(g)];
}
const Option* find_option(Game g, const std::string& id) {
    for (const Option& o : options(g)) if (o.id == id) return &o;
    return nullptr;
}
const char* tab_name(Tab t) {
    switch (t) {
    case Tab::Display: return "Display";
    case Tab::Graphics: return "Graphics";
    case Tab::Controls: return "Controls";
    case Tab::Gameplay: return "Gameplay";
    case Tab::Achievements: return "Achievements";   // achievements part 4
    }
    return "?";
}

// PS1 263-285
Availability can(Game g, const std::string& f, Version ver, const Environment& env) {
    const int gi = static_cast<int>(g);
    auto no = [](const std::string& why) { Availability a; a.ok = false; a.why = why; return a; };
    if (f == "version") {
        // Title updates required (0.4.0): the disc-only version is no longer supported in any game
        if (g == Game::Gears2 || g == Game::Gears3) return no("Title updates are now required - this game always runs with its Title Update 6.");
        else if (g == Game::Gears1) return no("Gears 1 always runs with its Title Update 5.");
        else return no("Judgment always runs with its Title Update 4.");
    } else if (f == "start" || f == "prebuild") {
        if ((g == Game::Judgment || g == Game::Gears1) && start_names(g).size() < 2)
            return no(not_yet(gi, f == "start" ? "its chapter list is coming" : "needs its chapter list"));
    } else if (f == "achpopups") {   // achievements part 2: the text is in cogbox_ach_style's description
        if (!cap_exe(env, "COGBox achievement popups")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "gamefps") {
        if (g == Game::Judgment || g == Game::Gears1) return no(not_yet(gi, "game's own counter"));
    } else if (f == "ultrawide") {   // 0.2.0: Judgment too (--gowj_ultrawide, a new build); Gears 1 when gow1.exe has it
        if (g == Game::Judgment && !cap_exe(env, "gowj_ultrawide")) return no(not_yet(2));
        if (g == Game::Gears1 && !cap_exe(env, "gow1_ultrawide")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "aspect43") {   // 0.4.9: SDK patch 70; Gears 1's video-mode code is not checked yet
        if (g == Game::Gears1) return no(not_yet(gi, "not checked yet"));
        if (!cap_rt(env, "video_mode_widescreen")) return no(not_yet(gi, "needs the next runtime (SDK patch 70)"));
    } else if (f == "cheats" || f == "mutators") {
        if (g != Game::Gears3) return no(not_yet(gi));
        if (ver == Version::TitleUpdate && env.update_ready)
            return no("Gears 3: not with Title Update 6 yet (it shows a dirty-disc error with it).");
    } else if (f == "splitlayout") {   // GitHub #136: needs the port's --ue3_exec (Gears 3); #237: Gears 2 binds it to Y
        if (g == Game::Gears2) {
            if (!cap_exe(env, "gow2_split_vertical")) return no(not_yet(gi, "needs its next build"));
        } else {
            if (g != Game::Gears3) return no(not_yet(gi, "needs its console-command support"));
            if (!cap_exe(env, "ue3_exec")) return no(not_yet(gi, "needs its next build"));
        }
    } else if (f == "levelsel") {
        if (g != Game::Judgment) return no(not_yet(gi));
    } else if (f == "bloom") {   // Canary: Gears 2 / 3 / Judgment; Gears 1's bloom shares its blur (DoF option there)
        if (g == Game::Gears1) return no("Gears 1: its bloom is drawn with the depth-of-field blur - Depth of field off softens it.");
    } else if (f == "gamma") {   // GitHub #185 (Shagg 7 Nov): Judgment only
        if (g != Game::Judgment) return no("Judgment only.");
        if (!cap_exe(env, "gow2_present_gamma")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "filmgrain") {
        if (g != Game::Judgment) return no("Judgment only.");
        if (!cap_exe(env, "gowj_no_film_grain")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "blurfx" || f == "doffx") {   // GitHub #179: Gears 1 by Canary's TU5 patches, the others by ini
        if (g == Game::Gears1 && !cap_exe(env, f == "blurfx" ? "gow1_no_motion_blur" : "gow1_no_gaussian_blur"))
            return no(not_yet(gi, "needs its next build"));
    } else if (f == "inifx") {   // motion blur, depth of field, lens flares: [SystemSettings] switches
        if (g == Game::Gears1) return no("Gears 1's engine (2006) has no setting for this.");
    } else if (f == "drawdist") {
        if (g == Game::Gears2) return no(not_yet(0, "its config has no draw-distance setting"));
        if (g == Game::Gears1) return no("Gears 1's engine (2006) has no draw-distance setting.");
    } else if (f == "iniedit") {   // the options sent through --ue3_ini_edits; Gears 1 (flat Coalesced.ini) when gow1.exe edits it
        if (g == Game::Gears1 && !cap_exe(env, "gow1_ini_edits")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "skiplogos" || f == "trilinear") {
        if (g == Game::Gears2 && !env.g2_disc_ini_edits) return no(not_yet(0, "needs the next Gears 2 build"));
        if (g == Game::Gears1 && !cap_exe(env, f == "skiplogos" ? "gow1_skip_logos" : "gow1_trilinear")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "originalunlocks") {   // insane050: the exe must have the unlock (else the game's own rule applies anyway)
        // job 1519: Gears 1's execHasCompletedGame hook didn't unlock Insane on the difficulty screen - not done yet
        if (g == Game::Gears1) return no(not_yet(gi, "its Insane check isn't hooked yet - the game's own rule applies"));
        if (!cap_exe(env, "Unlock Insane difficulty from the start")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "hordebots") {   // GitHub #89 (Gears 3), #237 (Gears 2)
        if (g != Game::Gears3 && g != Game::Gears2) return no("Gears 2 and 3 only.");
        if (!cap_exe(env, g == Game::Gears2 ? "gow2_horde_bots" : "gow3_horde_bots")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "pcss") {   // notes/80 s.16
        if (g != Game::Gears3) return no("Gears 3 only.");
        if (!cap_exe(env, "soft shadows (PCSS) for the dynamic shadows")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "dofscale") {   // GitHub #105
        if (g != Game::Gears3 && g != Game::Judgment) return no("Gears 3 and Judgment only.");
        if (!cap_exe(env, "GitHub #105: surface pitches")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "mipfade") {
        if (g == Game::Gears2) return no(not_yet(0, "its config has no mip-fade setting"));
        if (g == Game::Gears1) return no("Gears 1's engine (2006) has no mip-fade setting - its sharper textures already show at once.");
    } else if (f == "aniso") {
        if (g == Game::Gears1 && !cap_exe(env, "gow1_aniso16")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "pool") {
        if (g == Game::Gears1 && !cap_exe(env, "gow1_texture_pool_mb")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "syslink") {   // 0.3.5: SDK patch 49b (the runtime's net_system_link); Gears 2 first
        if (!cap_rt(env, "net_system_link")) return no(not_yet(gi, "needs its next build"));
        if (ver != Version::TitleUpdate) return no("System Link is for the Title Update version only.");   // Shagg 15:53
    } else if (f == "syslinkpeers") {   // 0.4.8 (GitHub #229): the runtime's net_peer_ip (SDK patch 49b / 50)
        if (!cap_rt(env, "net_peer_ip")) return no(not_yet(gi, "needs its next build"));
        if (ver != Version::TitleUpdate) return no("System Link is for the Title Update version only.");
    } else if (f == "framecap") {
        if (g == Game::Gears1 && !cap_exe(env, "gow1_max_fps")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "lang") {   // the SDK runtime's --user_language / --user_country
        if (g == Game::Gears1 && !cap_rt(env, "user_language") && !cap_exe(env, "user_language")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "voicelang") {   // GitHub #87: layer 2's ue3_voice_language
        if (!cap_exe(env, "ue3_voice_language")) return no(not_yet(gi, "needs its next build"));
        // GitHub #176 (job 1212): Gears 1's packages can't be checked by the export reader yet, so the swap now changes
        // nothing there (an unchecked swap crashed the game while loading) - greyed until the reader knows Gears 1
        if (g == Game::Gears1) return no(not_yet(gi, "its voice files can't be checked yet (GitHub #176)"));
    } else if (f == "padlayout") {   // GitHub #95 / #97: SDK patch 55
        if (!cap_rt(env, "pad_remap")) return no(not_yet(gi, "needs the next runtime (SDK patch 55)"));
    } else if (f == "lowlag") {
        if (!cap_exe(env, "gow2_renderer_frames_in_flight")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "scenescale") {
        if (!cap_exe(env, "gow2_upscale_mode")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "dlssmodel") {   // 0.4.9
        if (!cap_exe(env, "gow2_dlss_preset")) return no(not_yet(gi, "needs its next build"));
        if (!cap_file(env, "nvngx_dlss.dll")) return no(not_yet(gi, "this build has no DLSS file"));
    } else if (f == "framegen") {
        if (!cap_exe(env, "gow2_frame_generation")) return no(not_yet(gi, "needs its next build"));
        if (!cap_file(env, "amd_fidelityfx_framegeneration_dx12.dll") && !dlss_fg_present(env) && !xefg_present(env))
            return no(not_yet(gi, "this build has no AMD, NVIDIA or Intel frame generation files"));
    } else if (f == "mute") {
        if (!cap_exe(env, std::string(k_procNames[gi]) + "_mute_unfocused")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "ao") {   // notes/80: Gears 2 and Gears 3 (s.9); notes/81: Judgment, with the fog anchor its exe sets; Gears 1 (s.14)
        // job 1272d: the short cvar name is not in gowj.exe as plain text - its description is (Gears 1: the scene-copy anchor's)
        const char* mark = g == Game::Judgment ? "AO passes that the chain detection does not"
                         : g == Game::Gears1   ? "has no fog pass to anchor ours"
                                               : "gow2_ao_quality";
        if (!cap_exe(env, mark)) return no(not_yet(gi, "needs its next build"));
    } else if (f == "realocclusion") {   // PR #43: Gears 3 and Judgment; GitHub #75: Gears 2 too
        // an older Gears 2 exe without strict queries would only warn - keep the box greyed out for it
        if (g == Game::Gears2 && !cap_exe(env, "gow2_renderer_queries")) return no(not_yet(0, "needs its next build"));
        // 0.3.0: with Gears 1's shader pack it froze the title screen (frame 183, 14:10 runs) - off until that is fixed
        if (g == Game::Gears1) return no(not_yet(gi, "freezes Gears 1's title screen with its shader pack - off for now"));
    } else if (f == "walkkey") {   // GitHub #125: SDK patch 60 (the 0.4.5 runtime)
        if (!cap_rt(env, "keybind_walk")) return no(not_yet(gi, "needs its next build (SDK patch 60)"));
    } else if (f == "vsync") {   // launcher patch 0155
        if (!cap_rt(env, "d3d12_present_vsync")) return no(not_yet(gi, "needs its next build (SDK patch 37)"));
    }
    return Availability();
}

const std::vector<KeyAction> &key_actions(Game g)
{
    // The Gears layout COGBox has always sent (PS1 930-937), now one row each so the player can change them.
    static const std::vector<KeyAction> v = {
        { "keybind_right_trigger", "RT", "Fire", "LMB" },
        { "keybind_left_trigger", "LT", "Aim", "RMB" },
        { "keybind_a", "A", "Cover / roadie run / evade", "Space" },
        { "keybind_b", "B", "Melee / chainsaw", "F" },
        { "keybind_x", "X", "Use / pick up", "E" },
        { "keybind_y", "Y", "Look at / point of interest", "Q" },
        { "keybind_right_shoulder", "RB", "Reload (active reload)", "R" },
        { "keybind_left_shoulder", "LB", "Tactical display", "Tab" },
        { "keybind_dpad_left", "D-pad left", "Weapon (left)", "1" },
        { "keybind_dpad_right", "D-pad right", "Weapon (right)", "2" },
        { "keybind_dpad_up", "D-pad up", "Weapon (up)", "3" },
        { "keybind_dpad_down", "D-pad down", "Grenades", "4" },
        { "keybind_rstick_press", "Right stick click", "Zoom", "MMB,Z" },
        { "keybind_lstick_press", "Left stick click", "Left stick click", "C" },
        { "keybind_start", "Start", "Pause menu", "Escape,Return" },
        { "keybind_back", "Back", "Back / scores", "Backspace" },
        { "keybind_walk", "-", "Walk", "None" },   // SDK patch 60: no key until chosen
    };
    // GitHub #208: Judgment's LB is the grenade and D-pad down the tactical display - the same keys (Tab, 4) keep
    // their meaning, so the two rows swap names and default keys
    static const std::vector<KeyAction> judgment = [] {
        std::vector<KeyAction> j = v;
        for (KeyAction &k : j) {
            if (!strcmp(k.cvar, "keybind_left_shoulder")) { k.what = "Grenades"; k.def = "4"; }
            else if (!strcmp(k.cvar, "keybind_dpad_down")) { k.what = "Tactical display"; k.def = "Tab"; }
        }
        return j;
    }();
    if (g == Game::Judgment) return judgment;
    return v;
}

Availability available(const Option& o, Game g, Version ver, const Settings& s, const Environment& env) {
    Availability a;
    if (!o.feature.empty()) a = can(g, o.feature, ver, env);
    if (!a.ok) return a;
    bool on = true;
    if (o.id == "WindowSize") on = s.i("Fullscreen") == 0;                             // PS1 333: windowed only
    else if (o.id == "PaperWhite" || o.id == "Peak") on = s.b("HDR");                 // PS1 366
    else if (o.id == "PadPrompts") on = s.b("Mnk");                                  // GitHub #104
    else if (o.id == "WalkMode") on = s.b("Mnk");                                    // GitHub #125
    else if (o.id == "AimSens") on = s.b("Mnk") && cap_exe(env, "ue3_mouse_aim_scale");    // 0.5.0: games whose exe knows the camera FOV
    else if (o.id == "ZoomSens") on = s.b("Mnk") && cap_exe(env, "ue3_mouse_zoom_scale");
    else if (o.id == "SceneMode") on = s.i("AAMode") >= 2 && s.i("AAMode") <= 4 && (s.i("Api") == 0 || (s.i("AAMode") == 2 && fsr_vulkan_on(s, env)));   // PS1 352, 473; job 521; T2x: no scene scale; fsr049: FSR on Vulkan (opt-in)
    else if (o.id == "DlssModel") on = s.i("AAMode") == 3 && !runs_vulkan(s, env);      // 0.4.9: DLSS only (DirectX 12)
    else if (o.id == "FrameGen" && runs_vulkan(s, env)) {   // job 521; notes/83: AMD FSR frame generation on Vulkan
        const Availability v = framegen_choice_available(1, env, true);
        if (!v.ok) { a.ok = false; a.why = "DirectX 12 only for now. " + v.why; return a; }
    } else if (o.id == "FrameGen" && env.wine) { a.ok = false; a.why = "Vulkan only on Proton / Wine."; return a; }
    else if (o.id == "FsrVulkan") on = runs_vulkan(s, env) && fsr_vulkan(env);      // review: opt-in, Vulkan builds with AMD's VK DLLs
    else if (o.id == "Sharpen") on = s.i("Resolution") == 5;                         // GitHub #59: the 1080p mode only
    else if (o.id == "SplitLayout") on = s.i("Players") == 1;                       // GitHub #136: 2 players only
    else if (o.id == "PeerIps") on = s.b("SystemLink");                              // GitHub #229
    else if (o.id == "Monitor") on = env.screen_count >= 2;                           // PS1 335
    else if (o.id == "AspectRatio") on = !s.b("Ultrawide");                          // 0.4.9: 4:3 not with ultrawide
    else if (o.id == "WineApi") on = env.wine && s.i("Api") == 0;                     // Shagg 13:16, job 643
    else if (o.id == "HDR" && env.wine && env.deck_lcd) {                            // proton049
        a.ok = false; a.why = "The Steam Deck LCD's screen has no HDR."; return a;
    }
    a.ok = on;
    return a;
}

Availability choice_available(const Option& o, Game g, int index) {
    Availability a;
    if (o.id == "FpsMode" && index == 3 && (g == Game::Judgment || g == Game::Gears1)) { a.ok = false; a.why = not_yet(static_cast<int>(g), "game's own counter"); }   // PS1 267, 475
    if (o.id == "Lang" && g == Game::Gears1 && !gears1_lang(index)) { a.ok = false; a.why = "Not in Gears 1 - it would show English."; }
    if (o.id == "AAMode" && index == 5 && g != Game::Gears2 && g != Game::Gears3) { a.ok = false; a.why = "Gears 2 and Gears 3 for now."; }   // notes/80 s.7
    return a;
}

std::string tooltip_for(const Option& o, Game g, const Availability& a, const Environment& env) {
    std::string base = o.tooltip;
    if (o.id == "Api" && env.wine && (g == Game::Gears3 || g == Game::Judgment))   // Shagg 3 Nov: Vulkan is the Proton path
        base = "On Steam Deck / Linux (Proton) Vulkan is the usual choice - DirectX 12 can crash there.\n"
               "On Vulkan, DLSS / XeSS are not available and FSR is experimental (\"FSR on Vulkan (experimental)\"); otherwise SMAA is used, "
               "frame generation is AMD FSR only (experimental), "
               "and Reflex / Anti-Lag are off. If something looks wrong, make a problem report and send it to us.";
    if (o.id == "AO")   // notes/81
        base += "\nCACAO: AMD's FidelityFX CACAO at half resolution: costs much less than XeGTAO and shows less grain with DLAA and SMAA T2x, though fine contact detail is softer.";
    if (o.id == "AO" && g == Game::Judgment)   // notes/81
        base += "\nJudgment: XeGTAO and CACAO replace the game's own AO, which can flicker with DLSS.";
    if (o.id == "DoF" && g == Game::Gears1)   // GitHub #179
        base += "\nGears 1: off turns off the game's blur pass, which the bloom uses too - the glow is softer.";
    if (o.id == "AO" && g == Game::Gears1)   // notes/80 s.14
        base += "\nGears 1: the game has no AO of its own - XeGTAO and CACAO add it under the level's lighting, so lit areas keep their light. Off and Game's own are the same here.";
    if (o.id == "SoftShadows")   // notes/80 s.16
        base += "\nUltra's finer shadow maps follow the scene's render size: with DLSS Ultra Performance the scene is drawn at "
                "720p and Ultra only adds samples; with DLAA or Quality it pays off.";
    if (o.id == "AO" && g == Game::Gears3)   // notes/80 s.9
        base += "\nGears 3: most levels have their shading baked in and no real-time AO - XeGTAO adds a gentle contact shading there and replaces the game's own where a level has it.";
    if (o.id == "AAMode") {   // PS1 345
        std::string gpu = gpu_main(env);
        base += "\nYour graphics card: " + (gpu.empty() ? std::string("not detected") : gpu);
    }
    if (g == Game::Judgment && o.id == "FpsMode") return not_yet(2, "game's own counter") + " " + base;   // PS1 476
    if (g == Game::Judgment && o.id == "Players") return "Untested in Judgment. " + base;
    if (o.id == "SystemLink" && g != Game::Gears2 && a.ok)   // Shagg 16:07: all four games' title updates; Gears 2 tested
        return std::string("Untested in ") + (g == Game::Gears3 ? "Gears 3" : g == Game::Judgment ? "Judgment" : "Gears 1") + " - please tell us in #72. " + base;                 // PS1 477
    if (g == Game::Gears1 && o.id == "FpsMode") return not_yet(3, "game's own counter") + " " + base;
    if (g == Game::Gears1 && o.id == "Players" && a.why.empty()) return "Untested in Gears 1. " + base;
    if (g == Game::Judgment && o.id == "FrameCap")   // GitHub #38: as on Xenia Canary, unlimited can leave a loading screen endless
        return base + "\nJudgment: Unlimited can leave the loading screen after some chapters (Monroe Commons) running for "
                      "ever. 60 is the recommended limit.";
    if (!a.why.empty()) return trim(a.why + " " + base);                                                  // PS1 415
    return base;
}

// proton049: the line added under a row's description under Proton / Wine ("" on Windows and for other rows)
std::string wine_note(const Option& o, const Environment& env) {
    if (!env.wine) return "";
    if (o.id == "AAMode")
        return "On Linux / Steam Deck: DLSS needs an NVIDIA RTX card with NVIDIA's own Linux driver and DirectX 12 as the Graphics API on Proton (testing). Proton 9 and newer turn NVIDIA's NVAPI on for most games; if DLSS still doesn't start, add PROTON_ENABLE_NVAPI=1 %command% to the game's launch options in Steam.";
    if (o.id == "HDR")
        return "On Linux / Steam Deck: only where Proton can show HDR - the Steam Deck OLED in Game Mode with HDR on in Steam's display settings, or a desktop running HDR through gamescope. Otherwise leave it off.";
    if (o.id == "LowLag")
        return "Under Proton / Wine fewer frames wait in line as usual; NVIDIA Reflex and AMD Anti-Lag 2 are not used there.";
    if (o.id == "FrameCap" && env.steam_deck)
        return "Steam Deck: 60 is the recommended limit. 40, with the screen's refresh rate set to 40 Hz in Steam's Performance menu, saves battery and stays smooth. Leave Steam's own frame limit off when you use this one.";
    return "";
}

// proton049
Availability wine_choice_available(const Option& o, int index, const Environment& env) {
    Availability a;
    if (!env.wine) return a;   // Windows: nothing changes
    if (o.id == "Fullscreen" && index == 2) {
        a.ok = false;
        a.why = "Not under Proton / Wine: the Linux desktop or the Steam Deck's Game Mode owns the screen, so borderless fullscreen is used.";
    } else if (o.id == "AAMode" && index == 3 && !icase_search(gpu_main(env), "RTX")) {
        a.ok = false;
        a.why = "Needs an NVIDIA RTX graphics card with NVIDIA's own driver.";
    } else if (o.id == "FrameGen" && framegen_is_dlss(index)) {
        a.ok = false;
        a.why = "Not under Proton / Wine in COGBox: NVIDIA DLSS frame generation needs NVIDIA's driver, DirectX 12 and Streamline, untested on Linux.";
    } else if (o.id == "FrameGen" && index == k_fgXefg) {
        a.ok = false;
        a.why = "Not under Proton / Wine.";
    }
    return a;
}

std::vector<std::pair<std::string, double>> proton_recommended(Game g, const Environment& env) {
    std::vector<std::pair<std::string, double>> v;
    if (!env.wine) return v;
    (void)g;
    v.push_back({ "Api", 1 });        // Vulkan: the renderer talks to the Linux driver directly (no vkd3d-proton)
    v.push_back({ "WineApi", 1 });    // ... and Vulkan if the player later picks DirectX 12 + Automatic
    if (fsr_vulkan(env)) { v.push_back({ "AAMode", 2 }); v.push_back({ "FsrVulkan", 1 }); }
    else v.push_back({ "AAMode", 1 });   // FSR on Vulkan needs a newer build: SMAA, as the launcher would use anyway
    v.push_back({ "SceneMode", 0 });
    v.push_back({ "FrameGen", 0 });
    if (env.steam_deck) {
        v.push_back({ "Fullscreen", 1 });   // borderless: the Deck's own 1280x800 screen
        v.push_back({ "Resolution", 0 });   // 1x (720p), shown on the 1280x800 screen
        v.push_back({ "FrameCap", 3 });     // 60 (40 with a 40 Hz screen: the row's tooltip)
        v.push_back({ "PromptStyle", 4 });  // Steam Deck buttons
        v.push_back({ "Mnk", 0 });          // the controller, not keyboard and mouse
    }
    return v;
}

Version effective_version(Game g, const Settings& s, const Environment& env) {   // PS1 459
    if (g == Game::Judgment || g == Game::Gears1) return Version::TitleUpdate;   // one build (disc + its title update)
    if (!env.update_ready) return Version::Disc;
    // Title updates required (0.4.0): an older saved "Disc (original release)" choice is no longer followed.
    (void)s;
    return Version::TitleUpdate;
}
bool exe_for_update(Game g, Version ver, const Environment& env) {   // PS1 442-445, 804, 809, 821
    if (g == Game::Judgment || g == Game::Gears1) return env.update_ready;
    return env.update_ready && ver == Version::TitleUpdate;
}
const char* normal_code_exe_name(Game g) {   // cg050
    switch (g) {
    case Game::Gears2: return "gow2-normal.exe";
    case Game::Gears3: return "gow3-normal.exe";
    case Game::Judgment: return "gowj-normal.exe";
    default: return "gow1-normal.exe";
    }
}
std::string normal_code_exe(const std::string& exePath) {
    const size_t dot = exePath.find_last_of('.'), sep = exePath.find_last_of("\\/");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return exePath + "-normal";
    return exePath.substr(0, dot) + "-normal" + exePath.substr(dot);
}

// ---- Presets (PS1 573-592) ------------------------------------------------------------------------------------------
void apply_preset(Settings& s, int p) {
    if (p < 0 || p >= 3) return;   // PS1 576: Custom changes nothing
    struct P { int res, fps, aa; bool aniso, tri, mip; int pool, draw; bool blur, dof, flare; int shake, fov; };
    static const P v[3] = {        // PS1 579-581
        { 0, 0, 0, false, false, false, 0, 0, true, true, true, 0, 100 },
        { 1, 3, 1, true, true, true, 1, 1, true, true, true, 0, 100 },   // fps: FrameCap indices (3 = 60, 5 = 120)
        { 4, 5, 1, true, true, true, 2, 2, true, true, true, 0, 100 },
    };
    const P& x = v[p];
    s.set("Preset", p);
    // 0.4.6 (Shagg 4 Nov): the frame-rate limit (x.fps) is its own setting - the presets leave it alone
    s.set("Resolution", x.res); s.set("AAMode", x.aa); s.set("Aniso", x.aniso);
    s.set("Trilinear", x.tri); s.set("MipFade", x.mip); s.set("Pool", x.pool); s.set("DrawDist", x.draw);
    s.set("MotionBlur", x.blur); s.set("DoF", x.dof); s.set("Shake", x.shake); s.set("Fov", x.fov);
}

void set_value(Game g, Settings& s, const std::string& id, double value) {
    const Option* o = find_option(g, id);
    if (o) value = clamp_value(*o, value, nullptr);
    if (id == "Preset") { s.set("Preset", value); apply_preset(s, static_cast<int>(value)); return; }   // PS1 592
    double old = s.f(id);
    s.set(id, value);
    if (o && o->in_preset && old != value && s.i("Preset") != PresetCustom) s.set("Preset", PresetCustom);   // PS1 588
}

// ---- Kind::Text (0.4.8, GitHub #229) -----------------------------------------------------------------------------------
// One IPv4 address, dotted decimal: four parts of 1-3 digits, each 0-255, no leading zeros (an "010" could be read as
// octal elsewhere). Nothing else - no ports, names or IPv6: the runtime's ParseIpv4 skips those anyway.
static bool parse_ipv4(const std::string& t, unsigned char out[4]) {
    int part = 0; size_t i = 0;
    while (part < 4) {
        size_t st = i; unsigned v = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9' && i - st < 3) { v = v * 10 + unsigned(t[i] - '0'); ++i; }
        const size_t len = i - st;
        if (len == 0 || v > 255 || (len > 1 && t[st] == '0')) return false;
        out[part++] = static_cast<unsigned char>(v);
        if (part < 4) { if (i >= t.size() || t[i] != '.') return false; ++i; }
    }
    return i == t.size();
}

bool text_char_ok(const Option& o, unsigned c) {
    if (o.id == "PeerIps") return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == ' ';
    return false;   // no other text option: nothing typed is accepted
}

std::string sanitize_text(const Option& o, const std::string& in, int* dropped) {
    if (dropped) *dropped = 0;
    if (o.kind != Kind::Text || o.id != "PeerIps") return std::string();
    const std::string src = in.size() > 4096 ? in.substr(0, 4096) : in;   // never walk an absurd value
    std::vector<std::string> keep;
    size_t at = 0;
    while (at <= src.size()) {
        size_t end = src.find_first_of(", ;\t", at);
        if (end == std::string::npos) end = src.size();
        const std::string tok = src.substr(at, end - at);
        at = end + 1;
        if (tok.empty()) continue;
        unsigned char b[4];
        const bool ok = parse_ipv4(tok, b) && !(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) &&
                        !(b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255);
        if (!ok || keep.size() >= 16) { if (dropped) ++*dropped; continue; }
        if (std::find(keep.begin(), keep.end(), tok) == keep.end()) keep.push_back(tok);
    }
    std::string out;
    for (const std::string& k : keep) {
        const std::string next = out.empty() ? k : out + ", " + k;
        if (o.max_len > 0 && static_cast<int>(next.size()) > o.max_len) { if (dropped) ++*dropped; continue; }
        out = next;
    }
    return out;
}

std::string monitor_label(int i, int w, int h, bool primary) {   // PS1 147
    return std::to_string(i) + ": " + std::to_string(w) + "x" + std::to_string(h) + (primary ? " (main)" : "");
}
std::string main_gpu(const std::vector<std::string>& gpus) {     // PS1 152
    for (const std::string& n : gpus) if (icase_search(n, "NVIDIA|GeForce|Quadro|AMD|Radeon|Intel.*Arc")) return n;
    for (const std::string& n : gpus) if (!n.empty()) return n;
    return "";
}
std::string launch_url(Game g, int chapter) {                    // PS1 141-144
    if (g == Game::Judgment) return std::string(k_mapsGJ[std::max(0, std::min(17, chapter))]) + " -nostartupmovies";
    if (g == Game::Gears1) {   // the chapter's level file and its start point (the game's chapter select passes it the same way)
        const int n = int(sizeof(k_gears1Chapters) / sizeof(k_gears1Chapters[0]));
        const Gears1Chapter& c = k_gears1Chapters[std::max(0, std::min(n - 1, chapter))];
        return std::string(c.map) + "?LoadChapter=" + std::to_string(c.point);
    }
    return "GearGame_P?loadcheckpoint=0?chapter=" + std::to_string(chapter) + " -nostartupmovies";
}

// ---- Store (PS1 103-123, 418-464, 759-774) --------------------------------------------------------------------------
Store::Store() { reset(); }

void Store::reset() {
    top.clear();
    for (int g = 0; g < kGameCount; ++g) { per[g].clear(); own[g].clear(); }
    last_game = 0;
    texts.clear();
    for (const Option& o : options(Game::Gears2)) if (o.kind == Kind::Text) texts[o.id] = std::string();
    for (const Option& o : options(Game::Gears2)) if (o.scope != Scope::GameOwn && o.kind != Kind::Text) top[o.id] = o.def;
    for (int g = 0; g < kGameCount; ++g)
        for (const Option& o : options(static_cast<Game>(g))) if (o.scope == Scope::GameOwn) own[g][o.id] = o.def;
    // PS1 104-105 / 154 / 157-158: AAMode and the FPS counters start at -1 and are worked out from the older boxes.
    // Shagg 11-06 (GitHub #212: jagged shadow edges at 2x with anti-aliasing off): SMAA by default. AAMode still starts at
    // -1 so a saved settings file that only has the old AA box keeps its choice (migrate); a new install gets AA = 1 -> SMAA.
    top["AAMode"] = -1; top["AA"] = 1; top["ShowFps"] = 0; top["G3ShowFps"] = 0;
    own[0]["FpsMode"] = -1; own[1]["FpsMode"] = -1;
    migrate();
}

void Store::migrate() {
    if (top["AAMode"] < 0) top["AAMode"] = top["AA"] != 0 ? 1 : 0;                      // PS1 154
    if (own[0]["FpsMode"] < 0) own[0]["FpsMode"] = top["ShowFps"] != 0 ? 3 : 0;          // PS1 157
    if (own[1]["FpsMode"] < 0) own[1]["FpsMode"] = top["G3ShowFps"] != 0 ? 3 : 0;        // PS1 158
}

Settings Store::view(Game g, const Environment& env) const {
    const int gi = static_cast<int>(g);
    Settings s;
    for (const Option& o : options(g)) {
        if (o.kind == Kind::Text) {   // 0.4.8: Shared text, sanitised again on the way out
            auto t = texts.find(o.id);
            s.t[o.id] = t == texts.end() ? std::string() : sanitize_text(o, t->second);
            continue;
        }
        double x = o.def;
        const std::map<std::string, double>* src = &top;
        if (o.scope == Scope::GameOwn) src = &own[gi];
        else if (o.scope == Scope::PerGame && per[gi].count(o.id)) src = &per[gi];   // PS1 429: else the top-level value
        auto it = src->find(o.id);
        if (it != src->end()) x = it->second;
        if (o.id == "Fullscreen" && x == 1.0) {   // 29 Oct: the old "Exclusive fullscreen instead" box (same scope rules)
            const auto ex = src->find("Exclusive");
            if (ex != src->end() && ex->second != 0.0) x = 2.0;
        }
        s.set(o.id, clamp_value(o, x, &env));   // PS1 300 / 431 / 437-438 clamping
    }
    s.set("Version", static_cast<int>(effective_version(g, s, env)));   // PS1 459
    if ((g == Game::Judgment || g == Game::Gears1) && s.i("FpsMode") == 3) s.set("FpsMode", 0); // PS1 475
    if (g == Game::Gears1 && !gears1_lang(s.i("Lang"))) s.set("Lang", 0);   // a language Gears 1 has no text for
    return s;
}

void Store::remember(Game g, const Settings& s) {   // PS1 450-455
    const int gi = static_cast<int>(g);
    own[gi]["Start"] = s.f("Start"); own[gi]["Aniso"] = s.f("Aniso"); own[gi]["FpsMode"] = s.f("FpsMode");
    if (g != Game::Judgment && g != Game::Gears1) own[gi]["Version"] = s.f("Version");
    for (const Option& o : options(g)) if (o.scope == Scope::GameOwn && o.id != "Version") own[gi][o.id] = s.f(o.id);   // GitHub #222: AimTurn, HideNotes, UnlockInsane (now OriginalUnlocks), HordeBots were never kept
    for (const Option& o : options(g)) if (o.scope == Scope::PerGame) per[gi][o.id] = s.f(o.id);   // PS1 419-427
}

void Store::commit(Game g, const Settings& s, const Environment& env) {   // PS1 759-774
    const int gi = static_cast<int>(g);
    last_game = gi;
    own[gi]["Start"] = s.f("Start"); own[gi]["Aniso"] = s.f("Aniso"); own[gi]["FpsMode"] = s.f("FpsMode");
    if (g != Game::Judgment && g != Game::Gears1 && env.update_ready) own[gi]["Version"] = s.f("Version");   // PS1 761-762
    for (const Option& o : options(g)) if (o.scope == Scope::GameOwn && o.id != "Version") own[gi][o.id] = s.f(o.id);   // GitHub #222: AimTurn, HideNotes, UnlockInsane (now OriginalUnlocks), HordeBots were never kept
    for (const Option& o : options(g)) {
        if (o.scope == Scope::GameOwn) continue;
        if (o.kind == Kind::Text) { texts[o.id] = sanitize_text(o, s.str(o.id)); continue; }
        top[o.id] = s.f(o.id);
        if (o.scope == Scope::PerGame) per[gi][o.id] = s.f(o.id);
    }
    top["AA"] = s.i("AAMode") == 1 ? 1 : 0;   // PS1 764
}

// Job 638: the old "Fps" index (0 = 30, 1 = 60, 2 = 120, 3 = Unlimited) as a FrameCap index (list 2).
// FrameCap list 1 { 30, 45, 60, 90, 120, 144, 165, Unlimited } -> list 2 { 30, 40, 45, 60, 90, 120, 144, 165, 180, Unlimited }.
static double old_cap_list1_to_2(double x) {
    static const int map[8] = { 0, 2, 3, 4, 5, 6, 7, 9 };
    return map[std::max(0, std::min(7, static_cast<int>(x)))];
}

static double old_fps_to_cap(double x) {
    static const int map[4] = { 0, 3, 5, 9 };
    return map[std::max(0, std::min(3, static_cast<int>(x)))];
}

bool Store::load_ini(const std::string& text) {
    reset();
    top["AAMode"] = -1; own[0]["FpsMode"] = -1; own[1]["FpsMode"] = -1;   // worked out again after reading
    std::istringstream in(text);
    std::string line, section;
    bool any = false;
    // FrameCap list version per section (0 = [General]); only FrameCap values read from a file without
    // "FrameCapList=2" are moved over, once, after reading (values from the old "Fps" are already list 2).
    bool capList2[kGameCount + 1] = {}, capRead[kGameCount + 1] = {};
    bool brightness75 = false;   // menu5: the file was written with Brightness' new default
    // The PS1's flat keys (launcher-settings.json converted to key=value) are read in [General] too.
    struct Legacy { const char* key; int game; const char* id; };
    static const Legacy legacy[] = {
        { "StartG2", 0, "Start" }, { "StartG3", 1, "Start" }, { "StartGJ", 2, "Start" },
        { "Version", 0, "Version" }, { "G3Version", 1, "Version" },
        { "Aniso", 0, "Aniso" }, { "G3Aniso", 1, "Aniso" }, { "GJAniso", 2, "Aniso" },
        { "FpsMode", 0, "FpsMode" }, { "G3FpsMode", 1, "FpsMode" }, { "GJFpsMode", 2, "FpsMode" } };
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (section_game(section) < 0) {   // 0.4.8: a Kind::Text value ([General] only), kept only as sanitised
            const Option* to = find_option(Game::Gears2, key);
            if (to && to->kind == Kind::Text) { texts[key] = sanitize_text(*to, val); any = true; continue; }
        }
        std::string lv = val;
        for (char& c : lv) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        double x = 0;
        if (lv == "true") x = 1;
        else if (lv == "false") x = 0;
        else {
            std::istringstream num(val); num.imbue(std::locale::classic());
            if (!(num >> x)) continue;
        }
        int gsec = section_game(section);
        if (gsec < 0) {
            if (key == "Game") { last_game = std::max(0, std::min(kGameCount - 1, static_cast<int>(x))); any = true; continue; }
            if (key == "WineD3D12") {   // job 643: the old box; on = DirectX 12, off = the new Automatic
                top["WineApi"] = x != 0 ? 2 : 0;
                any = true;
                continue;
            }
            if (key == "Exclusive") {   // job 666: 0.2.2's "Exclusive fullscreen instead" box, read by Store::view
                top["Exclusive"] = x;
                any = true;
                continue;
            }
            if (key == "FrameCapList") { capList2[0] = x >= 2; continue; }
            if (key == "BrightnessDefault") { brightness75 = x >= kBrightnessDefault; continue; }   // menu5 (0.5.0)
            if (key == "FrameCap") { top["FrameCap"] = x; capRead[0] = true; any = true; continue; }
            if (key == "Fps") {   // job 638: the old 4-step frame rate limit (the shared value)
                top["FrameCap"] = old_fps_to_cap(x);
                any = true;
                continue;
            }
            bool done = false;
            for (const Legacy& l : legacy) if (key == l.key) { own[l.game][l.id] = x; done = true; }
            if (key == "AA" || key == "ShowFps" || key == "G3ShowFps") { top[key] = x; done = true; }
            const Option* o = find_option(Game::Gears2, key);
            if (!done && o && o->scope != Scope::GameOwn) { top[key] = x; done = true; }
            any = any || done;
        } else {
            if (key == "WineD3D12") {   // job 643: the old box of this game
                per[gsec]["WineApi"] = x != 0 ? 2 : 0;
                any = true;
                continue;
            }
            if (key == "Exclusive") {   // job 666: the old box of this game
                per[gsec]["Exclusive"] = x;
                any = true;
                continue;
            }
            if (key == "FrameCapList") { capList2[gsec + 1] = x >= 2; continue; }
            if (key == "FrameCap") { per[gsec]["FrameCap"] = x; capRead[gsec + 1] = true; any = true; continue; }
            if (key == "Fps") {   // job 638: the old 4-step frame rate limit of this game
                per[gsec]["FrameCap"] = old_fps_to_cap(x);
                any = true;
                continue;
            }
            const Option* o = find_option(static_cast<Game>(gsec), key);
            if (!o || o->scope == Scope::Shared) continue;
            if (o->scope == Scope::GameOwn) own[gsec][key] = x; else per[gsec][key] = x;
            any = true;
        }
    }
    if (capRead[0] && !capList2[0]) top["FrameCap"] = old_cap_list1_to_2(top["FrameCap"]);
    for (int g = 0; g < kGameCount; ++g)
        if (capRead[g + 1] && !capList2[g + 1]) per[g]["FrameCap"] = old_cap_list1_to_2(per[g]["FrameCap"]);
    // menu5 (Shagg, RC4-4): Brightness' default 65 -> 75. A file written before (no "BrightnessDefault=75") always holds
    // the old default 65 in [General] (save_ini writes every shared value) and in a game's section once its settings were
    // saved: there 65 is taken as "the default", and becomes the new one. Any other value is the player's and stays.
    if (!brightness75) {
        auto lift = [](std::map<std::string, double>& m) { auto it = m.find("Brightness"); if (it != m.end() && it->second == kBrightnessOldDefault) it->second = kBrightnessDefault; };
        lift(top);
        for (int g = 0; g < kGameCount; ++g) lift(per[g]);
    }
    migrate();
    return any;
}

std::string Store::save_ini() const {
    auto val = [](const Option& o, double x) {
        if (o.kind == Kind::Bool) return fmt_bool(x != 0);
        if (o.kind == Kind::Float) return fmt_double(x);
        return std::to_string(static_cast<long long>(std::nearbyint(x)));
    };
    auto get = [](const std::map<std::string, double>& m, const Option& o) { auto it = m.find(o.id); return it == m.end() ? o.def : it->second; };
    std::ostringstream out;
    out << "; COGBox - Gears of War settings. Boxes: true / false; lists: the position from 0.\n";
    out << "[General]\nGame=" << last_game << "\nFrameCapList=2\nBrightnessDefault=" << kBrightnessDefault << "\n";
    for (const Option& o : options(Game::Gears2)) {
        if (o.scope == Scope::GameOwn) continue;
        if (o.kind == Kind::Text) { auto t = texts.find(o.id); out << o.id << "=" << (t == texts.end() ? std::string() : sanitize_text(o, t->second)) << "\n"; continue; }
        out << o.id << "=" << val(o, get(top, o)) << "\n";
    }
    for (int g = 0; g < kGameCount; ++g) {
        out << "\n[" << section_name(g) << "]\nFrameCapList=2\n";
        for (const Option& o : options(static_cast<Game>(g))) {
            if (o.scope == Scope::GameOwn) out << o.id << "=" << val(o, get(own[g], o)) << "\n";
            else if (o.scope == Scope::PerGame && per[g].count(o.id)) out << o.id << "=" << val(o, per[g].at(o.id)) << "\n";
        }
    }
    return out.str();
}

// ---- The command line (PS1 788-961) ---------------------------------------------------------------------------------
PromptComposer g_prompt_composer = nullptr;   // job 661: set by keycaps.cpp

std::vector<std::string> build_args(Game g, Version ver, const Settings& s, const Environment& env, std::vector<std::string>* notesOut) {
    const int gi = static_cast<int>(g);
    const bool isG3 = g == Game::Gears3, isGJ = g == Game::Judgment, isG1 = g == Game::Gears1;
    std::vector<std::string> a, notes;
    auto q = [](const std::string& p) { return "\"" + p + "\""; };
    auto logf = [&](const char* name) { return env.log_dir + env.path_sep + name; };
    auto avail = [&](const char* id) { const Option* o = find_option(g, id); return o && available(*o, g, ver, s, env).ok; };
    auto on = [&](const char* id) { return avail(id) && s.b(id); };   // PS1 796: a greyed box is never sent
    const bool useUpdate = exe_for_update(g, ver, env);
    // 0.2.0 mouse look (below): needs SDK patch 38 in the exe and the config edits' text replace ("~=")
    // 0.2.1: native mouse look (the engine's own PC mouse axes, ue3_mouse_look in the exe) replaces 0.2.0's boosted
    // right stick; the old way stays for exes without it
    const bool nativeMouse = s.b("Mnk") && cap_exe(env, "ue3_mouse_look");
    const bool mouseLook = !nativeMouse && (!isG1 || cap_exe(env, "gow1_ini_edits")) && s.b("Mnk") && cap_exe(env, "pad_look_scale") &&
                           !(g == Game::Gears2 && !useUpdate && !env.g2_disc_ini_edits);

    // PS1 800-823: data roots, log, crash folder, renderer, shader cache, title update
    if (g == Game::Gears2) {
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf("gow2.log")), "--gow2_crash_dir=" + q(env.log_dir), "--gpu_plugin=xenos",
              // GitHub #58: without this Gears 2 kept its shaders in port\out\shader-cache, so "Prepare all shaders"
              // wrote its progress there and the launcher, reading Gears2\shadercache, said it never finished
              "--gow2_shader_cache_dir=" + q(env.shader_cache_dir) };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    } else if (isG3) {
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf("gow3.log")), "--gow2_crash_dir=" + q(env.log_dir),
              "--gow2_shader_cache_dir=" + q(env.shader_cache_dir) };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    } else {   // Judgment, Gears 1 (the same layout: one build, its title update when present)
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf(isG1 ? "gow1.log" : "gowj.log")), "--gow2_crash_dir=" + q(env.log_dir), "--gpu_plugin=xenos",
              "--gow2_shader_cache_dir=" + q(env.shader_cache_dir) };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    }
    // social step 1: one online account per profile for all four games (only a runtime that has the option)
    if (!env.identity_dir.empty() && cap_rt(env, "net_identity_dir")) a.push_back("--net_identity_dir=" + q(env.identity_dir));
    // social step 3: the COGBox multiplayer server, when the player enabled it in the SOCIAL tab
    if (!env.online_server.empty() && cap_rt(env, "net_master_server")) {
        a.insert(a.end(), { "--net_online=true", "--net_live_emulation=true", "--net_master_server=" + q(env.online_server) });
        if (!env.online_stun.empty()) a.push_back("--net_stun_server=" + env.online_stun);
        if (!env.online_region.empty() && cap_rt(env, "net_region")) a.push_back("--net_region=" + env.online_region);
    }
    // social step 4: Gears 3 TU6's UE3 GNames / GEngine variables, for the runtime's private match fixes (a friend can
    // join the pregame lobby); the runtime checks the script bytecode before it changes anything
    if (isG3 && useUpdate && cap_rt(env, "cogbox_ue3_gnames")) {
        a.push_back("--cogbox_ue3_gnames=8344D918");
        a.push_back("--cogbox_ue3_gengine=8343987C");
    }
    // 0.4.5 (jobs 1198 / 1201): the window's own thread paints and presents the picture, not the game's renderer thread
    // (the presenter waited 0.5-1.4 ms per frame there): Gears 2 1440p 191 -> 195 fps, 1% low 150 -> 160.
    a.push_back("--host_present_from_non_ui_thread=false");
    if (env.wine) notes.push_back("launcher: running under Wine / Proton");   // job 523
    // PS1 832-836: start at a chapter
    // 0.3.0: "Start at" removed (see the option list) - the game always opens at its own main menu.
    // PS1 837-842: render resolution ("Match my monitor" = scale 0 + native size; counts as 2x below)
    int scale = s.i("Resolution") + 1;
    if (scale == 6) {   // 0.2.0 "1080p": 720p drawn, upscaled to the window (Lanczos) and sharpened (RCAS)
        a.insert(a.end(), { "--gow2_renderer_resolution_scale=1", "--gow2_present_native_size=true",
                            "--gow2_present_upscale_filter=lanczos" });
        // GitHub #59: Automatic sharpens except under Wine; On / Off are the player's
        const int sh = s.i("Sharpen");
        const bool sharpen = sh == 1 || (sh == 0 && !env.wine);
        a.push_back(sharpen ? "--gow2_present_sharpen=0.5" : "--gow2_present_sharpen=0");
        if (env.wine && sh == 0) notes.push_back("launcher: Wine / Proton - upscale sharpening off (Automatic; GitHub #59)");
        scale = 1;
    } else if (scale >= 5) { a.push_back("--gow2_renderer_resolution_scale=0"); a.push_back("--gow2_present_native_size=true"); scale = 2; }
    else a.push_back("--gow2_renderer_resolution_scale=" + std::to_string(scale));
    // PS1 843-849: Gears 3's threshold, memexport, aspect
    if (g == Game::Gears2 && avail("HordeBots") && on("HordeBots")) a.push_back("--gow2_horde_bots=true");   // GitHub #237
    // insane050: Insane from the start unless "Original unlock rules" is on (GitHub #174)
    if (avail("OriginalUnlocks") && !on("OriginalUnlocks")) {
        static const char* const k_unlock[kGameCount] = { "--gow2_unlock_insane=true", "--gow3_unlock_insane=true",
                                                          "--gowj_unlock_insane=true", "--gow1_unlock_insane=true" };
        a.push_back(k_unlock[gi]);
    }
    if (isG3) {
        if (scale > 1) a.push_back("--gow2_renderer_resolution_scale_threshold=1120");

        if (avail("HordeBots") && on("HordeBots")) a.push_back("--gow3_horde_bots=true");   // GitHub #89
        // GitHub #105: the depth-of-field chain's half-size targets (pitch 640) follow the scale despite the threshold
        if (scale > 1 && avail("DofScale") && on("DofScale")) a.push_back("--gow2_renderer_resolution_scale_pitches=640");
        a.push_back("--gow2_renderer_memexport=true");
        if (s.i("Resolution") >= 4 && !s.b("Ultrawide")) {
            if (cap_exe(env, "gow3_aspect")) a.push_back("--gow3_aspect=auto");
            else notes.push_back("launcher: this Gears 3 build has no --gow3_aspect yet - the picture stays 16:9");
        }
    }
    // PS1 851-855: anti-aliasing; DLSS needs an RTX card, FSR / DLSS / XeSS their DLL next to the exe, else SMAA
    int aa = s.i("AAMode");
    {
        const std::vector<std::string> gpus = gpu_list(env);
        bool hasRtx = false;
        for (const std::string& n : gpus) if (icase_search(n, "RTX")) hasRtx = true;
        if (aa == 3 && !hasRtx && !gpus.empty()) { notes.push_back("launcher: DLSS needs an NVIDIA RTX card (" + gpu_main(env) + ") - FSR instead"); aa = 2; }
        static const char* const need[5] = { "", "", "amd_fidelityfx_loader_dx12.dll", "nvngx_dlss.dll", "libxess.dll" };
        if (aa == 5 && ((g != Game::Gears2 && g != Game::Gears3) || !cap_exe(env, "smaa_t2x"))) {   // notes/80 s.7: ours, in the Gears 2 exe from 0.3.x
            notes.push_back("launcher: SMAA T2x is not in this game / build - SMAA instead"); aa = 1;
        }
        if (aa >= 2 && aa <= 4 && !cap_file(env, need[aa])) {
            notes.push_back(std::string("launcher: ") + k_aaNames[aa] + " is not available for this game / build - SMAA instead"); aa = 1;
        }
        // 0.2.1 (GitHub #15, #3, #11): under Proton / Wine (Steam Deck, SteamOS, Linux) DirectX 12 ends the game a few
        // seconds in (exit code 3, right after the renderer starts, every game); Vulkan gets in game there - so there
        // it is always Vulkan, and the log says so
        // Shagg 13:16: no longer forced - Vulkan by default there, DirectX 12 when "DirectX 12 on Proton / Wine" is on
        // Job 643: Automatic = DirectX 12 on a Mac (Vulkan can't start there: CrossOver has 1.2, the renderer needs 1.3,
        // GitHub #36 / #40), Vulkan on SteamOS / Linux (DirectX 12 there still being tested).
        const int wapi = s.i("WineApi");
        const bool wineDx = env.wine && s.i("Api") == 0 && (wapi == 2 || (wapi == 0 && env.mac));
        const bool vk = runs_vulkan(s, env);
        if (env.wine && s.i("Api") != 1) notes.push_back(wineDx ? (s.i("WineApi") == 0 ? "launcher: Mac (CrossOver) - DirectX 12 (Automatic: Vulkan can't start there)"
                                                                                : "launcher: Proton / Wine - DirectX 12 (testing, chosen in the launcher)")
                                                               : "launcher: Proton / Wine - Vulkan (Graphics API on Proton / Wine: DirectX 12 is being tested)");
        if (vk) {   // job 521: Vulkan (experimental); the upscalers are DirectX 12 only
            a.push_back("--gow2_renderer_api=vulkan");
            // Notes/80 s.7: SMAA T2x (5) is ours - built-in shaders, both APIs
            // fsr049: FSR has a Vulkan upscaler (AMD FSR SDK 2.4) when the exe and its DLLs are new enough
            // review: opt-in - only with "FSR on Vulkan (experimental)" on; otherwise SMAA as before 0.4.9
            if (aa == 2 && fsr_vulkan_on(s, env)) {
                a.push_back("--gow2_fsr_vulkan=true");
                notes.push_back("launcher: Vulkan - FSR (AMD FSR SDK 2.4 Vulkan upscaler, experimental)");
            } else if (aa >= 2 && aa <= 4) {
                notes.push_back(std::string("launcher: Vulkan - ") + k_aaNames[aa] +
                                (aa == 2 ? (fsr_vulkan(env) ? " on Vulkan is experimental (FSR on Vulkan box off)" : " on Vulkan needs a newer build")
                                         : " is DirectX 12 only") + ", SMAA instead");
                aa = 1;
            }
        }
        switch (aa) {
        case 1: a.push_back("--gow2_present_aa=smaa"); break;
        case 2: a.push_back("--gow2_upscaler=fsr"); break;
        case 3: {
            a.push_back("--gow2_upscaler=dlss");
            // 0.4.9: the DLSS model; Automatic passes nothing (the exe's default = NVIDIA's table)
            static const char* const k_model[5] = { "", "J", "K", "L", "M" };
            const int m = s.i("DlssModel");
            if (m >= 1 && m <= 4 && avail("DlssModel")) a.push_back(std::string("--gow2_dlss_preset=") + k_model[m]);
            break;
        }
        case 4: a.push_back("--gow2_upscaler=xess"); break;
        case 5: a.push_back("--gow2_upscaler=smaa_t2x"); break;   // notes/80 s.7
        default: break;
        }
    }
    // PS1 856 (patch 0142): scene resolution, with an upscaler that is really used
    {
        static const char* const modes[4] = { "", "--gow2_upscale_mode=quality", "--gow2_upscale_mode=performance", "--gow2_upscale_mode=ultra" };
        int m = s.i("SceneMode");
        if (aa >= 2 && aa <= 4 && avail("SceneMode") && m > 0 && m < 4) a.push_back(modes[m]);
        // to-do 18 (GitHub #98): with the scene at full size the upscaler only anti-aliases (DLSS = DLAA, FSR / XeSS =
        // native AA) - say so, so nobody expects more frames from it
        if (aa >= 2 && aa <= 4 && !(avail("SceneMode") && m > 0 && m < 4)) {
            static const char* const aaOnly[5] = { "", "", "FSR native AA", "DLAA", "XeSS native AA" };
            notes.push_back(std::string("launcher: ") + k_aaNames[aa] + " with Scene resolution Native runs as " + aaOnly[aa] +
                            " - anti-aliasing only, no extra frames; pick a lower Scene resolution for more frames (GitHub #98)");
        }
        // GitHub #98 (Gears 1 "DLSS" runs were DLAA): the scene scale is a whole number of times smaller than the render
        // resolution only for Performance at 2x / 4x and Quality / Ultra performance at 3x; any other pairing (and every
        // 720p or "1080p" render resolution, both drawn at 1x) runs as native anti-aliasing. Say so before launch. Fixed
        // scales only - "Match my monitor" picks its scale in the game (its log names the result).
        if (aa >= 2 && aa <= 4 && avail("SceneMode") && m > 0 && m < 4 && s.i("Resolution") + 1 != 5) {
            const int out = scale;
            const bool fits = (m == 2 && (out == 2 || out == 4)) || ((m == 1 || m == 3) && out == 3);
            if (!fits) {
                static const char* const mname[4] = { "", "Quality", "Performance", "Ultra performance" };
                notes.push_back(std::string("launcher: Scene resolution ") + mname[m] + " has no whole-number scene size at render "
                                "resolution " + std::to_string(out) + "x - the scene stays at full size and " + k_aaNames[aa] +
                                " only anti-aliases (Performance needs 2x or 4x; Quality and Ultra performance need 3x) (GitHub #98)");
            }
        }
    }
    // PS1 857 (0146 / 0147): frame generation
    // FrameGen: 1 AMD FSR; 2-5 NVIDIA DLSS 2x / 3x / 4x / dynamic, 6 NVIDIA DLSS 6x (0.4.9); 7 Intel XeSS 2x (0.4.9,
    // experimental) (DLSS / XeSS fall back to FSR, or off, when not available here)
    // job 521: DirectX 12 (not under Proton / Wine); notes/83: AMD's on Vulkan too, Proton / Wine included (Steam Deck)
    const bool fgVk = runs_vulkan(s, env);
    int fgChoice = avail("FrameGen") && (fgVk || (s.i("Api") == 0 && !env.wine)) ? s.i("FrameGen") : 0;
    if (fgVk && fgChoice >= 2) {
        notes.push_back(std::string("launcher: Vulkan - ") + (fgChoice == k_fgXefg ? "Intel XeSS" : "NVIDIA DLSS") +
                        " frame generation is DirectX 12 only -> AMD FSR frame generation (experimental on Vulkan)");
        fgChoice = 1;
    }
    if (fgChoice >= 2 && !framegen_choice_available(fgChoice, env).ok) {
        const char* what = fgChoice == k_fgXefg ? "Intel XeSS" : "NVIDIA DLSS";
        fgChoice = cap_file(env, "amd_fidelityfx_framegeneration_dx12.dll") ? 1 : 0;
        notes.push_back(std::string("launcher: ") + what + " frame generation is not available here -> " + (fgChoice ? "AMD FSR frame generation" : "off"));
    }
    if (fgChoice == 1 && !framegen_choice_available(1, env, fgVk).ok) {
        if (fgVk) notes.push_back("launcher: Vulkan - AMD FSR frame generation is not available here (" +
                                  framegen_choice_available(1, env, true).why + ") -> off");
        fgChoice = 0;
    }
    const bool fgOn = fgChoice >= 1;
    // frames on screen per frame drawn (the frame-rate cap below); dynamic varies - counted as 2x; Intel XeSS: 2x
    const int fgFactor = fgChoice == 3 ? 3 : fgChoice == 4 ? 4 : fgChoice == 6 ? 6 : 2;
    if (fgOn) {
        if (fgChoice == k_fgXefg) {
            a.push_back("--gow2_frame_generation=xess");   // 0.4.9 (xefg049): 2x with XeLL; the exe falls back to FSR itself
        } else if (framegen_is_dlss(fgChoice)) {
            static const char* const k_dlssMode[5] = { "2x", "3x", "4x", "dynamic", "6x" };   // 0.4.9: 6x
            a.push_back("--gow2_frame_generation=dlss");
            a.push_back(std::string("--gow2_dlss_fg_mode=") + k_dlssMode[fgChoice - 2]);
        } else {
            a.push_back("--gow2_frame_generation=fsr");
            // Judgment on Vulkan froze inside AMD's swap chain with its own composition (FG-off and compose=game
            // runs did not freeze, Shagg 2026-11-07) - the game composes the HUD there
            if (fgVk && g == Game::Judgment && cap_exe(env, "gow2_fg_vk_compose")) a.push_back("--gow2_fg_vk_compose=game");
        }
        // 237f736 (Shagg: no smearing in Gears 2): generation from the HUD-less colour, with per-object motion; the
        // character-motion pass reads Gears 2's velocity-pass layout (c5-c12), so it is turned on for Gears 2 only
        if (cap_exe(env, "gow2_frame_generation_hudless")) a.push_back("--gow2_frame_generation_hudless=true");  // notes/83: Vulkan too
        // notes/83 (job 1385): on Vulkan the object-motion re-draw cost ~17 ms a frame and gave bad vectors (31 resets,
        // 393 px/frame centre motion; off: 7.5 ms, 0 resets) - off there for now, also over Gears 3 / Judgment's default
        if (cap_exe(env, "gow2_upscale_object_motion"))
            a.push_back(fgVk ? "--gow2_upscale_object_motion=false" : "--gow2_upscale_object_motion=true");
        if (g == Game::Gears2 && cap_exe(env, "gow2_upscale_character_motion"))
            a.push_back("--gow2_upscale_character_motion=true");
    }
    // PR #43 / to-do 23: real (strict) occlusion queries, a fixed value per game (no player setting any more): on in
    // Gears 2, Gears 3 and Judgment on DirectX 12 and not under Wine / Proton; off in Gears 1 (title-screen freeze, 0.3.0)
    // and for a Gears 2 exe without strict queries. The exe's own default is off.
    const bool realOcclusion = s.i("Api") == 0 && !env.wine && g != Game::Gears1 &&
                               (g != Game::Gears2 || cap_exe(env, "gow2_renderer_queries"));
    if (realOcclusion) a.push_back("--gow2_renderer_queries=strict");
    // Notes/80: XeGTAO (2 high, 3 ultra); notes/81: CACAO (4); 0 Off goes through the game's own [SystemSettings] below,
    // 1 changes nothing
    if (avail("AO") && s.i("AO") >= 2) {
        if (s.i("AO") == 4 && !cap_exe(env, "or cacao (FidelityFX CACAO")) {
            notes.push_back("launcher: CACAO is not in this build - the game's own AO");
        } else {
            a.push_back("--gow2_ao=gtao");
            if (s.i("AO") == 4) {   // low, with the blur passes that looked best in play
                a.push_back("--gow2_ao_algo=cacao");
                a.push_back("--gow2_ao_quality=low");
                a.push_back("--gow2_ao_cacao_blur=3");
            } else {
                a.push_back(s.i("AO") == 3 ? "--gow2_ao_quality=ultra" : "--gow2_ao_quality=high");
                // Shagg's to-do 1(d) (XeGTAO ~5 ms/frame): High denoises twice (medium); Ultra leaves it to the game's
                // automatic choice (#204: 2 without DLSS / FSR / XeSS, by the scene scale with one)
                if (s.i("AO") == 2) a.push_back("--gow2_ao_denoise=2");
            }
        }
    }
    // Notes/80 s.16: PCSS presets (Gears 3)
    if (avail("SoftShadows") && s.i("SoftShadows") >= 1) a.push_back(s.i("SoftShadows") == 2 ? "--gow2_pcss=ultra" : "--gow2_pcss=high");
    // PS1 858: 16x anisotropic
    if (on("Aniso")) a.push_back(gi == 0 ? "--gow2_patch_aniso16=true" : gi == 1 ? "--gow3_patch_aniso16=true" : isG1 ? "--gow1_aniso16=true" : "--gowj_aniso16=true");
    // PS1 860-864: texture pool (-1 = the port's own default; Judgment then 200)
    if (avail("Pool")) {
        int mb = k_poolMb[std::max(0, std::min(3, s.i("Pool")))];
        if (mb < 0 && isGJ) mb = 200;
        if (mb < 0 && isG1) mb = 196;   // as Gears 2 (the same 140 MB pool on the disc)
        if (mb >= 0) {
            static const char* const sw[kGameCount] = { "--gow2_patch_texture_pool_mb=", "--gow3_texture_pool_mb=", "--gowj_texture_pool_mb=",
                                                        "--gow1_texture_pool_mb=" };
            a.push_back(sw[gi] + std::to_string(mb));
        }
    }
    // PS1 866-867: Gears 3 / Judgment trilinear and fast mip fade (on by default there, so always sent)
    if (isG3) { a.push_back("--gow3_trilinear=" + fmt_bool(s.b("Trilinear"))); a.push_back("--gow3_fast_mip_fade=" + fmt_bool(s.b("MipFade"))); }
    // 0.3.0 (GitHub #54, soldiermxdeath: Monroe Commons' loading screen ends at 30 fps, not at 60): loading screens at 30
    if (isGJ) a.push_back("--gowj_loading_fps=30");
    // GitHub #105 (0.4.8): Judgment's post chain has Gears 3's half-size 640-pitch targets and a quarter-size 320-pitch
    // set (colour, 16-bit and its depth); both stay 1x under the renderer's default 1120 threshold otherwise
    if (isGJ && scale > 1 && avail("DofScale") && on("DofScale")) a.push_back("--gow2_renderer_resolution_scale_pitches=640,320");
    if (isGJ) { a.push_back("--gowj_trilinear=" + fmt_bool(s.b("Trilinear"))); a.push_back("--gowj_fast_mip_fade=" + fmt_bool(s.b("MipFade"))); }
    if (isG1) {   // only when gow1.exe has the switch (can(): trilinear / mipfade)
        if (avail("Trilinear")) a.push_back("--gow1_trilinear=" + fmt_bool(s.b("Trilinear")));
        if (avail("MipFade")) a.push_back("--gow1_fast_mip_fade=" + fmt_bool(s.b("MipFade")));
    }
    // PS1 869-871: cheats, mutators (level select removed, Shagg 31 Oct)
    if (on("Cheats")) a.push_back("--gow3_cheats=true");
    if (on("Mutators")) a.push_back("--gow3_unlock_mutators=true");
    // PS1 873-898: the game's own engine settings through --ue3_ini_edits
    {
        const std::string ei = k_engineIni[gi];
        std::vector<std::string> ini;
        if (isG1) {   // GitHub #179: no [SystemSettings] in Gears 1 - Canary's patches as switches
            if (avail("MotionBlur") && !s.b("MotionBlur")) a.push_back("--gow1_no_motion_blur=true");
            if (avail("DoF") && !s.b("DoF")) a.push_back("--gow1_no_gaussian_blur=true");
        } else {
            if (avail("MotionBlur") && !s.b("MotionBlur")) ini.push_back(ei + "|SystemSettings|MotionBlur=False");
            if (avail("DoF") && !s.b("DoF")) ini.push_back(ei + "|SystemSettings|DepthOfField=False");
            if (avail("Bloom") && !s.b("Bloom")) ini.push_back(ei + "|SystemSettings|Bloom=False");
        }
        if (avail("FilmGrain") && !s.b("FilmGrain")) a.push_back("--gowj_no_film_grain=true");
        // GitHub #102: lens flares are not a player setting. Flares are hidden behind walls only by the game's occlusion
        // queries, which only Real occlusion answers (DirectX 12, not under Wine / Proton); without them every flare shows
        // through the scenery, so they are off for that launch. Gears 1's engine has no lens-flare switch.
        if (!isG1) {
            const bool occlusion = realOcclusion;   // to-do 23: the fixed per-game value above
            if (!occlusion) {
                ini.push_back(ei + "|SystemSettings|LensFlares=False");
                notes.push_back("launcher: lens flares off for this launch - no Real occlusion (DirectX 12) to hide them behind walls (GitHub #102)");
            }
        }
        // Notes/80; notes/81: Judgment's own AO goes whenever ours is on - ours is added, not swapped in
        if (avail("AO") && g != Game::Gears1 && (s.i("AO") == 0 || (g == Game::Judgment && s.i("AO") >= 2)))  // Gears 1 has none
            ini.push_back(ei + "|SystemSettings|AmbientOcclusion=False");
        if (avail("Fov") && s.i("Fov") != 100) {
            std::string k = fmt_double(static_cast<double>(s.i("Fov")) / 100);
            ini.push_back(std::string(isG1 ? "WarCamera.ini" : "GearCamera.ini") + "|*|FOVAngle*=" + k);
            if (g == Game::Gears2) ini.push_back("GearPawn.ini|*|DefaultFOV*=" + k);
        }
        if (on("SkipLogos")) {   // PS1 882-887: only the logo films; Judgment has its own switch
            if (g == Game::Gears2) { ini.push_back(ei + "|FullScreenMovie|-StartupMovies=EpicLogo"); ini.push_back(ei + "|FullScreenMovie|-StartupMovies=MGSLogo"); }
            else if (isG3) ini.push_back(ei + "|FullScreenMovie|-StartupMovies=Open_Logos");
            else if (isG1) a.push_back("--gow1_skip_logos=true");
            else a.push_back("--gowj_skip_logos=true");
        }
        if (avail("MeshLod") && s.i("MeshLod") > 0)   // GitHub #113
            ini.push_back(ei + "|SystemSettings|SkeletalMeshLODBias=" + (s.i("MeshLod") >= 2 ? "-3" : "-1"));
        int dd = std::max(0, std::min(2, s.i("DrawDist")));
        if (avail("DrawDist") && dd > 0) ini.push_back(ei + "|SystemSettings|MaxDrawDistanceScale=" + k_drawScale[dd]);
        int sh = std::max(0, std::min(2, s.i("Shake")));
        if (avail("Shake") && sh > 0) {   // PS1 889-896
            std::string v = k_shakeValue[sh];
            if (g == Game::Gears2) ini.push_back("GearCamera.ini|GearGame.GearCamMod_CameraBone|RoadieRunShakyCamDampeningFactor" + v);
            else if (isG1) ini.push_back("WarCamera.ini|WarfareGame.WarCamMod_CameraBone|RoadieRunShakyCamDampeningFactor" + v);
            else if (isG3) { for (const char* pf : { "GearPawn.ini", "GearPawnMP.ini" }) ini.push_back(std::string(pf) + "|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor" + v); }
            else { for (const char* pf : { "GearPawn.ini", "GearPawnMP.ini", "GearPawnFFA.ini" }) ini.push_back(std::string(pf) + "|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor" + v); }
        }
        if (g == Game::Gears2 && on("Trilinear")) ini.push_back(ei + "|SystemSettings|~=MipFilter=point>MipFilter=linear");   // PS1 897
        // 0.2.0 mouse look (GitHub #2): the mouse reaches the game as the right stick. The game's look binding has a dead
        // zone (Gears 2 30 %, Judgment 20 %) that swallows small mouse movements, and its Speed caps the turn rate at a
        // full stick. With keyboard and mouse on, the binding gets no dead zone and k_lookBoost x the speed; the mouse is
        // scaled down by the same factor (same feel, 4x the ceiling) and a controller used alongside gets both undone by
        // the SDK (patch 38: pad_look_scale / pad_look_deadzone). Each game's own binding text, from its Coalesced_int.
        // GitHub #95: with the radial dead zone on, Gears 1 and 2's look binding loses its own per-axis dead zone (the
        // same binding text the mouse-look edits below replace; Speed unchanged). Not with mouseLook, which does it too.
        if (!mouseLook && avail("RadialDZ") && s.i("RadialDZ") > 0 && (g == Game::Gears2 || isG1)) {
            if (g == Game::Gears2) {
                const std::string fi = "GearInput.ini|Engine.PlayerInput|~=";
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.3>Axis aTurn Speed=1.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=0.75 DeadZone=0.3>Axis aLookup Speed=0.75 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=-0.75 DeadZone=0.3>Axis aLookup Speed=-0.75 DeadZone=0.0");
            } else {
                const std::string fi = "WarInput.ini|Engine.PlayerInput|~=";
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.2>Axis aTurn Speed=1.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=0.8 DeadZone=0.2>Axis aLookup Speed=0.8 DeadZone=0.0");
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.3>Axis aTurn Speed=1.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=0.75 DeadZone=0.3>Axis aLookup Speed=0.75 DeadZone=0.0");
            }
        }
        if (mouseLook) {
            const std::string fi = "GearInput.ini|Engine.PlayerInput|~=";
            if (g == Game::Gears2) {
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.3>Axis aTurn Speed=4.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=0.75 DeadZone=0.3>Axis aLookup Speed=3.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=-0.75 DeadZone=0.3>Axis aLookup Speed=-3.0 DeadZone=0.0");
            } else if (isG1) {   // WarInput.ini / Xe-WarInput.ini: two sets (0.2 and 0.3 dead zones)
                ini.push_back("WarInput.ini|Engine.PlayerInput|~=Axis aTurn Speed=1.0 DeadZone=0.2>Axis aTurn Speed=4.0 DeadZone=0.0");
                ini.push_back("WarInput.ini|Engine.PlayerInput|~=Axis aLookup Speed=0.8 DeadZone=0.2>Axis aLookup Speed=3.2 DeadZone=0.0");
                ini.push_back("WarInput.ini|Engine.PlayerInput|~=Axis aTurn Speed=1.0 DeadZone=0.3>Axis aTurn Speed=4.0 DeadZone=0.0");
                ini.push_back("WarInput.ini|Engine.PlayerInput|~=Axis aLookup Speed=0.75 DeadZone=0.3>Axis aLookup Speed=3.0 DeadZone=0.0");
            } else if (isG3) {
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.0>Axis aTurn Speed=4.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=1.0 DeadZone=0.0>Axis aLookup Speed=4.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=-0.75 DeadZone=0.3>Axis aLookup Speed=-3.0 DeadZone=0.0");
            } else {
                ini.push_back(fi + "Axis aTurn Speed=1.0 DeadZone=0.2>Axis aTurn Speed=4.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=1.0 DeadZone=0.2>Axis aLookup Speed=4.0 DeadZone=0.0");
                ini.push_back(fi + "Axis aLookup Speed=-1.0 DeadZone=0.2>Axis aLookup Speed=-4.0 DeadZone=0.0");
            }
        }
        // GitHub #118: with native mouse look the turn speed changed with the frame rate (an RTSS cap changed in game
        // turned faster or slower; one set before start-up did not). UE3's PlayerInput.SmoothMouse scales each frame's
        // aMouseX/Y by DeltaTime / (MouseSamplingTotal / MouseSamples) - the average frame time since start-up - so a
        // frame rate different from the history's average turns by the ratio. Mouse smoothing off (as the PC games
        // allow in their Input.ini, and as Judgment's own KBM work did) passes the counts through unscaled.
        if (nativeMouse && (!isG1 || cap_exe(env, "gow1_ini_edits"))) {
            const std::string pi = isG1 ? "WarInput.ini" : "GearInput.ini";
            ini.push_back(pi + "|*|bEnableMouseSmoothing=FALSE");                 // wherever the game's own ini sets it
            ini.push_back(pi + "|Engine.PlayerInput|bEnableMouseSmoothing=FALSE");  // and added when it does not
        }
        if (!ini.empty()) {
            std::string j;
            for (size_t i = 0; i < ini.size(); ++i) j += (i ? ";" : "") + ini[i];
            a.push_back("--ue3_ini_edits=" + q(j));
        }
    }
    // PS1 899: ultrawide
    // GitHub #60: the picture widens to the monitor's own shape (21:9, 32:9...), not a fixed 21:9, when the build can
    if (on("Ultrawide")) {
        const char *k = isG3 ? "gow3_aspect" : isGJ ? "gowj_aspect" : isG1 ? "gow1_aspect" : "gow2_aspect";
        if (cap_exe(env, k)) a.push_back(std::string("--") + k + "=auto");
    }
    if (on("Ultrawide")) a.push_back(isG3 ? "--gow3_ultrawide=true" : isGJ ? "--gowj_ultrawide=true" : isG1 ? "--gow1_ultrawide=true" : "--gow2_ultrawide=true");
    // 0.4.9: 4:3 (SDK patch 70). Gears 2 / 3 / Judgment make a 4:3 back buffer (720 x w / h wide) only for a mode that is
    // not widescreen and at most 720 lines, so the runtime reports a 960x720 4:3 mode; the presenter fits it to the screen.
    if (avail("AspectRatio") && !on("Ultrawide")) {
        const int ar = s.i("AspectRatio");
        // Review: while 4:3 is untested, Auto means 16:9 - 4:3 only on an explicit choice. When it is trusted, Auto can
        // pick it from the monitor: kAuto43 = true, a landscape screen (w >= h) of at most 1.4:1 (4:3 / 5:4); a portrait
        // screen never counts. env.monitor_w / monitor_h are kept for that.
        constexpr bool kAuto43 = false;
        const bool screen43 = kAuto43 && env.monitor_w > 0 && env.monitor_h > 0 && env.monitor_w >= env.monitor_h &&
                              env.monitor_w * 10 <= env.monitor_h * 14;   // landscape, <= 1.4:1
        if (ar == 2 || (ar == 0 && screen43)) {
            a.push_back("--video_mode_width=960");
            a.push_back("--video_mode_height=720");
            a.push_back("--video_mode_widescreen=false");
            if (ar == 0) notes.push_back("launcher: Aspect ratio Auto: " + std::to_string(env.monitor_w) + "x" + std::to_string(env.monitor_h) + " screen -> 4:3");
        }
    }
    // GitHub #185: launcher Gamma -> the display ramp
    if (avail("Brightness") && s.i("Brightness") != 100) {   // display-ramp gamma = 100 / Brightness (80 -> 1.25, darker)
        char g[32];
        std::snprintf(g, sizeof(g), "%.3f", 100.0 / std::max(s.i("Brightness"), 10));
        a.push_back(std::string("--gow2_present_gamma=") + g);
    }
    // PS1 900: HDR
    if (s.b("HDR")) {
        a.push_back("--present_hdr=true");
        a.push_back("--gow2_hdr_paper_white=" + std::to_string(s.i("PaperWhite")));
        a.push_back("--gow2_hdr_peak=" + std::to_string(s.i("Peak")));
    }
    // PS1 901-902: fullscreen / window size
    // job 523: under Wine / Proton the compositor (gamescope on a Steam Deck) owns the screen - no exclusive fullscreen
    if (s.i("Fullscreen") >= 1) { a.push_back("--fullscreen=true"); if (s.i("Fullscreen") == 2 && !env.wine) a.push_back("--fullscreen_exclusive=true"); }
    else {
        a.push_back("--fullscreen=false");   // GitHub #81/#67: the ports default to full screen; windowed must say so
        int w = std::max(0, std::min(5, s.i("WindowSize")));
        if (w > 0) { a.push_back("--window_width=" + std::to_string(k_winSizes[w][0])); a.push_back("--window_height=" + std::to_string(k_winSizes[w][1])); }
    }
    // PS1 904-908: stick dead zone, vibration
    {
        int d = std::max(0, std::min(5, s.i("DeadZone")));
        if (d > 0) { std::string dz = fmt_double(k_deadValue[d]); a.push_back("--left_stick_deadzone_percentage=" + dz); a.push_back("--right_stick_deadzone_percentage=" + dz); }
    }
    if (!s.b("Vibration")) a.push_back("--vibration=false");
    // GitHub #95 / #97 (SDK patch 55): radial dead zone and controller layout
    if (avail("RadialDZ")) {
        static const double k_radial[6] = { 0, 0.08, 0.12, 0.16, 0.20, 0.25 };
        const int r = std::max(0, std::min(5, s.i("RadialDZ")));
        if (r > 0) a.push_back("--pad_radial_deadzone=" + fmt_double(k_radial[r]));
    }
    if (avail("PadLayout")) {
        static const char* k_layouts[4] = { "", "sticks=swap", "a=b,b=a", "lb=lt,lt=lb,rb=rt,rt=rb" };
        const int l = std::max(0, std::min(4, s.i("PadLayout")));
        std::string spec = l < 4 ? k_layouts[l] : env.pad_remap;
        std::string clean;
        for (char c : spec) if (std::isalnum(static_cast<unsigned char>(c)) || c == '=' || c == ',') clean += c;
        if (!clean.empty()) a.push_back("--pad_remap=" + clean);
    }
    // PS1 911-925 (patches 0133 / 0135 / 0136 / 0152): lower input lag; Reflex / Anti-Lag 2 by the main card's maker
    if (avail("LowLag")) {
        const std::string gpu = gpu_main(env);
        if (s.b("LowLag")) {
            a.push_back("--gow2_renderer_frames_in_flight=1");
            const bool dx = s.i("Api") == 0 && !env.wine;   // job 521: Reflex / Anti-Lag 2 are DirectX 12 here; job 523: not under Wine
            if (dx && icase_search(gpu, "NVIDIA|GeForce|Quadro") && cap_exe(env, "gow2_reflex")) a.push_back("--gow2_reflex=true");
            if (dx && icase_search(gpu, "AMD|Radeon") && cap_exe(env, "gow2_antilag2")) a.push_back("--gow2_antilag2=true");
        } else {
            if (cap_exe(env, "gow2_renderer_frames_in_flight")) a.push_back("--gow2_renderer_frames_in_flight=2");
            if (cap_rt(env, "d3d12_max_frame_latency")) a.push_back("--d3d12_max_frame_latency=0");
        }
    }
    // PS1 926-928: mute, monitor, players
    if (on("Mute")) a.push_back(std::string("--") + k_procNames[gi] + "_mute_unfocused=true");
    {   // 0.2.0 (#4, Shagg 12:55: every language the US and European releases use): the console's language AND country.
        // UE3 picks the game's language files from both - Spain vs Latin American Spanish by country, and the languages
        // the Xbox had no system language for (Czech, Hungarian, Danish, Finnish) from English + that country.
        // Xbox language IDs: 1 en, 2 ja, 3 de, 4 fr, 5 es, 6 it, 7 ko, 8 zh-Hant, 9 pt, 11 pl, 12 ru, 13 sv, 15 nb, 16 nl,
        // 17 zh-Hans. Country IDs (the SDK's user_country table): 13 BR, 20 CN, 23 CZ, 24 DE, 25 DK, 31 ES, 32 FI, 34 FR,
        // 35 GB, 42 HU, 50 IT, 53 JP, 56 KR, 71 MX, 74 NL, 75 NO, 82 PL, 88 RU, 90 SE, 101 TW, 103 US.
        static const int lang[22] = { 0, 1, 1, 4, 3, 6, 5, 5, 9, 11, 12, 1, 1, 16, 1, 1, 15, 13, 2, 7, 8, 10 };   // job 666: Chinese = 10 (job 664)
        static const int country[22] = { 0, 103, 35, 34, 24, 50, 31, 71, 13, 82, 88, 23, 42, 74, 25, 32, 75, 90, 53, 56, 101, 20 };
        const int l = std::max(0, std::min(21, s.i("Lang")));
        const Option* lo = find_option(g, "Lang");
        const bool missing = l < static_cast<int>(env.lang_missing.size()) && env.lang_missing[l];   // GitHub #159
        if (l > 0 && avail("Lang") && lo && choice_available(*lo, g, l).ok && missing)
            notes.push_back("launcher: Game language " + lo->choices[l] + " is not on this copy of the game - the game's own default instead");
        if (l > 0 && avail("Lang") && lo && choice_available(*lo, g, l).ok && !missing) {
            a.push_back("--user_language=" + std::to_string(lang[l]));
            a.push_back("--user_country=" + std::to_string(country[l]));
            // GitHub #76: Gears 1 plays Latin American Spanish only for a North American game region (SDK patch 47)
            if (isG1 && l == 7 && cap_rt(env, "user_game_region")) a.push_back("--user_game_region=255");
        }
    }
    if (avail("VoiceLang") && s.i("VoiceLang") > 0) {   // GitHub #87
        static const char* k_voice[10] = { "", "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "POL", "RUS", "JPN" };
        const int v = std::max(0, std::min(9, s.i("VoiceLang")));
        if (v < static_cast<int>(env.voice_missing.size()) && env.voice_missing[v]) {   // GitHub #159
            const Option* vo = find_option(g, "VoiceLang");
            if (vo) notes.push_back("launcher: Voice language " + vo->choices[v] + " is not on this copy of the game - the game language's voices instead");
        } else {
            a.push_back(std::string("--ue3_voice_language=") + k_voice[v]);
        }
    }
    if (s.i("Monitor") > 0) a.push_back("--monitor=" + std::to_string(s.i("Monitor")));
    if (avail("Players") && s.i("Players") > 0) a.push_back("--local_players=" + std::to_string(s.i("Players") + 1));
    if (g == Game::Gears2 && avail("SplitLayout") && s.i("SplitLayout") == 1)   // GitHub #237: press Y in game
        a.push_back("--gow2_split_vertical=true");
    else if (avail("SplitLayout") && s.i("SplitLayout") == 1)   // GitHub #136 (avail: Gears 3, 2 players)
        // GitHub #208: one run at the front end was undone when a level started - again at every level where the exe can
        a.push_back((cap_exe(env, "ue3_exec_each_level") ? "--ue3_exec_each_level=" : "--ue3_exec=") +
                    q("set GameViewportClient DesiredSplitscreenType eSST_2P_VERTICAL"));
    if (on("SystemLink")) a.push_back("--net_system_link=true");
    // GitHub #229: the friends' addresses (sanitised: digits, dots and commas only - nothing that needs quoting)
    if (on("SystemLink") && avail("PeerIps")) {
        const Option* po = find_option(g, "PeerIps");
        std::string peers = po ? sanitize_text(*po, s.str("PeerIps")) : std::string();
        peers.erase(std::remove(peers.begin(), peers.end(), ' '), peers.end());
        if (!peers.empty()) a.push_back("--net_peer_ip=" + peers);
    }
    // GitHub #119: the gamertag the games show for player 1 (CogBox.ini [Profile] Gamertag=). Only with a runtime that
    // has SDK patch 54; it keeps 15 letters / digits / spaces. The profile's save folder is not renamed.
    // 0.4.0: the same rules as the runtime's Player1Gamertag - ASCII letters / digits and single spaces, trimmed, 15 max.
    auto clean_tag = [](const std::string& in) {
        std::string t;
        for (char c : in) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
            if (ok && !(c == ' ' && (t.empty() || t.back() == ' '))) t += c;
            if (t.size() == 15) break;
        }
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    };
    if (!env.gamertag.empty() && cap_rt(env, "user_gamertag")) {
        const std::string t = clean_tag(env.gamertag);
        if (!t.empty()) a.push_back("--user_gamertag=" + q(t));
    }   // 0.3.5: off = not sent (the runtime's default)
    // 0.4.9 split-screen: players 2-4's names ([Profile] Gamertag2..4), only for the players this play has and only with
    // a runtime that has SDK patch 71; empty = not sent (the runtime's "Player N")
    if (avail("Players") && s.i("Players") > 0 && cap_rt(env, "user_gamertag2"))
        for (int k = 0; k < s.i("Players") && k < 3; k++) {
            const std::string t = clean_tag(env.gamertags[k]);
            if (!t.empty()) a.push_back("--user_gamertag" + std::to_string(k + 2) + "=" + q(t));
        }
    // GitHub #121: only an exe whose layer 0 has the switch (an older one would refuse the unknown flag)
    if (s.b("HideNotes") && cap_exe(env, "gow2_hide_onscreen_notes")) a.push_back("--gow2_hide_onscreen_notes=true");
    // Achievements part 2: the popups (only an exe that has them; an older one would refuse the unknown options)
    if (cap_exe(env, "COGBox achievement popups")) {
        static const char* const styles[] = { "x360", "xbox", "ps5" };
        static const char* const colours[] = { "dark", "red", "green", "blue", "purple", "orange", "pink", "teal", "grey", "gold" };
        static const char* const positions[] = { "default", "top_left", "top_centre", "top_right", "bottom_left",
                                                 "bottom_centre", "bottom_right" };
        auto pick = [&](const char* id, const char* const* names, int count) {
            const int i = s.i(id);
            return names[i >= 0 && i < count ? i : 0];
        };
        a.push_back(std::string("--cogbox_ach_style=") + pick("AchievementStyle", styles, 3));
        a.push_back(std::string("--cogbox_ach_colour=") + pick("AchievementColour", colours, 10));
        a.push_back(std::string("--cogbox_ach_position=") + pick("AchievementPosition", positions, 7));
        a.push_back(std::string("--cogbox_ach_sound=") + (s.b("AchievementSound") ? "true" : "false"));
        a.push_back(std::string("--cogbox_ach_hide=") + (s.b("HideAchievementPopups") ? "true" : "false"));
    }
    // PS1 929: FPS counter
    switch (s.i("FpsMode")) {
    case 1: a.push_back("--gow2_fps_bar=top"); break;
    case 2: a.push_back("--gow2_fps_bar=bottom"); break;
    case 3: if (!isGJ && !isG1) a.push_back(isG3 ? "--gow3_patch_show_fps=true" : "--gow2_patch_show_fps=true"); break;
    default: break;
    }
    // PS1 930-937: keyboard and mouse with the Gears layout
    if (s.b("Mnk")) {
        // 0.2.0 mouse look: with the look binding boosted (above), the mouse is scaled down by the same factor and a
        // controller gets the game's own dead zone back (SDK patch 38)
        const double boost = mouseLook ? 4.0 : 1.0;
        a.insert(a.end(), { "--mnk_mode=true", "--mnk_mouse=true", "--mnk_sensitivity=" + fmt_double(s.f("Sens") * 2.0 / boost) });
        if (nativeMouse) a.insert(a.end(), { "--ue3_mouse_look=true", "--ue3_mouse_scale=" + fmt_double(s.f("Sens")) });
        // GitHub #52: keep the turn speed per count the same whatever Field of view (%) is (UE3 scales the mouse by the
        // FOV). Only the FOV % here: the ultrawide patch (gow3_aspect) widens the projection, not FOVAngle, so
        // PlayerInput's FOVScale does not see it - check that in the aspect code before adding its factor.
        // GitHub #52: Gears 3 - the mouse turns the same angle per count in every camera mode unless the player keeps the
        // game's slower aiming / zoom turn ("Mouse turn when aiming": Same as running around / Game's (slower))
        if (nativeMouse && (isG3 || g == Game::Gears2) && s.i("AimTurn") == 1 && cap_exe(env, "ue3_mouse_fov_constant")) a.push_back("--ue3_mouse_fov_constant=false");
        if (nativeMouse && s.i("AimSens") != 100 && cap_exe(env, "ue3_mouse_aim_scale"))
            a.push_back("--ue3_mouse_aim_scale=" + fmt_double(s.i("AimSens") / 100.0));
        if (nativeMouse && s.i("ZoomSens") != 100 && cap_exe(env, "ue3_mouse_zoom_scale"))
            a.push_back("--ue3_mouse_zoom_scale=" + fmt_double(s.i("ZoomSens") / 100.0));
        if (nativeMouse && avail("Fov") && s.i("Fov") != 100 && cap_exe(env, "ue3_mouse_fov_comp"))
            a.push_back("--ue3_mouse_fov_comp=" + fmt_double(100.0 / static_cast<double>(s.i("Fov"))));
        if (mouseLook) {
            a.push_back("--pad_look_scale=0.25");
            a.push_back(std::string("--pad_look_deadzone=") + (g == Game::Gears2 ? "0.3" : isG3 ? "0" : "0.2"));
        }
        bool defaultKeys = true;
        for (const KeyAction &k : key_actions(g)) {   // 0.2.0: the Gears layout, or the player's own keys (Controls tab)
            auto it = env.keys.find(k.cvar);
            const std::string v = it != env.keys.end() && !it->second.empty() ? it->second : k.def;
            if (v != k.def) defaultKeys = false;
            if (std::string(k.cvar) == "keybind_walk") {   // GitHub #125: a runtime with SDK patch 60 only
                if (v != "None" && cap_rt(env, "keybind_walk")) {
                    a.push_back("--keybind_walk=" + v);
                    // 0.4.6 (Shagg 5 Nov, Gears 3): the runtime's 0.5 was a jog and 0.3 sat inside the game's stick dead
                    // zone (no movement); 0.4 walks ("works perfectly")
                    a.push_back("--mnk_walk_scale=0.4");
                    if (s.i("WalkMode") == 1) a.push_back("--mnk_walk_toggle=true");
                }
                continue;
            }
            a.push_back(std::string("--") + k.cvar + "=" + v);
        }
        // Job 629 (GitHub #41): keyboard button prompts - our key caps drawn over the game's controller-button sheet.
        // Job 661: drawn by the launcher at 4x for whatever keys are set (keycaps.cpp).
        // Gears 1 (GitHub #214): its button font and HUD sheets are in the runtime's prompt catalog now, so the game
        // switches them to PlayStation / Switch / Steam / key-cap art itself; the launcher's own 4x key caps (below) are
        // still Gears 2 / 3 / Judgment only (keycaps.cpp has no Gears 1 sheet).
        // GitHub #104 with PR #117's auto button prompts: keep the pad's prompts there too (no switch to key caps).
        // GitHub #249: manual prompt style (Automatic = the runtime's auto, nothing passed)
        if (s.i("PromptStyle") > 0 && s.i("PromptStyle") <= 4 && cap_exe(env, "gow_button_prompts")) {
            static const char* const k_prompt[5] = { "auto", "original", "playstation", "switch", "steamdeck" };
            a.push_back(std::string("--gow_button_prompts=") + k_prompt[s.i("PromptStyle")]);
        }
        if (s.b("PadPrompts") && cap_exe(env, "gow_button_prompts_keep_pad"))
            a.push_back("--gow_button_prompts_keep_pad=true");
        if (!isG1 && !s.b("PadPrompts") && !env.prompt_dir.empty() && cap_exe(env, "gow2_texture_overlay_dir")) {
            // Job 661 (Shagg 30 Oct: "make those prompts higher resolution"): the launcher draws the key caps itself, 4x the
            // sheet's size and for the player's own keys too (job 660); without it, the shipped default-key overlays.
            std::string dir;
            if (g_prompt_composer) {
                std::map<std::string, std::string> keys;
                for (const KeyAction &k : key_actions(g)) {
                    auto it = env.keys.find(k.cvar);
                    keys[k.cvar] = it != env.keys.end() && !it->second.empty() ? it->second : k.def;
                }
                static const char* const kGameDir[kGameCount] = { "gears2", "gears3", "judgment", "gears1" };
                // 0.5.0 Play hang: live_lines runs build_args ~460 times, and each run drew the 4x key caps again
                // (~280 ms each, ~130 s per Play with keyboard and mouse on). Drawn again only when the keys change:
                // the composer always writes the same folder (hires-<game>), so only the last keys drawn there count
                // (review: a key set changed and changed back must be drawn again).
                struct Drawn { std::string keys, dir; };
                static std::map<std::string, Drawn> s_drawn;   // prompt dir + game -> what that folder holds now
                const std::string folder = env.prompt_dir + "|" + kGameDir[gi];
                std::string ks;
                for (const auto &kv : keys) ks += kv.first + "=" + kv.second + "|";
                auto hit = s_drawn.find(folder);
                if (hit != s_drawn.end() && hit->second.keys == ks && !hit->second.dir.empty()) dir = hit->second.dir;
                else { dir = g_prompt_composer(env.prompt_dir, kGameDir[gi], keys); s_drawn[folder] = { ks, dir }; }
                notes.push_back(dir.empty() ? "launcher: key caps could not be drawn - the shipped ones"
                                            : "launcher: key caps drawn (4x) in " + dir);
            }
            if (dir.empty() && defaultKeys) dir = env.prompt_dir;
            if (!dir.empty()) a.push_back("--gow2_texture_overlay_dir=" + q(dir));
        }
        // 0.2.1 (quick win): move with WASD and the arrow keys alike; the arrows used to turn the camera (the mouse does)
        for (const char* m : { "keybind_lstick_up=W,Up", "keybind_lstick_down=S,Down", "keybind_lstick_left=A,Left", "keybind_lstick_right=D,Right",
                               "keybind_rstick_up=None", "keybind_rstick_down=None", "keybind_rstick_left=None", "keybind_rstick_right=None" })
            a.push_back(std::string("--") + m);
    }
    // Job 630 (Shagg 19:05): the shipped chapter recipes, built during the opening videos - every package of the game's
    // folders is read at start-up for their shaders (the player's own files; only the recipes' shaders are built).
    if (!env.recipe_pack.empty() && cap_exe(env, "ue3_shader_prebuild_at_start")) {
        a.push_back("--ue3_shader_prebuild=true");
        a.push_back("--gow2_renderer_recipe_pack=" + q(env.recipe_pack));
        std::string dirs = env.game_data_root;
        if (!env.update_data_root.empty()) dirs += ";" + env.update_data_root;
        a.push_back("--ue3_shader_prebuild_at_start=" + q(dirs));
    }
    // 0.2.0: mods - the enabled ones in load order (layer 2's --ue3_mods folder overlay)
    if (!env.mods.empty()) {
        std::string j;
        for (const std::string &m : env.mods) j += (j.empty() ? "" : ";") + m;
        a.push_back("--ue3_mods=" + q(j));
    }
    // PS1 940-958 (launcher patches 0155 / 0158): frame rate. 0.4.6 (Shagg 5 Nov: "FPS cap needs to be 60"): the cap the
    // player picks is the cap. With vsync, only a cap ABOVE the refresh rate (or Unlimited) is brought down - to the refresh
    // rate itself (half of it with frame generation), no longer to 3 under it (60 on a 60 Hz screen stays 60; 120 on a
    // 60 Hz screen becomes 60, not 57).
    {
        // Gears 1: no frame-rate limit until gow1.exe has gow1_max_fps (can() "framecap") - then the game's own rate
        const int fps = avail("FrameCap") ? std::max(0, std::min(k_fpsCapCount - 1, s.i("FrameCap"))) : 0;
        int cap = k_fpsCaps[fps];
        const bool vsync = on("Vsync");
        if (vsync && fps >= 1) {
            const int hz = env.refresh_hz;
            const int top = hz;   // the cap is on-screen fps (frame generation is halved below)
            // review 0.4.6: 60 on a 59 Hz mode (59.94 Hz TVs) is "at the refresh rate" too (GitHub #106) - kept
            if (hz >= 50 && (cap < 0 || cap > top + 1)) cap = top;
            // GitHub #225: Gears 3's 60 stays 60 with vsync too (62 on a 60 Hz screen ran at 62 with uneven pacing)
            notes.push_back("launcher: vsync on; " + env.monitor_device + " at " + std::to_string(hz) + " Hz (0 = unknown)" +
                            (fgOn ? ", frame generation on" : "") + " -> frame-rate cap " + std::to_string(cap));
        }
        // Shagg 7 Nov: with frame generation the cap is what's ON SCREEN - the game renders half of it (120 -> 60 + 60 generated);
        // a third / a quarter with DLSS 3x / 4x
        if (fgOn && cap > 0) {
            cap = std::max(1, cap / fgFactor);
            notes.push_back("launcher: frame generation on -> the game renders at most " + std::to_string(cap) + " fps (" +
                            std::to_string(cap * fgFactor) + " on screen)");
        }
        if (vsync) {
            a.push_back("--d3d12_present_vsync=true");
            // 0.3.0 (Shagg 14:09, Gears 1 on Vulkan ran past its cap): the SDK's Vulkan presenter picks IMMEDIATE (no vsync)
            // whenever the driver offers it, and MAILBOX next - only FIFO waits for the screen. With vsync on, those two are
            // switched off so Vulkan gets FIFO like DirectX 12's vsync. (Runtime cvars; DirectX 12 ignores them.)
            if (cap_rt(env, "vulkan_allow_present_mode_immediate")) {
                a.push_back("--vulkan_allow_present_mode_immediate=false");
                a.push_back("--vulkan_allow_present_mode_mailbox=false");
            }
        }
        if (g == Game::Gears2 && fps > 0) { a.push_back("--gow2_gpu_vsync=false"); a.push_back("--gow2_max_fps=" + std::to_string(cap)); }
        if (isG3 && fps > 0) {
            a.push_back("--gow2_gpu_vsync=false");
            // GitHub #225: always passed - the game's own limit is 62 (MaxSmoothedFrameRate), not 60, so "60" gave 62 fps
            // and uneven frame pacing on a 60 Hz screen.
            a.push_back("--gow3_max_fps=" + std::to_string(cap));
        }
        if (isGJ && fps > 0) { a.push_back("--gow2_gpu_vsync=false"); a.push_back("--gowj_max_fps=" + std::to_string(cap)); }
        if (isG1 && fps > 0) { a.push_back("--gow2_gpu_vsync=false"); a.push_back("--gow1_max_fps=" + std::to_string(cap)); }
    }
    // PS1 960-961: out\extra-args.txt, one switch per line (# = comment, {LOG} = this session's folder)
    for (const std::string& raw : env.extra_args_lines) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        for (size_t p = 0; (p = l.find("{LOG}", p)) != std::string::npos; p += env.log_dir.size()) l.replace(p, 5, env.log_dir);
        a.push_back(l);
    }
    if (notesOut) notesOut->insert(notesOut->end(), notes.begin(), notes.end());
    return a;
}

bool blocked_by_cpu(Game g, const Environment& env, std::string* message) {   // PS1 966-972
    if (!cap_exe(env, "gn_cpu_level=x86-64-v") || env.has_avx2) return false;
    if (message)
        *message = std::string("This build of ") + k_gameNames[static_cast<int>(g)] + " needs a CPU with AVX2 (Intel Haswell / AMD "
                   "Ryzen, about 2013 and newer), and this PC's CPU does not report it.\n\nIf this is a virtual machine, turn on "
                   "passing the host CPU's features through to it.";
    return true;
}

// ---- 0.4.9 live settings (the in-game menu applies these at once; see gears_settings.h) ---------------------------------
namespace {
struct LiveCvar { std::string name, def; };   // def: the game's own default, set when build_args leaves the cvar out
                                               // 0.4.9 batch 2: "" = not set when build_args leaves it out (the game
                                               // keeps what it has - for cvars whose default the launcher can't know)
struct LiveSpec { const char* id; std::vector<LiveCvar> cvars; };

// "--name=value" -> value (quotes dropped); false when `arg` is not that cvar
bool live_arg_value(const std::string& arg, const std::string& name, std::string* value) {
    const std::string pre = "--" + name + "=";
    if (arg.compare(0, pre.size(), pre) != 0) return false;
    std::string v = arg.substr(pre.size());
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (value) *value = v;
    return true;
}
bool is_live_arg(const std::string& arg, const std::vector<LiveCvar>& cvars) {
    for (const LiveCvar& c : cvars) if (live_arg_value(arg, c.name, nullptr)) return true;
    return false;
}
}  // namespace

std::map<std::string, std::vector<std::string>> live_lines(Game g, Version ver, const Settings& s, const Environment& env) {
    const int gi = static_cast<int>(g);
    const std::string p = k_procNames[gi];
    // Every cvar below is read on each use in the game (per tick / frame / poll / unlock), checked for 0.4.9:
    // max_fps per tick (Judgment's cap no longer leans on the start-up ini), gamma via XenosPresentGammaChanged,
    // HDR levels per present, sharpening per present (only when its pass was made at start - see below), the FPS bar
    // and the shader note made again (XenosRefreshOverlays), mute per focus change, and the SDK's mnk_sensitivity,
    // stick dead zones, vibration and mnk_walk_toggle per poll / SetState (rexglue-sdk src/input).
    const std::vector<LiveSpec> specs = {
        { "FrameCap", { { p + "_max_fps", "0" } } },
        { "Brightness", { { "gow2_present_gamma", "1" } } },
        { "PaperWhite", { { "gow2_hdr_paper_white", "203" } } },
        { "Peak", { { "gow2_hdr_peak", "1000" } } },
        { "Sens", { { "mnk_sensitivity", "1" }, { "ue3_mouse_scale", "1" } } },
        { "AimTurn", { { "ue3_mouse_fov_constant", "true" } } },
        { "AimSens", { { "ue3_mouse_aim_scale", "1" } } },
        { "ZoomSens", { { "ue3_mouse_zoom_scale", "1" } } },
        { "Sharpen", { { "gow2_present_sharpen", "0" } } },
        { "FpsMode", { { "gow2_fps_bar", "off" } } },
        { "HideNotes", { { "gow2_hide_onscreen_notes", "false" } } },
        { "Mute", { { p + "_mute_unfocused", "false" } } },
        { "Vibration", { { "vibration", "true" } } },
        { "DeadZone", { { "left_stick_deadzone_percentage", "0" }, { "right_stick_deadzone_percentage", "0" } } },
        { "WalkMode", { { "mnk_walk_toggle", "false" } } },
        { "AchievementStyle", { { "cogbox_ach_style", "x360" } } },
        { "AchievementColour", { { "cogbox_ach_colour", "dark" } } },
        { "AchievementPosition", { { "cogbox_ach_position", "default" } } },
        { "AchievementSound", { { "cogbox_ach_sound", "true" } } },
        { "HideAchievementPopups", { { "cogbox_ach_hide", "false" } } },
        // 0.4.9 batch 2 (each checked in the game / SDK for 0.4.9):
        // - Vsync: DirectX 12 only - the SDK's D3D12 presenter reads d3d12_present_vsync at every Present (patch 37);
        //   Vulkan's present mode is fixed when its swap chain is made (skipped below). The cap moves with it.
        { "Vsync", { { "d3d12_present_vsync", "false" }, { "vulkan_allow_present_mode_immediate", "" },
                     { "vulkan_allow_present_mode_mailbox", "" }, { p + "_max_fps", "0" } } },
        // - Display mode windowed <-> borderless and the window size: the in-game menu calls the SDK window's
        //   SetFullscreen / SetDesiredLogicalSize (the same path as Alt+Enter or dragging the window). Exclusive full
        //   screen adds --fullscreen_exclusive, so it stays a restart.
        { "Fullscreen", { { "fullscreen", "" }, { "window_width", "" }, { "window_height", "" } } },
        { "WindowSize", { { "window_width", "" }, { "window_height", "" } } },
        // - SMAA on / off: the present makes SMAA's textures when first needed; the menu tells the renderer
        //   (XenosPresentAaChanged). FSR / DLSS / XeSS / T2x change other cvars, so they stay a restart.
        { "AAMode", { { "gow2_present_aa", "off" } } },
        // - Button prompt style: the prompt monitor reads its mode again (GlobalPromptDeviceMonitor().ReloadMode()).
        { "PromptStyle", { { "gow_button_prompts", "auto" } } },
        // - Frame generation: AMD FSR on DirectX 12, when it was on at start - the menu pauses / resumes generation
        //   (the swap chain stays AMD's) and the frame cap goes to / from the halved one. DLSS and Vulkan: restart.
        { "FrameGen", { { "gow2_frame_generation", "off" }, { "gow2_frame_generation_hudless", "" },
                        { "gow2_upscale_object_motion", "" }, { "gow2_upscale_character_motion", "" },
                        { p + "_max_fps", "0" } } },
    };
    std::map<std::string, std::vector<std::string>> out;
    const std::vector<std::string> base = build_args(g, ver, s, env);
    for (const LiveSpec& spec : specs) {
        const Option* o = find_option(g, spec.id);
        if (!o || o->kind == Kind::Text) continue;
        // Sharpening: the present makes its RCAS pass only when the cvar is above 0 at start, so it is live only then.
        if (std::string(spec.id) == "Sharpen") {
            bool made = false;
            std::string v;
            for (const std::string& arg : base)
                if (live_arg_value(arg, "gow2_present_sharpen", &v)) made = std::atof(v.c_str()) > 0.0;
            if (!made) continue;
        }
        // 0.4.9 batch 2: Vsync live on DirectX 12 only; frame generation only when AMD FSR's swap chain was made at start
        // (DirectX 12). Both read build_args' own result, so a fallback (no DLL, Vulkan) counts as what really runs.
        if (std::string(spec.id) == "Vsync" && runs_vulkan(s, env)) continue;
        // the prompt monitor isn't started at all with "original" (the Xbox choice): from there it needs the restart
        if (std::string(spec.id) == "PromptStyle") {
            bool original = false;
            std::string v;
            for (const std::string& arg : base)
                if (live_arg_value(arg, "gow_button_prompts", &v)) original = v == "original";
            if (original) continue;
        }
        if (std::string(spec.id) == "FrameGen") {
            bool fsr = false;
            std::string v;
            for (const std::string& arg : base)
                if (live_arg_value(arg, "gow2_frame_generation", &v)) fsr = v == "fsr";
            if (!fsr || runs_vulkan(s, env)) continue;
        }
        std::vector<std::string> baseRest;
        for (const std::string& arg : base) if (!is_live_arg(arg, spec.cvars)) baseRest.push_back(arg);
        // the values the menu can give: every choice / both states / each slider step (the menu snaps to the step)
        std::vector<double> values;
        if (o->kind == Kind::Bool) values = { 0, 1 };
        else if (o->kind == Kind::Choice) for (size_t k = 0; k < o->choices.size(); k++) values.push_back(static_cast<double>(k));
        else {
            const double st = o->step > 0 ? o->step : (o->kind == Kind::Int ? 1.0 : 0.1);
            const int n = static_cast<int>(std::lround((o->max - o->min) / st));
            for (int k = 0; k <= n && k <= 2000; k++) values.push_back(std::min(o->max, o->min + k * st));
        }
        for (double val : values) {
            Settings s2 = s;
            set_value(g, s2, spec.id, val);
            const std::vector<std::string> args = build_args(g, ver, s2, env);
            std::vector<std::string> rest;
            for (const std::string& arg : args) if (!is_live_arg(arg, spec.cvars)) rest.push_back(arg);
            if (rest != baseRest) continue;   // something else changes with it (vsync, a patch, an ini edit): restart
            char num[32];
            std::snprintf(num, sizeof num, "%.6g", val);
            std::string line = std::string(num) + ":", sets;
            for (size_t c = 0; c < spec.cvars.size(); c++) {
                std::string v = spec.cvars[c].def;
                for (const std::string& arg : args) live_arg_value(arg, spec.cvars[c].name, &v);   // the last one wins, as in the game
                if (v.empty()) continue;   // 0.4.9 batch 2: left out and no known default - the game keeps its value
                sets += (sets.empty() ? "" : ";") + spec.cvars[c].name + "=" + v;
            }
            if (sets.empty()) continue;   // nothing to set: restart
            out[spec.id].push_back(line + sets);
        }
    }
    return out;
}

}  // namespace gears
}  // namespace cogbox
