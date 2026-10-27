// COGBox - the Gears of War settings. See gears_settings.h; "PS1 nnn" = a line of Gears2-Native's tools\launcher.ps1.
#include "gears_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <locale>
#include <regex>
#include <sstream>

namespace cogbox {
namespace gears {

namespace {

const char* const k_gameNames[3] = { "Gears of War 2", "Gears of War 3", "Gears of War: Judgment" };   // PS1 95
const char* const k_gameShort[3] = { "Gears 2", "Gears 3", "Judgment" };                               // PS1 96
const char* const k_procNames[3] = { "gow2", "gow3", "gowj" };                                         // PS1 98
const char* const k_engineIni[3] = { "Xe-GearEngine.ini", "Xbox360-GearEngine.ini", "Xbox360-GearEngine.ini" };   // PS1 101

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
    } else {
        const char* n[] = { "Prologue (museum / trial)", "Sera University", "Onyx Mansion", "Onyx Mansion (part 3)",
            "Halvo Bay military base", "Halvo Bay - to the rooftops", "Rooftops", "Rooftops (part 2)", "Courthouse",
            "Courthouse (transition)", "Courthouse - boss", "Aftermath 1", "Aftermath 2", "Aftermath 2 - oil rig", "Aftermath 3",
            "Aftermath 3 - outro", "Aftermath 4", "Aftermath 5" };
        v.insert(v.end(), std::begin(n), std::end(n));
    }
    return v;
}
// PS1 135-137
const char* const k_mapsGJ[18] = { "SP_00_Museum_P", "SP_01_University_P", "SP_02_Mansion_P", "SP_02_Mansion_3_P", "SP_03_Island_P",
    "SP_03_to_04_P", "SP_04_Rooftops_P", "SP_04_Rooftops2_P", "SP_05_Courthouse_P", "SP_05_CourthouseTrans_P",
    "SP_05_CourthouseBoss_P", "SPE_E1_P", "SPE_E2_P", "SPE_E2oilrig_P", "SPE_E3_P", "SPE_E3_Outro_P", "SPE_E4_P", "SPE_E5_P" };

const char* const k_aaNames[5] = { "Off", "SMAA", "FSR", "DLSS (NVIDIA RTX)", "XeSS" };   // PS1 150
const int k_fpsCaps[4] = { 0, 60, 120, -1 };                                                 // PS1 159
const char* const k_drawScale[3] = { "1", "1.5", "2" };                                      // PS1 163
const char* const k_shakeValue[3] = { "", "0.8", "1.0" };                                    // PS1 167
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
    // Fullscreen has no PS1 tooltip; text from 59-how-to-play ("Borderless fullscreen").
    add(make("Fullscreen", "Fullscreen (borderless)", Tab::Display, Kind::Bool, PG, 0,
             "A borderless window that covers the whole screen.", 330));
    add(make("Exclusive", "Exclusive fullscreen instead", Tab::Display, Kind::Bool, PG, 0,
             "Real fullscreen mode instead of a borderless window (lower latency on some PCs; alt-tab is slower).", 331),
        "", "Fullscreen");
    add(choice("WindowSize", "Window size", Tab::Display, PG,
               { "Default", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" }, 0,
               "The window's size when not fullscreen. Default = the game's own.", 332), "", "!Fullscreen");
    // Monitor: no PS1 tooltip (59-how-to-play: "Which monitor fullscreen uses"). Entries 1..N: monitor_label().
    add(choice("Monitor", "Monitor", Tab::Display, SH, { "Default (main screen)" }, 0,
               "Which screen the game uses. Default = the main screen.", 334), "", "screens>=2");
    add(make("HDR", "HDR", Tab::Display, Kind::Bool, PG, 0, "Needs HDR turned on in Windows display settings.", 363));
    add(number("PaperWhite", "HDR paper white (nits)", Tab::Display, Kind::Int, PG, 80, 400, 10, 203,
               "How bright normal scenes are. 203 is the reference; raise it in a bright room.", 364), "", "HDR");
    add(number("Peak", "HDR peak (nits)", Tab::Display, Kind::Int, PG, 400, 4000, 100, 1000,
               "Your screen's peak brightness (see its specs or Windows HDR calibration).", 365), "", "HDR");
    add(make("Vsync", "Vsync (no tearing)", Tab::Display, Kind::Bool, SH, 0,
             "Each frame waits for your screen's next refresh, so the picture never tears. With G-Sync / FreeSync the screen "
             "follows the game below its refresh rate. The frame rate is capped a little under your screen's refresh rate (half "
             "of it with frame generation), which keeps the extra input lag small. Off = frames are shown at once (lowest lag, "
             "may tear).", 344), "vsync");
    // Frame rate limit: no PS1 tooltip; text from 59-how-to-play ("The game itself asks for 30 fps").
    add(choice("Fps", "Frame rate limit", Tab::Display, PG, { "30 (original)", "60", "120", "Unlimited" }, 0,
               "The game itself asks for 30 frames per second, as on the console. 60 / 120 lift the limit to that; Unlimited "
               "removes it.", 341), "", "", true);
    add(choice("FpsMode", "FPS counter", Tab::Display, OWN, { "Off", "Bar at the top", "Bar at the bottom", "Game's own (column)" }, 0,
               "Bar: one line (FPS, frame time, slowest frame) drawn by the port itself, nothing in the game changed. Game's own: "
               "the game's built-in FPS / frame-time column.", 336));
    add(make("Ultrawide", "Ultrawide 21:9", Tab::Display, Kind::Bool, PG, 0,
             "A wider view for a 21:9 screen; black bars on other shapes.", 362), "ultrawide");

    // ---- Graphics ------------------------------------------------------------------------------------------------
    add(choice("Preset", "Quality preset", Tab::Graphics, PG, { "Original (as on the console)", "High", "Ultra", "Custom" }, 3,
               "Sets the graphics boxes in one go. Original = the game as on the console. Change any box afterwards and the "
               "preset shows Custom.", 316));
    // Job 521 (Shagg 19:02): Vulkan opened for testing - the play exes have the full Vulkan renderer.
    add(choice("Api", "Graphics API", Tab::Graphics, PG, { "DirectX 12 (recommended)", "Vulkan (experimental)" }, 0,
               "Experimental - Vulkan is here so players can test it and send us their logs. DirectX 12 is the tested "
               "renderer; on Vulkan, Gears 3 is known to look darker in places and show some black patches.\n"
               "On Vulkan, FSR / DLSS / XeSS and frame generation are not available (SMAA is used), and Reflex / Anti-Lag "
               "are off. If something looks wrong, make a problem report and send it to us.", 0));
    // Render resolution: no PS1 tooltip; text from 59-how-to-play's Picture table.
    add(choice("Resolution", "Render resolution", Tab::Graphics, PG,
               { "1x - 720p (original)", "2x - 1440p", "3x - 4K", "4x - 5K (supersampling, heavy)", "Match my monitor (native)" }, 0,
               "How much detail the game draws: 1x = 1280x720 as on the console, 2x = 2560x1440, 3x = 3840x2160 (4K), "
               "4x = 5120x2880. Real detail, not an upscale. Match my monitor: the smallest scale that covers the screen, "
               "resampled to its exact size.", 340), "", "", true);
    add(choice("AAMode", "Anti-aliasing", Tab::Graphics, PG, { "Off", "SMAA", "FSR", "DLSS (NVIDIA RTX)", "XeSS" }, 0,
               "SMAA: light, sharp, any GPU. FSR: temporal anti-aliasing of the 3D scene, any DX12 GPU. DLSS: the same on "
               "NVIDIA RTX cards. XeSS: the same, best on Intel Arc cards.", 346), "", "", true);
    add(choice("SceneMode", "Scene resolution", Tab::Graphics, SH,
               { "Native (same as render resolution)", "Quality (scene at 2/3)", "Performance (scene at 1/2)",
                 "Ultra performance (scene at 1/3)" }, 0,
               "With FSR, DLSS or XeSS: draw the 3D scene at a lower resolution and let the upscaler fill in the detail - more "
               "frames per second, a little softer. HUD and menus stay at the full resolution.\nPerformance needs render "
               "resolution 2x (1440p) or 4x; Quality and Ultra performance need 3x (4K).", 351), "scenescale", "AAMode>=2");
    add(choice("FrameGen", "Frame generation (highly experimental)", Tab::Graphics, SH, { "Off", "AMD FSR frame generation" }, 0,
               "Highly experimental - it's rough for now. Shows a generated frame between each two the game draws: smoother motion on a fast monitor, about one frame more "
               "input lag. Works best when the game already runs at 60 fps or more. Any DirectX 12 graphics card.\nEarly version: "
               "the HUD and characters' own movement can smear a little in the in-between frames.", 356), "framegen");
    // Aniso: no PS1 tooltip; text from 59-how-to-play ("16x anisotropic filtering").
    add(make("Aniso", "Sharper textures (16x anisotropic)", Tab::Graphics, Kind::Bool, OWN, g == Game::Gears2 ? 1 : 0,
             "16x anisotropic filtering: textures seen at an angle (floors, walls) stay sharp.", 357), "aniso", "", true);
    add(make("Trilinear", "Smooth texture detail steps (trilinear)", Tab::Graphics, Kind::Bool, PG, 1,
             "Texture detail levels blend instead of showing a line where they step down (the game uses point filtering "
             "between levels).", 358), "trilinear", "", true);
    add(make("MipFade", "Sharp textures show at once (fast mip fade)", Tab::Graphics, Kind::Bool, PG, 1,
             "A newly loaded sharper texture shows at once instead of fading in over 0.3-2 seconds.", 359), "mipfade", "", true);
    add(choice("Pool", "Texture memory", Tab::Graphics, PG,
               { "Game's own (smallest)", "Standard (196 MB / Gears 3 200 MB)", "Large (256 MB)", "Larger (320 MB, not long-tested)" }, 1,
               "The texture streaming pool. Bigger = fewer blurry textures waiting to sharpen. The larger sizes leave less memory "
               "for the rest of the game.", 360), "pool", "", true);
    add(choice("DrawDist", "Draw distance", Tab::Graphics, PG, { "Original", "Far (1.5x)", "Farthest (2x)" }, 0,
               "How far away detail and objects are still drawn.", 361), "drawdist", "", true);
    // Motion blur / lens flares: no PS1 tooltip.
    add(make("MotionBlur", "Motion blur", Tab::Graphics, Kind::Bool, PG, 1, "The blur of fast movement (the game's own setting).", 370),
        "", "", true);
    add(make("DoF", "Depth of field", Tab::Graphics, Kind::Bool, PG, 1,
             "The blur of distant / out-of-focus areas (cutscenes, aiming).", 371), "", "", true);
    add(make("LensFlares", "Lens flares", Tab::Graphics, Kind::Bool, PG, 1, "Light flares from bright lights (the game's own setting).", 372),
        "", "", true);
    add(choice("Shake", "Camera shake (roadie run)", Tab::Graphics, PG, { "Original", "Less", "Off" }, 0,
               "The camera shake while roadie-running. Less / Off are not yet checked by eye in every game - if Off looks "
               "stronger, tell Claude.", 373), "", "", true);
    add(number("Fov", "Field of view (%)", Tab::Graphics, Kind::Int, PG, 80, 130, 5, 100,
               "100 = the original. Widens (or narrows) every camera view by this much.", 374), "", "", true);

    // ---- Controls ------------------------------------------------------------------------------------------------
    add(make("Mnk", "Keyboard and mouse (Gears layout)", Tab::Controls, Kind::Bool, SH, 0,
             "WASD move, mouse look, left click fire, right click aim, Space cover/roll, F melee, E use, Q look at, R reload, "
             "Tab Tac/Com, 1-4 weapons, middle click / Z zoom, C left-stick click, Esc / Enter pause, Backspace back. A "
             "controller still works.", 378));
    // 0.2.0 (GitHub #2): up to 20 (was 5) - the mouse drives the right stick, so slow moves sat inside the game's stick
    // dead zone at 5. True mouse look (no stick limits) is planned.
    add(number("Sens", "Mouse sensitivity", Tab::Controls, Kind::Float, SH, 0.1, 20, 0.1, 1.0,
               "How fast the mouse turns the camera (up to 20). The mouse is passed to the game as its right stick, so the "
               "fastest turn is the stick's fastest turn; true mouse look is planned.", 381, 2));
    add(choice("DeadZone", "Extra stick dead zone", Tab::Controls, SH, { "Game's own", "5%", "10%", "15%", "20%", "25%" }, 0,
               "Added on top of the game's own dead zone, for worn sticks that drift. Game's own = nothing added.", 382));
    add(make("Vibration", "Controller vibration", Tab::Controls, Kind::Bool, SH, 1, "The controller's rumble.", 383));   // no PS1 tooltip
    add(make("LowLag", "Lower input lag", Tab::Controls, Kind::Bool, SH, 1,
             "Fewer frames waiting in line between the game and your screen, so the controller feels quicker. May cost a few "
             "fps on a busy graphics card; untick to compare.", 385), "lowlag");


    // ---- Gameplay ------------------------------------------------------------------------------------------------
    add(choice("Start", "Start at", Tab::Gameplay, OWN, start_names(g), 0,
               "Main menu, or straight into a chapter from its beginning (it opens paused: press A / Start). Your saves are not "
               "changed.", 323), "start");
    add(choice("Version", "Game version", Tab::Gameplay, OWN, { "Title Update (latest)", "Disc (original release)" },
               0,   // COGBox (Shagg 19:46): the PGO build is the default - Gears 2's and Gears 3's title-update builds
                    // (the PS1 has G3Version = 1, the disc; Gears 3's chat built both its exes with PGO, job 143)
               "Title Update: the latest patch from Epic (Gears 2: TU6; Gears 3: TU6, new; Judgment: TU4). Disc: the original "
               "release.", 324), "version");
    add(choice("Players", "Players", Tab::Gameplay, PG, { "1", "2 (split-screen)", "3 (split-screen)", "4 (split-screen)" }, 0,
               "Local split-screen co-op (one controller each).", 325), "players");
    add(make("SkipLogos", "Skip the intro logos", Tab::Gameplay, Kind::Bool, PG, 0,
             "Starts at the title screen without the publisher / studio logo films. The loading film and cutscenes still play.", 326),
        "skiplogos");
    add(make("Cheats", "Cheats (hotkeys F5-F11)", Tab::Gameplay, Kind::Bool, SH, 0,
             "The developers' cheat keys (Gears 3 chat, its notes/25): F5-F11 in game. Achievements and saves are your own "
             "business.", 327), "cheats");
    add(make("Mutators", "Unlock all mutators", Tab::Gameplay, Kind::Bool, SH, 0,
             "Every mutator (gameplay modifier) available from the start. Writes nothing to your profile.", 328), "mutators");
    add(make("LevelSelect", "Level select menu (experimental)", Tab::Gameplay, Kind::Bool, SH, 0,
             "The game's own hidden level-select menu (Judgment). Its list has no text yet: Down moves, A loads (entry 1 Sera "
             "University ... 17 Aftermath 5).", 329), "levelsel");
    // 0.2.0 (GitHub #4): the game's own language - the runtime's user_language (Xbox language IDs, issue #4's table)
    add(choice("Lang", "Game language", Tab::Gameplay, PG,
               { "Default (English)", "English", "Japanese", "German", "French", "Spanish", "Italian", "Korean", "Chinese",
                 "Portuguese", "Polish", "Russian" }, 0,
               "The language of the game's own text and voices. Only the languages on your copy of the game are there - "
               "the others fall back to English. Takes effect when the game starts.", 0));
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
    }
    return x;
}

bool cap_exe(const Environment& e, const std::string& n) { return e.cap.exe_has && e.cap.exe_has(n); }
bool cap_rt(const Environment& e, const std::string& n) { return e.cap.runtime_has && e.cap.runtime_has(n); }
bool cap_file(const Environment& e, const std::string& n) { return e.cap.file_next_to_exe && e.cap.file_next_to_exe(n); }

std::vector<std::string> gpu_list(const Environment& env) {
    if (env.gpus.empty() && !env.gpu_name.empty()) return { env.gpu_name };
    return env.gpus;
}
std::string gpu_main(const Environment& env) { return env.gpu_name.empty() ? main_gpu(env.gpus) : env.gpu_name; }

const char* section_name(int g) { return g == 0 ? "Gears2" : g == 1 ? "Gears3" : "Judgment"; }

}  // namespace

// ---- The model ------------------------------------------------------------------------------------------------------
const std::vector<Option>& options(Game g) {
    static const std::vector<Option> all[3] = { make_options(Game::Gears2), make_options(Game::Gears3), make_options(Game::Judgment) };
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
    }
    return "?";
}

// PS1 263-285
Availability can(Game g, const std::string& f, Version ver, const Environment& env) {
    const int gi = static_cast<int>(g);
    auto no = [](const std::string& why) { Availability a; a.ok = false; a.why = why; return a; };
    if (f == "version") {
        if (g == Game::Gears2) { if (!env.update_ready) return no("The Title Update 6 build is not there yet."); }
        else if (g == Game::Gears3) { if (!env.update_ready) return no("The Gears 3 Title Update 6 build is not there yet."); }
        else return no("Judgment always runs with its Title Update 4.");
    } else if (f == "start" || f == "prebuild") {
        if (g == Game::Judgment && start_names(g).size() < 2)
            return no(not_yet(2, f == "start" ? "its chapter list is coming" : "needs its chapter list"));
    } else if (f == "gamefps") {
        if (g == Game::Judgment) return no(not_yet(2, "game's own counter"));
    } else if (f == "ultrawide") {
        if (g == Game::Judgment) return no(not_yet(2));
    } else if (f == "cheats" || f == "mutators") {
        if (g != Game::Gears3) return no(not_yet(gi));
        if (ver == Version::TitleUpdate && env.update_ready)
            return no("Gears 3: the Disc version only for now (Title Update 6 shows a dirty-disc error with it).");
    } else if (f == "levelsel") {
        if (g != Game::Judgment) return no(not_yet(gi));
    } else if (f == "drawdist") {
        if (g == Game::Gears2) return no(not_yet(0, "its config has no draw-distance setting"));
    } else if (f == "skiplogos" || f == "trilinear") {
        if (g == Game::Gears2 && !env.g2_disc_ini_edits) return no(not_yet(0, "needs the next Gears 2 build"));
    } else if (f == "mipfade") {
        if (g == Game::Gears2) return no(not_yet(0, "its config has no mip-fade setting"));
    } else if (f == "lowlag") {
        if (!cap_exe(env, "gow2_renderer_frames_in_flight")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "scenescale") {
        if (!cap_exe(env, "gow2_upscale_mode")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "framegen") {
        if (!cap_exe(env, "gow2_frame_generation")) return no(not_yet(gi, "needs its next build"));
        if (!cap_file(env, "amd_fidelityfx_framegeneration_dx12.dll")) return no(not_yet(gi, "this build has no AMD frame generation files"));
    } else if (f == "mute") {
        if (!cap_exe(env, std::string(k_procNames[gi]) + "_mute_unfocused")) return no(not_yet(gi, "needs its next build"));
    } else if (f == "vsync") {   // launcher patch 0155
        if (!cap_rt(env, "d3d12_present_vsync")) return no(not_yet(gi, "needs its next build (SDK patch 37)"));
    }
    return Availability();
}

const std::vector<KeyAction> &key_actions()
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
    };
    return v;
}

Availability available(const Option& o, Game g, Version ver, const Settings& s, const Environment& env) {
    Availability a;
    if (!o.feature.empty()) a = can(g, o.feature, ver, env);
    if (!a.ok) return a;
    bool on = true;
    if (o.id == "Exclusive") on = s.b("Fullscreen");                                  // PS1 333
    else if (o.id == "WindowSize") on = !s.b("Fullscreen");                           // PS1 333
    else if (o.id == "PaperWhite" || o.id == "Peak") on = s.b("HDR");                 // PS1 366
    else if (o.id == "SceneMode") on = s.i("AAMode") >= 2 && s.i("Api") == 0;        // PS1 352, 473; job 521
    else if (o.id == "FrameGen" && s.i("Api") != 0) { a.ok = false; a.why = "DirectX 12 only."; return a; }   // job 521
    else if (o.id == "Monitor") on = env.screen_count >= 2;                           // PS1 335
    a.ok = on;
    return a;
}

Availability choice_available(const Option& o, Game g, int index) {
    Availability a;
    if (o.id == "FpsMode" && index == 3 && g == Game::Judgment) { a.ok = false; a.why = not_yet(2, "game's own counter"); }   // PS1 267, 475
    return a;
}

std::string tooltip_for(const Option& o, Game g, const Availability& a, const Environment& env) {
    std::string base = o.tooltip;
    if (o.id == "AAMode") {   // PS1 345
        std::string gpu = gpu_main(env);
        base += "\nYour graphics card: " + (gpu.empty() ? std::string("not detected") : gpu);
    }
    if (g == Game::Judgment && o.id == "FpsMode") return not_yet(2, "game's own counter") + " " + base;   // PS1 476
    if (g == Game::Judgment && o.id == "Players") return "Untested in Judgment. " + base;                 // PS1 477
    if (!a.why.empty()) return trim(a.why + " " + base);                                                  // PS1 415
    return base;
}

Version effective_version(Game g, const Settings& s, const Environment& env) {   // PS1 459
    if (g == Game::Judgment) return Version::TitleUpdate;
    if (!env.update_ready) return Version::Disc;
    return s.i("Version") == 0 ? Version::TitleUpdate : Version::Disc;
}
bool exe_for_update(Game g, Version ver, const Environment& env) {   // PS1 442-445, 804, 809, 821
    if (g == Game::Judgment) return env.update_ready;
    return env.update_ready && ver == Version::TitleUpdate;
}

// ---- Presets (PS1 573-592) ------------------------------------------------------------------------------------------
void apply_preset(Settings& s, int p) {
    if (p < 0 || p >= 3) return;   // PS1 576: Custom changes nothing
    struct P { int res, fps, aa; bool aniso, tri, mip; int pool, draw; bool blur, dof, flare; int shake, fov; };
    static const P v[3] = {        // PS1 579-581
        { 0, 0, 0, false, false, false, 0, 0, true, true, true, 0, 100 },
        { 1, 1, 1, true, true, true, 1, 1, true, true, true, 0, 100 },
        { 4, 2, 1, true, true, true, 2, 2, true, true, true, 0, 100 },
    };
    const P& x = v[p];
    s.set("Preset", p);
    s.set("Resolution", x.res); s.set("Fps", x.fps); s.set("AAMode", x.aa); s.set("Aniso", x.aniso);
    s.set("Trilinear", x.tri); s.set("MipFade", x.mip); s.set("Pool", x.pool); s.set("DrawDist", x.draw);
    s.set("MotionBlur", x.blur); s.set("DoF", x.dof); s.set("LensFlares", x.flare); s.set("Shake", x.shake); s.set("Fov", x.fov);
}

void set_value(Game g, Settings& s, const std::string& id, double value) {
    const Option* o = find_option(g, id);
    if (o) value = clamp_value(*o, value, nullptr);
    if (id == "Preset") { s.set("Preset", value); apply_preset(s, static_cast<int>(value)); return; }   // PS1 592
    double old = s.f(id);
    s.set(id, value);
    if (o && o->in_preset && old != value && s.i("Preset") != PresetCustom) s.set("Preset", PresetCustom);   // PS1 588
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
    return "GearGame_P?loadcheckpoint=0?chapter=" + std::to_string(chapter) + " -nostartupmovies";
}

// ---- Store (PS1 103-123, 418-464, 759-774) --------------------------------------------------------------------------
Store::Store() { reset(); }

void Store::reset() {
    top.clear();
    for (int g = 0; g < 3; ++g) { per[g].clear(); own[g].clear(); }
    last_game = 0;
    for (const Option& o : options(Game::Gears2)) if (o.scope != Scope::GameOwn) top[o.id] = o.def;
    for (int g = 0; g < 3; ++g)
        for (const Option& o : options(static_cast<Game>(g))) if (o.scope == Scope::GameOwn) own[g][o.id] = o.def;
    // PS1 104-105 / 154 / 157-158: AAMode and the FPS counters start at -1 and are worked out from the older boxes.
    top["AAMode"] = -1; top["AA"] = 0; top["ShowFps"] = 0; top["G3ShowFps"] = 0;
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
        double x = o.def;
        const std::map<std::string, double>* src = &top;
        if (o.scope == Scope::GameOwn) src = &own[gi];
        else if (o.scope == Scope::PerGame && per[gi].count(o.id)) src = &per[gi];   // PS1 429: else the top-level value
        auto it = src->find(o.id);
        if (it != src->end()) x = it->second;
        s.set(o.id, clamp_value(o, x, &env));   // PS1 300 / 431 / 437-438 clamping
    }
    s.set("Version", static_cast<int>(effective_version(g, s, env)));   // PS1 459
    if (g == Game::Judgment && s.i("FpsMode") == 3) s.set("FpsMode", 0); // PS1 475
    return s;
}

void Store::remember(Game g, const Settings& s) {   // PS1 450-455
    const int gi = static_cast<int>(g);
    own[gi]["Start"] = s.f("Start"); own[gi]["Aniso"] = s.f("Aniso"); own[gi]["FpsMode"] = s.f("FpsMode");
    if (g != Game::Judgment) own[gi]["Version"] = s.f("Version");
    for (const Option& o : options(g)) if (o.scope == Scope::PerGame) per[gi][o.id] = s.f(o.id);   // PS1 419-427
}

void Store::commit(Game g, const Settings& s, const Environment& env) {   // PS1 759-774
    const int gi = static_cast<int>(g);
    last_game = gi;
    own[gi]["Start"] = s.f("Start"); own[gi]["Aniso"] = s.f("Aniso"); own[gi]["FpsMode"] = s.f("FpsMode");
    if (g != Game::Judgment && env.update_ready) own[gi]["Version"] = s.f("Version");   // PS1 761-762
    for (const Option& o : options(g)) {
        if (o.scope == Scope::GameOwn) continue;
        top[o.id] = s.f(o.id);
        if (o.scope == Scope::PerGame) per[gi][o.id] = s.f(o.id);
    }
    top["AA"] = s.i("AAMode") == 1 ? 1 : 0;   // PS1 764
}

bool Store::load_ini(const std::string& text) {
    reset();
    top["AAMode"] = -1; own[0]["FpsMode"] = -1; own[1]["FpsMode"] = -1;   // worked out again after reading
    std::istringstream in(text);
    std::string line, section;
    bool any = false;
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
        std::string lv = val;
        for (char& c : lv) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        double x = 0;
        if (lv == "true") x = 1;
        else if (lv == "false") x = 0;
        else {
            std::istringstream num(val); num.imbue(std::locale::classic());
            if (!(num >> x)) continue;
        }
        int gsec = section == "Gears2" ? 0 : section == "Gears3" ? 1 : section == "Judgment" ? 2 : -1;
        if (gsec < 0) {
            if (key == "Game") { last_game = std::max(0, std::min(2, static_cast<int>(x))); any = true; continue; }
            bool done = false;
            for (const Legacy& l : legacy) if (key == l.key) { own[l.game][l.id] = x; done = true; }
            if (key == "AA" || key == "ShowFps" || key == "G3ShowFps") { top[key] = x; done = true; }
            const Option* o = find_option(Game::Gears2, key);
            if (!done && o && o->scope != Scope::GameOwn) { top[key] = x; done = true; }
            any = any || done;
        } else {
            const Option* o = find_option(static_cast<Game>(gsec), key);
            if (!o || o->scope == Scope::Shared) continue;
            if (o->scope == Scope::GameOwn) own[gsec][key] = x; else per[gsec][key] = x;
            any = true;
        }
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
    out << "[General]\nGame=" << last_game << "\n";
    for (const Option& o : options(Game::Gears2)) if (o.scope != Scope::GameOwn) out << o.id << "=" << val(o, get(top, o)) << "\n";
    for (int g = 0; g < 3; ++g) {
        out << "\n[" << section_name(g) << "]\n";
        for (const Option& o : options(static_cast<Game>(g))) {
            if (o.scope == Scope::GameOwn) out << o.id << "=" << val(o, get(own[g], o)) << "\n";
            else if (o.scope == Scope::PerGame && per[g].count(o.id)) out << o.id << "=" << val(o, per[g].at(o.id)) << "\n";
        }
    }
    return out.str();
}

// ---- The command line (PS1 788-961) ---------------------------------------------------------------------------------
std::vector<std::string> build_args(Game g, Version ver, const Settings& s, const Environment& env, std::vector<std::string>* notesOut) {
    const int gi = static_cast<int>(g);
    const bool isG3 = g == Game::Gears3, isGJ = g == Game::Judgment;
    std::vector<std::string> a, notes;
    auto q = [](const std::string& p) { return "\"" + p + "\""; };
    auto logf = [&](const char* name) { return env.log_dir + env.path_sep + name; };
    auto avail = [&](const char* id) { const Option* o = find_option(g, id); return o && available(*o, g, ver, s, env).ok; };
    auto on = [&](const char* id) { return avail(id) && s.b(id); };   // PS1 796: a greyed box is never sent
    const bool useUpdate = exe_for_update(g, ver, env);

    // PS1 800-823: data roots, log, crash folder, renderer, shader cache, title update
    if (g == Game::Gears2) {
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf("gow2.log")), "--gow2_crash_dir=" + q(env.log_dir), "--gpu_plugin=xenos" };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    } else if (isG3) {
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf("gow3.log")), "--gow2_crash_dir=" + q(env.log_dir),
              "--gow2_shader_cache_dir=" + q(env.shader_cache_dir) };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    } else {
        a = { "--game_data_root=" + q(env.game_data_root), "--user_data_root=" + q(env.user_data_root),
              "--log_file=" + q(logf("gowj.log")), "--gow2_crash_dir=" + q(env.log_dir), "--gpu_plugin=xenos",
              "--gow2_shader_cache_dir=" + q(env.shader_cache_dir) };
        if (useUpdate) a.push_back("--update_data_root=" + q(env.update_data_root));
    }
    if (env.wine) notes.push_back("launcher: running under Wine / Proton");   // job 523
    // PS1 832-836: start at a chapter
    const int chapter = s.i("Start") - 1;
    if (avail("Start") && chapter >= 0) {
        std::string url = launch_url(g, chapter);
        a.push_back((g == Game::Gears2 ? "--gow2_launch=" : "--ue3_launch=") + q(url));
    }
    // PS1 837-842: render resolution ("Match my monitor" = scale 0 + native size; counts as 2x below)
    int scale = s.i("Resolution") + 1;
    if (scale >= 5) { a.push_back("--gow2_renderer_resolution_scale=0"); a.push_back("--gow2_present_native_size=true"); scale = 2; }
    else a.push_back("--gow2_renderer_resolution_scale=" + std::to_string(scale));
    // PS1 843-849: Gears 3's threshold, memexport, aspect
    if (isG3) {
        if (scale > 1) a.push_back("--gow2_renderer_resolution_scale_threshold=1120");
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
        if (aa >= 2 && aa <= 4 && !cap_file(env, need[aa])) {
            notes.push_back(std::string("launcher: ") + k_aaNames[aa] + " is not available for this game / build - SMAA instead"); aa = 1;
        }
        if (s.i("Api") == 1) {   // job 521: Vulkan (experimental); the upscalers are DirectX 12 only
            a.push_back("--gow2_renderer_api=vulkan");
            if (aa >= 2) { notes.push_back(std::string("launcher: Vulkan - ") + k_aaNames[aa] + " is DirectX 12 only, SMAA instead"); aa = 1; }
        }
        switch (aa) {
        case 1: a.push_back("--gow2_present_aa=smaa"); break;
        case 2: a.push_back("--gow2_upscaler=fsr"); break;
        case 3: a.push_back("--gow2_upscaler=dlss"); break;
        case 4: a.push_back("--gow2_upscaler=xess"); break;
        default: break;
        }
    }
    // PS1 856 (patch 0142): scene resolution, with an upscaler that is really used
    {
        static const char* const modes[4] = { "", "--gow2_upscale_mode=quality", "--gow2_upscale_mode=performance", "--gow2_upscale_mode=ultra" };
        int m = s.i("SceneMode");
        if (aa >= 2 && avail("SceneMode") && m > 0 && m < 4) a.push_back(modes[m]);
    }
    // PS1 857 (0146 / 0147): frame generation
    const bool fgOn = avail("FrameGen") && s.i("FrameGen") == 1 && s.i("Api") == 0;   // job 521: DirectX 12 only
    if (fgOn) a.push_back("--gow2_frame_generation=fsr");
    // PS1 858: 16x anisotropic
    if (on("Aniso")) a.push_back(gi == 0 ? "--gow2_patch_aniso16=true" : gi == 1 ? "--gow3_patch_aniso16=true" : "--gowj_aniso16=true");
    // PS1 860-864: texture pool (-1 = the port's own default; Judgment then 200)
    if (avail("Pool")) {
        int mb = k_poolMb[std::max(0, std::min(3, s.i("Pool")))];
        if (mb < 0 && isGJ) mb = 200;
        if (mb >= 0) {
            static const char* const sw[3] = { "--gow2_patch_texture_pool_mb=", "--gow3_texture_pool_mb=", "--gowj_texture_pool_mb=" };
            a.push_back(sw[gi] + std::to_string(mb));
        }
    }
    // PS1 866-867: Gears 3 / Judgment trilinear and fast mip fade (on by default there, so always sent)
    if (isG3) { a.push_back("--gow3_trilinear=" + fmt_bool(s.b("Trilinear"))); a.push_back("--gow3_fast_mip_fade=" + fmt_bool(s.b("MipFade"))); }
    if (isGJ) { a.push_back("--gowj_trilinear=" + fmt_bool(s.b("Trilinear"))); a.push_back("--gowj_fast_mip_fade=" + fmt_bool(s.b("MipFade"))); }
    // PS1 869-871: cheats, mutators, level select
    if (on("Cheats")) a.push_back("--gow3_cheats=true");
    if (on("Mutators")) a.push_back("--gow3_unlock_mutators=true");
    if (on("LevelSelect")) a.push_back("--gowj_level_select=true");
    // PS1 873-898: the game's own engine settings through --ue3_ini_edits
    {
        const std::string ei = k_engineIni[gi];
        std::vector<std::string> ini;
        if (!s.b("MotionBlur")) ini.push_back(ei + "|SystemSettings|MotionBlur=False");
        if (!s.b("DoF")) ini.push_back(ei + "|SystemSettings|DepthOfField=False");
        if (!s.b("LensFlares")) ini.push_back(ei + "|SystemSettings|LensFlares=False");
        if (s.i("Fov") != 100) {
            std::string k = fmt_double(static_cast<double>(s.i("Fov")) / 100);
            ini.push_back("GearCamera.ini|*|FOVAngle*=" + k);
            if (g == Game::Gears2) ini.push_back("GearPawn.ini|*|DefaultFOV*=" + k);
        }
        if (on("SkipLogos")) {   // PS1 882-887: only the logo films; Judgment has its own switch
            if (g == Game::Gears2) { ini.push_back(ei + "|FullScreenMovie|-StartupMovies=EpicLogo"); ini.push_back(ei + "|FullScreenMovie|-StartupMovies=MGSLogo"); }
            else if (isG3) ini.push_back(ei + "|FullScreenMovie|-StartupMovies=Open_Logos");
            else a.push_back("--gowj_skip_logos=true");
        }
        int dd = std::max(0, std::min(2, s.i("DrawDist")));
        if (avail("DrawDist") && dd > 0) ini.push_back(ei + "|SystemSettings|MaxDrawDistanceScale=" + k_drawScale[dd]);
        int sh = std::max(0, std::min(2, s.i("Shake")));
        if (sh > 0) {   // PS1 889-896
            std::string v = k_shakeValue[sh];
            if (g == Game::Gears2) ini.push_back("GearCamera.ini|GearGame.GearCamMod_CameraBone|RoadieRunShakyCamDampeningFactor=" + v);
            else if (isG3) { for (const char* pf : { "GearPawn.ini", "GearPawnMP.ini" }) ini.push_back(std::string(pf) + "|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor=" + v); }
            else { for (const char* pf : { "GearPawn.ini", "GearPawnMP.ini", "GearPawnFFA.ini" }) ini.push_back(std::string(pf) + "|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor=" + v); }
        }
        if (g == Game::Gears2 && on("Trilinear")) ini.push_back(ei + "|SystemSettings|~=MipFilter=point>MipFilter=linear");   // PS1 897
        if (!ini.empty()) {
            std::string j;
            for (size_t i = 0; i < ini.size(); ++i) j += (i ? ";" : "") + ini[i];
            a.push_back("--ue3_ini_edits=" + q(j));
        }
    }
    // PS1 899: ultrawide
    if (on("Ultrawide")) a.push_back(isG3 ? "--gow3_ultrawide=true" : "--gow2_ultrawide=true");
    // PS1 900: HDR
    if (s.b("HDR")) {
        a.push_back("--present_hdr=true");
        a.push_back("--gow2_hdr_paper_white=" + std::to_string(s.i("PaperWhite")));
        a.push_back("--gow2_hdr_peak=" + std::to_string(s.i("Peak")));
    }
    // PS1 901-902: fullscreen / window size
    // job 523: under Wine / Proton the compositor (gamescope on a Steam Deck) owns the screen - no exclusive fullscreen
    if (s.b("Fullscreen")) { a.push_back("--fullscreen=true"); if (s.b("Exclusive") && !env.wine) a.push_back("--fullscreen_exclusive=true"); }
    else {
        int w = std::max(0, std::min(5, s.i("WindowSize")));
        if (w > 0) { a.push_back("--window_width=" + std::to_string(k_winSizes[w][0])); a.push_back("--window_height=" + std::to_string(k_winSizes[w][1])); }
    }
    // PS1 904-908: stick dead zone, vibration
    {
        int d = std::max(0, std::min(5, s.i("DeadZone")));
        if (d > 0) { std::string dz = fmt_double(k_deadValue[d]); a.push_back("--left_stick_deadzone_percentage=" + dz); a.push_back("--right_stick_deadzone_percentage=" + dz); }
    }
    if (!s.b("Vibration")) a.push_back("--vibration=false");
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
    {   // 0.2.0 (#4): game language - Xbox language IDs (1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian,
        // 7 Korean, 8 Chinese, 9 Portuguese, 11 Polish, 12 Russian)
        static const int ids[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12 };
        const int l = std::max(0, std::min(11, s.i("Lang")));
        if (l > 0) a.push_back("--user_language=" + std::to_string(ids[l]));
    }
    if (s.i("Monitor") > 0) a.push_back("--monitor=" + std::to_string(s.i("Monitor")));
    if (avail("Players") && s.i("Players") > 0) a.push_back("--local_players=" + std::to_string(s.i("Players") + 1));
    // PS1 929: FPS counter
    switch (s.i("FpsMode")) {
    case 1: a.push_back("--gow2_fps_bar=top"); break;
    case 2: a.push_back("--gow2_fps_bar=bottom"); break;
    case 3: if (!isGJ) a.push_back(isG3 ? "--gow3_patch_show_fps=true" : "--gow2_patch_show_fps=true"); break;
    default: break;
    }
    // PS1 930-937: keyboard and mouse with the Gears layout
    if (s.b("Mnk")) {
        a.insert(a.end(), { "--mnk_mode=true", "--mnk_mouse=true", "--mnk_sensitivity=" + fmt_double(s.f("Sens")) });
        for (const KeyAction &k : key_actions()) {   // 0.2.0: the Gears layout, or the player's own keys (Controls tab)
            auto it = env.keys.find(k.cvar);
            const std::string v = it != env.keys.end() && !it->second.empty() ? it->second : k.def;
            a.push_back(std::string("--") + k.cvar + "=" + v);
        }
    }
    // PS1 940-958 (launcher patches 0155 / 0158): frame rate; with vsync a cap at or above the refresh rate becomes 3
    // under it (half the refresh minus 2 with frame generation)
    {
        const int fps = std::max(0, std::min(3, s.i("Fps")));
        int cap = k_fpsCaps[fps];
        const bool vsync = on("Vsync");
        if (vsync && fps >= 1) {
            const int hz = env.refresh_hz;
            const int top = fgOn ? static_cast<int>(std::floor(hz / 2.0)) - 2 : hz - 3;
            if (isG3 && cap == 60) cap = 62;   // review 0158: Gears 3's 60 is its own 62 cap
            if (hz >= 50 && (cap < 0 || cap > top)) cap = top;
            notes.push_back("launcher: vsync on; " + env.monitor_device + " at " + std::to_string(hz) + " Hz (0 = unknown)" +
                            (fgOn ? ", frame generation on" : "") + " -> frame-rate cap " + std::to_string(cap));
        }
        if (vsync) a.push_back("--d3d12_present_vsync=true");
        if (g == Game::Gears2 && fps > 0) { a.push_back("--gow2_gpu_vsync=false"); a.push_back("--gow2_max_fps=" + std::to_string(cap)); }
        if (isG3 && fps > 0) {
            a.push_back("--gow2_gpu_vsync=false");
            if (cap != k_fpsCaps[fps]) a.push_back("--gow3_max_fps=" + std::to_string(cap));
            else if (fps == 2) a.push_back("--gow3_max_fps=120");
            else if (fps == 3) a.push_back("--gow3_max_fps=-1");
        }
        if (isGJ && fps > 0) { a.push_back("--gow2_gpu_vsync=false"); a.push_back("--gowj_max_fps=" + std::to_string(cap)); }
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

}  // namespace gears
}  // namespace cogbox
