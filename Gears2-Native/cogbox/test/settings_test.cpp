// CogBox - test of gears_settings: builds the command line for the cases in a cases file (or the built-in ones) and
// prints them in the same form as ps1_args.ps1 prints the PS1's, so the two can be diffed.
//   settings_test                      the built-in cases' args
//   settings_test --cases FILE         the cases in FILE (one per line: key=value;key=value...)
//   settings_test --gen N [SEED]       print the built-in cases plus N random ones (a cases file)
//   settings_test --summary            the option model (counts per game / tab) and the self-checks
// Case keys: game, upd (title update ready), hz, screens, gpus (a/b), exe / rt / files (aliases below), rtfile,
// extra (lines split by |), and any option id (the setting's value).
#include "../src/gears_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
    static const char* proc[3] = { "gow2", "gow3", "gowj" };
    if (alias == "fif") return "gow2_renderer_frames_in_flight";
    if (alias == "ups") return "gow2_upscale_mode";
    if (alias == "fg") return "gow2_frame_generation";
    if (alias == "mute") return std::string(proc[g]) + "_mute_unfocused";
    if (alias == "reflex") return "gow2_reflex";
    if (alias == "al2") return "gow2_antilag2";
    if (alias == "aspect") return "gow3_aspect";
    if (alias == "edits") return "'file|section|~=old>new'";
    if (alias == "avx") return "gn_cpu_level=x86-64-v";
    return alias;
}
std::string rt_needle(const std::string& a) { return a == "vs" ? "d3d12_present_vsync" : a == "lat" ? "d3d12_max_frame_latency" : a; }
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
    const char* roots[3] = { "C:\\P\\Gears2-Native", "C:\\P\\Gears3-Native", "C:\\P\\GearsJ-Native" };
    const std::string root = roots[0];   // the PS1's $root: saves, logs and shader caches live in Gears2-Native\out
    const char* udirs[3] = { "\\out\\userdata", "\\out\\gears3-userdata", "\\out\\judgment-userdata" };
    const char* pre[3] = { "", "gears3-", "judgment-" };
    const char* tus[3] = { "\\game\\tu6\\package", "\\game\\tu6\\package", "\\game\\tu4\\package" };
    Environment& e = c.env;
    e.game_data_root = std::string(roots[g]) + "\\game\\extracted";
    e.update_data_root = std::string(roots[g]) + tus[g];
    e.user_data_root = root + udirs[g];
    e.shader_cache_dir = g == 1 ? root + "\\out\\gears3-shader-cache" : g == 2 ? root + "\\out\\judgment-shader-cache" : "";
    e.log_dir = root + "\\out\\play-logs\\" + pre[g] + "20260101-000000";
    e.update_ready = kv.count("upd") && kv["upd"] == "1";
    e.refresh_hz = kv.count("hz") ? std::atoi(kv["hz"].c_str()) : 0;
    e.screen_count = kv.count("screens") ? std::atoi(kv["screens"].c_str()) : 1;
    if (kv.count("gpus") && !kv["gpus"].empty()) e.gpus = split(kv["gpus"], '/');
    if (kv.count("extra")) e.extra_args_lines = split(kv["extra"], '|');
    if (kv.count("exe") && !kv["exe"].empty()) for (auto& x : split(kv["exe"], ',')) c.exe.insert(exe_needle(x, g));
    if (kv.count("rt") && !kv["rt"].empty()) for (auto& x : split(kv["rt"], ',')) c.rt.insert(rt_needle(x));
    if (kv.count("files") && !kv["files"].empty()) for (auto& x : split(kv["files"], ',')) c.files.insert(file_name(x));
    bool rtfile = kv.count("rtfile") ? kv["rtfile"] == "1" : !c.rt.empty();
    e.g2_disc_ini_edits = c.exe.count("'file|section|~=old>new'") != 0;
    for (auto& p : kv) {
        if (p.first == "game" || p.first == "upd" || p.first == "hz" || p.first == "screens" || p.first == "gpus" || p.first == "extra" ||
            p.first == "exe" || p.first == "rt" || p.first == "files" || p.first == "rtfile") continue;
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
    "game=0;upd=1;Version=0;Resolution=2;AAMode=3;gpus=NVIDIA GeForce RTX 4080;files=dlss,fsr;exe=fif,ups,reflex,mute,edits;rt=vs,lat;SceneMode=2;Fps=3;Vsync=1;hz=144;LowLag=1;Mute=1;SkipLogos=1;Trilinear=1",
    "game=0;Resolution=4;AAMode=3;gpus=AMD Radeon RX 7900 XTX;exe=fif,al2;LowLag=1;Fps=2;Fullscreen=1;Exclusive=1;Monitor=2;screens=2;HDR=1;PaperWhite=250;Peak=1400;Mnk=1;Sens=1.3",
    "game=0;Start=5;Fov=115;MotionBlur=0;DoF=0;LensFlares=0;Shake=2;DeadZone=2;Vibration=0;WindowSize=3;Players=1;FpsMode=1;Pool=0;Aniso=0;extra=--gow2_frame_times={LOG}\\ft.csv|# comment| --foo=1 |",
    "game=0;upd=1;Version=1;LowLag=0;exe=fif,fg;rt=vs,lat;Fps=1;Vsync=1;hz=60;FrameGen=1;files=fgdll;FpsMode=3",
    "game=0;AAMode=2;Fps=2;Vsync=1;hz=0;rt=vs;Pool=3;DrawDist=2;MipFade=1;Ultrawide=1;gpus=Microsoft Basic Display Adapter",
    "game=1",
    "game=1;upd=1;Version=0;Cheats=1;Mutators=1;Resolution=4;exe=fif;Fps=1",
    "game=1;upd=1;Version=1;Cheats=1;Mutators=1;Resolution=4;exe=fif,aspect,mute;Mute=1;Fps=1;Vsync=1;rt=vs;hz=0",
    "game=1;Fps=1;Vsync=1;rt=vs;hz=144;Ultrawide=1;Resolution=4;FpsMode=3;Aniso=1;Trilinear=0;MipFade=0;Shake=1;DrawDist=1;SkipLogos=1",
    "game=1;Fps=2;AAMode=4;files=xess;exe=ups,fif;SceneMode=1;Resolution=2;Players=3;LowLag=0;rt=lat",
    "game=1;Fps=3;Vsync=1;rt=vs;hz=240;FrameGen=1;exe=fg;files=fgdll;Start=31;Fov=80",
    "game=2",
    "game=2;upd=1;Start=3;LevelSelect=1;SkipLogos=1;Pool=1;FpsMode=3;Fps=3;Ultrawide=1",
    "game=2;Pool=3;Fps=1;Vsync=1;rt=vs;hz=165;FrameGen=1;exe=fg,fif,mute;files=fgdll;Mute=1;LowLag=1;gpus=Intel(R) UHD Graphics/NVIDIA GeForce GTX 1080",
    "game=2;AAMode=3;gpus=NVIDIA GeForce GTX 1080;files=fsr;Fps=2;Shake=2;Fov=125;DoF=0;Cheats=1;Mutators=1;Start=18",
    "game=2;Resolution=4;Fullscreen=0;WindowSize=5;Exclusive=1;Mnk=1;Sens=0.1;DeadZone=5;Vibration=0;Monitor=1;screens=1;HDR=1",
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
    // All games use the startup barrier, and old builds never receive an unsupported startup flag.
    for (int g = 0; g < 3; ++g) {
        Environment e;
        e.cap.exe_has = [](const std::string&) { return true; };
        e.cap.runtime_has = e.cap.exe_has; e.cap.file_next_to_exe = e.cap.exe_has;
        Store store;
        Settings settings = store.view(static_cast<Game>(g), e);
        settings.set("ShaderPrebuild", 1);
        auto args = build_args(static_cast<Game>(g), Version::Disc, settings, e);
        auto has = [&](const std::string& value) { return std::find(args.begin(), args.end(), value) != args.end(); };
        if (!has("--ue3_shader_prebuild_startup=true") || has("--gow2_renderer_shader_prebuild_hold=true")) {
            std::cout << "FAIL startup shader flags for game " << g << "\n"; ++fails;
        }
        settings.set("ShaderPrebuild", 0);
        args = build_args(static_cast<Game>(g), Version::Disc, settings, e);
        if (has("--ue3_shader_prebuild_startup=true") || !has("--ue3_shader_prebuild=false")) {
            std::cout << "FAIL shader toggle off for game " << g << "\n"; ++fails;
        }
    }
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
        s.set("Sens", 1.7); s.set("Start", 4);
        st.commit(Game::Gears3, s, e);
        std::string ini = st.save_ini();
        Store st2; st2.load_ini(ini);
        if (st2.save_ini() != ini) { std::cout << "FAIL ini round trip\n"; ++fails; }
        Settings s2 = st2.view(Game::Gears3, e);
        if (s2.f("Sens") != 1.7 || s2.i("Start") != 4 || s2.i("Fov") != 110 || st2.last_game != 1) { std::cout << "FAIL ini values\n"; ++fails; }
        // Gears 2 has no set of its own yet: it starts from the last game's (PS1 429)
        Settings g2 = st2.view(Game::Gears2, e);
        if (g2.i("Fov") != 110 || g2.i("Start") != 0 || !g2.b("Aniso")) { std::cout << "FAIL per-game fallback\n"; ++fails; }
        g2.set("Fov", 90); st2.remember(Game::Gears2, g2);
        if (st2.view(Game::Gears3, e).i("Fov") != 110 || st2.view(Game::Gears2, e).i("Fov") != 90) { std::cout << "FAIL per-game memory\n"; ++fails; }
        std::cout << "--- ini after a Gears 3 save ---\n" << ini;
    }
    std::cout << (fails ? "SELF-CHECKS FAILED\n" : "self-checks ok\n");
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> cases(std::begin(k_builtin), std::end(k_builtin));
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
