// COGBox - mod conflicts: the screens' side (see mod_conflicts.h). The mods' files are listed on a thread of its own, not
// fabricator_state.cpp's worker: that one runs installs and packs that can take minutes, and the conflicts would wait
// behind them (and a 50 000-file texture pack's listing would hold an install back). One request per game waits at a
// time - a newer one replaces it - so a burst of clicks on Up / Down is one listing. The thread touches only the mods'
// folders (read) and its own cache; the UI thread only posts requests and takes the finished result (a shared_ptr swap).
#include "app.h"
#include "mod_conflicts.h"
#include "i18n.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace {

std::string lower_k(std::string s) { for (char &c : s) c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; return s; }

struct Want {   // one game's request
    int game = -1;
    std::string key;
    struct One { std::string folder, name, path; bool overlay; };
    std::vector<One> mods;   // the enabled ones, in load order
    std::string dataDir;
    bool force = false;      // every folder listed again, whatever its signature (a screen opened, a game closed)
};

// a mod folder's listing, and the signature it was made at
struct Listing { std::string sig; std::vector<std::string> files; };

// What the two threads share: on the heap and never freed (review: as fabricator_state's worker), so the thread can still
// be running at exit without touching a destroyed mutex; quit stops it between two mods.
struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool started = false, quit = false;
    std::optional<Want> next[G_COUNT];                     // (m) the request waiting, per game
    std::shared_ptr<const ModConflictSet> done[G_COUNT];   // (m) the latest result per game
};
Shared &sh() { static Shared *s = new Shared; return *s; }
// (the UI thread's)
std::string g_asked[G_COUNT];          // the key last asked for
Uint64 g_askedAt[G_COUNT] = {};
std::optional<Want> g_last[G_COUNT];   // the request last posted (mod_conflicts_refresh posts it again, forced)
bool g_force[G_COUNT] = {};            // a refresh asked for before any request: the next one is forced

// the signature: the folder's own write time and its subfolders' (two levels down) - a file added, removed or renamed
// in those changes one; a pack's 30 000 textures in <TITLEID>\replace are not listed for it
void sign_dir(const std::wstring &dir, int depth, std::string &sig)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        sig += narrow(n) + ":" + std::to_string(fd.ftLastWriteTime.dwLowDateTime) + "." + std::to_string(fd.ftLastWriteTime.dwHighDateTime) + ";";
        if (depth < 1) sign_dir(dir + L"\\" + n, depth + 1, sig);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
std::string signature(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return "-";
    std::string sig = std::to_string(d.ftLastWriteTime.dwLowDateTime) + "." + std::to_string(d.ftLastWriteTime.dwHighDateTime) + "|";
    sign_dir(widen(path), 0, sig);
    return sig;
}

// every file under dir (rel paths, '\'), never through a link (the game doesn't follow one either); at most 400 000
void list_dir(const std::wstring &dir, const std::string &rel, std::vector<std::string> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::string r = rel.empty() ? narrow(n) : rel + "\\" + narrow(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_dir(dir + L"\\" + n, r, out);
        else out.push_back(r);
    } while (out.size() < 400000 && FindNextFileW(f, &fd));
    FindClose(f);
}

void run()
{
    Shared &s = sh();
    std::map<std::string, Listing> caches[G_COUNT];   // (this thread) per game, by lower-case folder path
    std::string lastTops[G_COUNT];                    // ... and the game's top folders its last result had
    for (;;) {
        Want w;
        {
            std::unique_lock<std::mutex> l(s.m);
            s.cv.wait(l, [&] {
                if (s.quit) return true;
                for (const auto &n : s.next) if (n) return true;
                return false;
            });
            if (s.quit) return;
            for (auto &n : s.next)
                if (n) { w = std::move(*n); n.reset(); break; }
        }
        std::map<std::string, Listing> &cache = caches[w.game];
        ModConflictRules rules;
        rules.cooked = w.game == G_GEARS2 ? "cookedxenon" : w.game == G_GEARS3 ? "cookedxbox360" : "";   // (ModGameKey's bundle folder)
        rules.fragments = w.game == G_GEARS2 || w.game == G_GEARS3;   // (Gears 1 / Judgment: config\ is a folder like any)
        if (!w.dataDir.empty()) {   // the game's top folders: a mod's file in a folder the game hasn't is never used
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((widen(w.dataDir) + L"\\*").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring n = fd.cFileName;
                    if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) rules.game_tops.insert(lower_k(narrow(n)));
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
        }
        std::string tops;
        for (const std::string &t : rules.game_tops) tops += t + "|";
        // each folder: its listing kept while its signature is the same (a forced request lists every one again)
        bool same = !w.force && tops == lastTops[w.game];
        std::map<std::string, Listing> keep;
        for (const Want::One &m : w.mods) {
            {
                std::lock_guard<std::mutex> l(s.m);
                if (s.quit) return;
            }
            const std::string k = lower_k(m.path);
            const std::string sig = signature(m.path);
            auto it = cache.find(k);
            Listing &li = keep[k];
            if (!w.force && it != cache.end() && it->second.sig == sig) li = std::move(it->second);
            else { li.sig = sig; list_dir(widen(m.path), "", li.files); same = false; }
        }
        if (keep.size() != cache.size()) same = false;   // (a folder no longer enabled)
        cache = std::move(keep);
        lastTops[w.game] = tops;
        std::shared_ptr<const ModConflictSet> prev;
        {
            std::lock_guard<std::mutex> l(s.m);
            prev = s.done[w.game];
        }
        if (same && prev && prev->key == w.key) continue;   // nothing changed: the result stands
        std::vector<ModFileList> in;   // (pointing at the cached listings, not copies)
        for (const Want::One &m : w.mods) {
            ModFileList f;
            f.name = m.name;
            f.ref = &cache[lower_k(m.path)].files;
            f.overlay = m.overlay;
            in.push_back(std::move(f));
        }
        auto set = std::make_shared<ModConflictSet>();
        set->key = w.key;
        for (const Want::One &m : w.mods) set->folders.push_back(lower_k(m.folder));
        set->of = mod_conflicts(in, rules);
        std::lock_guard<std::mutex> l(s.m);
        s.done[w.game] = set;
    }
}

void post(Want w)   // (UI thread)
{
    Shared &s = sh();
    g_last[w.game] = w;
    std::lock_guard<std::mutex> l(s.m);
    if (s.quit) return;
    s.next[w.game] = std::move(w);
    if (!s.started) {
        s.started = true;
        std::thread(run).detach();   // (ends at mod_conflicts_shutdown, or with the process)
    }
    s.cv.notify_one();
}

}  // namespace

const ModConflicts *ModConflictSet::find(const std::string &folder) const
{
    const std::string k = lower_k(folder);
    for (size_t i = 0; i < folders.size() && i < of.size(); i++) if (folders[i] == k) return &of[i];
    return nullptr;
}

void mod_conflicts_request(int g, const std::vector<ModItem> &mods, const std::string &dataDir)
{
    if (g < 0 || g >= G_COUNT) return;
    Want w;
    w.game = g;
    w.dataDir = dataDir;
    for (const ModItem &m : mods) {
        if (!m.enabled) continue;
        const bool overlay = m.problem.empty();   // a mod the game leaves out brings no files (its textures still)
        w.key += lower_k(m.folder) + (overlay ? "+" : "-") + "|";
        w.mods.push_back({ m.folder, m.name, m.path, overlay });
    }
    // a new set or order: now; the same: the folders checked again every 5 s (the screens read mods_list every 2 s)
    const Uint64 now = SDL_GetTicks();
    if (!g_force[g] && w.key == g_asked[g] && now - g_askedAt[g] < 5000) return;
    w.force = g_force[g];
    g_force[g] = false;
    g_asked[g] = w.key;
    g_askedAt[g] = now;
    post(std::move(w));
}

void mod_conflicts_refresh(int g)
{
    if (g < 0 || g >= G_COUNT) return;
    if (!g_last[g]) { g_force[g] = true; return; }   // (the first request lists every folder anyway)
    Want w = *g_last[g];
    w.force = true;
    g_askedAt[g] = SDL_GetTicks();
    post(std::move(w));   // (the result shown stays until the new one is in: its key is the same)
}

void mod_conflicts_shutdown()
{
    Shared &s = sh();
    std::lock_guard<std::mutex> l(s.m);
    s.quit = true;
    s.cv.notify_all();
}

std::shared_ptr<const ModConflictSet> mod_conflicts_get(int g)
{
    if (g < 0 || g >= G_COUNT) return nullptr;
    Shared &s = sh();
    std::lock_guard<std::mutex> l(s.m);
    const std::shared_ptr<const ModConflictSet> &r = s.done[g];
    return r && r->key == g_asked[g] ? r : nullptr;   // (another order's result would name the wrong winners)
}

// a conflict's line (the LIBRARY's rows, the Mods tab). kind: 0 its files a later mod replaces, 1 files of an earlier
// mod it replaces, 2 textures a later pack wins, 3 textures it wins (mod_conflicts.h)
std::string mod_conflict_text(const ModConflictPair &p, int kind)
{
    const std::string n = std::to_string(p.count);
    const bool one = p.count == 1;
    switch (kind) {
    case 0: return one ? trf("1 file replaced by {1}", { p.other_name }) : trf("{1} files replaced by {2}", { n, p.other_name });
    case 1: return one ? trf("Replaces 1 file of {1}", { p.other_name }) : trf("Replaces {1} files of {2}", { n, p.other_name });
    case 2: return one ? trf("1 texture also in {1} ({1} wins)", { p.other_name }) : trf("{1} textures also in {2} ({2} wins)", { n, p.other_name });
    default: return one ? trf("1 texture also in {1} (this one wins)", { p.other_name }) : trf("{1} textures also in {2} (this one wins)", { n, p.other_name });
    }
}
// ... and its tip: what wins, then the paths (this mod's own), at most p.names, and how many more
std::vector<std::string> mod_conflict_tip(const ModConflictPair &p, int kind)
{
    std::vector<std::string> v;
    v.push_back(kind == 0 || kind == 2 ? trf("{1} is lower in the load order, so the game uses its copy.", { p.other_name })
                                       : trf("This mod is lower in the load order than {1}, so the game uses its copy.", { p.other_name }));
    for (const std::string &x : p.names) v.push_back("  " + x);
    if (p.count > p.names.size()) v.push_back(trf("  and {1} more", { std::to_string(p.count - p.names.size()) }));
    return v;
}
