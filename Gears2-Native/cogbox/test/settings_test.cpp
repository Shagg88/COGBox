// CogBox - test of gears_settings: builds the command line for the cases in a cases file (or the built-in ones) and
// prints them in the same form as ps1_args.ps1 prints the PS1's, so the two can be diffed.
//   settings_test                      the built-in cases' args
//   settings_test --cases FILE         the cases in FILE (one per line: key=value;key=value...)
//   settings_test --gen N [SEED]       print the built-in cases plus N random ones (a cases file)
//   settings_test --summary            the option model (counts per game / tab) and the self-checks
// Case keys: game, upd (title update ready), hz, screens, gpus (a/b), exe / rt / files (aliases below), rtfile,
// extra (lines split by |), cards (the graphics cards the PC has, a/b - 0.6.2), and any option id (the setting's value; for
// a text option such as GraphicsCard its text).
#include "../src/gears_settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <sstream>

using namespace cogbox::gears;

namespace {

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) { if (ch == c) { out.push_back(cur); cur.clear(); } else cur += ch; }
    out.push_back(cur);
    return out;
}

std::string exe_needle(const std::string& alias, int g) {
    static const char* proc[4] = { "gow2", "gow3", "gowj", "gow1" };
    if (alias == "gpucard") return "gow2_graphics_card";   // 0.6.2 (GitHub #304)
    if (alias == "fif") return "gow2_renderer_frames_in_flight";
    if (alias == "ups") return "gow2_upscale_mode";
    if (alias == "fg") return "gow2_frame_generation";
    if (alias == "mute") return std::string(proc[g]) + "_mute_unfocused";
    if (alias == "reflex") return "gow2_reflex";
    if (alias == "al2") return "gow2_antilag2";
    if (alias == "aspect") return "gow3_aspect";
    if (alias == "edits") return "'file|section|~=old>new'";
    if (alias == "avx") return "gn_cpu_level=x86-64-v";
    if (alias == "chx") return "GitHub #342: the HUD hidden whenever a player exists";   // console.cpp's --ue3_cheat_* (#342)
    if (alias == "chk") return "GitHub #264 / #342: the developers' cheat keys";            // the keys' new description
    if (alias == "adt") return g == 0 ? "Gears 2 weapon feel for DualSense pads" : "Gears 3 weapon feel for DualSense pads";   // GitHub #245
    if (alias == "adtg") {   // GitHub #245: each game's own needle (Judgment, Gears 1 too)
        static const char* adt[4] = { "Gears 2 weapon feel for DualSense pads", "Gears 3 weapon feel for DualSense pads",
                                      "Judgment weapon feel for DualSense pads", "Gears 1 weapon feel for DualSense pads" };
        return adt[g];
    }
    if (alias == "trr") {   // 0.6.3 (GitHub #245): each game's trigger pulses (Xbox impulse triggers)
        static const char* trr[4] = { "Gears 2 trigger pulses for Xbox pads", "Gears 3 trigger pulses for Xbox pads",
                                      "Judgment trigger pulses for Xbox pads", "Gears 1 trigger pulses for Xbox pads" };
        return trr[g];
    }
    return alias;
}
std::string rt_needle(const std::string& a) {
    if (a == "adt") return "DualSense adaptive triggers: games that support it";   // GitHub #245 (SDK patch 98)
    if (a == "trr") return "Xbox impulse triggers: games that support it";        // 0.6.3, GitHub #245 (SDK patch 104)
    return a == "vs" ? "d3d12_present_vsync" : a == "lat" ? "d3d12_max_frame_latency" : a;
}
std::string file_name(const std::string& a) {
    if (a == "fsr") return "amd_fidelityfx_loader_dx12.dll";
    if (a == "dlss") return "nvngx_dlss.dll";
    if (a == "xess") return "libxess.dll";
    if (a == "fgdll") return "amd_fidelityfx_framegeneration_dx12.dll";
    return a;
}

struct Case {
    Game g = Game::Gears2;
    Environment env;
    std::map<std::string, double> values;
    std::map<std::string, std::string> texts;   // Kind::Text options (GraphicsCard)
    std::set<std::string> exe, rt, files;
};

Case parse_case(const std::string& line) {
    Case c;
    std::map<std::string, std::string> kv;
    for (const std::string& part : split(line, ';')) {
        size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        kv[part.substr(0, eq)] = part.substr(eq + 1);
    }
    int g = kv.count("game") ? std::atoi(kv["game"].c_str()) : 0;
    c.g = static_cast<Game>(g);
    const char* roots[4] = { "C:\\P\\Gears2-Native", "C:\\P\\Gears3-Native", "C:\\P\\GearsJ-Native", "C:\\P\\Gears1-Native" };
    const std::string root = roots[0];   // the PS1's $root: saves, logs and shader caches live in Gears2-Native\out
    const char* udirs[4] = { "\\out\\userdata", "\\out\\gears3-userdata", "\\out\\judgment-userdata", "\\out\\gears1-userdata" };
    const char* pre[4] = { "", "gears3-", "judgment-", "gears1-" };
    const char* tus[4] = { "\\game\\tu6\\package", "\\game\\tu6\\package", "\\game\\tu4\\package", "\\game\\tu5\\package" };
    Environment& e = c.env;
    e.game_data_root = std::string(roots[g]) + "\\game\\extracted";
    e.update_data_root = std::string(roots[g]) + tus[g];
    e.user_data_root = root + udirs[g];
    e.shader_cache_dir = g == 1 ? root + "\\out\\gears3-shader-cache" : g == 2 ? root + "\\out\\judgment-shader-cache" : "";
    e.log_dir = root + "\\out\\play-logs\\" + pre[g] + "20260101-000000";
    e.update_ready = kv.count("upd") && kv["upd"] == "1";
    e.refresh_hz = kv.count("hz") ? std::atoi(kv["hz"].c_str()) : 0;
    e.screen_count = kv.count("screens") ? std::atoi(kv["screens"].c_str()) : 1;
    e.wine = kv.count("wine") && kv["wine"] == "1";   // r4 review: Wine / Proton / CrossOver cases
    e.mac = kv.count("mac") && kv["mac"] == "1";
    if (kv.count("gpus") && !kv["gpus"].empty()) e.gpus = split(kv["gpus"], '/');
    if (kv.count("cards") && !kv["cards"].empty()) e.graphics_cards = split(kv["cards"], '/');   // 0.6.2 (GitHub #304)
    if (kv.count("extra")) e.extra_args_lines = split(kv["extra"], '|');
    if (kv.count("exe") && !kv["exe"].empty()) for (auto& x : split(kv["exe"], ',')) c.exe.insert(exe_needle(x, g));
    if (kv.count("rt") && !kv["rt"].empty()) for (auto& x : split(kv["rt"], ',')) c.rt.insert(rt_needle(x));
    if (kv.count("files") && !kv["files"].empty()) for (auto& x : split(kv["files"], ',')) c.files.insert(file_name(x));
    bool rtfile = kv.count("rtfile") ? kv["rtfile"] == "1" : !c.rt.empty();
    e.g2_disc_ini_edits = c.exe.count("'file|section|~=old>new'") != 0;
    for (auto& p : kv) {
        if (p.first == "game" || p.first == "upd" || p.first == "hz" || p.first == "screens" || p.first == "gpus" || p.first == "extra" ||
            p.first == "exe" || p.first == "rt" || p.first == "files" || p.first == "rtfile" || p.first == "wine" || p.first == "mac" || p.first == "cards") continue;
        const Option* to = find_option(c.g, p.first);
        if (to && to->kind == Kind::Text) { c.texts[p.first] = p.second; continue; }
        c.values[p.first] = std::atof(p.second.c_str());
    }
    int mon = c.values.count("Monitor") ? static_cast<int>(c.values["Monitor"]) : 0;
    e.monitor_device = "DISPLAY" + std::to_string(mon > 0 && mon <= e.screen_count ? mon : 1);
    // The capability callbacks capture copies of the sets.
    std::set<std::string> exe = c.exe, rt = c.rt, files = c.files;
    auto ci = [](std::string s) { for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
    e.cap.exe_has = [exe, ci](const std::string& n) { for (auto& x : exe) if (ci(x) == ci(n)) return true; return false; };
    e.cap.runtime_has = [rt, rtfile](const std::string& n) { return rtfile && rt.count(n) != 0; };
    e.cap.file_next_to_exe = [files](const std::string& n) { return files.count(n) != 0; };
    return c;
}

void run_case(int n, const std::string& line) {
    Case c = parse_case(line);
    const int gi = static_cast<int>(c.g);
    Store st;
    for (auto& p : c.values) {
        const Option* o = find_option(c.g, p.first);
        if (!o) { std::fprintf(stderr, "unknown option %s\n", p.first.c_str()); std::exit(2); }
        if (o->scope == Scope::GameOwn) st.own[gi][p.first] = p.second; else st.top[p.first] = p.second;
    }
    for (auto& p : c.texts) st.texts[p.first] = p.second;
    Settings s = st.view(c.g, c.env);
    Version ver = effective_version(c.g, s, c.env);
    std::vector<std::string> notes;
    std::vector<std::string> args = build_args(c.g, ver, s, c.env, &notes);
    std::cout << "CASE " << n << " " << line << "\n";
    for (auto& a : args) std::cout << "ARG " << a << "\n";
    for (auto& x : notes) std::cout << "NOTE " << x << "\n";
    std::cout << "END\n";
}

const char* const k_builtin[] = {
    "game=0",
    "game=0;upd=1;Version=0;Resolution=2;AAMode=3;gpus=NVIDIA GeForce RTX 4080;files=dlss,fsr;exe=fif,ups,reflex,mute,edits;rt=vs,lat;SceneMode=2;FrameCap=7;Vsync=1;hz=144;LowLag=1;Mute=1;SkipLogos=1;Trilinear=1",
    "game=0;Resolution=4;AAMode=3;gpus=AMD Radeon RX 7900 XTX;exe=fif,al2;LowLag=1;FrameCap=4;Fullscreen=2;Monitor=2;screens=2;HDR=1;PaperWhite=250;Peak=1400;Mnk=1;Sens=1.3",
    "game=0;Fov=115;MotionBlur=0;DoF=0;Shake=2;DeadZoneL=10;DeadZoneR=10;Vibration=0;WindowSize=3;Players=1;FpsMode=1;Pool=0;Aniso=0;extra=--gow2_frame_times={LOG}\\ft.csv|# comment| --foo=1 |",
    "game=0;upd=1;Version=1;LowLag=0;exe=fif,fg;rt=vs,lat;FrameCap=2;Vsync=1;hz=60;FrameGen=1;files=fgdll;FpsMode=3",
    "game=0;AAMode=2;FrameCap=4;Vsync=1;hz=0;rt=vs;Pool=3;DrawDist=2;MipFade=1;Ultrawide=1;gpus=Microsoft Basic Display Adapter",
    "game=1",
    "game=1;upd=1;Version=0;Cheats=1;Mutators=1;Resolution=4;exe=fif;FrameCap=2",
    "game=1;upd=1;Version=1;Cheats=1;Mutators=1;Resolution=4;exe=fif,aspect,mute;Mute=1;FrameCap=2;Vsync=1;rt=vs;hz=0",
    "game=1;FrameCap=2;Vsync=1;rt=vs;hz=144;Ultrawide=1;Resolution=4;FpsMode=3;Aniso=1;Trilinear=0;MipFade=0;Shake=1;DrawDist=1;SkipLogos=1",
    "game=1;FrameCap=4;AAMode=4;files=xess;exe=ups,fif;SceneMode=1;Resolution=2;Players=3;LowLag=0;rt=lat",
    "game=1;FrameCap=7;Vsync=1;rt=vs;hz=240;FrameGen=1;exe=fg;files=fgdll;Fov=80",
    "game=1;AdaptiveTriggers=1;exe=adt;rt=adt",   // GitHub #245: sent
    "game=1;AdaptiveTriggers=1;exe=adt",          // runtime without patch 98: greyed, not sent
    "game=2",
    "game=2;upd=1;SkipLogos=1;Pool=1;FpsMode=3;FrameCap=7;Ultrawide=1",
    "game=2;Pool=3;FrameCap=2;Vsync=1;rt=vs;hz=165;FrameGen=1;exe=fg,fif,mute;files=fgdll;Mute=1;LowLag=1;gpus=Intel(R) UHD Graphics/NVIDIA GeForce GTX 1080",
    "game=2;AAMode=3;gpus=NVIDIA GeForce GTX 1080;files=fsr;FrameCap=4;Shake=2;Fov=125;DoF=0;Cheats=1;Mutators=1",
    "game=2;Resolution=4;Fullscreen=0;WindowSize=5;Mnk=1;Sens=0.1;DeadZoneL=25;DeadZoneR=8;Vibration=0;Monitor=1;screens=1;HDR=1",
    "game=0;AdaptiveTriggers=1;exe=adt;rt=adt",   // GitHub #245, Gears 2: sent
    "game=0;AdaptiveTriggers=1;rt=adt",           // Gears 2 exe without the weapon table: greyed, not sent
    "game=2;AdaptiveTriggers=1;exe=adt;rt=adt",   // Judgment exe without its own weapon table (Gears 3's needle): not sent
    "game=2;AdaptiveTriggers=1;exe=adtg;rt=adt",  // GitHub #245, Judgment: sent
    // GitHub #342: the cheats that are simply on - sent when the exe has them; Gears 3 TU6: all weapons greyed (no cheat manager)
    "game=0;exe=chx;CheatGod=1;CheatWeapons=1;CheatSpeed=2;CheatHud=1;Cheats=1",
    "game=0;exe=chx,chk;Cheats=1",   // the cheat keys with the rewritten description (review r342): --gow2_cheats sent
    "game=3;exe=chk;Cheats=1",       // Gears 1 too
    "game=1;upd=1;exe=chx;CheatGod=1;CheatWeapons=1;CheatSpeed=3;CheatHud=1",
    "game=2;exe=chx;CheatGod=1;CheatSpeed=1",
    "game=3;exe=chx;CheatWeapons=1;CheatHud=1",
    "game=3;CheatGod=1;CheatWeapons=1;CheatSpeed=2",   // an exe without them: greyed, nothing sent
    "game=2;AdaptiveTriggers=1;exe=adtg",         // Judgment, runtime without patch 98: greyed, not sent
    "game=3;AdaptiveTriggers=1;exe=adtg;rt=adt",  // GitHub #245, Gears 1: sent
    "game=3;AdaptiveTriggers=1;rt=adt",           // Gears 1 exe without the weapon table: greyed, not sent
    // 0.6.2 (GitHub #304): Graphics card - sent for every game when the card is in the PC; not when missing / Automatic / no support
    "game=0;exe=gpucard;cards=NVIDIA GeForce RTX 4090/NVIDIA GeForce RTX 3090;GraphicsCard=NVIDIA GeForce RTX 3090",
    "game=1;exe=gpucard;cards=NVIDIA GeForce RTX 4090/NVIDIA GeForce RTX 3090;GraphicsCard=NVIDIA GeForce RTX 3090",
    "game=2;exe=gpucard;cards=NVIDIA GeForce RTX 4090/NVIDIA GeForce RTX 3090;GraphicsCard=nvidia geforce rtx 4090",
    "game=3;exe=gpucard;cards=NVIDIA GeForce RTX 4090/NVIDIA GeForce RTX 3090;GraphicsCard=NVIDIA GeForce RTX 3090",
    "game=0;exe=gpucard;cards=NVIDIA GeForce RTX 4090;GraphicsCard=NVIDIA GeForce RTX 3090",          // missing: not sent, a note
    "game=0;exe=gpucard;GraphicsCard=NVIDIA GeForce RTX 3090",                                          // no card list: not sent
    "game=0;exe=gpucard;cards=NVIDIA GeForce RTX 4090/NVIDIA GeForce RTX 3090",                         // Automatic: nothing
    "game=0;cards=NVIDIA GeForce RTX 4090;GraphicsCard=NVIDIA GeForce RTX 4090",                        // older exe: greyed, not sent
    "game=0;exe=gpucard;cards=NVIDIA GeForce RTX 3080/NVIDIA GeForce RTX 3080 Ti;GraphicsCard=NVIDIA GeForce RTX 3080",   // name inside another's: a note
    "game=1;exe=gpucard;cards=AMD Radeon(TM) Graphics/AMD Radeon RX 7900 XTX;GraphicsCard=  AMD Radeon RX 7900 XTX  ",   // the name is trimmed
    // 0.6.3 (GitHub #245): Trigger rumble (Xbox controllers) - sent with the runtime's patch 104 and the game's trigger pulses
    "game=0;TriggerRumble=1;exe=trr;rt=trr",                                   // Gears 2: sent
    "game=1;TriggerRumble=1;AdaptiveTriggers=1;exe=trr,adt;rt=trr,adt",       // Gears 3: both trigger options sent
    "game=2;TriggerRumble=1;exe=trr;rt=trr",                                   // Judgment: sent
    "game=3;TriggerRumble=1;exe=trr;rt=trr",                                   // Gears 1: sent
    "game=1;TriggerRumble=1;exe=trr",                                          // runtime without patch 104: greyed, not sent
    "game=1;TriggerRumble=1;rt=trr",                                           // exe without trigger pulses: greyed, not sent
    "game=1;TriggerRumble=1;exe=trr;rt=trr;Vibration=0",                       // Controller vibration off: greyed, not sent
    "game=1;TriggerRumble=1;exe=trr;rt=trr;wine=1",                            // Proton / Wine: greyed, not sent
};

std::string random_case(std::mt19937& r) {
    auto pick = [&](int n) { return static_cast<int>(r() % static_cast<unsigned>(n)); };
    std::ostringstream o;
    int g = pick(3);
    o << "game=" << g << ";upd=" << pick(2);
    const int hzs[] = { 0, 30, 59, 60, 75, 120, 144, 165, 240 };
    o << ";hz=" << hzs[pick(9)];
    int screens = 1 + pick(3);
    o << ";screens=" << screens;
    const char* gpus[] = { "", "NVIDIA GeForce RTX 4070", "NVIDIA GeForce GTX 1660", "AMD Radeon RX 6800", "Intel(R) Arc(TM) A770 Graphics",
                           "Intel(R) UHD Graphics 770/NVIDIA GeForce RTX 3060", "Microsoft Basic Display Adapter", "NVIDIA Quadro RTX 5000" };
    o << ";gpus=" << gpus[pick(8)];
    const char* ex[] = { "fif", "ups", "fg", "mute", "reflex", "al2", "aspect", "edits" };
    std::string sep;
    o << ";exe=";
    for (const char* x : ex) if (pick(2)) { o << sep << x; sep = ","; }
    o << ";rt="; sep.clear();
    for (const char* x : { "vs", "lat" }) if (pick(2)) { o << sep << x; sep = ","; }
    o << ";files="; sep.clear();
    for (const char* x : { "fsr", "dlss", "xess", "fgdll" }) if (pick(2)) { o << sep << x; sep = ","; }
    if (pick(5) == 0) o << ";extra=--a=1|#x|  --b={LOG}  ";
    for (const Option& op : options(static_cast<Game>(g))) {
        if (pick(3) == 0) continue;   // leave some at the default
        double v = 0;
        switch (op.kind) {
        case Kind::Bool: v = pick(2); break;
        case Kind::Choice: v = op.id == "Monitor" ? pick(screens + 1) : pick(static_cast<int>(op.choices.size())); break;
        case Kind::Text: break;   // (typed text: not in the random cases)
        case Kind::Int: case Kind::Float: {
            int steps = static_cast<int>((op.max - op.min) / op.step + 0.5);
            v = op.min + pick(steps + 1) * op.step;
            break;
        }
        }
        if (op.kind == Kind::Float) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); o << ";" << op.id << "=" << b; }
        else o << ";" << op.id << "=" << static_cast<long long>(v + (v < 0 ? -0.5 : 0.5));
    }
    return o.str();
}

int summary() {
    int fails = 0;
    const char* gn[3] = { "Gears 2", "Gears 3", "Judgment" };
    for (int g = 0; g < 3; ++g) {
        std::map<std::string, int> per;
        for (const Option& o : options(static_cast<Game>(g))) per[tab_name(o.tab)]++;
        std::cout << gn[g] << ": " << options(static_cast<Game>(g)).size() << " options;";
        for (auto& p : per) std::cout << " " << p.first << " " << p.second;
        // availability with a current build (every capability present) and with none
        for (int full = 1; full >= 0; --full) {
            Environment e; e.update_ready = full; e.g2_disc_ini_edits = full; e.screen_count = full ? 2 : 1;
            e.cap.exe_has = [full](const std::string&) { return full != 0; };
            e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
            Store st; Settings s = st.view(static_cast<Game>(g), e);
            s.set("Fullscreen", 1); s.set("HDR", 1); s.set("AAMode", 2);
            int greyed = 0;
            for (const Option& o : options(static_cast<Game>(g)))
                if (!available(o, static_cast<Game>(g), Version::Disc, s, e).ok) ++greyed;
            std::cout << (full ? "; greyed (all builds there, Disc): " : "; greyed (nothing there): ") << greyed;
        }
        std::cout << "\n";
    }
    // ini round trip
    {
        Environment e; e.update_ready = true;
        Store st; Settings s = st.view(Game::Gears3, e);
        set_value(Game::Gears3, s, "Preset", PresetUltra);
        if (s.i("Resolution") != 4 || s.i("Preset") != 2) { std::cout << "FAIL preset\n"; ++fails; }
        set_value(Game::Gears3, s, "Fov", 110);
        if (s.i("Preset") != 3) { std::cout << "FAIL preset -> custom\n"; ++fails; }
        s.set("Sens", 1.7);   // (not "Start": removed in 0.3.0, a saved value is ignored)
        st.commit(Game::Gears3, s, e);
        std::string ini = st.save_ini();
        Store st2; st2.load_ini(ini);
        if (st2.save_ini() != ini) { std::cout << "FAIL ini round trip\n"; ++fails; }
        Settings s2 = st2.view(Game::Gears3, e);
        if (s2.f("Sens") != 1.7 || s2.i("Fov") != 110 || st2.last_game != 1) { std::cout << "FAIL ini values\n"; ++fails; }
        // Gears 2 has no set of its own yet: it starts from the last game's (PS1 429)
        Settings g2 = st2.view(Game::Gears2, e);
        if (g2.i("Fov") != 110 || g2.i("Start") != 0 || !g2.b("Aniso")) { std::cout << "FAIL per-game fallback\n"; ++fails; }
        g2.set("Fov", 90); st2.remember(Game::Gears2, g2);
        if (st2.view(Game::Gears3, e).i("Fov") != 110 || st2.view(Game::Gears2, e).i("Fov") != 90) { std::cout << "FAIL per-game memory\n"; ++fails; }
        std::cout << "--- ini after a Gears 3 save ---\n" << ini;
    }
    // GitHub #316: the window-mode key shown on the Controls tab: Alt+Return unless extra-args.txt changes it
    {
        Environment e;
        auto key = [&](std::vector<std::string> lines) { e.extra_args_lines = std::move(lines); return window_key(e); };
        const std::pair<std::vector<std::string>, const char*> want[] = {
            { {}, "Alt+Return" },
            { { "# --bind_cogbox_fullscreen=F11", "--gow2_cheats=true" }, "Alt+Return" },   // a comment, another switch
            { { "--bind_cogbox_fullscreen=F12" }, "F12" },
            { { "  --bind_cogbox_fullscreen=\"Ctrl+Shift+F11\"  " }, "Ctrl+Shift+F11" },
            { { "--bind_cogbox_fullscreen=F10", "--bind_cogbox_fullscreen=None" }, "None" },   // the last line wins
            { { "--bind_cogbox_fullscreen=" }, "None" } };
        for (const auto& w : want)
            if (key(w.first) != w.second) { std::cout << "FAIL window key, want " << w.second << " got " << key(w.first) << "\n"; ++fails; }
        if (std::string(kWindowKeyDefault) != "Alt+Return") { std::cout << "FAIL window key default\n"; ++fails; }
    }
    // Achievements part 4: the popup settings are on their own tab, with these labels
    {
        const std::pair<const char*, const char*> want[] = {
            { "AchievementStyle", "Popup style" }, { "AchievementColour", "Popup colour (Xbox style)" },
            { "AchievementPosition", "Popup position" }, { "AchievementSound", "Popup sound" },
            { "HideAchievementPopups", "Hide achievement popups" } };
        for (int g = 0; g < 3; ++g)
            for (const auto& w : want) {
                const Option* found = nullptr;
                for (const Option& o : options(static_cast<Game>(g))) if (o.id == w.first) found = &o;
                if (!found || std::string(tab_name(found->tab)) != "Achievements" || found->label != w.second) {
                    std::cout << "FAIL achievements tab " << gn[g] << " " << w.first << "\n";
                    ++fails;
                }
            }
    }
    // GitHub #159: a game / voice language not on the player's copy is not passed (a choice saved before), and said so
    for (int miss = 0; miss <= 1; ++miss) {
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        if (miss) { e.lang_missing.assign(22, false); e.lang_missing[7] = true; e.voice_missing.assign(10, false); e.voice_missing[6] = true; }
        Store st; Settings s = st.view(Game::Gears3, e);
        s.set("Lang", 7); s.set("VoiceLang", 6);
        std::vector<std::string> notes;
        const std::vector<std::string> a = build_args(Game::Gears3, Version::TitleUpdate, s, e, &notes);
        auto has = [&](const std::string& v) { for (const std::string& x : a) if (x.rfind(v, 0) == 0) return true; return false; };
        int said = 0;
        for (const std::string& n : notes) if (n.find("is not on this copy") != std::string::npos) ++said;
        if (has("--user_language=") == (miss != 0) || has("--ue3_voice_language=") == (miss != 0) || said != (miss ? 2 : 0)) {
            std::cout << "FAIL languages not on the copy (" << (miss ? "missing" : "present") << ")\n"; ++fails;
        }
    }
    {   // cg050: "Faster game code" is no user option; 0.6.3: no -normal exe or crash fallback either (one build per game)
        for (Game g : { Game::Gears1, Game::Gears2, Game::Gears3, Game::Judgment })
            if (find_option(g, "FastCode")) { std::cout << "FAIL FastCode should not be a user option\n"; ++fails; }
    }
    // KiltedKernel: texture packs - the enabled mods that hold pack textures (r95 L1: only those), the one loaded last
    // first, and only for a game exe that has the option; none at all without such a mod
    for (int cap = 0; cap <= 1; ++cap)
        for (int packs = 0; packs <= 1; ++packs) {
            Environment e; e.update_ready = true;
            e.cap.exe_has = [cap](const std::string& n) { return cap || n != "gow2_texture_pack_dirs"; };
            e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
            e.mods = { "C:/G/mods/A", "C:/G/mods/B", "C:/G/mods/C" };
            if (packs) e.texture_pack_mods = { "C:/G/mods/A", "C:/G/mods/C" };
            Store st; Settings s = st.view(Game::Gears3, e);
            const std::vector<std::string> a = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
            std::string got;
            for (const std::string& x : a) if (x.rfind("--gow2_texture_pack_dirs=", 0) == 0) got = x;
            const std::string want = "--gow2_texture_pack_dirs=\"C:/G/mods/C;C:/G/mods/A\"";
            if (cap && packs ? got != want : !got.empty()) { std::cout << "FAIL texture pack folders (" << got << ")\n"; ++fails; }
        }
    // 0.6.4 (GitHub #341): the texture cache - Auto sends nothing (512) without a texture pack and 2048 with one; a size
    // other than 512 is sent as picked; nothing for an exe without the cvar
    for (Game g : { Game::Gears1, Game::Gears2, Game::Gears3, Game::Judgment })
        for (int cap = 0; cap <= 1; ++cap)
            for (int packs = 0; packs <= 1; ++packs)
                for (int pick = 0; pick <= 4; ++pick)
                for (unsigned vram : { 0u, 4096u, 5900u, 8192u }) {
                    Environment e; e.update_ready = true; e.vram_mb = vram;
                    e.cap.exe_has = [cap](const std::string& n) { return cap || n != "gow2_renderer_texture_cache_mb"; };
                    e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
                    if (packs) { e.mods = { "C:/G/mods/A" }; e.texture_pack_mods = { "C:/G/mods/A" }; }
                    Store st; Settings s = st.view(g, e);
                    s.set("TexCache", pick);
                    const std::vector<std::string> a = build_args(g, Version::TitleUpdate, s, e, nullptr);
                    std::string got;
                    for (const std::string& x : a) if (x.rfind("--gow2_renderer_texture_cache_mb=", 0) == 0) got = x;
                    static const int k_mb[5] = { 0, 512, 1024, 2048, 4096 };
                    const int mb = pick ? k_mb[pick] : packs ? (vram >= 5800 ? 2048 : 1024) : 512;
                    const std::string want = cap && mb != 512 ? "--gow2_renderer_texture_cache_mb=" + std::to_string(mb) : "";
                    if (got != want) { std::cout << "FAIL texture cache (game " << int(g) << ", cap " << cap << ", packs " << packs
                                                 << ", pick " << pick << ", vram " << vram << "): '" << got << "'\n"; ++fails; }
                }
    // social step 3: with the multiplayer server enabled the games start online; without, none of it
    for (int on = 0; on <= 1; ++on) {
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        if (on) { e.online_server = "http://1.2.3.4:18443"; e.online_stun = "1.2.3.4:3478"; }
        Store st; Settings s = st.view(Game::Gears3, e);
        const std::vector<std::string> a = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
        auto has = [&](const std::string& v) { for (const std::string& x : a) if (x == v) return true; return false; };
        const bool all = has("--net_online=true") && has("--net_live_emulation=true") &&
                         has("--net_master_server=\"http://1.2.3.4:18443\"") && has("--net_stun_server=1.2.3.4:3478");
        const bool any = has("--net_online=true") || has("--net_live_emulation=true");
        if (on ? !all : any) { std::cout << "FAIL online args (" << (on ? "on" : "off") << ")\n"; ++fails; }
        e.online_region = "europe";
        const std::vector<std::string> b = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
        const bool sent = std::find(b.begin(), b.end(), "--net_region=europe") != b.end();
        if (sent != (on != 0)) { std::cout << "FAIL region arg (" << on << ")\n"; ++fails; }
        e.online_region.clear();
        const std::vector<std::string> c = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
        for (const std::string& x : c) if (x.rfind("--net_region", 0) == 0) { std::cout << "FAIL region arg for Any\n"; ++fails; }
    }
    // Network rates: online, Gears 2 / 3 / Judgment get 100000 B/s and a 60 Hz host in their own engine ini; offline none
    for (Game g : { Game::Gears2, Game::Gears3, Game::Judgment }) {
        for (int on = 0; on <= 1; ++on) {
            Environment e; e.update_ready = true;
            e.cap.exe_has = [](const std::string&) { return true; };
            e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
            if (on) { e.online_server = "http://1.2.3.4:18443"; e.online_stun = "1.2.3.4:3478"; }
            Store st; Settings s = st.view(g, e);
            std::string ini;
            for (const std::string& x : build_args(g, Version::TitleUpdate, s, e, nullptr))
                if (x.rfind("--ue3_ini_edits=", 0) == 0) ini = x;
            const std::string ei = g == Game::Gears2 ? "Xe-GearEngine.ini" : "Xbox360-GearEngine.ini";
            bool all = true;
            for (const char* k : { "|IpDrv.TcpNetDriver|NetServerMaxTickRate=60", "|IpDrv.TcpNetDriver|MaxInternetClientRate=100000",
                                   "|IpDrv.TcpNetDriver|MaxClientRate=100000", "|Engine.Player|ConfiguredInternetSpeed=100000",
                                   "|Engine.Player|ConfiguredLanSpeed=100000", "|IpDrv.TcpNetDriver|LanServerMaxTickRate=60" })
                all = all && ini.find(ei + k) != std::string::npos;
            const bool any = ini.find("MaxInternetClientRate") != std::string::npos;
            if (on ? !all : any) { std::cout << "FAIL net rates for game " << static_cast<int>(g) << " (" << (on ? "on" : "off") << ")\n"; ++fails; }
        }
    }
    for (int has = 0; has < 2; ++has) {   // 0.6.5: Horde bots on Gears 3 also send the bot-characters fix when gow3.exe has it
        Environment e; e.update_ready = true;
        if (has) e.cap.exe_has = [](const std::string&) { return true; };
        else e.cap.exe_has = [](const std::string& k) { return k != "gow3_horde_bot_characters"; };
        e.cap.runtime_has = [](const std::string&) { return true; }; e.cap.file_next_to_exe = e.cap.runtime_has;
        Store st; Settings s = st.view(Game::Gears3, e);
        s.set("HordeBots", 1);
        const std::vector<std::string> a = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
        const bool hb = std::find(a.begin(), a.end(), "--gow3_horde_bots=true") != a.end();
        const bool hc = std::find(a.begin(), a.end(), "--gow3_horde_bot_characters=true") != a.end();
        if (!hb || hc != (has == 1)) { std::cout << "FAIL horde bot characters: exe has it " << has << ", bots " << hb << ", characters " << hc << "\n"; ++fails; }
        s.set("HordeBots", 0);
        const std::vector<std::string> b = build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr);
        if (std::find(b.begin(), b.end(), "--gow3_horde_bot_characters=true") != b.end()) { std::cout << "FAIL horde bot characters sent with Horde bots off\n"; ++fails; }
    }
    {   // 0.6.5: a gow1.exe without --gow1_no_aspect_lock keeps the option off (it would letterbox)
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string& k) { return k != "gow1_no_aspect_lock"; };
        e.cap.runtime_has = [](const std::string&) { return true; }; e.cap.file_next_to_exe = e.cap.runtime_has;
        e.monitor_w = 1280; e.monitor_h = 800;
        Store st; Settings s = st.view(Game::Gears1, e);
        s.set("AspectRatio", 3);
        const std::vector<std::string> a = build_args(Game::Gears1, Version::TitleUpdate, s, e, nullptr);
        if (std::find(a.begin(), a.end(), "--video_mode_width=1152") != a.end()) { std::cout << "FAIL 16:10 old gow1.exe\n"; ++fails; }
    }
    // 0.6.5 16:10: Aspect ratio 16:10 (index 3, added last) sends a 1152x720 non-widescreen mode; Auto does on a 16:10
    // screen (1280x800 / 1920x1200) and not on 16:9 (1920x1080) or 21:9 (3440x1440); 16:9 sends no mode; Gears 1 also gets --gow1_no_aspect_lock
    for (Game g : { Game::Gears2, Game::Gears3, Game::Judgment, Game::Gears1 }) {
        struct Case { int ar, w, h; bool want; } cases[] = {
            { 3, 1920, 1080, true }, { 0, 1280, 800, true }, { 0, 1920, 1200, true }, { 0, 1920, 1080, false },
            { 0, 3440, 1440, false }, { 0, 0, 0, false }, { 1, 1280, 800, false } };
        for (const Case& k : cases) {
            Environment e; e.update_ready = true;
            e.cap.exe_has = [](const std::string&) { return true; };
            e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
            e.monitor_w = k.w; e.monitor_h = k.h;
            Store st; Settings s = st.view(g, e);
            s.set("AspectRatio", k.ar);
            const std::vector<std::string> a = build_args(g, Version::TitleUpdate, s, e, nullptr);
            auto has = [&](const std::string& v) { return std::find(a.begin(), a.end(), v) != a.end(); };
            const bool want = k.want;
            const bool sent = has("--video_mode_width=1152") && has("--video_mode_height=720") && has("--video_mode_widescreen=false");
            const bool any = has("--video_mode_width=1152") || has("--video_mode_widescreen=false");
            if (g == Game::Gears1 && has("--gow1_no_aspect_lock=true") != want) {
                std::cout << "FAIL 16:10 Gears 1 lock: choice " << k.ar << " screen " << k.w << "x" << k.h << "\n"; ++fails;
            }
            if (g != Game::Gears1 && has("--gow1_no_aspect_lock=true")) { std::cout << "FAIL 16:10 lock sent to game " << static_cast<int>(g) << "\n"; ++fails; }
            if (want ? !sent : any) {
                std::cout << "FAIL 16:10 args: game " << static_cast<int>(g) << " choice " << k.ar << " screen " << k.w << "x" << k.h << "\n";
                ++fails;
            }
        }
    }
    // 0.6.5 stick dead zones: one slider per stick, 1 % steps; the old 6-step "DeadZone" choice read into both sliders
    {
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        Store st; Settings s = st.view(Game::Gears2, e);
        s.set("DeadZoneL", 12); s.set("DeadZoneR", 0);
        const std::vector<std::string> a = build_args(Game::Gears2, Version::TitleUpdate, s, e, nullptr);
        auto has = [&](const std::string& v) { return std::find(a.begin(), a.end(), v) != a.end(); };
        bool right = false;
        for (const std::string& x : a) right = right || x.rfind("--right_stick_deadzone_percentage", 0) == 0;
        if (!has("--left_stick_deadzone_percentage=0.12") || right) { std::cout << "FAIL per-stick dead zone args\n"; ++fails; }
        Store old;
        old.load_ini("[General]\nDeadZone=3\n");
        const Settings o = old.view(Game::Gears3, e);
        if (o.i("DeadZoneL") != 15 || o.i("DeadZoneR") != 15) { std::cout << "FAIL old DeadZone choice not moved to both sliders\n"; ++fails; }
        Store both;
        both.load_ini("[General]\nDeadZoneL=7\nDeadZone=3\nDeadZoneR=9\n");
        const Settings b = both.view(Game::Gears3, e);
        if (b.i("DeadZoneL") != 7 || b.i("DeadZoneR") != 9) { std::cout << "FAIL saved sliders overridden by the old choice\n"; ++fails; }
        const std::string saved = old.save_ini();
        if (saved.find("DeadZoneL=15") == std::string::npos || saved.find("\nDeadZone=") != std::string::npos) { std::cout << "FAIL dead zone save\n"; ++fails; }
    }
    // Ping in the FPS bar: Show ping sends --cogbox_show_ping=true (the exe must know it); off sends nothing
    for (int on = 0; on <= 1; ++on) {
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        Store st; Settings s = st.view(Game::Gears3, e);
        if (!find_option(Game::Gears3, "ShowPing")) { std::cout << "FAIL ShowPing is not an option\n"; ++fails; break; }
        s.set("ShowPing", on);
        bool has = false;
        for (const std::string& x : build_args(Game::Gears3, Version::TitleUpdate, s, e, nullptr)) has = has || x == "--cogbox_show_ping=true";
        if (has != (on != 0)) { std::cout << "FAIL show ping arg (" << on << ")\n"; ++fails; }
    }
    // social step 4: Gears 3 TU6 gets its UE3 GNames / GEngine addresses (the private match lobby fixes), others not
    for (int gi = 0; gi < 2; ++gi) {
        const Game g = gi ? Game::Judgment : Game::Gears3;
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        Store st; Settings s = st.view(g, e);
        const std::vector<std::string> a = build_args(g, Version::TitleUpdate, s, e, nullptr);
        auto has = [&](const std::string& v) { for (const std::string& x : a) if (x == v) return true; return false; };
        const bool want = g == Game::Gears3;
        if (has("--cogbox_ue3_gnames=8344D918") != want || has("--cogbox_ue3_gengine=8343987C") != want) {
            std::cout << "FAIL UE3 addresses for " << (gi ? "Judgment" : "Gears 3") << "\n"; ++fails;
        }
    }
    // 0.6.2 (GitHub #304): Graphics card - Automatic sends nothing; a card that is in the PC is sent by name to every game; one that
    // is not (removed, other PC, no list) falls back to Automatic; an exe without the switch greys the option; the name survives the ini
    {
        const std::string k_4090 = "NVIDIA GeForce RTX 4090", k_3090 = "NVIDIA GeForce RTX 3090";
        for (Game g : { Game::Gears2, Game::Gears3, Game::Judgment, Game::Gears1 }) {
            const int gi = static_cast<int>(g);
            Environment e; e.update_ready = true;
            e.graphics_cards = { k_4090, k_3090 };
            e.cap.exe_has = [](const std::string& n) { return n == "gow2_graphics_card"; };
            e.cap.runtime_has = [](const std::string&) { return false; }; e.cap.file_next_to_exe = e.cap.runtime_has;
            auto card_args = [&](const Environment& env, const std::string& name) {
                Store st; st.texts["GraphicsCard"] = name;
                Settings s = st.view(g, env);
                std::vector<std::string> out;
                for (const std::string& x : build_args(g, Version::TitleUpdate, s, env, nullptr))
                    if (x.find("graphics_card") != std::string::npos) out.push_back(x);
                return out;
            };
            const std::string want = "--gow2_graphics_card=\"" + k_3090 + "\"";
            if (!card_args(e, "").empty()) { std::cout << "FAIL graphics card Automatic sends something (game " << gi << ")\n"; ++fails; }
            const auto sent = card_args(e, k_3090);
            if (sent.size() != 1 || sent[0] != want) { std::cout << "FAIL graphics card not sent (game " << gi << ")\n"; ++fails; }
            const auto lower = card_args(e, "nvidia geforce rtx 3090");
            if (lower.size() != 1 || lower[0] != "--gow2_graphics_card=\"nvidia geforce rtx 3090\"") { std::cout << "FAIL graphics card case (game " << gi << ")\n"; ++fails; }
            if (!card_args(e, "AMD Radeon RX 7900 XTX").empty()) { std::cout << "FAIL missing graphics card is sent (game " << gi << ")\n"; ++fails; }
            Environment none = e; none.graphics_cards.clear();
            if (!card_args(none, k_3090).empty()) { std::cout << "FAIL graphics card sent without a card list (game " << gi << ")\n"; ++fails; }
            Environment old = e; old.cap.exe_has = [](const std::string&) { return false; };
            if (!card_args(old, k_3090).empty()) { std::cout << "FAIL graphics card sent to an exe without it (game " << gi << ")\n"; ++fails; }
        }
        const Option* o = find_option(Game::Gears1, "GraphicsCard");
        if (!o || o->kind != Kind::Text || o->tab != Tab::Graphics || o->label != "Graphics card") { std::cout << "FAIL graphics card option\n"; ++fails; }
        else {
            if (sanitize_text(*o, "  NVIDIA \"GeForce\"\tRTX 4090\n ") != "NVIDIA GeForceRTX 4090") { std::cout << "FAIL graphics card sanitise\n"; ++fails; }
            if (static_cast<int>(sanitize_text(*o, std::string(500, 'x')).size()) != o->max_len) { std::cout << "FAIL graphics card length\n"; ++fails; }
            Environment e; e.update_ready = true; e.graphics_cards = { k_4090 };
            Store st; Settings s = st.view(Game::Gears2, e);
            s.t["GraphicsCard"] = k_4090;
            st.commit(Game::Gears2, s, e);
            const std::string ini = st.save_ini();
            Store st2; st2.load_ini(ini);
            if (st2.view(Game::Gears3, e).str("GraphicsCard") != k_4090 || st2.save_ini() != ini ||
                ini.find("GraphicsCard=" + k_4090) == std::string::npos) { std::cout << "FAIL graphics card ini\n"; ++fails; }
            Store st3; if (!st3.view(Game::Gears2, e).str("GraphicsCard").empty()) { std::cout << "FAIL graphics card default\n"; ++fails; }
        }
    }
    // first-person camera: each game's own switch (Gears 2, Gears 3, Judgment) when its exe has the option, none in Gears 1
    for (Game g : { Game::Gears2, Game::Gears3, Game::Judgment, Game::Gears1 }) {
        if (!kOfferFirstPerson) break;   // 0.6.2: hidden tester option, not in the launcher
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        Store st; Settings s = st.view(g, e);
        s.set("FirstPerson", 1);
        const std::vector<std::string> a = build_args(g, Version::TitleUpdate, s, e, nullptr);
        const char* want = g == Game::Gears2 ? "--gow2_first_person=true" : g == Game::Gears3 ? "--gow3_first_person=true" :
                           g == Game::Judgment ? "--gowj_first_person=true" : "";
        int n = 0; bool ok = true;
        for (const std::string& x : a)
            if (x.find("_first_person") != std::string::npos) { ++n; ok = ok && x == want; }
        if (n != (*want ? 1 : 0) || !ok) { std::cout << "FAIL first person for game " << static_cast<int>(g) << "\n"; ++fails; }
    }
    // 0.6.3: the Custom controller layout editor - CogBox.ini [Pad] Remap= read as the runtime reads pad_remap (SDK patch 55),
    // written back with only the moved buttons; a controller button taken by another game button swaps with it
    {
        auto bad = [&](const std::string& what) { std::cout << "FAIL pad layout: " << what << "\n"; ++fails; };
        PadMap id;
        if (format_pad_remap(id) != "" || !parse_pad_remap("").same_layout(id) || !parse_pad_remap("").unknown.empty()) bad("empty");
        if (pad_input_from_token("LB") != PadLB || pad_input_from_token("Start") != PadStart || pad_input_from_token("l3") != -1) bad("names");
        for (int i = 0; i < kPadInputs; ++i)
            if (pad_input_from_token(pad_input_token(i)) != i || !pad_input_label(i)) bad(std::string("token ") + pad_input_token(i));
        // the presets read back as written (pairs in the list's order) and send what they always sent
        const char* want[4] = { "", "sticks=swap", "a=b,b=a", "lb=lt,rb=rt,lt=lb,rt=rb" };
        for (int l = 0; l < 4; ++l) {
            const PadMap m = parse_pad_remap(pad_layout_preset(l));
            if (format_pad_remap(m) != want[l] || !m.unknown.empty()) bad(std::string("preset ") + std::to_string(l));
        }
        {
            const PadMap m = parse_pad_remap(pad_layout_preset(3));
            if (m.to[PadLB] != PadLT || m.to[PadLT] != PadLB || m.to[PadRB] != PadRT || m.to[PadRT] != PadRB || m.to[PadA] != PadA) bad("preset 3 map");
        }
        // hand-written: any case, spaces, empty parts, identity pairs, the last pair for a button wins, unknown parts kept
        {
            const PadMap m = parse_pad_remap(" A = B , b=a,,x=x, Sticks=Swap, y=lb, y=rt, l3=a, a=, up=down=left, foo ");
            if (m.to[PadA] != PadB || m.to[PadB] != PadA || m.to[PadX] != PadX || m.to[PadY] != PadRT || !m.sticks_swapped) bad("hand-written map");
            if (m.unknown != std::vector<std::string>({ "l3=a", "a=", "up=down=left", "foo" })) bad("unknown parts");
            const std::string f = format_pad_remap(m);
            if (f != "a=b,b=a,y=rt,sticks=swap,l3=a,a=,up=down=left,foo") bad("hand-written format: " + f);
            const PadMap again = parse_pad_remap(f);   // round trip: same layout, same text the second time
            if (!again.same_layout(m) || again.unknown != m.unknown || format_pad_remap(again) != f) bad("round trip");
            // what the runtime is given (build_args) reads as the same layout
            if (!parse_pad_remap(pad_remap_clean(f)).same_layout(m)) bad("clean");
        }
        // two controller buttons on one game button, a game button with none: shown and kept
        {
            const PadMap m = parse_pad_remap("a=b");
            if (pad_sources(m, PadB) != std::vector<int>({ PadA, PadB }) || !pad_sources(m, PadA).empty()) bad("sources");
            if (format_pad_remap(m) != "a=b") bad("many to one format");
        }
        // assigning: B pressed for the game's A swaps A and B; again is no change; back again is the game's own
        {
            PadMap m;
            PadAssign r = pad_assign(m, PadA, PadB);
            if (!r.changed || r.took_from != PadB || r.swapped != PadA || r.left_empty || format_pad_remap(m) != "a=b,b=a") bad("swap A B");
            if (pad_assign(m, PadA, PadB).changed || format_pad_remap(m) != "a=b,b=a") bad("same again");
            r = pad_assign(m, PadA, PadA);
            if (!r.changed || r.took_from != PadB || r.swapped != PadB || format_pad_remap(m) != "") bad("swap back: " + format_pad_remap(m));
            // a chain: LT for the game's RB (RB takes LT), then RT for the game's LT (RB, which did LT, takes RT): one button each
            pad_assign(m, PadRB, PadLT);
            pad_assign(m, PadLT, PadRT);
            for (int i = 0; i < kPadInputs; ++i) if (pad_sources(m, i).size() != 1) bad(std::string("chain ") + pad_input_token(i));
            if (format_pad_remap(m) != "rb=rt,lt=rb,rt=lt") bad("chain format: " + format_pad_remap(m));
            // sticks stay as they are
            PadMap s2 = parse_pad_remap("sticks=swap");
            pad_assign(s2, PadX, PadY);
            if (!s2.sticks_swapped || format_pad_remap(s2) != "x=y,y=x,sticks=swap") bad("sticks kept");
        }
        // from a hand-written layout where the game's A has no button: taking the only button of the game's B leaves B empty
        {
            PadMap m = parse_pad_remap("a=b");                         // A and B both press B; nothing presses A
            PadAssign r = pad_assign(m, PadA, PadB);                   // B for the game's A: the game's B keeps A
            if (!r.changed || r.swapped != -1 || r.left_empty || format_pad_remap(m) != "a=b,b=a") bad("fill the empty one");
            PadMap n = parse_pad_remap("a=b,b=x");                     // nothing presses A or B... A presses B, B presses X
            r = pad_assign(n, PadY, PadA);                             // A for the game's Y: the game's B has no button, nor had Y another
            if (!r.changed || r.took_from != PadB || r.swapped != PadY || n.to[PadY] != PadB) bad("swap into a moved layout");
            PadMap e = parse_pad_remap("a=b,y=b");                     // the game's A: no button; the game's Y: no button
            r = pad_assign(e, PadA, PadX);                             // X for A: the game's X had only X, and A had none to give
            if (!r.changed || !r.left_empty || r.swapped != -1 || !pad_sources(e, PadX).empty()) bad("left empty");
        }
        if (pad_assign(id, -1, PadA).changed || pad_assign(id, PadA, kPadInputs).changed) bad("out of range");
        // build_args: Custom sends the saved text cleaned (no spaces), a preset its own text, Game's own nothing
        Environment e; e.update_ready = true;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        auto remap_arg = [&](int layout, const std::string& ini) {
            Environment env = e; env.pad_remap = ini;
            Store st; Settings s = st.view(Game::Gears2, env);
            s.set("PadLayout", layout);
            std::string out;
            for (const std::string& x : build_args(Game::Gears2, Version::TitleUpdate, s, env, nullptr))
                if (x.rfind("--pad_remap=", 0) == 0) out = x;
            return out;
        };
        if (remap_arg(4, " a = b, b = a ") != "--pad_remap=a=b,b=a" || remap_arg(1, "x=y") != "--pad_remap=sticks=swap" ||
            remap_arg(0, "x=y") != "" || remap_arg(4, "") != "" || remap_arg(3, "") != "--pad_remap=lb=lt,lt=lb,rb=rt,rt=rb") bad("build_args");
        const Option* o = find_option(Game::Gears2, "PadLayout");
        if (!o || o->scope != Scope::Shared || o->choices.size() != 5 || o->choices[4] != "Custom (choose below)") bad("option");
    }
    // 0.6.3 (reviews r49 / r50): Shared values edited on one page go to every page before a save (sync_shared). The r50 case:
    // Gears 3's page has unsaved Shared edits while Gears 2's in-game menu changes others and saves - both are kept; on the
    // same option the page being saved (Gears 2) wins. PerGame values stay on their own page.
    {
        Environment envs[kGameCount];
        for (Environment& e : envs) e.update_ready = true;
        Store st;
        Settings pages[kGameCount];
        for (int g = 0; g < kGameCount; ++g) { pages[g] = st.view(static_cast<Game>(g), envs[g]); st.commit(static_cast<Game>(g), pages[g], envs[g]); }
        const int G2 = static_cast<int>(Game::Gears2), G3 = static_cast<int>(Game::Gears3), GJ = static_cast<int>(Game::Judgment);
        const int fs3 = pages[G3].i("Fullscreen"), fs2 = pages[G2].i("Fullscreen");
        // Gears 3 (open page, unsaved): Vibration off, a graphics card, Extra dead zone 2, its own display mode
        pages[G3].set("Vibration", 0); pages[G3].t["GraphicsCard"] = "NVIDIA GeForce RTX 4090"; pages[G3].set("DeadZoneL", 2);
        pages[G3].set("Fullscreen", fs3 == 0 ? 1 : 0);
        // Gears 2 (in-game menu, then Restart to apply): mouse sensitivity and Extra dead zone 4
        pages[G2].set("Sens", 1.2); pages[G2].set("DeadZoneL", 4);
        bool dirty[kGameCount] = {}; dirty[G2] = dirty[G3] = true;
        bool changed[kGameCount] = {};
        sync_shared(st, envs, pages, dirty, G2, changed);
        auto bad = [&](const std::string& w) { std::cout << "FAIL sync_shared: " << w << "\n"; ++fails; };
        for (int g : { G2, G3, GJ }) {
            if (pages[g].i("Vibration") != 0 || pages[g].f("Sens") != 1.2 || pages[g].str("GraphicsCard") != "NVIDIA GeForce RTX 4090")
                bad("edits of both pages on game " + std::to_string(g));
            if (pages[g].i("DeadZoneL") != 4) bad("the saved page wins on game " + std::to_string(g));
        }
        if (pages[G2].i("Fullscreen") != fs2 || pages[G3].i("Fullscreen") == fs3) bad("PerGame value moved");
        if (!changed[G2] || !changed[G3] || !changed[GJ]) bad("changed flags");
        bool again[kGameCount] = {};
        sync_shared(st, envs, pages, dirty, G2, again);
        for (bool c : again) if (c) bad("second sync changed something");
        st.commit(Game::Gears2, pages[G2], envs[G2]); st.commit(Game::Gears3, pages[G3], envs[G3]);   // either order keeps it
        const Settings g1 = st.view(Game::Gears1, envs[static_cast<int>(Game::Gears1)]);
        if (g1.i("Vibration") != 0 || g1.f("Sens") != 1.2 || g1.i("DeadZoneL") != 4) bad("store after both commits");
        // nothing unsaved: nothing moves
        bool none[kGameCount] = {};
        Settings fresh[kGameCount];
        for (int g = 0; g < kGameCount; ++g) fresh[g] = st.view(static_cast<Game>(g), envs[g]);
        fresh[G3].set("Sens", 0.3);   // not dirty: not an edit to share
        sync_shared(st, envs, fresh, none, G3, changed);
        if (fresh[G2].f("Sens") != 1.2) bad("a page that isn't dirty was shared");
    }
    // review r51: the cases r50 / r51 left out - Monitor's clamp, an edit changed back, a text conflict between two unsaved
    // pages, and a save from a page with no unsaved edits
    {
        const int G2 = static_cast<int>(Game::Gears2), G3 = static_cast<int>(Game::Gears3), GJ = static_cast<int>(Game::Judgment),
                  G1 = static_cast<int>(Game::Gears1);
        auto bad = [&](const std::string& w) { std::cout << "FAIL sync_shared (r51): " << w << "\n"; ++fails; };
        struct World { Environment envs[kGameCount]; Store st; Settings pages[kGameCount]; bool dirty[kGameCount] = {};
                       bool changed[kGameCount] = {}; };
        auto world = [&](int screens, double monitor) {   // every page saved, nothing unsaved
            auto w = std::make_unique<World>();
            for (Environment& e : w->envs) { e.update_ready = true; e.screen_count = screens; }
            for (int g = 0; g < kGameCount; ++g) {
                w->pages[g] = w->st.view(static_cast<Game>(g), w->envs[g]);
                w->pages[g].set("Monitor", monitor);
                w->st.commit(static_cast<Game>(g), w->pages[g], w->envs[g]);
            }
            return w;
        };
        // 1. Monitor 3 saved on 3 screens, then a screen goes: Gears 2's unsaved page still holds 3 (view now shows 2) -
        //    not an edit, so Judgment's Monitor 1 is kept when Gears 2 saves
        {
            auto w = world(3, 3);
            for (Environment& e : w->envs) e.screen_count = 2;
            w->pages[G2].set("Sens", 0.7); w->dirty[G2] = true;   // dirty for another reason
            w->pages[GJ].set("Monitor", 1); w->dirty[GJ] = true;
            sync_shared(w->st, w->envs, w->pages, w->dirty, G2, w->changed);
            for (int g = 0; g < kGameCount; ++g)
                if (w->pages[g].i("Monitor") != 1 || w->pages[g].f("Sens") != 0.7) bad("Monitor clamp on game " + std::to_string(g));
            w->st.commit(Game::Gears2, w->pages[G2], w->envs[G2]);
            if (w->st.view(Game::Gears3, w->envs[G3]).i("Monitor") != 1) bad("Monitor clamp: store after the save");
        }
        // 1b. (r51 #3) a Monitor edit copied to a game whose screen count is lower gets that game's clamp
        {
            auto w = world(3, 0);
            w->envs[G1].screen_count = 2;
            w->pages[G3].set("Monitor", 3); w->dirty[G3] = true;
            sync_shared(w->st, w->envs, w->pages, w->dirty, G3, w->changed);
            if (w->pages[G2].i("Monitor") != 3 || w->pages[G1].i("Monitor") != 2) bad("Monitor clamp when copied");
            bool again[kGameCount] = {};
            sync_shared(w->st, w->envs, w->pages, w->dirty, G3, again);
            for (bool c : again) if (c) bad("Monitor clamp: second sync changed something");
        }
        // 2. the page being saved changes Extra dead zone and back: not an edit, so Gears 3's unsaved 2 is kept (by design)
        {
            auto w = world(1, 0);
            const int dz = w->pages[G2].i("DeadZoneL"), other = dz == 2 ? 3 : 2;
            w->pages[G2].set("DeadZoneL", dz + 1); w->pages[G2].set("DeadZoneL", dz); w->pages[G2].set("Sens", 0.7);
            w->dirty[G2] = true;
            w->pages[G3].set("DeadZoneL", other); w->dirty[G3] = true;
            sync_shared(w->st, w->envs, w->pages, w->dirty, G2, w->changed);
            for (int g = 0; g < kGameCount; ++g)
                if (w->pages[g].i("DeadZoneL") != other || w->pages[g].f("Sens") != 0.7) bad("reverted edit on game " + std::to_string(g));
        }
        // 3. two unsaved pages with different graphics cards: the page being saved wins either way round
        for (int prefer : { G3, GJ }) {
            auto w = world(1, 0);
            w->pages[G3].t["GraphicsCard"] = "Card A"; w->dirty[G3] = true;
            w->pages[GJ].t["GraphicsCard"] = "Card B"; w->dirty[GJ] = true;
            sync_shared(w->st, w->envs, w->pages, w->dirty, prefer, w->changed);
            const std::string want = prefer == G3 ? "Card A" : "Card B";
            for (int g = 0; g < kGameCount; ++g)
                if (w->pages[g].str("GraphicsCard") != want) bad("text conflict, prefer " + std::to_string(prefer) + ", game " + std::to_string(g));
        }
        // 4. saved from Gears 2, which has no unsaved edits: the other pages' edits still reach it; on a conflict the later
        //    game wins (Judgment after Gears 3)
        {
            auto w = world(1, 0);
            const int vib = w->pages[G2].i("Vibration");
            w->pages[G3].set("Vibration", vib ? 0 : 1); w->pages[G3].t["GraphicsCard"] = "Card A"; w->dirty[G3] = true;
            w->pages[GJ].t["GraphicsCard"] = "Card B"; w->dirty[GJ] = true;
            sync_shared(w->st, w->envs, w->pages, w->dirty, G2, w->changed);
            if (w->pages[G2].i("Vibration") == vib || w->pages[G2].str("GraphicsCard") != "Card B" || !w->changed[G2])
                bad("save from a page with no edits");
            if (w->pages[G3].str("GraphicsCard") != "Card B") bad("no edits on the saved page: game order");
        }
    }
    // 0.6.3 (GitHub #245): Trigger rumble (Xbox controllers) - Shared, off by default, on the Controls tab; sent only with
    // the runtime's pad_trigger_rumble (SDK patch 104) and the game's trigger pulses, not under Wine / Proton, and not while
    // Controller vibration is off; one setting for all four games.
    {
        auto bad = [&](const std::string& w) { std::cout << "FAIL TriggerRumble: " << w << "\n"; ++fails; };
        static const char* pulses[kGameCount] = { "Gears 2 trigger pulses for Xbox pads", "Gears 3 trigger pulses for Xbox pads",
                                                  "Judgment trigger pulses for Xbox pads", "Gears 1 trigger pulses for Xbox pads" };
        for (int g = 0; g < kGameCount; ++g) {
            const Game game = static_cast<Game>(g);
            const Option* o = find_option(game, "TriggerRumble");
            if (!o || o->scope != Scope::Shared || o->def != 0 || o->tab != Tab::Controls || o->kind != Kind::Bool ||
                o->label != "Trigger rumble (Xbox controllers)")
                { bad("option on game " + std::to_string(g)); continue; }
            // rt: the runtime has patch 104; exe: this game's pulses (or another game's); wine; vibration; the value
            auto sent = [&](bool rt, int exe_game, bool wine, bool vibration, bool value) {
                Environment e; e.update_ready = true; e.wine = wine;
                const std::string needle = exe_game >= 0 ? pulses[exe_game] : "";
                e.cap.exe_has = [needle](const std::string& n) { return !needle.empty() && n == needle; };
                e.cap.runtime_has = [rt](const std::string& n) { return rt && n == "Xbox impulse triggers: games that support it"; };
                e.cap.file_next_to_exe = [](const std::string&) { return false; };
                Store st; Settings s = st.view(game, e);
                s.set("Vibration", vibration ? 1 : 0);
                if (value) s.set("TriggerRumble", 1);
                int n = 0;
                for (const std::string& x : build_args(game, effective_version(game, s, e), s, e, nullptr))
                    if (x == "--pad_trigger_rumble=true") ++n;
                    else if (x.find("pad_trigger_rumble") != std::string::npos) n += 100;   // any other form: wrong
                return n;
            };
            const std::string gs = " (game " + std::to_string(g) + ")";
            if (sent(true, g, false, true, true) != 1) bad("not sent" + gs);
            if (sent(true, g, false, true, false) != 0) bad("sent while off (the default)" + gs);
            if (sent(false, g, false, true, true) != 0) bad("sent to a runtime without patch 104" + gs);
            if (sent(true, -1, false, true, true) != 0) bad("sent to an exe without trigger pulses" + gs);
            if (sent(true, (g + 1) % kGameCount, false, true, true) != 0) bad("another game's needle accepted" + gs);
            if (sent(true, g, true, true, true) != 0) bad("sent under Wine / Proton" + gs);
            if (sent(true, g, false, false, true) != 0) bad("sent with Controller vibration off" + gs);
        }
        // Shared: ticked on Gears 3's page, every game's page has it after the save
        Environment envs[kGameCount];
        for (Environment& e : envs) e.update_ready = true;
        Store st;
        Settings g3 = st.view(Game::Gears3, envs[static_cast<int>(Game::Gears3)]);
        g3.set("TriggerRumble", 1);
        st.commit(Game::Gears3, g3, envs[static_cast<int>(Game::Gears3)]);
        for (int g = 0; g < kGameCount; ++g)
            if (st.view(static_cast<Game>(g), envs[g]).i("TriggerRumble") != 1) bad("not shared to game " + std::to_string(g));
        Store st2; st2.load_ini(st.save_ini());
        if (st2.view(Game::Gears1, envs[static_cast<int>(Game::Gears1)]).i("TriggerRumble") != 1) bad("ini round trip");
    }
    std::cout << (fails ? "SELF-CHECKS FAILED\n" : "self-checks ok\n");
    return fails ? 1 : 0;
}

// First-person camera (layer3 first_person.cpp): separate from k_builtin so the cases above keep their numbers
const char* const k_builtin_first_person[] = {
    "game=1;FirstPerson=1;exe=First-person camera (experimental): the game's own camera at eye level",   // Gears 3: sent
    "game=1;FirstPerson=1",                                                                             // older exe: greyed, not sent
    "game=0;FirstPerson=1;exe=First-person camera (experimental): the game's own camera at eye level",   // Gears 2: sent
    "game=2;FirstPerson=1;exe=First-person camera (experimental): the game's own camera at eye level",   // Judgment: sent
    "game=2;FirstPerson=1",                                                                             // older Judgment exe: greyed, not sent
    "game=3;FirstPerson=1;exe=First-person camera (experimental): the game's own camera at eye level",   // Gears 1: not yet
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> cases(std::begin(k_builtin), std::end(k_builtin));
    if (kOfferFirstPerson) cases.insert(cases.end(), std::begin(k_builtin_first_person), std::end(k_builtin_first_person));
    if (argc >= 2 && std::string(argv[1]) == "--summary") return summary();
    if (argc >= 3 && std::string(argv[1]) == "--gen") {
        std::mt19937 r(argc >= 4 ? static_cast<unsigned>(std::atoi(argv[3])) : 59u);
        int n = std::atoi(argv[2]);
        for (auto& c : cases) std::cout << c << "\n";
        for (int i = 0; i < n; ++i) std::cout << random_case(r) << "\n";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--cases") {
        cases.clear();
        std::ifstream in(argv[2]);
        std::string l;
        while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); if (!l.empty()) cases.push_back(l); }
    }
    for (size_t i = 0; i < cases.size(); ++i) run_case(static_cast<int>(i + 1), cases[i]);
    return 0;
}
