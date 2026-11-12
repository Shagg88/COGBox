// COGBox - updates (adapted from Athelas' update.cpp, same author). 0.5.0: from GitLab first (releases API + generic
// package registry), SourceForge as the fallback while it exists; GitHub code kept, unused
// (the sources and their order: update_logic.h kSources - the one place to change them).
//
// Checking. SourceForge: the project's files RSS (rss?path=/) and best_release.json are read and every COGBox-X.Y.Z.7z /
// .zip named in them counts (update_logic.cpp sf_parse_listing); the newest wins, .7z over .zip. Both are read because
// best_release.json only names the file marked "default", which can lag (or be README.md). GitHub: /releases/latest
// answers with a redirect to /releases/tag/<version>; the version is read from that address (no API, no key).
// The first source that names a release is used.
//
// Downloading. Redirects are followed by hand (up to 12 hops, https only, logged): SourceForge's files/.../download sends
// a program (not a browser) a 302 to downloads.sourceforge.net, which sends a 302 to a mirror (<name>.dl.sourceforge.net).
// If a page comes back instead of the file (SourceForge's "your download will start shortly" page), its meta refresh /
// mirror link is followed. The file is streamed next to the exe while its SHA-256 (and MD5) are worked out, then it must
// start like a 7z / zip and match the release's .sha256 file (SourceForge: from the same folder; GitHub: the release asset,
// or the notes' "SHA-256: ..."). Without a .sha256, SourceForge's own MD5 from the listing is used; with neither, or on a
// mismatch, nothing is installed. A second address / mirror is tried once before giving up.
//
// Installing: the .7z (LZMA SDK, archive7z.cpp) or .zip (miniz) is unpacked over this folder. A running program can be
// renamed but not overwritten, so each file replaced is first renamed to <name>.old-cogbox (removed at the next start); if
// anything fails half way, the files already replaced are put back. (Old program files a release no longer ships are moved
// to _old-files\ by stale_files.cpp at the next start, not here.)
//
// Each check / install writes what it did to logs\update.txt. CogBox.exe --check-update-only <log> does the check plus a
// dry run of the download (the .sha256 and the first bytes of the archive through the same redirects) and installs nothing.
#include "update.h"
#include "update_logic.h"
#include "archive7z.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <vector>
#include <algorithm>

#include "../third_party/miniz/miniz.h"
#include "app.h"

using namespace cogbox_update;

static volatile LONG g_started = 0, g_done = 0;
static std::string g_current, g_latest;
static int g_foundSource = -1;   // kSources index the latest release came from
static SfFile g_found;           // its file (SourceForge), or version + ext (GitHub)

static CRITICAL_SECTION *lock_()
{
    static CRITICAL_SECTION cs;
    static LONG init = 0;
    if (InterlockedCompareExchange(&init, 1, 0) == 0) { InitializeCriticalSection(&cs); InterlockedExchange(&init, 2); }
    while (init != 2) Sleep(0);
    return &cs;
}
struct Locked { Locked() { EnterCriticalSection(lock_()); } ~Locked() { LeaveCriticalSection(lock_()); } };

// ---- log -------------------------------------------------------------------------------------------------------------
static std::string g_log;
static void ulog(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[32];
    snprintf(ts, sizeof ts, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    Locked l;
    g_log += ts;
    g_log += buf;
    g_log += "\r\n";
}

static std::wstring exe_dir_here()
{
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(NULL, &p[0], (DWORD)p.size());
    p.resize(n);
    return p.substr(0, p.find_last_of(L'\\'));
}

static std::wstring wide(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string utf8(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static void write_log_file(const std::wstring &path)
{
    std::string text;
    { Locked l; text = g_log; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &w, NULL);
    CloseHandle(f);
}
static void save_log()
{
    std::wstring dir = exe_dir_here() + L"\\logs";
    CreateDirectoryW(dir.c_str(), NULL);
    write_log_file(dir + L"\\update.txt");
}

const char *update_page()
{
    static std::string page = source_page(kSources[0]);
    return page.c_str();
}

// ---- hashes (Windows' BCrypt) ------------------------------------------------------------------------------------------
struct Hash {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    ULONG len;
    Hash(LPCWSTR id, ULONG n) : len(n) { if (BCryptOpenAlgorithmProvider(&alg, id, NULL, 0) == 0) BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0); }
    ~Hash() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void *p, size_t n) { if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex()
    {
        unsigned char d[64];
        std::string out;
        if (!h || BCryptFinishHash(h, d, len, 0) != 0) return "";
        for (ULONG i = 0; i < len; i++) { out += "0123456789abcdef"[d[i] >> 4]; out += "0123456789abcdef"[d[i] & 15]; }
        return out;
    }
};

// ---- HTTP: any https address, redirects (and SourceForge's download page) followed by hand -------------------------
static volatile LONG g_ipermille = 0;

struct Fetch {
    // in
    HANDLE file = NULL;          // stream the body here (else into body)
    size_t maxBody = 4 << 20;    // to memory: at most this much
    unsigned long long maxFile = 4ull << 30;   // 0.6.1 (review R1): to a file: at most this much (a release is well under 1 GB)
    bool followPages = false;    // an HTML page instead of the file: follow its refresh / mirror link
    bool rangeHead = false;      // only the first 64 bytes (dry run)
    Hash *sha = nullptr, *md5 = nullptr;
    // out
    std::string body, head, finalUrl, contentType;
    DWORD status = 0;
    int hops = 0;
};

static std::string query_header(HINTERNET r, DWORD which)
{
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (!WinHttpQueryHeaders(r, which, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX)) return "";
    return utf8(std::wstring(buf, size / sizeof(wchar_t)));
}

static bool fetch(std::string url, Fetch &f)
{
    static const std::wstring agent = L"COGBox-launcher/" + wide(COGBOX_VERSION) + L" (Windows; updater)";   // not a browser: SourceForge answers with plain redirects
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // (pre-8.1 Windows)
    if (!s) { ulog("  WinHttpOpen failed (%lu)", GetLastError()); return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    bool ok = false;
    for (f.hops = 0; f.hops < 12; f.hops++) {
        std::string host, path;
        int port = 443;
        if (!split_https_url(url, &host, &port, &path)) { ulog("  refused (not https): %s", url.c_str()); break; }
        HINTERNET c = WinHttpConnect(s, wide(host).c_str(), (INTERNET_PORT)port, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", wide(path).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
        if (!r) { ulog("  GET %s: no connection (%lu)", url.c_str(), GetLastError()); if (c) WinHttpCloseHandle(c); break; }
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        std::wstring hdr = L"Accept: */*\r\n";
        if (f.rangeHead) hdr += L"Range: bytes=0-63\r\n";
        bool sent = WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL);
        if (!sent) {
            ulog("  GET %s: no answer (%lu)", url.c_str(), GetLastError());
            WinHttpCloseHandle(r); WinHttpCloseHandle(c);
            break;
        }
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        f.status = status;
        f.contentType = query_header(r, WINHTTP_QUERY_CONTENT_TYPE);
        std::string lenText = query_header(r, WINHTTP_QUERY_CONTENT_LENGTH);
        unsigned long long total = lenText.empty() ? 0 : strtoull(lenText.c_str(), NULL, 10);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::string loc = query_header(r, WINHTTP_QUERY_LOCATION);
            ulog("  GET %s -> %lu %s", url.c_str(), status, loc.c_str());
            WinHttpCloseHandle(r); WinHttpCloseHandle(c);
            if (loc.empty()) break;
            url = resolve_location(url, loc);
            continue;
        }
        ulog("  GET %s -> %lu %s%s%s", url.c_str(), status, f.contentType.c_str(), lenText.empty() ? "" : ", bytes ", lenText.c_str());
        if (status != 200 && status != 206) { WinHttpCloseHandle(r); WinHttpCloseHandle(c); break; }
        // read: the first bytes decide whether this is a page to go on from
        std::vector<char> buf(1 << 20);
        std::string page;
        bool isPage = false, readOk = true;
        unsigned long long have = 0;
        f.head.clear();
        f.body.clear();
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) { readOk = false; break; }
            if (!got) break;
            if (have == 0) {
                f.head.assign(buf.data(), std::min<DWORD>(got, 512));
                isPage = f.followPages && (looks_like_html(f.head) || f.contentType.find("text/html") != std::string::npos);
            }
            have += got;
            if (isPage) {
                page.append(buf.data(), got);
                if (page.size() > (1u << 20)) break;
                continue;
            }
            if (f.rangeHead) { f.body.append(buf.data(), got); if (f.body.size() >= 64) break; continue; }
            if (f.file) {
                DWORD wrote = 0;
                if (have > f.maxFile) { readOk = false; ulog("  download too big (over %llu MB) - stopped", f.maxFile >> 20); break; }
                if (!WriteFile(f.file, buf.data(), got, &wrote, NULL) || wrote != got) { readOk = false; ulog("  write failed (disk full?)"); break; }
            } else {
                f.body.append(buf.data(), got);
                if (f.body.size() > f.maxBody) { readOk = false; ulog("  answer too big"); break; }
            }
            if (f.sha) f.sha->add(buf.data(), got);
            if (f.md5) f.md5->add(buf.data(), got);
            if (f.file && total) InterlockedExchange(&g_ipermille, (LONG)(have * 1000ull / total));
        }
        WinHttpCloseHandle(r); WinHttpCloseHandle(c);
        if (isPage) {
            std::string next = html_next_url(page, url);
            ulog("  (a web page, not the file) -> %s", next.empty() ? "no link to go on with" : next.c_str());
            if (next.empty()) break;
            if (f.file) { SetFilePointer(f.file, 0, NULL, FILE_BEGIN); SetEndOfFile(f.file); }
            url = next;
            continue;
        }
        if (!readOk) break;
        if (!f.rangeHead && total && have != total) { ulog("  cut short: %llu of %llu bytes", have, total); break; }
        f.finalUrl = url;
        ok = true;
        break;
    }
    if (f.hops >= 12) ulog("  too many redirects");
    WinHttpCloseHandle(s);
    return ok;
}

// ---- checking ----------------------------------------------------------------------------------------------------------
static bool check_sourceforge(const Source &src, SfFile *out)
{
    std::vector<SfFile> all;
    for (const std::string &url : { sf_rss_url(src.project), sf_best_release_url(src.project) }) {
        Fetch f;
        if (!fetch(url, f)) continue;
        auto files = sf_parse_listing(f.body);
        ulog("  %u release file(s) named:", (unsigned)files.size());
        for (auto &x : files) ulog("    %s (version %s, md5 %s)", x.path.c_str(), x.version.c_str(), x.md5.empty() ? "-" : x.md5.c_str());
        all.insert(all.end(), files.begin(), files.end());
    }
    return sf_pick_latest(all, out);
}

static bool check_github(const Source &src, SfFile *out)
{
    // HEAD /releases/latest, not followed: the Location names the tag (unchanged from 0.4.8, so it works if the account returns)
    std::string found;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) {
        WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
        HINTERNET c = WinHttpConnect(s, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"HEAD", (wide(src.project) + L"/releases/latest").c_str(), NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
        if (r) {
            DWORD off = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
                DWORD status = 0, size = sizeof status;
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                std::string loc = query_header(r, WINHTTP_QUERY_LOCATION);
                ulog("  HEAD https://github.com%s/releases/latest -> %lu %s", src.project, status, loc.c_str());
                size_t t = loc.find("/releases/tag/");
                if (t != std::string::npos)
                    for (t += 14; t < loc.size() && loc[t] != '/' && loc[t] != '?' && found.size() < 32; t++) found += loc[t];
            } else ulog("  HEAD https://github.com%s/releases/latest: no answer (%lu)", src.project, GetLastError());
            WinHttpCloseHandle(r);
        }
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
    }
    if (found.empty() || !isdigit((unsigned char)(found[0] == 'v' || found[0] == 'V' ? found.c_str()[1] : found[0]))) return false;
    out->version = found[0] == 'v' || found[0] == 'V' ? found.substr(1) : found;
    out->path = found;   // the tag as written
    out->ext.clear();    // GitHub: .7z tried first, then .zip
    out->md5.clear();
    return true;
}

// 0.5.0: GitLab - the API's latest-release permalink (public project, no key): {"tag_name":"0.5.1", ...}
static std::string gitlab_api(const Source &src)
{
    std::string p = src.project;
    std::string enc;
    for (char ch : p) { if (ch == '/') enc += "%2F"; else enc += ch; }
    return "https://gitlab.com/api/v4/projects/" + enc;
}
static bool check_gitlab(const Source &src, SfFile *out)
{
    Fetch g;
    g.maxBody = 256 << 10;
    if (!fetch(gitlab_api(src) + "/releases/permalink/latest", g)) { ulog("  GitLab: no answer"); return false; }
    const size_t k = g.body.find("\"tag_name\"");
    if (k == std::string::npos) { ulog("  GitLab: no tag_name in the answer"); return false; }
    const size_t a = g.body.find('"', g.body.find(':', k) + 1);
    const size_t b = a == std::string::npos ? std::string::npos : g.body.find('"', a + 1);
    if (b == std::string::npos) return false;
    std::string tag = g.body.substr(a + 1, b - a - 1);
    const std::string ver = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    bool shape = !ver.empty() && isdigit((unsigned char)ver.back());   // 0.5.0 review: digits separated by single dots
    for (size_t i = 0; shape && i < ver.size(); ++i)
        shape = isdigit((unsigned char)ver[i]) || (ver[i] == '.' && i > 0 && ver[i - 1] != '.');
    if (!shape) {
        ulog("  GitLab: tag '%s' is not a version", tag.c_str());
        return false;
    }
    out->version = ver;
    out->path = tag;
    out->ext.clear();   // .7z first, then .zip
    out->md5.clear();
    return true;
}

static void run_check()
{
    ulog("COGBox %s: checking for updates", g_current.c_str());
    int src = -1;
    SfFile found;
    for (int i = 0; i < kSourceCount && src < 0; i++) {
        ulog("source %d: %s (%s)", i + 1, kSources[i].name, source_page(kSources[i]).c_str());
        SfFile f;
        bool ok = kSources[i].kind == SourceKind::SourceForge ? check_sourceforge(kSources[i], &f)
                : kSources[i].kind == SourceKind::GitLab    ? check_gitlab(kSources[i], &f) : check_github(kSources[i], &f);
        if (ok) { src = i; found = f; ulog("  latest there: %s", f.version.c_str()); }
        else ulog("  no release found there");
    }
    bool isNewer = src >= 0 && version_newer(found.version, g_current);
    {
        Locked l;
        g_foundSource = src;
        g_found = found;
        g_latest = isNewer ? found.version : "";
    }
    if (src < 0) ulog("RESULT: no source answered - current %s, no update", g_current.c_str());
    else ulog("RESULT: latest %s from %s, current %s -> %s", found.version.c_str(), kSources[src].name, g_current.c_str(),
              isNewer ? "UPDATE AVAILABLE" : "no update");
}

static DWORD WINAPI check(void *)
{
    run_check();
    save_log();
    InterlockedExchange(&g_done, 1);
    return 0;
}

void update_check_start(const std::string &current)
{
    if (InterlockedExchange(&g_started, 1)) return;
    g_current = current;
    HANDLE h = CreateThread(NULL, 0, check, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_done, 1);
}

std::string update_newer()
{
    if (!g_done) return "";
    Locked l;
    return g_latest;
}

bool update_checked() { return g_done != 0; }

// ---- where the release's files are -----------------------------------------------------------------------------------
struct Candidate { std::vector<std::string> urls; std::vector<std::string> sumUrls; std::string ext, md5; };

static std::vector<Candidate> candidates(int src, const SfFile &f)
{
    std::vector<Candidate> out;
    if (src < 0) return out;
    const Source &s = kSources[src];
    if (s.kind == SourceKind::SourceForge) {
        Candidate c;
        c.urls = sf_download_urls(s.project, f.path);
        c.sumUrls = sf_download_urls(s.project, f.path + ".sha256");
        c.ext = f.ext;
        c.md5 = f.md5;
        out.push_back(c);
    } else if (s.kind == SourceKind::GitLab) {
        for (const char *ext : { ".7z", ".zip" }) {
            Candidate c;
            const std::string base = gitlab_api(s) + "/packages/generic/cogbox/" + f.version + "/COGBox-" + f.version + ext;
            c.urls = { base };
            c.sumUrls = { base + ".sha256" };
            c.ext = ext;
            out.push_back(c);
        }
    } else {
        for (const char *ext : { ".7z", ".zip" }) {
            Candidate c;
            std::string base = "https://github.com" + std::string(s.project) + "/releases/download/" + f.path + "/COGBox-" + f.version + ext;
            c.urls = { base };
            c.sumUrls = { base + ".sha256" };
            c.ext = ext;
            out.push_back(c);
        }
    }
    return out;
}

// the .sha256 next to the file (or, on GitHub, "SHA-256: ..." in the release notes)
static std::string find_sha256(int src, const SfFile &f, const Candidate &c)
{
    for (auto &u : c.sumUrls) {
        Fetch g;
        g.followPages = true;
        g.maxBody = 64 << 10;
        if (fetch(u, g)) {
            std::string h = parse_sha256_text(g.body);
            if (!h.empty()) return h;
            ulog("  no SHA-256 in that answer");
        }
    }
    if (kSources[src].kind == SourceKind::GitHub) {
        Fetch g;
        if (fetch("https://github.com" + std::string(kSources[src].project) + "/releases/tag/" + f.path, g)) {
            for (size_t at = g.body.find("SHA-256"); at != std::string::npos; at = g.body.find("SHA-256", at + 7)) {
                std::string h = parse_sha256_text(g.body.substr(at, 400));
                if (!h.empty()) return h;
            }
        }
    }
    return "";
}

// ---- installing ----------------------------------------------------------------------------------------------------
static volatile LONG g_istate = 0;   // 0 idle, 1 downloading, 2 unpacking, 3 done, -1 failed
static std::string g_ierror;
static std::wstring g_exe_dir;

static void make_dirs_for(const std::wstring &file)
{
    for (size_t i = g_exe_dir.size() + 1; (i = file.find(L'\\', i)) != std::wstring::npos; i++) CreateDirectoryW(file.substr(0, i).c_str(), NULL);
}

static bool fail(const std::string &why)
{
    ulog("FAILED: %s", why.c_str());
    g_ierror = why;
    InterlockedExchange(&g_istate, -1);
    return false;
}

// The player's own things are never touched by an update, even if an archive held them by mistake (cleanup049: the
// list is update_logic.cpp update_protected_path, so test/upgrade_cleanup_test.cpp checks it on Linux).
static bool protected_path(const std::string &rel) { return update_protected_path(rel); }

// an opened archive, whichever kind: names, folders, and one file's bytes at a time
struct Entries {
    std::vector<std::string> names;
    std::vector<bool> dirs;
    std::function<bool(unsigned, const unsigned char **, size_t *)> get;
    std::function<void()> release;   // after each get
};

static bool unpack(const std::wstring &archive, const std::string &ext)
{
    Archive7z sz;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    bool zipOpen = false;
    void *zipBuf = NULL;
    Entries e;
    if (ext == ".7z") {
        if (!sz.open(utf8(archive))) return fail("The update couldn't be opened (" + sz.error() + ").");
        for (unsigned i = 0; i < sz.count(); i++) { e.names.push_back(sz.name(i)); e.dirs.push_back(sz.is_dir(i)); }
        e.get = [&](unsigned i, const unsigned char **d, size_t *n) {
            if (sz.extract(i, d, n)) return true;
            ulog("  7z: %s", sz.error().c_str());
            return false;
        };
        e.release = [] {};
    } else {
        // miniz opens files with fopen (the ANSI code page): a folder name it can't hold falls back to the short 8.3 path
        std::string zipA = utf8(archive);
        std::wstring shortPath(MAX_PATH * 2, L'\0');
        DWORD sn = GetShortPathNameW(archive.c_str(), &shortPath[0], (DWORD)shortPath.size());
        if (sn && sn < shortPath.size()) {
            shortPath.resize(sn);
            int n = WideCharToMultiByte(CP_ACP, 0, shortPath.c_str(), -1, NULL, 0, NULL, NULL);
            zipA.assign(n, '\0');
            WideCharToMultiByte(CP_ACP, 0, shortPath.c_str(), -1, &zipA[0], n, NULL, NULL);
            zipA.resize(strlen(zipA.c_str()));
        }
        if (!mz_zip_reader_init_file(&zip, zipA.c_str(), 0)) return fail("The update couldn't be opened.");
        zipOpen = true;
        mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; i++) {
            char n[1024];
            mz_zip_reader_get_filename(&zip, i, n, sizeof n);
            e.names.push_back(n);
            e.dirs.push_back(mz_zip_reader_is_file_a_directory(&zip, i) != 0);
        }
        e.get = [&](unsigned i, const unsigned char **d, size_t *n) {
            zipBuf = mz_zip_reader_extract_to_heap(&zip, i, n, 0);
            *d = (const unsigned char *)zipBuf;
            return zipBuf != NULL;
        };
        e.release = [&] { if (zipBuf) mz_free(zipBuf); zipBuf = NULL; };
    }
    struct Closer { std::function<void()> f; ~Closer() { f(); } } closer{ [&] { if (zipOpen) mz_zip_reader_end(&zip); sz.close(); } };

    const unsigned count = (unsigned)e.names.size();
    std::string top;
    bool one_top = true, has_root_exe = false;
    for (unsigned i = 0; i < count; i++) {
        std::string &nm = e.names[i];
        for (char &ch : nm) if (ch == '\\') ch = '/';
        if (e.dirs[i] && (nm.empty() || nm.back() != '/')) nm += '/';   // (7z names folders without the /)
        std::string first = nm.substr(0, nm.find('/') + 1);
        if (i == 0) top = first;
        if (first.empty() || first != top) one_top = false;
        if (nm == "CogBox.exe") has_root_exe = true;
    }
    size_t strip = one_top && !has_root_exe ? top.size() : 0;
    // an update must hold the launcher itself (review 12): otherwise nothing is replaced and it says so
    bool has_launcher = false;
    for (auto &nm : e.names) if (nm.size() >= strip && nm.substr(strip) == "CogBox.exe") has_launcher = true;
    if (!has_launcher) return fail("The update doesn't hold CogBox.exe - nothing was changed.");
    ulog("unpacking %u entries", count);

    struct Done { std::wstring target; bool had_old; };
    std::vector<Done> done;
    auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (it->had_old) MoveFileExW((it->target + L".old-cogbox").c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    bool ok = true;
    for (unsigned i = 0; i < count && ok; i++) {
        if (e.dirs[i]) continue;
        std::string rel = e.names[i].substr(std::min(strip, e.names[i].size()));
        if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) continue;   // P0-6: rooted with \ too
        for (char &ch : rel) if (ch == '/') ch = '\\';
        if (rel.back() == '\\') continue;   // 0.2.0: a folder entry
        if (protected_path(rel)) continue;
        std::wstring target = g_exe_dir + L"\\" + wide(rel), fresh = target + L".new-cogbox";
        // 0.6.1 (review R1): a second entry with the same name would replace the first one's .old-cogbox backup with
        // the file just installed, and the undo would then put the new file back instead of the old one.
        bool twice = false;
        for (const Done &d : done) if (_wcsicmp(d.target.c_str(), target.c_str()) == 0) { twice = true; break; }
        if (twice) { ulog("  skipped a second entry for %s", rel.c_str()); continue; }
        make_dirs_for(target);
        const unsigned char *p = NULL;
        size_t size = 0;
        if (!e.get(i, &p, &size)) { e.release(); ok = fail("The update couldn't be unpacked (" + rel + ")."); break; }
        HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        bool written = f != INVALID_HANDLE_VALUE;
        for (size_t at = 0; written && at < size;) {   // (WriteFile takes 32-bit sizes)
            DWORD part = (DWORD)std::min<size_t>(size - at, 1u << 30), wrote = 0;
            written = WriteFile(f, p + at, part, &wrote, NULL) && wrote == part;
            at += part;
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        e.release();
        if (!written) { DeleteFileW(fresh.c_str()); ok = fail("Couldn't write " + rel + " (is the disk full?)."); break; }
        bool had = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (had && !MoveFileExW(target.c_str(), (target + L".old-cogbox").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(fresh.c_str());
            ok = fail("Couldn't replace " + rel + " (is a game still running?).");
            break;
        }
        done.push_back({target, had});   // from here on the undo puts the old one back (review 2)
        if (!MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(fresh.c_str());
            ok = fail("Couldn't replace " + rel + ".");
            break;
        }
        InterlockedExchange(&g_ipermille, (LONG)((i + 1) * 1000ull / count));
    }
    if (!ok) { undo(); return false; }
    ulog("installed %u files", (unsigned)done.size());
    return true;
}

static bool install(const std::string &version)
{
    std::wstring probe = g_exe_dir + L"\\cogbox-update-test.tmp";
    HANDLE ph = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (ph == INVALID_HANDLE_VALUE) return fail("Windows doesn't let COGBox change its own folder. Download the update from its page instead.");
    CloseHandle(ph);

    int src;
    SfFile f;
    { Locked l; src = g_foundSource; f = g_found; }
    if (src < 0 || f.version != version) return fail("The update's files couldn't be found. Download it from its page instead.");
    ulog("installing COGBox %s from %s", version.c_str(), kSources[src].name);

    std::string lastError = "The update's files couldn't be found. Try again later, or download it from its page.";
    for (const Candidate &c : candidates(src, f)) {
        std::string sum = find_sha256(src, f, c);
        if (sum.empty() && c.md5.empty()) {
            ulog("  no .sha256 and no md5 for COGBox-%s%s", version.c_str(), c.ext.c_str());
            lastError = "The update's checksum couldn't be found. Try again later, or download it from its page.";
            continue;
        }
        ulog("  expecting %s %s", sum.empty() ? "md5" : "sha256", sum.empty() ? c.md5.c_str() : sum.c_str());
        std::wstring part = g_exe_dir + L"\\CogBox-update" + wide(c.ext) + L".part";
        for (const std::string &url : c.urls) {
            HANDLE zf = CreateFileW(part.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (zf == INVALID_HANDLE_VALUE) return fail("Couldn't make room for the download in the COGBox folder.");
            Hash sha(BCRYPT_SHA256_ALGORITHM, 32), md5(BCRYPT_MD5_ALGORITHM, 16);
            Fetch g;
            g.file = zf;
            g.followPages = true;
            g.sha = &sha;
            g.md5 = &md5;
            InterlockedExchange(&g_ipermille, 0);
            bool got = fetch(url, g);
            CloseHandle(zf);
            if (!got) { DeleteFileW(part.c_str()); lastError = "The download didn't finish. Check the internet connection (and the free disk space) and try again."; continue; }
            if (!archive_magic_ok(g.head, c.ext)) { DeleteFileW(part.c_str()); ulog("  not a %s file", c.ext.c_str()); lastError = "The download wasn't the update (a web page came instead). Try again later, or download it from its page."; continue; }
            std::string shaHex = sha.hex(), md5Hex = md5.hex();
            ulog("  downloaded: sha256 %s md5 %s", shaHex.c_str(), md5Hex.c_str());
            if (sum.empty() ? md5Hex != c.md5 : shaHex != sum) { DeleteFileW(part.c_str()); lastError = "The download was damaged (its checksum doesn't match). Try again."; continue; }
            InterlockedExchange(&g_istate, 2);
            InterlockedExchange(&g_ipermille, 0);
            bool ok = unpack(part, c.ext);
            DeleteFileW(part.c_str());
            if (!ok) return false;
            InterlockedExchange(&g_istate, 3);
            return true;
        }
    }
    return fail(lastError);
}

static std::string g_iversion;
static DWORD WINAPI install_thread(void *) { install(g_iversion); save_log(); return 0; }

void update_install_start(const std::string &version)
{
    if (g_istate == 1 || g_istate == 2 || g_istate == 3) return;
    g_exe_dir = exe_dir_here();
    g_iversion = version;
    g_ierror.clear();
    InterlockedExchange(&g_ipermille, 0);
    InterlockedExchange(&g_istate, 1);
    HANDLE h = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else fail("The update couldn't start.");
}

int update_install_state(float *progress, std::string *error)
{
    if (progress) *progress = g_ipermille / 1000.0f;
    if (error && g_istate == -1) *error = g_ierror;
    return (int)g_istate;
}

// ---- dry run: CogBox.exe --check-update-only <log> --------------------------------------------------------------------
int update_check_only(const std::string &current, const std::wstring &logPath)
{
    g_current = current;
    ulog("--check-update-only (dry run: nothing is downloaded in full, nothing installed)");
    run_check();
    int src;
    SfFile f;
    std::string latest;
    { Locked l; src = g_foundSource; f = g_found; latest = g_latest; }
    int rc = src < 0 ? 2 : latest.empty() ? 0 : 1;
    // the download path of what was found, without downloading it: the .sha256, then the archive's first 64 bytes
    for (const Candidate &c : candidates(src, f)) {
        ulog("dry run: COGBox-%s%s", f.version.c_str(), c.ext.c_str());
        std::string sum = find_sha256(src, f, c);
        ulog("  sha256 file: %s", sum.empty() ? "NOT FOUND" : sum.c_str());
        ulog("  md5 from the listing: %s", c.md5.empty() ? "-" : c.md5.c_str());
        bool reached = false;
        for (const std::string &url : c.urls) {
            Fetch g;
            g.rangeHead = true;
            g.followPages = true;
            bool got = fetch(url, g);
            bool magic = got && archive_magic_ok(g.body.empty() ? g.head : g.body, c.ext);
            ulog("  %s: %s after %d hop(s)%s%s", url.c_str(), got ? (magic ? "reached the archive" : "reached something that ISN'T the archive") : "FAILED",
                 g.hops, got ? ", served by " : "", got ? g.finalUrl.c_str() : "");
            if (magic) { reached = true; break; }
        }
        ulog("  verdict: %s", reached && (!sum.empty() || !c.md5.empty()) ? "an update from here would download and verify"
                                                                          : "an update from here would FAIL");
        if (reached) break;
    }
    ulog("exit code %d (0 = no update, 1 = update available, 2 = no source answered)", rc);
    write_log_file(logPath);
    return rc;
}

// the files the last update replaced, left as <name>.old-cogbox while the old launcher still ran them
static void tidy(const std::wstring &dir, int depth)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // never follow a link out of the folder (review 10)
            if (depth < 3 && n != L"game" && n != L"user" && n != L"logs" && n != L"shadercache" && _wcsicmp(n.c_str(), L"_old-files") != 0) tidy(dir + L"\\" + n, depth + 1);
        } else if (n.size() > 11 && (n.compare(n.size() - 11, 11, L".old-cogbox") == 0 || n.compare(n.size() - 11, 11, L".new-cogbox") == 0)) {
            DeleteFileW((dir + L"\\" + n).c_str());
        } else if ((_wcsicmp(n.c_str(), L"TracyClientrd.dll") == 0 || _wcsicmp(n.c_str(), L"TracyClient.dll") == 0 ||
                    _wcsicmp(n.c_str(), L"rexruntimerd.dll") == 0 || _wcsicmp(n.c_str(), L"rexgpu-xenosrd.dll") == 0) &&
                   GetFileAttributesW((dir + L"\\rexruntime.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            // 0.2.2 (GitHub #24): the debug-info runtime and the Tracy profiler older releases shipped beside a game - unused
            // once that folder holds the Release runtime (rexruntime.dll), and flagged by antivirus. cleanup049: moved to
            // <COGBox>\_old-files\developer-dlls\<its folder>\ (never deleted), out of the game's DLL search
            const std::wstring root = exe_dir_here();
            std::wstring rel = dir.size() > root.size() ? dir.substr(root.size() + 1) : L"";
            std::wstring to = root + L"\\_old-files\\developer-dlls" + (rel.empty() ? L"" : L"\\" + rel);
            for (size_t i = root.size() + 1; i <= to.size(); i++)
                if (i == to.size() || to[i] == L'\\') CreateDirectoryW(to.substr(0, i).c_str(), NULL);
            if (!MoveFileExW((dir + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), 0))
                MoveFileExW((dir + L"\\" + n).c_str(), (to + L"\\" + n + L"-" + std::to_wstring(GetTickCount64())).c_str(), 0);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (depth == 0) {
        DeleteFileW((dir + L"\\CogBox-update.zip.part").c_str());
        DeleteFileW((dir + L"\\CogBox-update.7z.part").c_str());
    }
}

void update_tidy() { tidy(exe_dir_here(), 0); }

bool update_restart()
{
    std::wstring dir = exe_dir_here(), exe = dir + L"\\CogBox.exe";
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
