// COGBox - the library's music (Shagg 1 Nov, the Athelas design: Athelas launcher/src/music.cpp): the chosen game's own
// menu music, read from the player's copy when its box comes to rest in front of the carousel (and on into that game's
// settings page), decoded on one reader thread, looped, crossfaded when the box changes, silent while a game runs.
// Nothing is copied or saved; COGBox ships no game audio.
//
// Where each game's track is (gamesound.h has the formats): a SoundNodeWave in a UE3 package of the game's front end,
// see kTracks. Each copy the player added is tried in turn - the title update's folder, the game folder, the disc image
// or Games on Demand copy it was added from (read in place) - until one decodes.
// CogBox.ini [Launcher]: Music (1 / 0, the MUSIC button or M / R3), MusicVolume (0-100, default 42).
//   [<game key>] MusicPackage / MusicWave: another package (path in the game, '/') / part of the wave's export name,
//   for trying other tracks without a new build.
// Test: COGBOX_MUSIC_DUMP=<folder> writes each decoded track there as <key>.wav (44.1 kHz stereo 16-bit), and
// CogBox.exe --musictest <key> <copy> <report.txt> decodes one game's track from one copy and reports what it found.
#include "app.h"
#include "gamesound.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

// The front end's packages, in the order tried; 'waves': parts of the export name wanted (none known: the export names
// can't be read from these games' summaries yet); 'pin': the track itself - channels, rate and length as its format
// says (job 991, Shagg's copies, 1 Nov 2026), within half a second. When no wave matches (another release), the longest
// stereo wave of 30 s or more is tried.
struct Pin { int channels, rate; float seconds; };
struct TrackDef { const char *packages[3]; const char *waves[3]; Pin pin; };
const TrackDef kTracks[G_COUNT] = {
    // GearStart.xxx: 33 waves, one long: 109.3 s (decoded: RMS 3011, peak 19346)
    /* G_GEARS2   */ { { "GearGame/CookedXenon/GearStart.xxx", "GearGame/CookedXenon/GearStartTransition.xxx", nullptr }, { nullptr }, { 2, 32000, 109.3f } },
    // GearStart.xxx: 26 waves, five long (125.7, 128.8, 143.8, 177.8 s at 32 kHz, 242.7 s at 44.1 kHz); the 125.7 s one
    // (Shagg 6 Nov picked it by ear; the 242.7 s one was 'terrible')
    /* G_GEARS3   */ { { "GearGame/CookedXbox360/GearStart.xxx", "GearGame/CookedXbox360/GearStartTransition.xxx", nullptr }, { nullptr }, { 2, 32000, 125.7f } },
    // GearStart.xxx: 92 waves, two long (138.3 s, 128.1 s); the 138.3 s one (decoded: RMS 2692, peak 23920)
    /* G_JUDGMENT */ { { "GearGame/CookedXbox360/GearStart.xxx", "GearGame/CookedXbox360/GearStartTransition.xxx", nullptr }, { nullptr }, { 2, 32000, 138.3f } },
    // WarStart.xxx (the front end; the same track is in WarIntermission.xxx): little-endian XMA1, two long waves, 293.7 s
    // and 141.8 s; the 293.7 s one is the menu theme (job 997, Shagg listened; decoded: RMS 2437, peak 22142)
    /* G_GEARS1   */ { { "WarGame/CookedXenon/WarStart.xxx", "WarGame/CookedXenon/WarIntermission.xxx", nullptr }, { nullptr }, { 2, 44100, 293.7f } },
};

const int kRate = 44100;
const size_t kMaxPackageFile = size_t(96) << 20;

std::string lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

// ---- one game's track, from its copies ---------------------------------------------------------------------------

struct Request {
    int game = -1;
    std::vector<std::string> copies;          // folders and images, tried in order
    std::vector<std::string> packages, waves;  // from kTracks or CogBox.ini
    Pin pin{ 0, 0, 0 };                        // from kTracks (not with CogBox.ini's MusicPackage / MusicWave)
    int gen = 0;
};

std::atomic<int> g_gen{0};                    // bumped when the wanted track changes: a stale read is dropped
bool stale(void *gen) { return *(int *)gen != g_gen.load(); }

// Reads and decodes the track; 'log' (optional) gets a line per step. The newest request only: returns false as soon as
// another one is made (gen changed).
bool load_track(const Request &r, std::vector<int16_t> &pcm, std::string *log, bool checkGen)
{
    auto say = [&](const std::string &s) { if (log) *log += s + "\r\n"; };
    int gen = r.gen;
    void *ctx = &gen;
    std::string why;
    if (!gamesound::xma_available(&why)) { say("no XMA decoder: " + why); return false; }
    for (const std::string &copy : r.copies) {
        for (const std::string &rel : r.packages) {
            if (checkGen && stale(ctx)) return false;
            std::vector<uint8_t> file, pkg;
            // a title update folder keeps its packages at its top (Gears 1) or in the game's own folders
            if (!gamesound::read_copy_file(copy, rel, file, kMaxPackageFile) &&
                !gamesound::read_copy_file(copy, rel.substr(rel.find_last_of('/') + 1), file, kMaxPackageFile)) {
                say(copy + " | " + rel + ": not there");
                continue;
            }
            if (!gamesound::ue3_unpack(file, pkg, why)) { say(copy + " | " + rel + ": " + why); continue; }
            std::vector<uint8_t>().swap(file);
            std::vector<gamesound::Wave> ws = gamesound::ue3_find_waves(pkg);
            char b[256];
            snprintf(b, sizeof b, "%s | %s: %zu bytes unpacked, %zu XMA waves", copy.c_str(), rel.c_str(), pkg.size(), ws.size());
            say(b);
            for (const auto &w : ws) {
                snprintf(b, sizeof b, "  wave '%s' XMA%d %d ch %d Hz %.1f s at 0x%zX", w.name.c_str(), w.xma, w.channels, w.rate, w.seconds(), w.at);
                say(b);
            }
            // the wanted ones first: by name, else the longest stereo waves of 30 s or more
            std::vector<const gamesound::Wave *> order;
            for (const std::string &want : r.waves)
                for (const auto &w : ws)
                    if (!want.empty() && lower(w.name).find(lower(want)) != std::string::npos &&
                        std::find(order.begin(), order.end(), &w) == order.end()) order.push_back(&w);
            if (r.waves.empty()) {
                for (const auto &w : ws)
                    if (r.pin.channels && w.channels == r.pin.channels && w.rate == r.pin.rate && std::fabs(w.seconds() - r.pin.seconds) <= 0.5)
                        order.push_back(&w);
                const size_t pinned = order.size();
                for (const auto &w : ws)
                    if (w.seconds() >= 30 && w.channels >= 2 && std::find(order.begin(), order.end(), &w) == order.end()) order.push_back(&w);
                std::stable_sort(order.begin() + pinned, order.end(), [](const gamesound::Wave *x, const gamesound::Wave *y) { return x->seconds() > y->seconds(); });
            }
            // only the candidates' records are kept (each its own copy, from offset 0): the unpacked package (up to
            // ~100 MB) is freed before decoding, so it never sits beside the decoded track (review of 0004)
            std::vector<std::vector<uint8_t>> recs;
            std::vector<gamesound::Wave> cand;
            for (const gamesound::Wave *w : order) {
                if (cand.size() >= 6) break;
                recs.emplace_back(pkg.begin() + w->at, pkg.begin() + w->at + w->size);
                cand.push_back(*w);
                cand.back().at = 0;
            }
            std::vector<gamesound::Wave>().swap(ws);
            std::vector<uint8_t>().swap(pkg);
            for (size_t k = 0; k < cand.size(); k++) {
                const gamesound::Wave *w = &cand[k];
                if (gamesound::xma_decode(recs[k], *w, pcm, why, checkGen ? stale : nullptr, ctx)) {
                    gamesound::trim_leading_silence(pcm);
                    snprintf(b, sizeof b, "  playing '%s': %.1f s", w->name.c_str(), pcm.size() / 2.0 / kRate);
                    say(b);
                    return true;
                }
                say("  '" + w->name + "' refused: " + why);
                if (checkGen && stale(ctx)) return false;
            }
        }
    }
    return false;
}

void dump(int game, const std::vector<int16_t> &pcm)
{
    const char *dir = getenv("COGBOX_MUSIC_DUMP");
    if (!dir || !*dir || game < 0) return;
    std::string fn = join(dir, std::string(game_def(game).key) + ".wav");
    FILE *f = _wfopen(widen(fn).c_str(), L"wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

// ---- playing: one SDL stream, 44.1 kHz stereo s16, its callback mixing the current track and the one fading out ----

struct Track { int game = -1; std::vector<int16_t> pcm; size_t pos = 0; };
SDL_AudioStream *g_stream = nullptr;
SDL_Mutex *g_lock = nullptr;
SDL_Condition *g_wake = nullptr;
SDL_Thread *g_reader = nullptr;
bool g_initTried = false, g_quit = false;
Track *g_cur = nullptr, *g_old = nullptr;
float g_gainCur = 0, g_gainOld = 0, g_volume = 0.42f;
bool g_enabled = true, g_paused = false;
Request g_job;                 // the latest request (under g_lock); gen 0: none
int g_want = -1;               // the game asked for last (main thread)
std::string g_wantKey;         // ... and its copies, to notice a change

void SDLCALL feed(void *, SDL_AudioStream *s, int need, int)
{
    static int16_t out[2048 * 2];   // no allocation on the audio thread (review of 0004): filled 2048 frames at a time
    for (int left = need / 4; left > 0;) {
    const int frames = std::min(left, 2048);
    left -= frames;
    SDL_LockMutex(g_lock);
    const float target = g_enabled && !g_paused ? 1.0f : 0.0f;
    const float in = 1.0f / (kRate * 1.2f), outStep = 1.0f / (kRate * 0.6f);   // fade in 1.2 s, out 0.6 s
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (g_cur && !g_cur->pcm.empty()) {
            g_gainCur = g_gainCur < target ? std::min(target, g_gainCur + in) : std::max(target, g_gainCur - outStep);
            const int16_t *f = &g_cur->pcm[g_cur->pos * 2];
            l += f[0] * g_gainCur; r += f[1] * g_gainCur;
            if (++g_cur->pos * 2 >= g_cur->pcm.size()) g_cur->pos = 0;   // loops
        }
        if (g_old) {
            g_gainOld = std::max(0.0f, g_gainOld - outStep);
            const int16_t *f = &g_old->pcm[g_old->pos * 2];
            l += f[0] * g_gainOld; r += f[1] * g_gainOld;
            if (++g_old->pos * 2 >= g_old->pcm.size()) g_old->pos = 0;
            if (g_gainOld <= 0) { delete g_old; g_old = nullptr; }
        }
        out[i * 2] = (int16_t)std::max(-32768.0f, std::min(32767.0f, l * g_volume));
        out[i * 2 + 1] = (int16_t)std::max(-32768.0f, std::min(32767.0f, r * g_volume));
    }
    SDL_UnlockMutex(g_lock);
    SDL_PutAudioStreamData(s, out, frames * 4);
    }
}

// one reader, for the latest request only: turning the carousel quickly asks for many, and each read unpacks a package
// of tens of megabytes; it waits until the choice has stood 300 ms, and a result that is stale by then is dropped
int SDLCALL reader(void *)
{
    SDL_LockMutex(g_lock);
    for (;;) {
        while (!g_quit && !g_job.gen) SDL_WaitCondition(g_wake, g_lock);
        if (g_quit) break;
        Request j = g_job;
        if (SDL_WaitConditionTimeout(g_wake, g_lock, 300) || g_quit || g_job.gen != j.gen) continue;   // settle
        g_job.gen = 0;
        SDL_UnlockMutex(g_lock);
        Track *t = new Track();
        t->game = j.game;
        if (!load_track(j, t->pcm, nullptr, true) || j.gen != g_gen.load()) { delete t; t = nullptr; }
        else dump(j.game, t->pcm);
        SDL_LockMutex(g_lock);
        if (t && !g_quit && j.gen == g_gen.load()) {
            delete g_old;
            g_old = g_cur; g_gainOld = g_gainCur;
            g_cur = t; g_gainCur = 0;
            t = nullptr;
        }
        delete t;
    }
    SDL_UnlockMutex(g_lock);
    return 0;
}

bool music_init()
{
    if (g_initTried) return g_stream != nullptr;
    g_initTried = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    g_lock = SDL_CreateMutex();
    g_wake = SDL_CreateCondition();
    SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, kRate };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
    if (!g_stream) return false;
    g_reader = SDL_CreateThread(reader, "music", nullptr);
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

// what to play now (game -1: nothing); the same again does nothing
void music_play(const Request &r)
{
    std::string key;
    for (const auto &c : r.copies) key += c + "\n";
    for (const auto &p : r.packages) key += p + "\n";
    for (const auto &w : r.waves) key += w + "\n";
    if (r.game == g_want && key == g_wantKey) return;
    if (r.game >= 0 && !music_init()) return;
    g_want = r.game; g_wantKey = key;
    if (!g_stream) return;
    const int gen = ++g_gen;
    SDL_LockMutex(g_lock);
    if (g_cur) {                                   // what plays fades out now; the new one fades in when it is read
        delete g_old;
        g_old = g_cur; g_gainOld = g_gainCur;
        g_cur = nullptr; g_gainCur = 0;
    }
    if (r.game >= 0 && !r.copies.empty()) { g_job = r; g_job.gen = gen; }
    else g_job.gen = 0;
    SDL_SignalCondition(g_wake);
    SDL_UnlockMutex(g_lock);
}

Request request_for(const App &a, int g)
{
    Request r;
    r.game = g;
    if (g < 0 || g >= G_COUNT) { r.game = -1; return r; }
    const GameState &st = a.st[g];
    const char *key = game_def(g).key;
    if (!st.updateDir.empty()) r.copies.push_back(st.updateDir);
    if (!st.dataDir.empty()) r.copies.push_back(st.dataDir);
    const std::string img = a.ini.get(key, "Image");   // (a missing one just fails to read, on the reader thread)
    if (!img.empty()) r.copies.push_back(img);
    const std::string pk = a.ini.get(key, "MusicPackage"), wv = a.ini.get(key, "MusicWave");
    if (!pk.empty()) r.packages.push_back(pk);
    else for (const char *p : kTracks[g].packages) if (p) r.packages.push_back(p);
    if (!wv.empty()) r.waves.push_back(wv);
    else for (const char *w : kTracks[g].waves) if (w) r.waves.push_back(w);
    if (pk.empty() && wv.empty()) r.pin = kTracks[g].pin;
    if (r.copies.empty()) r.game = -1;
    return r;
}
}  // namespace

bool music_has_source(const App &a, int g) { return g >= 0 && g < G_COUNT && request_for(a, g).game >= 0; }

bool music_any_source(const App &a)
{
    for (int g = 0; g < G_COUNT; g++) if (game_in_library(a.st[g]) && music_has_source(a, g)) return true;
    return false;
}

bool music_on(const App &a) { return a.ini.geti("Launcher", "Music", 1) != 0; }

void music_set_on(App &a, bool on)
{
    a.ini.seti("Launcher", "Music", on ? 1 : 0);
    a.ini.save();
}

void music_update(App &a)
{
    int want = -1;
    const int slot = library_slot(a, a.front);
    if (a.screen == SCR_PAGE || a.zoomTo == SCR_PAGE) want = a.page;   // the game's own settings page: its music goes on
    else if (slot >= 0 && a.carPos == (float)slot && ImGui::GetTime() - a.restSince > 0.35) want = a.front;
    else if (slot >= 0) want = g_want;   // turning (or just stopped): what plays keeps playing for now
    // music off, or a game running: nothing is read or decoded (what plays fades out; the track is read again after)
    if (!music_on(a) || a.run.proc) want = -1;
    music_play(request_for(a, want));
    if (g_lock) {
        SDL_LockMutex(g_lock);
        g_enabled = music_on(a);
        g_paused = a.run.proc != nullptr;   // a game is running: silent until it closes, then it fades back in
        g_volume = std::max(0, std::min(100, a.ini.geti("Launcher", "MusicVolume", 42))) / 100.0f;
        SDL_UnlockMutex(g_lock);
    }
}

void music_stop()
{
    if (g_lock) { SDL_LockMutex(g_lock); g_quit = true; SDL_SignalCondition(g_wake); SDL_UnlockMutex(g_lock); }
    ++g_gen;                                          // a read in progress stops at its next step
    if (g_reader) SDL_WaitThread(g_reader, nullptr);
    g_reader = nullptr;
    if (g_stream) SDL_DestroyAudioStream(g_stream);   // the callback has stopped after this
    g_stream = nullptr;
    delete g_cur; delete g_old;
    g_cur = g_old = nullptr;
    if (g_wake) SDL_DestroyCondition(g_wake);
    if (g_lock) SDL_DestroyMutex(g_lock);
    g_wake = nullptr;
    g_lock = nullptr;
}

// CogBox.exe --musictest <key> <copy> <report.txt>: one game's track from one copy (folder, image or GoD header), with
// what was found on the way; COGBOX_MUSIC_DUMP=<folder> also writes the decoded track. Players never see it.
int music_test(const std::string &key, const std::string &copy, const std::string &report)
{
    int g = -1;
    for (int i = 0; i < G_COUNT; i++) if (key == game_def(i).key) g = i;
    std::string log;
    if (g < 0) { write_file(report, "unknown game key\r\nRESULT: FAILED\r\n"); return 2; }
    Request r;
    r.game = g;
    r.copies.push_back(copy);
    if (const char *p = getenv("COGBOX_MUSIC_PACKAGES")) {   // ';'-separated package paths to try instead
        std::string s = p, cur;
        for (char c : s + ";") { if (c == ';') { if (!cur.empty()) r.packages.push_back(cur); cur.clear(); } else cur += c; }
    }
    if (r.packages.empty()) for (const char *p : kTracks[g].packages) if (p) r.packages.push_back(p);
    for (const char *w : kTracks[g].waves) if (w) r.waves.push_back(w);
    r.pin = kTracks[g].pin;
    if (const char *w = getenv("COGBOX_MUSIC_WAVE")) { r.waves.clear(); r.waves.push_back(w); }
    if (const char *pn = getenv("COGBOX_MUSIC_PIN")) {   // test: "<channels>,<rate>,<seconds>" picks another track
        int ch = 0, rt = 0; float sec = 0;
        if (sscanf(pn, "%d,%d,%f", &ch, &rt, &sec) == 3) r.pin = { ch, rt, sec };
    }
    std::vector<int16_t> pcm;
    const bool ok = load_track(r, pcm, &log, false);
    if (ok) dump(g, pcm);
    log += ok ? "RESULT: ok\r\n" : "RESULT: FAILED\r\n";
    write_file(report, log);
    return ok ? 0 : 1;
}

// CogBox.exe --musicscan <game folder> <report.txt>: every package under the folder (not the _LOC_ language copies),
// unpacked in memory, and each XMA wave of 20 s or more in it - to find a game's menu music. Players never see it.
int music_scan(const std::string &folder, const std::string &report)
{
    namespace fs = std::filesystem;
    std::string log;
    std::error_code ec;
    size_t files = 0, unreadable = 0;
    for (fs::recursive_directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = narrow(it->path().filename().wstring()), low = lower(name);
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".xxx") || low.find("_loc_") != std::string::npos) continue;
        const uint64_t sz = it->file_size(ec);
        if (ec || sz > kMaxPackageFile) continue;
        files++;
        std::vector<uint8_t> file, pkg;
        std::string why;
        const std::string path = narrow(it->path().wstring());
        if (!gamesound::read_copy_file(path.substr(0, path.size() - name.size() - 1), name, file, kMaxPackageFile) ||
            !gamesound::ue3_unpack(file, pkg, why)) { unreadable++; log += "unreadable " + path + " " + why + "\r\n"; continue; }
        for (const auto &w : gamesound::ue3_find_waves(pkg)) {
            if (w.seconds() < 20) continue;
            char b[512];
            snprintf(b, sizeof b, "%s | '%s' XMA%d %d ch %d Hz %.1f s\r\n", path.c_str(), w.name.c_str(), w.xma, w.channels, w.rate, w.seconds());
            log += b;
        }
    }
    char b[128];
    snprintf(b, sizeof b, "packages: %zu, unreadable: %zu\r\nRESULT: %s\r\n", files, unreadable, files ? "ok" : "FAILED");
    log += b;
    write_file(report, log);
    return files ? 0 : 1;
}

// CogBox.exe --musicprobe <package file> <report.txt>: for a package whose sound isn't found (Gears 1): its first bytes,
// every bulk-data-like record of 64 KB or more (flags, element count == size on disk, offset) with its first 64 bytes,
// and where "RIFF" / "XMA" / the XMA format tags appear. Hex only, no sound written. Players never see it.
int music_probe(const std::string &file, const std::string &report)
{
    std::string log, s;
    char b[256];
    auto hex = [&](const std::vector<uint8_t> &d, size_t at, size_t n) {
        std::string h;
        for (size_t k = at; k < std::min(d.size(), at + n); k++) { snprintf(b, sizeof b, "%02X", d[k]); h += b; if ((k - at) % 4 == 3) h += ' '; }
        return h;
    };
    std::vector<uint8_t> raw, pkg;
    if (!read_file(file, s, kMaxPackageFile)) { write_file(report, "can't read\r\nRESULT: FAILED\r\n"); return 1; }
    raw.assign(s.begin(), s.end());
    log += "file " + std::to_string(raw.size()) + " bytes, starts " + hex(raw, 0, 48) + "\r\n";
    std::string why;
    if (!gamesound::ue3_unpack(raw, pkg, why)) { log += "unpack: " + why + " (probing the file as it is)\r\n"; pkg = raw; }
    else log += "unpacked " + std::to_string(pkg.size()) + " bytes, starts " + hex(pkg, 0, 48) + "\r\n";
    auto be = [&](size_t i) { return uint32_t(pkg[i]) << 24 | uint32_t(pkg[i + 1]) << 16 | uint32_t(pkg[i + 2]) << 8 | pkg[i + 3]; };
    size_t records = 0, hits = 0;
    for (size_t i = 0; i + 80 <= pkg.size(); i++) {
        if (records < 60 && be(i + 4) == be(i + 8) && be(i + 4) >= 65536 && be(i + 4) <= pkg.size() - i - 16 && be(i) <= 0xFFFF) {
            snprintf(b, sizeof b, "record at 0x%zX: flags %08X count %u offset %08X | ", i, be(i), be(i + 4), be(i + 12));
            log += b + hex(pkg, i + 16, 64) + "\r\n";
            records++;
        }
        if (hits < 60 && (!memcmp(&pkg[i], "RIFF", 4) || !memcmp(&pkg[i], "XMA", 3) || !memcmp(&pkg[i], "\x01\x65\x00", 3) ||
                          !memcmp(&pkg[i], "\x65\x01\x10\x00", 4) || !memcmp(&pkg[i], "\x01\x66\x00", 3))) {
            snprintf(b, sizeof b, "bytes at 0x%zX: ", i);
            log += b + hex(pkg, i > 16 ? i - 16 : 0, 80) + "\r\n";
            hits++;
        }
    }
    snprintf(b, sizeof b, "records: %zu, tag hits: %zu\r\nRESULT: ok\r\n", records, hits);
    log += b;
    write_file(report, log);
    return 0;
}
