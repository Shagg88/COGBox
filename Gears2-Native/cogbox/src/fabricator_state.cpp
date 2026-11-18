// COGBox - the FABRICATOR screen's state (see fabricator_state.h): the worker, the jobs, the LIBRARY's mods folders.
#include "fabricator_state.h"
#include "i18n.h"
#include "mod_ini_edit.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace {

// One thread running posted jobs in order. Never destroyed (its thread lives until the process ends).
class Worker {
public:
    void post(std::function<void()> f)
    {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(std::move(f));
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        cv_.notify_one();
    }
    bool idle()
    {
        std::lock_guard<std::mutex> l(m_);
        return q_.empty() && !running_;
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return !q_.empty(); });
                f = std::move(q_.front());
                q_.pop_front();
                running_ = true;
            }
            f();
            std::lock_guard<std::mutex> l(m_);
            running_ = false;
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool started_ = false, running_ = false;
};
Worker &worker() { static Worker *w = new Worker; return *w; }

// the jobs (the UI thread's list; the worker holds its own shared_ptr to the one it runs)
std::vector<std::shared_ptr<FabJob>> g_jobs;
int g_nextId = 1;

const char *const kClose = "Close the game first.";

std::string work_root() { return join(join(exe_dir(), "fabricator"), "work"); }

std::string base_name(const std::string &p)
{
    std::string s = p;
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const size_t at = s.find_last_of("\\/");
    return at == std::string::npos ? s : s.substr(at + 1);
}
std::string stem(const std::string &file)
{
    const std::string b = base_name(file);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}
std::string lower_s(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim_s(const std::string &s)
{
    const size_t x = s.find_first_not_of(" \t\r"), y = s.find_last_not_of(" \t\r");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}

int game_of_key(const std::string &k)
{
    for (int g = 0; g < G_COUNT; g++) if (k == game_def(g).key) return g;
    return -1;
}

std::string sha256_file(const std::string &path)
{
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD n = 0;
        bool good = true;
        while (good && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) good = BCryptHashData(h, buf.data(), n, 0) == 0;
        unsigned char d[32];
        if (good && BCryptFinishHash(h, d, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
            out.assign(hex, 64);
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// the temporary zip of a folder being added, and its folder (only ever COGBox's own, under fabricator\work)
void drop_temp(FabJob &j)
{
    if (j.work_dir.empty()) return;
    cogbox_remove_tree(widen(j.work_dir));
    j.work_dir.clear();
}

std::string first_problems(const std::vector<std::string> &v)
{
    std::string s;
    for (size_t i = 0; i < v.size() && i < 3; i++) s += (i ? "; " : "") + v[i];
    if (v.size() > 3) s += " (and " + std::to_string(v.size() - 3) + " more)";
    return s;
}

// ---- the worker's parts -----------------------------------------------------------------------------------------------
void run_add(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, j->from_folder ? FabJob::Packing : FabJob::Checking)) return;   // cancelled
    if (j->from_folder) {
        if (!make_dirs(j->work_dir)) { j->problems = { "couldn't make the Fabricator's work folder (is the disk full?)" }; j->phase = FabJob::Failed; return; }
        j->zip = join(j->work_dir, base_name(j->source) + ".zip");   // its name is the mod's name when mod.ini has none
        const std::string e = fab_pack_folder(j->source, j->zip, j->progress, &j->pack_left_out);
        if (e == "cancelled" || (e.empty() && j->progress.cancel)) { drop_temp(*j); j->phase = FabJob::Cancelled; return; }
        if (!e.empty()) { drop_temp(*j); j->problems = { e }; j->phase = FabJob::Failed; return; }
        j->phase = FabJob::Checking;
    }
    const std::string want = game_is_port(j->want_game) ? game_def(j->want_game).key : "gears3";
    std::vector<std::string> p = fab_check_zip(j->zip, want, &j->info);
    // a mod.ini for another game is not a problem here: the mod goes to that game (fab_install_unpack checks it again)
    const std::string other = "mod.ini is for game ";
    p.erase(std::remove_if(p.begin(), p.end(), [&](const std::string &s) {
        const bool o = s.compare(0, other.size(), other) == 0;
        if (o) j->other_game = true;
        return o;
    }), p.end());
    j->problems = p;
    if (p.empty()) j->sha256 = sha256_file(j->zip);
    if (j->progress.cancel) { drop_temp(*j); j->phase = FabJob::Cancelled; return; }   // Cancel pressed while it checked
    j->phase = FabJob::Checked;
}

void run_unpack(const std::shared_ptr<FabJob> &j)
{
    const std::string e = fab_install_unpack(j->plan, &j->left_out);
    if (!e.empty()) {
        drop_temp(*j);
        j->problems = { e };
        j->phase = FabJob::Failed;
        return;
    }
    j->phase = FabJob::Unpacked;
}

void run_pack(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, FabJob::Packing)) return;
    const std::string e = fab_pack_folder(j->source, j->zip, j->progress);
    if (e.empty() && j->progress.cancel) {   // cancelled as it finished: no zip is left, as the screen says
        DeleteFileW(widen(j->zip).c_str());
        j->phase = FabJob::Cancelled;
        return;
    }
    if (e == "cancelled") { j->phase = FabJob::Cancelled; return; }
    if (!e.empty()) { j->problems = { e }; j->phase = FabJob::Failed; return; }
    j->phase = FabJob::Done;
}

void run_check(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, FabJob::Checking)) return;
    j->problems = fab_check_folder(j->source, j->game_key);
    j->phase = FabJob::Done;
}

// a failure found on the UI thread: said now
void fail_ui(App &a, FabJob &j, const std::string &msg)
{
    drop_temp(j);
    j.said = true;
    j.phase = FabJob::Failed;
    set_message(a, msg, true);
}

std::shared_ptr<FabJob> find_job(int id)
{
    for (auto &j : g_jobs) if (j->id == id) return j;
    return nullptr;
}

void start_add(const std::string &path, int game, const std::string &updateFolder)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Add;
    j->source = path;
    j->from_folder = dir_exists(path);
    j->want_game = game;
    j->update_folder = updateFolder;
    j->name = j->from_folder ? base_name(path) : stem(path);
    if (j->from_folder) j->work_dir = join(work_root(), std::to_string(j->id) + "-" + now_stamp());
    else j->zip = path;
    g_jobs.push_back(j);
    worker().post([j] { run_add(j); });
}

// ---- the LIBRARY's rows -------------------------------------------------------------------------------------------------
int g_libGame = -1;
std::vector<FabLibRow> g_lib;
int g_libFor = -1;
Uint64 g_libAt = 0;
bool g_libDirty = true;

// <folder>\mod.ini's [Mod] Type ("" none), as fab_check_zip reads it from a zip
std::string mod_type(const std::string &folder)
{
    std::string ini;
    if (!read_file(join(folder, "mod.ini"), ini, kFabMaxIniBytes)) return "";
    if (ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
    std::string section, type;
    for (size_t at = 0; at <= ini.size();) {
        size_t e = ini.find('\n', at);
        if (e == std::string::npos) e = ini.size();
        const std::string line = trim_s(ini.substr(at, e - at));
        at = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') { section = lower_s(trim_s(line.substr(1, line.size() - 2))); continue; }
        if (!section.empty() && section != "mod") continue;
        const size_t eq = line.find('=');
        if (eq != std::string::npos && lower_s(trim_s(line.substr(0, eq))) == "type") type = trim_s(line.substr(eq + 1));
    }
    return type;
}

}  // namespace

// ---- jobs ---------------------------------------------------------------------------------------------------------------
void fab_add_path(App &a, const std::string &path)
{
    if (path.empty()) return;
    const bool folder = dir_exists(path);
    const std::string low = lower_s(path);
    if (!folder && (low.size() < 4 || low.compare(low.size() - 4, 4, ".zip") != 0)) {
        set_message(a, "Add a mod's .zip or its folder.", true);
        return;
    }
    if (!folder && !file_exists(path)) { set_message(a, "That file isn't there any more.", true); return; }
    start_add(path, fab_library_game(a), "");
}

void fab_update_from(App &a, int g, const std::string &folder, const std::string &zip)
{
    if (zip.empty() || !game_is_port(g)) return;
    if (a.run.proc || any_game_exe_running()) { set_message(a, kClose, true); return; }
    start_add(zip, g, folder);
}

std::shared_ptr<FabJob> fab_pack_start(const std::string &folder, const std::string &zip_path, const std::string &name)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Pack;
    j->source = folder;
    j->zip = zip_path;
    j->name = name;
    g_jobs.push_back(j);   // (listed while it runs: the LIBRARY's WORKING lines, the front button's figure, cancelled at exit)
    worker().post([j] { run_pack(j); });
    return j;
}

std::shared_ptr<FabJob> fab_check_start(const std::string &folder, const std::string &game_key)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Check;
    j->source = folder;
    j->game_key = game_key;
    worker().post([j] { run_check(j); });
    return j;
}

bool fab_can_cancel(const FabJob &j)
{
    const int ph = j.phase;
    return ph == FabJob::Queued || ph == FabJob::Packing || ph == FabJob::Checked || ph == FabJob::Asking ||
           ph == FabJob::Waiting || ph == FabJob::Unpacked;
}

void fab_cancel(const std::shared_ptr<FabJob> &j)
{
    if (!j) return;
    j->progress.cancel = true;   // a pack stops (fab_pack_folder watches it)
    int q = FabJob::Queued;
    if (j->phase.compare_exchange_strong(q, FabJob::Cancelled)) return;
    // the UI thread's own phases: nothing runs on the worker for them
    for (int ph : { (int)FabJob::Checked, (int)FabJob::Asking, (int)FabJob::Waiting }) {
        int x = ph;
        if (j->phase.compare_exchange_strong(x, FabJob::Cancelled)) {
            if (j->kind == FabJob::Add) drop_temp(*j);
            return;
        }
    }
    int u = FabJob::Unpacked;   // unpacked, waiting for the game to close: the .partial goes (on the worker, after any other file work)
    if (j->phase.compare_exchange_strong(u, FabJob::Cancelled)) {
        const FabInstallPlan plan = j->plan;
        const std::string work = j->work_dir;
        j->work_dir.clear();
        worker().post([plan, work] { fab_install_abandon(plan); if (!work.empty()) cogbox_remove_tree(widen(work)); });
    }
}

std::vector<std::shared_ptr<FabJob>> fab_jobs() { return g_jobs; }

bool fab_job_running(const FabJob &j)
{
    const int ph = j.phase;
    return ph != FabJob::Done && ph != FabJob::Failed && ph != FabJob::Cancelled;
}

int fab_job_percent()
{
    for (auto &j : g_jobs) {
        if (!fab_job_running(*j)) continue;
        const int64_t total = j->progress.total, done = j->progress.done;
        if (j->phase == FabJob::Packing && total > 0) return (int)std::min<int64_t>(100, done * 100 / total);
        return -2;
    }
    return -1;
}

// ---- the LIBRARY --------------------------------------------------------------------------------------------------------
int fab_library_game(const App &a)
{
    if (!game_is_port(g_libGame)) g_libGame = game_is_port(a.front) ? a.front : (int)G_GEARS3;
    return g_libGame;
}
void fab_library_set_game(int g)
{
    if (game_is_port(g) && g != g_libGame) { g_libGame = g; g_libDirty = true; }
}
void fab_library_changed() { g_libDirty = true; }

const std::vector<FabLibRow> &fab_library(App &a, int g)
{
    if (!g_libDirty && g == g_libFor && SDL_GetTicks() - g_libAt < 2000) return g_lib;
    g_libDirty = false;
    g_libFor = g;
    g_libAt = SDL_GetTicks();
    g_lib.clear();
    for (const ModItem &m : mods_list(a, g)) {
        FabLibRow r;
        r.m = m;
        if (m.fab) r.rec = fab_read_record(m.path);
        r.type = mod_type(m.path);
        g_lib.push_back(std::move(r));
    }
    return g_lib;
}

// ---- questions ----------------------------------------------------------------------------------------------------------
void fab_uninstall_ask(App &a, int g, const ModItem &m)
{
    ask(a, "fabuninstall", g, "Uninstall a mod",
        trf("Uninstall {1}? Its folder is moved to a backup in the mods folder (.fabricator-backup), so it can be put back by hand.", { m.name }),
        { "Uninstall", "Cancel" }, m.folder);
}

bool fab_answer(App &a, const Modal &m)
{
    if (m.tag == "fabupdate") {
        auto j = find_job(atoi(m.data.c_str()));
        if (!j) return true;
        int x = FabJob::Asking;
        if (m.answer == 0) j->phase.compare_exchange_strong(x, FabJob::Waiting);
        else if (j->phase.compare_exchange_strong(x, FabJob::Cancelled)) drop_temp(*j);
        return true;
    }
    if (m.tag == "fabuninstall") {
        if (m.answer != 0 || !game_is_port(m.game)) return true;
        const std::string e = fab_uninstall(a, m.game, m.data);
        if (e.empty()) {
            env_refresh(a, m.game);
            set_message(a, "The mod is uninstalled. Its folder is in the mods folder's .fabricator-backup.", false);
        } else set_message(a, e, true);
        fab_library_changed();
        return true;
    }
    return false;
}

// ---- once a frame -------------------------------------------------------------------------------------------------------
// (fab_sweep_partials: a .partial with COGBox's record is an install a closed or crashed COGBox never finished)
static void sweep_partials()
{
    for (int g = 0; g < G_COUNT; g++) fab_sweep_partials(g);
}

void fab_state_poll(App &a)
{
    static bool swept = false;
    if (!swept) {   // what a closed (or crashed) COGBox left: temporary zips, unfinished installs; no job exists yet
        swept = true;
        cogbox_remove_tree(widen(work_root()));
        sweep_partials();
    }
    bool installing = false;
    for (auto &j : g_jobs)
        if (j->kind == FabJob::Add && (j->phase == FabJob::Unpacking || j->phase == FabJob::Unpacked)) installing = true;

    for (auto &j : g_jobs) {
        if (j->kind == FabJob::Pack) {   // the CREATOR tab shows how it went; said here once
            if (j->phase == FabJob::Done && !j->said) set_message(a, trf("Made {1}.", { j->name }), false);
            if (!fab_job_running(*j)) j->said = true;
            continue;
        }
        if (j->kind != FabJob::Add) continue;
        const int ph = j->phase;
        if (ph == FabJob::Asking && !(a.modal.open && a.modal.tag == "fabupdate" && a.modal.data == std::to_string(j->id))) {
            j->phase = FabJob::Checked;   // its question was replaced by another one: asked again
            continue;
        }
        if (ph == FabJob::Checked) {
            if (j->progress.cancel) { drop_temp(*j); j->phase = FabJob::Cancelled; continue; }
            if (a.modal.open) continue;   // (decided once the open question is answered; no disk scan meanwhile)
            // the game it goes to: the LIBRARY's, or the one its mod.ini names
            int g = j->want_game;
            if (j->other_game) {
                g = game_of_key(j->info.game);
                if (g < 0) j->problems.push_back("its mod.ini is for a game COGBox doesn't know");
                else if (!j->update_folder.empty() && g != j->want_game)
                    j->problems.push_back(std::string("this zip is for ") + game_def(g).shortName);
            }
            if (!j->problems.empty()) {
                fail_ui(a, *j, trf("{1} can't be added: {2}.", { j->name, first_problems(j->problems) }));
                continue;
            }
            j->game = g;
            j->name = j->info.name;
            FabInstallItem item;
            item.name = j->info.name; item.version = j->info.version; item.sha256 = j->sha256;
            std::string match, oldAuthor;
            for (const ModItem &m : mods_list(a, g)) {
                if (!m.fab) continue;
                auto r = fab_read_record(m.path);
                if (r && fab_same_install(*r, item)) { match = m.folder; j->old_version = r->version; oldAuthor = m.author; break; }
            }
            if (!j->update_folder.empty() && match != j->update_folder) {
                fail_ui(a, *j, trf("This zip is {1}, not the mod you chose to update. Add it with Add a mod instead.", { j->name }));
                continue;
            }
            j->decided_update = !match.empty();
            if (match.empty()) { j->phase = FabJob::Waiting; continue; }
            const std::string from = j->old_version.empty() ? "" : " " + j->old_version, to = j->info.version.empty() ? "" : " to " + j->info.version;
            // "Update Horde Bank 1.0 to 1.1?" - the name and versions as the mod gives them (not translated)
            // review r369 F9: a mod is told apart by its Name only (until Nexus ids), so the question shows which file and
            // author the new version comes from, and says so when the author isn't the installed one's
            const std::string zipName = narrow(std::filesystem::path(widen(j->zip)).filename().wstring());
            const std::string newAuthor = j->info.author;
            std::string who = "\n\n" + trf("From: {1}", { zipName }) + (newAuthor.empty() ? "" : " - " + trf("by {1}", { newAuthor }));
            if (!oldAuthor.empty() && lower_s(newAuthor) != lower_s(oldAuthor))
                who += "\n" + trf("Careful: the installed {1} is by {2}, this one by {3}. Only update if you trust this file.",
                                   { j->name, oldAuthor, newAuthor.empty() ? tr("nobody named") : newAuthor });
            ask(a, "fabupdate", g, "Update a mod", tr("Update") + std::string(" ") + j->name + from + to + "?" + who + "\n\n" +
                tr("The installed version is kept as a backup in the mods folder. The mod keeps its place in the order, and stays on or off as it is now."),
                { "Update", "Cancel" }, std::to_string(j->id));
            j->phase = FabJob::Asking;
        } else if (ph == FabJob::Waiting) {
            if (installing) continue;
            if (a.run.proc || any_game_exe_running()) {
                if (!j->told_close) { j->told_close = true; set_message(a, std::string(tr(kClose)) + " " + tr("The mod is installed once the game is closed."), true); }
                continue;
            }
            FabInstallItem item;
            item.name = j->info.name; item.version = j->info.version; item.sha256 = j->sha256;
            FabInstallPlan plan;
            const std::string e = fab_install_prepare(a, j->game, item, j->zip, &plan);
            if (e == kClose) continue;
            if (!e.empty()) { fail_ui(a, *j, e); continue; }
            if (plan.update != j->decided_update) {   // another add of this name was installed meanwhile: decided (asked) again
                j->phase = FabJob::Checked;
                continue;
            }
            plan.strip_top = !j->from_folder;   // a folder add: the chosen folder is the mod's root
            j->plan = plan;   // (read by the worker after the post)
            j->phase = FabJob::Unpacking;
            installing = true;
            worker().post([j] { run_unpack(j); });
        } else if (ph == FabJob::Unpacked) {
            if (a.run.proc || any_game_exe_running()) {   // a game started during the unpack: the move waits for it to close (the .partial stays)
                if (!j->told_close) { j->told_close = true; set_message(a, std::string(tr(kClose)) + " " + tr("The mod is installed once the game is closed."), true); }
                continue;
            }
            const std::string e = fab_install_commit(a, j->plan);
            if (e == kClose) continue;
            if (e.rfind("The mods folder changed", 0) == 0) {   // (the commit removed the .partial) decided and unpacked again
                j->phase = FabJob::Checked;
                continue;
            }
            fab_library_changed();
            if (!e.empty()) { fail_ui(a, *j, e); continue; }
            drop_temp(*j);
            env_refresh(a, j->plan.g);
            j->phase = FabJob::Done;
            std::string msg = j->plan.update ? trf("{1} is updated.", { j->name })
                            : j->game != j->want_game ? trf("{1} is added to {2} (its mod.ini is for it) and on.", { j->name, game_def(j->game).shortName })
                                                      : trf("{1} is added and on.", { j->name });
            std::vector<std::string> left = j->pack_left_out;   // a folder's (left out by the pack) and a zip's (by the unpack)
            left.insert(left.end(), j->left_out.begin(), j->left_out.end());
            if (!left.empty()) {   // the program files not added: the first 5 names, then how many more
                std::string names;
                for (size_t i = 0; i < left.size() && i < 5; i++) names += (i ? ", " : "") + base_name(left[i]);
                if (left.size() > 5) names += " +" + std::to_string(left.size() - 5);
                msg += " " + trf("Left out (program files): {1}", { names });
            }
            set_message(a, msg, false);
        } else if (ph == FabJob::Failed && !j->said) {   // a failure on the worker (pack, unpack): said once
            j->said = true;
            set_message(a, trf("{1} can't be added: {2}.", { j->name, first_problems(j->problems) }), true);
        }
    }
    // finished jobs leave the list (a failure on the UI thread was said where it happened)
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<FabJob> &j) {
        const int ph = j->phase;
        return ph == FabJob::Done || ph == FabJob::Cancelled || (ph == FabJob::Failed && j->said);
    }), g_jobs.end());
}

void fab_state_shutdown()
{
    for (auto &j : g_jobs) j->progress.cancel = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until && !worker().idle()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!worker().idle()) return;   // still unpacking: its .partial is hidden from the game; the next start's sweep removes it
    for (auto &j : g_jobs) {
        if (j->kind != FabJob::Add) continue;
        if (j->phase == FabJob::Unpacked) fab_install_abandon(j->plan);
        drop_temp(*j);
    }
}
