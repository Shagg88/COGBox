// COGBox - the FABRICATOR screen's state (see fabricator_state.h): the workers, the cached answers, the transfers and
// the pictures. The server's side: tools/master-server/src/fabricator/routes.ts.
#include "fabricator_state.h"
#include "fabricator_install.h"
#include "fabricator_pack.h"
#include "app.h"

#include "../third_party/miniz/miniz.h"
#include "../third_party/nlohmann/json.hpp"
#include "../third_party/stb_image.h"   // (the implementation is in front.cpp: PNG and JPEG)

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

using json = nlohmann::json;

namespace {

const char *const kApi = "/api/v1/fabricator";
const char *const kImages = "/api/v1/fabricator/images/";
constexpr size_t kMaxImages = 120;           // textures kept
constexpr size_t kMaxImageBytes = 8u << 20;  // a picture read (the server keeps them under 4 MB)
constexpr int kMaxImageSide = 960;           // pictures are halved until they fit (thumbnails and the page's big one)

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
    void clear()
    {
        std::lock_guard<std::mutex> l(m_);
        q_.clear();
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
Worker &calls() { static Worker *w = new Worker; return *w; }
Worker &pictures() { static Worker *w = new Worker; return *w; }
Worker &downloads() { static Worker *w = new Worker; return *w; }
Worker &uploads() { static Worker *w = new Worker; return *w; }
Worker &installs() { static Worker *w = new Worker; return *w; }   // a Get's check and unpack (fab_install_unpack)

struct Img {
    int state = 0;   // 0 asked, 1 pixels ready, 2 texture, 3 can't be read
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    SDL_Texture *tex = nullptr;
    uint64_t used = 0, used_frame = 0;
};

struct Shared {
    std::mutex mu;
    std::string server_url, identity_dir;
    bool configured_once = false;
    FabView v;
    uint64_t store_gen = 0, page_gen = 0, library_gen = 0, creator_gen = 0;
    std::map<std::string, Img> imgs;
    uint64_t img_clock = 0;
    uint64_t frame = 0;   // fab_state_poll's count: a texture used this frame is never evicted
    std::vector<std::pair<std::string, bool>> messages;   // for set_message on the UI thread
    uint64_t local_changes = 0;
};
Shared &sh() { static Shared *s = new Shared; return *s; }   // never destroyed: the workers may still read it at exit

void say(const std::string &m, bool bad)
{
    std::lock_guard<std::mutex> l(sh().mu);
    sh().messages.push_back({ m, bad });
}

// What a worker job needs: the server and the account, read now (the account is identity.json, a small file).
struct Ctx {
    std::string server_url;
    std::optional<ServerUrl> server;
    std::optional<SocialAccount> acct;
};
Ctx ctx()
{
    Ctx c;
    std::string dir;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        c.server_url = sh().server_url;
        dir = sh().identity_dir;
    }
    c.server = parse_server_url(c.server_url);
    if (!dir.empty()) c.acct = read_account(dir);
    std::lock_guard<std::mutex> l(sh().mu);
    if (sh().server_url == c.server_url && sh().identity_dir == dir) {
        sh().v.configured = c.server.has_value();
        sh().v.signed_in = c.acct.has_value();
        sh().v.tag = c.acct ? c.acct->tag() : "";
        sh().v.account_checked = true;
    }
    return c;
}

std::string json_error(const std::string &body)
{
    json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return "";
    if (j.contains("problems") && j["problems"].is_array()) {
        std::string s;
        for (const json &p : j["problems"]) if (p.is_string()) s += (s.empty() ? "" : "; ") + p.get<std::string>();
        if (!s.empty()) return s;
    }
    if (j.contains("error") && j["error"].is_string()) return j["error"].get<std::string>();
    if (j.contains("problem") && j["problem"].is_string()) return j["problem"].get<std::string>();
    return "";
}
std::string net_problem(const HttpResult &r)
{
    if (r.status == 0) return "Can't reach the Fabricator server - check the internet connection.";
    if (r.status == 401) return "The server didn't accept your online account - see the SOCIAL tab.";
    const std::string e = json_error(r.body);
    if (!e.empty()) return "The server said: " + e + ".";
    return "The server answered " + std::to_string(r.status) + ".";
}

std::string url_encode(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

int game_of_key(const std::string &k)
{
    for (int g = 0; g < G_COUNT; g++) if (k == game_def(g).key) return g;
    return -1;
}

bool active(int phase) { return phase != FabTransfer::Done && phase != FabTransfer::Failed && phase != FabTransfer::Cancelled; }

void fail(const std::shared_ptr<FabTransfer> &t, const std::vector<std::string> &problems)
{
    {
        std::lock_guard<std::mutex> l(sh().mu);
        t->problems_ = problems;
    }
    t->failed_phase = t->phase.load();
    t->phase = FabTransfer::Failed;
    if (t->kind == FabTransfer::Download && !problems.empty()) say(problems[0], true);   // (a publish lists them on its form)
}

// ---- the player's ratings, remembered in <state dir>\ratings.json per server: {"<server>": {"<mod id>": stars}} ----
std::string ratings_file() { return join(fab_state_dir(), "ratings.json"); }
std::map<int64_t, int> load_ratings(const std::string &server)
{
    std::map<int64_t, int> m;
    std::string t;
    if (!read_file(ratings_file(), t, 1u << 20)) return m;
    json j = json::parse(t, nullptr, false);
    if (!j.is_object() || !j.contains(server) || !j[server].is_object()) return m;
    for (auto it = j[server].begin(); it != j[server].end(); ++it)
        if (it.value().is_number_integer()) m[atoll(it.key().c_str())] = it.value().get<int>();
    return m;
}
void save_rating(const std::string &server, int64_t id, int stars)
{
    std::string t;
    json j = read_file(ratings_file(), t, 1u << 20) ? json::parse(t, nullptr, false) : json();
    if (!j.is_object()) j = json::object();
    if (!j.contains(server) || !j[server].is_object()) j[server] = json::object();
    j[server][std::to_string(id)] = stars;
    make_dirs(fab_state_dir());
    write_file(ratings_file(), j.dump(1));
}

// ---- the jobs ----
void run_get(const std::shared_ptr<FabTransfer> &t)
{
    if (t->progress.cancel) { t->phase = FabTransfer::Cancelled; return; }
    Ctx c = ctx();
    if (!c.server || !c.acct) { fail(t, { "Sign in on the SOCIAL tab to get mods" }); return; }
    const std::string id = std::to_string(t->mod_id);
    for (int tries = 0; tries < 2; tries++) {
        t->phase = FabTransfer::Address;
        const HttpResult r = fab_call(*c.server, "POST", std::string(kApi) + "/mods/" + id + "/download", "", &*c.acct);
        if (r.status == 410) { fail(t, { "This mod was taken down - it can't be downloaded." }); return; }
        if (r.status == 429) { fail(t, { "You've asked for many downloads this hour - try again later." }); return; }
        if (r.status == 404) { fail(t, { "This mod isn't in the store (any more)." }); return; }
        if (r.status != 200) { fail(t, { net_problem(r) }); return; }
        json j = json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j["url"].is_string() || !j["sha256"].is_string() || !j["size"].is_number_integer()) {
            fail(t, { "The server's answer couldn't be read." });
            return;
        }
        const std::string url = j["url"].get<std::string>(), sha = j["sha256"].get<std::string>();
        const int64_t size = j["size"].get<int64_t>();
        t->item.version_id = j.value("version_id", (int64_t)0);
        t->item.sha256 = sha;
        t->item.size = size;
        fab_load_library();   // the download put the mod in the library
        const std::string dir = join(fab_state_dir(), "downloads");
        make_dirs(dir);
        const std::string dest = join(dir, id + "-" + std::to_string(t->item.version_id) + ".zip");
        DeleteFileW(widen(dest).c_str());   // a finished one left by a crash: fetched again (its hash is only checked on download)
        if (t->progress.cancel) { t->phase = FabTransfer::Cancelled; return; }   // cancelled (or Online turned off) meanwhile
        t->phase = FabTransfer::Downloading;
        const std::string e = fab_download(fab_full_url(c.server_url, url), dest, sha, size, t->progress);
        if (e.empty()) {
            t->zip = dest;
            t->phase = FabTransfer::Downloaded;   // fab_state_poll installs it on the UI thread
            return;
        }
        if (e == "cancelled") { t->phase = FabTransfer::Cancelled; return; }
        if (e.rfind("address expired", 0) == 0 && tries == 0) continue;   // a new address, once
        fail(t, { "The download didn't finish: " + e + "." });
        return;
    }
}

void run_publish(const std::shared_ptr<FabTransfer> &t, const FabPublish &p)
{
    if (t->progress.cancel) { t->phase = FabTransfer::Cancelled; return; }
    Ctx c = ctx();
    if (!c.server || !c.acct) { fail(t, { "Sign in on the SOCIAL tab to publish mods" }); return; }
    std::string zip = p.source;
    if (p.folder) {
        t->phase = FabTransfer::Packing;
        fab_pack_sweep(2 * 24 * 3600, fab_pack_path(p.source));   // packs no session can resume any more (a day on the server)
        bool reused = false;
        // a changed folder is packed again: the session of its old zip will never be resumed, so it is deleted
        const std::string e = fab_pack_folder(p.source, t->progress, &zip, &reused,
                                              [&] { fab_upload_discard(*c.server, *c.acct, fab_pack_path(p.source)); });
        if (e == "cancelled") { t->phase = FabTransfer::Cancelled; return; }
        if (!e.empty()) { fail(t, { e }); return; }
        t->zip_made = true;
    }
    t->zip = zip;
    // given up for good (refused here or by the server, or published): the session and the packed zip go
    auto drop = [&] {
        fab_upload_discard(*c.server, *c.acct, zip);
        if (t->zip_made) fab_pack_drop(zip);
    };
    t->phase = FabTransfer::Checking;
    const std::vector<std::string> local = fab_check_zip(zip, p.game_key);
    if (!local.empty()) { drop(); fail(t, local); return; }
    if (t->progress.cancel) { t->resumable = true; t->phase = FabTransfer::Cancelled; return; }
    t->progress.done = 0;
    t->progress.total = 0;
    t->phase = FabTransfer::Uploading;
    int64_t mod = 0;
    const std::string e = fab_upload(*c.server, *c.acct, p.meta_json, p.mod_id, zip, p.screens, t->progress, &mod);
    // kept for the next publish of the same folder or zip while the server keeps its session (cancelled, no answer)
    const bool kept = !e.empty() && (fab_upload_remembered(*c.server, *c.acct, zip) || e == "cancelled" ||
                                     e.rfind("no answer", 0) == 0 || e.rfind("the server keeps refusing", 0) == 0);
    if (kept) t->resumable = true;
    else if (t->zip_made) fab_pack_drop(zip);
    if (e.empty()) {
        t->mod_id = mod;
        t->phase = FabTransfer::Done;
        say(t->name + " is live in the store.", false);
        fab_load_creator();
        return;
    }
    if (e == "cancelled") { t->phase = FabTransfer::Cancelled; return; }
    std::vector<std::string> lines;
    for (size_t at = 0; at < e.size();) {
        size_t n = e.find('\n', at);
        if (n == std::string::npos) n = e.size();
        if (n > at) lines.push_back(e.substr(at, n - at));
        at = n + 1;
    }
    fail(t, lines);
    fab_load_creator();   // (a refusal after a lost answer, or a publish that did go through: the Creator tab says)
}

// A Get's install, the long part (the install worker): the zip checked and unpacked into <folder>.partial
void run_unpack(const std::shared_ptr<FabTransfer> &t)
{
    const std::string e = fab_install_unpack(t->plan);
    if (!e.empty()) {
        DeleteFileW(widen(t->zip).c_str());
        fail(t, { e });
        return;
    }
    int ph = FabTransfer::Installing;
    if (!t->phase.compare_exchange_strong(ph, FabTransfer::Unpacked)) fab_install_abandon(t->plan);   // cancelled meanwhile
}

// key: the cache's key; source: a store image path (local = false) or a file the player chose (local = true).
// Only the upload form passes local = true (fab_local_image); store data never reaches it.
void run_image(const std::string &key, const std::string &source, bool local)
{
    {
        std::lock_guard<std::mutex> l(sh().mu);
        auto it = sh().imgs.find(key);
        if (it == sh().imgs.end() || it->second.state != 0) return;   // freed meanwhile
    }
    std::string data;
    bool ok = false;
    if (local) ok = read_file(source, data, kMaxImageBytes);
    else {
        std::string url;
        {
            std::lock_guard<std::mutex> l(sh().mu);
            url = sh().server_url;
        }
        if (auto s = parse_server_url(url)) {
            HttpResult r = fab_get_file(*s, source, kMaxImageBytes);
            ok = r.status == 200;
            data = std::move(r.body);
        }
    }
    int w = 0, h = 0, n = 0;
    // the size first: nothing over the store's 3840 x 2160 is decoded
    if (ok && (!stbi_info_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &n) || w <= 0 || h <= 0 || w > 3840 || h > 2160))
        ok = false;
    unsigned char *px = ok ? stbi_load_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &n, 4) : nullptr;
    std::vector<unsigned char> out;
    if (px) {
        while ((w > kMaxImageSide || h > kMaxImageSide) && w >= 2 && h >= 2) {   // a 2x2 average, as front.cpp's covers
            const int nw = w / 2, nh = h / 2;
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    for (int k = 0; k < 4; k++) {
                        const unsigned char *a = px + ((size_t)(2 * y) * w + 2 * x) * 4 + k, *b = a + (size_t)w * 4;
                        px[((size_t)y * nw + x) * 4 + k] = (unsigned char)((a[0] + a[4] + b[0] + b[4] + 2) / 4);
                    }
            w = nw;
            h = nh;
        }
        out.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
    }
    std::lock_guard<std::mutex> l(sh().mu);
    auto it = sh().imgs.find(key);
    if (it == sh().imgs.end() || it->second.state != 0) return;
    if (out.empty()) { it->second.state = 3; return; }
    it->second.px = std::move(out);
    it->second.w = w;
    it->second.h = h;
    it->second.state = 1;
}

std::string lower_s(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim_s(const std::string &s)
{
    size_t x = s.find_first_not_of(" \t\r"), y = s.find_last_not_of(" \t\r");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}
void parse_mod_ini(std::string ini, FabModIni &m)
{
    if (ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
    std::string section;
    for (size_t at = 0; at <= ini.size();) {
        size_t e = ini.find('\n', at);
        if (e == std::string::npos) e = ini.size();
        const std::string line = trim_s(ini.substr(at, e - at));
        at = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') { section = lower_s(trim_s(line.substr(1, line.size() - 2))); continue; }
        if (section == "campaign" && m.type.empty()) m.type = "campaign";   // modcore: a [Campaign] section
        if (!section.empty() && section != "mod") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = lower_s(trim_s(line.substr(0, eq))), v = trim_s(line.substr(eq + 1));
        if (k == "name") m.name = v;
        else if (k == "game") m.game = lower_s(v);
        else if (k == "version") m.version = v;
        else if (k == "description") m.description = v;
        else if (k == "side") m.side = lower_s(v);
        else if (k == "type") m.type = lower_s(v);
    }
    m.found = true;
}

}  // namespace

// fail() above writes the problems under the state's lock; FabTransfer keeps them in a member only this file touches
std::vector<std::string> fab_transfer_problems(const FabTransfer &t)
{
    std::lock_guard<std::mutex> l(sh().mu);
    return t.problems_;
}

void fab_configure(const std::string &server_in, const std::string &identity_in, bool online)
{
    // Online play off: no server and no account, so no worker job can reach the network; running transfers stop
    const std::string server_url = online ? server_in : std::string(), identity_dir = online ? identity_in : std::string();
    {
        std::lock_guard<std::mutex> l(sh().mu);
        if (sh().v.online != online) {
            sh().v.online = online;
            if (!online) for (auto &t : sh().v.transfers) t->progress.cancel = true;
        }
        if (sh().configured_once && sh().server_url == server_url && sh().identity_dir == identity_dir) return;
        const bool serverChanged = sh().server_url != server_url;
        sh().configured_once = true;
        sh().server_url = server_url;
        sh().identity_dir = identity_dir;
        sh().v.account_checked = false;
        sh().store_gen++; sh().page_gen++; sh().library_gen++; sh().creator_gen++;   // answers to the old settings are dropped
        sh().v.store_loading = sh().v.page_loading = sh().v.library_loading = sh().v.creator_loading = false;
        sh().v.library_loaded = false;
        sh().v.library.clear();
        sh().v.creator_loaded = false;
        sh().v.creator.clear();
        sh().v.limits = FabLimits();
        if (serverChanged) { sh().v.store_asked = false; sh().v.cards.clear(); sh().v.page_id = 0; sh().v.page.reset(); }
    }
    calls().post([server_url] {
        auto r = load_ratings(server_url);
        {
            std::lock_guard<std::mutex> l(sh().mu);
            if (sh().server_url == server_url) sh().v.my_ratings = r;
        }
        ctx();
    });
}

FabView fab_view()
{
    std::lock_guard<std::mutex> l(sh().mu);
    return sh().v;
}

void fab_refresh_account() { calls().post([] { ctx(); }); }

void fab_store_query(const FabQuery &q)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        sh().v.query = q;
        sh().v.store_asked = true;
        sh().v.store_loading = true;
        sh().v.store_error.clear();
        gen = ++sh().store_gen;
    }
    calls().post([q, gen] {
        {
            std::lock_guard<std::mutex> l(sh().mu);
            if (gen != sh().store_gen) return;   // a newer search replaced it
        }
        Ctx c = ctx();
        HttpResult r;
        if (c.server) {
            std::string path = std::string(kApi) + "/mods?sort=" + url_encode(q.sort) + "&page=" + std::to_string(q.page) +
                               "&per_page=" + std::to_string(kFabPerPage);
            if (!q.search.empty()) path += "&search=" + url_encode(q.search);
            if (!q.game.empty()) path += "&game=" + url_encode(q.game);
            if (!q.type.empty()) path += "&type=" + url_encode(q.type);
            r = fab_call(*c.server, "GET", path, "", nullptr);
        }
        std::lock_guard<std::mutex> l(sh().mu);
        if (gen != sh().store_gen) return;
        sh().v.store_loading = false;
        int total = 0;
        auto list = r.status == 200 ? fab_parse_list(r.body, &total) : std::nullopt;
        if (!c.server) sh().v.store_error = "No Fabricator server is set (CogBox.ini [Online] Server).";
        else if (list) { sh().v.cards = *list; sh().v.total = total; }
        else sh().v.store_error = r.status == 200 ? "The server's answer couldn't be read." : net_problem(r);
    });
}

void fab_open_page(int64_t id)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        sh().v.page_id = id;
        sh().v.page_error.clear();
        if (!sh().v.page || sh().v.page->id != id) sh().v.page.reset();
        sh().v.page_loading = id != 0;
        gen = ++sh().page_gen;
    }
    if (!id) return;
    calls().post([id, gen] {
        Ctx c = ctx();
        HttpResult r;
        if (c.server) r = fab_call(*c.server, "GET", std::string(kApi) + "/mods/" + std::to_string(id), "", nullptr);
        std::lock_guard<std::mutex> l(sh().mu);
        if (gen != sh().page_gen) return;
        sh().v.page_loading = false;
        auto p = r.status == 200 ? fab_parse_page(r.body) : std::nullopt;
        if (p) sh().v.page = *p;
        else if (r.status == 404) sh().v.page_error = "This mod isn't in the store (any more).";
        else sh().v.page_error = !c.server ? "No Fabricator server is set." : r.status == 200 ? "The server's answer couldn't be read." : net_problem(r);
    });
}

void fab_load_library()
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        sh().v.library_loading = true;
        gen = ++sh().library_gen;
    }
    calls().post([gen] {
        {
            std::lock_guard<std::mutex> l(sh().mu);
            if (gen != sh().library_gen) return;
        }
        Ctx c = ctx();
        HttpResult r;
        if (c.server && c.acct) r = fab_call(*c.server, "GET", std::string(kApi) + "/library", "", &*c.acct);
        std::lock_guard<std::mutex> l(sh().mu);
        if (gen != sh().library_gen) return;
        sh().v.library_loading = false;
        if (!c.server || !c.acct) {   // signed out: the local mods only
            sh().v.library.clear();
            sh().v.library_loaded = true;
            sh().v.library_error.clear();
            return;
        }
        auto lib = r.status == 200 ? fab_parse_library(r.body) : std::nullopt;
        if (lib) { sh().v.library = *lib; sh().v.library_loaded = true; sh().v.library_error.clear(); }
        else sh().v.library_error = r.status == 200 ? "The server's answer couldn't be read." : net_problem(r);
    });
}

void fab_load_creator()
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        sh().v.creator_loading = true;
        gen = ++sh().creator_gen;
    }
    calls().post([gen] {
        Ctx c = ctx();
        HttpResult mods, lim;
        if (c.server && c.acct) {
            mods = fab_call(*c.server, "GET", std::string(kApi) + "/creator/mods", "", &*c.acct);
            lim = fab_call(*c.server, "GET", std::string(kApi) + "/creator/limits", "", &*c.acct);
        }
        std::vector<FabCreatorMod> list;
        FabLimits limits;
        json j = json::parse(mods.body, nullptr, false);
        const bool modsOk = mods.status == 200 && j.is_object() && j["mods"].is_array();
        if (modsOk)
            for (const json &m : j["mods"]) {
                if (!m.is_object()) continue;
                FabCreatorMod cm;
                cm.id = m.value("id", (int64_t)0);
                cm.name = m.value("name", "");
                cm.game = m.value("game", "");
                cm.state = m.value("state", "");
                cm.downloads = m.value("downloads", 0);
                cm.ratings = m.value("ratings", 0);
                cm.reports_open = m.value("reports_open", 0);
                cm.rating = m.value("rating", 0.0);
                if (m.contains("versions") && m["versions"].is_array())
                    for (const json &v : m["versions"]) {
                        if (!v.is_object()) continue;
                        FabCreatorVersion cv;
                        cv.id = v.value("id", (int64_t)0);
                        cv.version = v.value("version", "");
                        cv.state = v.value("state", "");
                        cv.created_at = v.value("created_at", (int64_t)0);
                        cv.size = v.value("size", (int64_t)0);
                        cm.versions.push_back(cv);
                    }
                list.push_back(cm);
            }
        json k = json::parse(lim.body, nullptr, false);
        if (lim.status == 200 && k.is_object()) {
            limits.known = true;
            limits.can_publish = k.contains("can_publish") && k["can_publish"].is_string() ? k["can_publish"].get<std::string>() : "";
            limits.bytes_used = k.value("bytes_used", (int64_t)0);
            limits.quota = k.value("quota", (int64_t)0);
            limits.max_file_bytes = k.value("max_file_bytes", (int64_t)0);
            limits.uploads_today = k.value("uploads_today", 0);
            limits.max_uploads_per_day = k.value("max_uploads_per_day", 0);
        }
        std::lock_guard<std::mutex> l(sh().mu);
        if (gen != sh().creator_gen) return;
        sh().v.creator_loading = false;
        if (!c.server || !c.acct) { sh().v.creator.clear(); sh().v.creator_loaded = true; return; }
        if (modsOk) { sh().v.creator = list; sh().v.creator_loaded = true; sh().v.creator_error.clear(); }
        else sh().v.creator_error = net_problem(mods);
        sh().v.limits = limits;
    });
}

void fab_rate(int64_t id, int stars)
{
    calls().post([id, stars] {
        Ctx c = ctx();
        if (!c.server || !c.acct) { say("Sign in on the SOCIAL tab to get mods", true); return; }
        const HttpResult r = fab_call(*c.server, "POST", std::string(kApi) + "/mods/" + std::to_string(id) + "/rating",
                                      json{ { "stars", stars } }.dump(), &*c.acct);
        if (r.status == 204 || r.status == 200) {
            save_rating(c.server_url, id, stars);
            {
                std::lock_guard<std::mutex> l(sh().mu);
                sh().v.my_ratings[id] = stars;
            }
            say("Thanks - your rating is saved.", false);
            int64_t open;
            {
                std::lock_guard<std::mutex> l(sh().mu);
                open = sh().v.page_id;
            }
            if (open == id) fab_open_page(id);   // the page's average again
        } else if (r.status == 403) say("Get the mod first to rate it.", true);
        else say(net_problem(r), true);
    });
}

void fab_report(int64_t id, const std::string &reason, const std::string &text)
{
    calls().post([id, reason, text] {
        Ctx c = ctx();
        if (!c.server || !c.acct) { say("Sign in on the SOCIAL tab to report a mod.", true); return; }
        const HttpResult r = fab_call(*c.server, "POST", std::string(kApi) + "/mods/" + std::to_string(id) + "/report",
                                      json{ { "reason", reason }, { "text", text } }.dump(), &*c.acct);
        if (r.status == 204 || r.status == 200) say("Thanks - the moderators will look at your report.", false);
        else if (r.status == 409) say("You have already reported this mod.", true);
        else say(net_problem(r), true);
    });
}

void fab_remove_own(int64_t id)
{
    calls().post([id] {
        Ctx c = ctx();
        if (!c.server || !c.acct) return;
        const HttpResult r = fab_call(*c.server, "DELETE", std::string(kApi) + "/creator/mods/" + std::to_string(id), "", &*c.acct);
        if (r.status == 204 || r.status == 200) say("The mod is removed from the store.", false);
        else say(net_problem(r), true);
        fab_load_creator();
    });
}

void fab_get(const FabLibraryItem &item, int game, bool update)
{
    auto t = std::make_shared<FabTransfer>();
    t->kind = FabTransfer::Download;
    t->mod_id = item.id;
    t->name = item.name;
    t->game = game;
    t->update = update;
    t->item = item;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        for (auto &o : sh().v.transfers)
            if (o->kind == FabTransfer::Download && o->mod_id == item.id && active(o->phase)) return;   // already on its way
        sh().v.transfers.push_back(t);
    }
    downloads().post([t] { run_get(t); });
}

void fab_publish(const FabPublish &p)
{
    auto t = std::make_shared<FabTransfer>();
    t->kind = FabTransfer::Upload;
    t->mod_id = p.mod_id;
    t->name = p.name;
    t->game = game_of_key(p.game_key);
    {
        std::lock_guard<std::mutex> l(sh().mu);
        for (auto &o : sh().v.transfers)
            if (o->kind == FabTransfer::Upload && active(o->phase)) return;   // one publish at a time
        sh().v.transfers.push_back(t);
    }
    uploads().post([t, p] { run_publish(t, p); });
}

void fab_cancel(const std::shared_ptr<FabTransfer> &t)
{
    if (!t) return;
    t->progress.cancel = true;
    // not started yet, or waiting for its install: nothing more runs for it
    int q = FabTransfer::Queued;
    if (t->phase.compare_exchange_strong(q, FabTransfer::Cancelled)) return;
    int d = FabTransfer::Downloaded;
    if (t->phase.compare_exchange_strong(d, FabTransfer::Cancelled)) { DeleteFileW(widen(t->zip).c_str()); return; }
    int u = FabTransfer::Unpacked;   // unpacked, waiting for the game to close: the .partial goes (on the install worker)
    if (t->phase.compare_exchange_strong(u, FabTransfer::Cancelled))
        installs().post([t] { fab_install_abandon(t->plan); DeleteFileW(widen(t->zip).c_str()); });
}

void fab_discard(const std::shared_ptr<FabTransfer> &t)
{
    if (!t || t->kind != FabTransfer::Upload || active(t->phase) || !t->resumable) return;
    t->resumable = false;
    uploads().post([t] {
        Ctx c = ctx();
        if (c.server && c.acct && !t->zip.empty()) fab_upload_discard(*c.server, *c.acct, t->zip);
        if (t->zip_made) fab_pack_drop(t->zip);
    });
}

void fab_dismiss(const std::shared_ptr<FabTransfer> &t)
{
    std::lock_guard<std::mutex> l(sh().mu);
    auto &v = sh().v.transfers;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == t && !active(t->phase)) { v.erase(v.begin() + (long)i); return; }
}

int fab_transfer_percent()
{
    std::lock_guard<std::mutex> l(sh().mu);
    for (auto &t : sh().v.transfers) {
        const int ph = t->phase;
        if (!active(ph)) continue;
        if (ph == FabTransfer::Downloaded || ph == FabTransfer::Installing || ph == FabTransfer::Unpacked) return 100;
        const int64_t total = t->progress.total, done = t->progress.done;
        if ((ph == FabTransfer::Downloading || ph == FabTransfer::Uploading || ph == FabTransfer::Packing) && total > 0)
            return (int)std::min<int64_t>(100, done * 100 / total);
        return 0;
    }
    return -1;
}

uint64_t fab_local_changes()
{
    std::lock_guard<std::mutex> l(sh().mu);
    return sh().local_changes;
}

namespace {
bool store_image_path(const std::string &p)   // /api/v1/fabricator/images/<64 hex>.png and nothing else
{
    const size_t n = strlen(kImages);
    if (p.size() != n + 64 + 4 || p.compare(0, n, kImages) != 0 || p.compare(n + 64, 4, ".png") != 0) return false;
    for (size_t i = n; i < n + 64; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}
SDL_Texture *image(App &a, const std::string &key, const std::string &source, bool local, int *w, int *h)
{
    std::lock_guard<std::mutex> l(sh().mu);
    auto it = sh().imgs.find(key);
    if (it == sh().imgs.end()) {
        while (sh().imgs.size() >= kMaxImages) {   // the least used one goes - never one drawn this frame
            auto old = sh().imgs.end();
            for (auto j = sh().imgs.begin(); j != sh().imgs.end(); ++j)
                if (j->second.used_frame != sh().frame && (old == sh().imgs.end() || j->second.used < old->second.used)) old = j;
            if (old == sh().imgs.end()) break;   // all on screen: over the cap for now
            if (old->second.tex) SDL_DestroyTexture(old->second.tex);
            sh().imgs.erase(old);
        }
        Img &m = sh().imgs[key];
        m.used = ++sh().img_clock;
        m.used_frame = sh().frame;
        pictures().post([key, source, local] { run_image(key, source, local); });
        return nullptr;
    }
    Img &m = it->second;
    m.used = ++sh().img_clock;
    m.used_frame = sh().frame;
    if (m.state == 1) {
        m.tex = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, m.w, m.h);
        if (m.tex) {
            SDL_UpdateTexture(m.tex, nullptr, m.px.data(), m.w * 4);
            SDL_SetTextureScaleMode(m.tex, SDL_SCALEMODE_LINEAR);
        }
        m.px.clear();
        m.px.shrink_to_fit();
        m.state = m.tex ? 2 : 3;
    }
    if (m.state != 2) return nullptr;
    if (w) *w = m.w;
    if (h) *h = m.h;
    return m.tex;
}
}  // namespace

SDL_Texture *fab_image(App &a, const std::string &path, int *w, int *h)
{
    if (!store_image_path(path)) return nullptr;   // anything else from the server is ignored (the placeholder shows)
    return image(a, "s:" + path, path, false, w, h);
}

SDL_Texture *fab_local_image(App &a, const std::string &file, int *w, int *h)
{
    if (file.empty()) return nullptr;
    return image(a, "l:" + file, file, true, w, h);
}

void fab_images_free()
{
    pictures().clear();
    std::lock_guard<std::mutex> l(sh().mu);
    for (auto &kv : sh().imgs) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    sh().imgs.clear();
}

void fab_state_poll(App &a)
{
    std::vector<std::pair<std::string, bool>> msgs;
    std::vector<std::shared_ptr<FabTransfer>> ts;
    {
        std::lock_guard<std::mutex> l(sh().mu);
        msgs.swap(sh().messages);
        ts = sh().v.transfers;
    }
    for (auto &m : msgs) set_message(a, m.first, m.second);
    {
        std::lock_guard<std::mutex> l(sh().mu);
        sh().frame++;
    }
    // Gets: the folder chosen here, the check and unpack on the install worker, the moves here. One install at a time
    // (two Gets of mods with the same name would otherwise choose the same folder).
    bool installing = false;
    for (auto &t : ts)
        if (t->kind == FabTransfer::Download && (t->phase == FabTransfer::Installing || t->phase == FabTransfer::Unpacked)) installing = true;
    for (auto &t : ts) {
        if (t->kind != FabTransfer::Download) continue;
        const int ph = t->phase;
        if (ph == FabTransfer::Downloaded) {
            if (a.run.proc) {   // the game reads the mods folder: it installs once the game has closed
                if (!t->told_close) { t->told_close = true; set_message(a, "Close the game first.", true); }
                continue;
            }
            if (installing) continue;
            const int g = game_of_key(t->item.game);
            std::string e = g < 0 ? "This mod is for a game COGBox doesn't know." : g != t->game && t->game >= 0 ? "This mod is for another game." : "";
            FabInstallPlan plan;
            if (e.empty()) e = fab_install_prepare(a, g, t->item, t->zip, &plan);
            if (e == "Close the game first.") continue;
            if (!e.empty()) {
                DeleteFileW(widen(t->zip).c_str());
                fail(t, { e });
                continue;
            }
            t->plan = plan;   // (read by the worker after the post)
            int d = FabTransfer::Downloaded;
            if (!t->phase.compare_exchange_strong(d, FabTransfer::Installing)) continue;   // cancelled this frame
            installing = true;
            installs().post([t] { run_unpack(t); });
        } else if (ph == FabTransfer::Unpacked) {
            if (a.run.proc) {   // a game started during the unpack: the move waits for it to close
                if (!t->told_close) { t->told_close = true; set_message(a, "Close the game first.", true); }
                continue;
            }
            const std::string e = fab_install_commit(a, t->plan);
            if (e == "Close the game first.") continue;
            DeleteFileW(widen(t->zip).c_str());
            {
                std::lock_guard<std::mutex> l(sh().mu);
                sh().local_changes++;
            }
            if (e.empty()) {
                env_refresh(a, t->plan.g);
                t->phase = FabTransfer::Done;
                set_message(a, t->name + (t->plan.update ? " is updated." : " is installed and on."), false);
            } else fail(t, { e });   // (it says why)
        }
    }
    // the pictures only live while the screen is open
    if (a.screen != SCR_FABRICATOR && a.zoomTo != SCR_FABRICATOR) {
        bool any;
        {
            std::lock_guard<std::mutex> l(sh().mu);
            any = !sh().imgs.empty();
        }
        if (any) fab_images_free();
    }
}

void fab_state_shutdown()
{
    {
        std::lock_guard<std::mutex> l(sh().mu);
        for (auto &t : sh().v.transfers) t->progress.cancel = true;   // a download or upload goes on from here next time
    }
    calls().clear();
    pictures().clear();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until &&
           !(calls().idle() && pictures().idle() && downloads().idle() && uploads().idle() && installs().idle()))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fab_images_free();
}

void fab_mark_local_change()
{
    std::lock_guard<std::mutex> l(sh().mu);
    sh().local_changes++;
}

FabModIni fab_read_mod_ini(const std::string &source, bool folder)
{
    FabModIni m;
    std::string ini;
    if (folder) {
        if (!read_file(join(source, "mod.ini"), ini, 256 * 1024)) {
            // a folder holding the mod's own folder: one level down
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW(widen(join(source, "*")).c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    const std::string n = narrow(fd.cFileName);
                    if (n == "." || n == ".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (read_file(join(join(source, n), "mod.ini"), ini, 256 * 1024)) break;
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
        }
        if (!ini.empty()) parse_mod_ini(ini, m);
        return m;
    }
    FILE *f = _wfopen(widen(source).c_str(), L"rb");
    if (!f) return m;
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (mz_zip_reader_init_cfile(&z, f, 0, 0)) {
        int best = -1, bestDepth = 99;
        const mz_uint n = mz_zip_reader_get_num_files(&z);
        for (mz_uint i = 0; i < n; i++) {
            char name[512];
            if (!mz_zip_reader_get_filename(&z, i, name, sizeof name)) continue;
            std::string low = lower_s(name);
            for (char &c : low) if (c == '\\') c = '/';
            const int depth = low == "mod.ini" ? 0 : (low.size() > 8 && low.compare(low.size() - 8, 8, "/mod.ini") == 0 && low.find('/') == low.size() - 8) ? 1 : 99;
            if (depth < bestDepth) { bestDepth = depth; best = (int)i; }
        }
        mz_zip_archive_file_stat st;
        if (best >= 0 && mz_zip_reader_file_stat(&z, (mz_uint)best, &st) && st.m_uncomp_size <= 256 * 1024) {
            size_t len = 0;
            if (void *p = mz_zip_reader_extract_to_heap(&z, (mz_uint)best, &len, 0)) {
                ini.assign((const char *)p, len);
                mz_free(p);
            }
        }
        mz_zip_reader_end(&z);
    }
    fclose(f);
    if (!ini.empty()) parse_mod_ini(ini, m);
    return m;
}
