// COGBox - updates from GitHub (adapted from Athelas' update.cpp, same author). GitHub answers /releases/latest with a
// redirect to /releases/tag/<version>; the version is read from that address without following it (no API, no key).
// Installing: the release's CogBox-<version>.zip is streamed to a file next to the exe while its SHA-256 is worked
// out, checked against the release's .sha256 (or the notes' "SHA-256: ..."), then unpacked over this folder. A running
// program can be renamed but not overwritten, so each file replaced is first renamed to <name>.old-cogbox (removed at
// the next start); if anything fails half way, the files already replaced are put back.
#include "update.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include "../third_party/miniz/miniz.h"

// The GitHub repository the releases come from. CHANGE HERE if the repository gets another name.
#define COGBOX_REPO L"/Shagg88/COGBox"
#define COGBOX_REPO_A "/Shagg88/COGBox"

static volatile LONG g_started = 0, g_done = 0;
static std::string g_current, g_latest;
static CRITICAL_SECTION g_lock;

static const wchar_t kHost[] = L"github.com";
const char *update_page() { return "https://github.com" COGBOX_REPO_A "/releases/latest"; }

static bool newer(const std::string &a, const std::string &b)
{
    const char *p = a.c_str(), *q = b.c_str();
    if (*p == 'v' || *p == 'V') p++;
    if (*q == 'v' || *q == 'V') q++;
    if (*p < '0' || *p > '9' || *q < '0' || *q > '9') return false;
    for (int i = 0; i < 4; i++) {
        long x = strtol(p, (char **)&p, 10), y = strtol(q, (char **)&q, 10);
        if (x != y) return x > y;
        if (*p == '.') p++;
        if (*q == '.') q++;
        if (!*p && !*q) return false;
    }
    return false;
}

static DWORD WINAPI check(void *)
{
    std::string found;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) {
        WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
        HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"HEAD", COGBOX_REPO L"/releases/latest", NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
        if (r) {
            DWORD off = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
                wchar_t loc[1024];
                DWORD size = sizeof loc;
                if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &size, WINHTTP_NO_HEADER_INDEX)) {
                    const wchar_t *t = wcsstr(loc, L"/releases/tag/");
                    if (t)
                        for (t += 14; *t && *t != L'/' && *t != L'?' && found.size() < 32; t++) found += (char)(*t < 128 ? *t : '?');
                }
            }
            WinHttpCloseHandle(r);
        }
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
    }
    EnterCriticalSection(&g_lock);
    g_latest = newer(found, g_current) ? (found[0] == 'v' || found[0] == 'V' ? found.substr(1) : found) : "";
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_done, 1);
    return 0;
}

void update_check_start(const std::string &current)
{
    if (InterlockedExchange(&g_started, 1)) return;
    InitializeCriticalSection(&g_lock);
    g_current = current;
    HANDLE h = CreateThread(NULL, 0, check, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_done, 1);
}

std::string update_newer()
{
    if (!g_done) return "";
    EnterCriticalSection(&g_lock);
    std::string v = g_latest;
    LeaveCriticalSection(&g_lock);
    return v;
}

bool update_checked() { return g_done != 0; }

// ---- installing ----------------------------------------------------------------------------------------------------
static volatile LONG g_istate = 0;   // 0 idle, 1 downloading, 2 unpacking, 3 done, -1 failed
static volatile LONG g_ipermille = 0;
static std::string g_ierror;
static std::wstring g_exe_dir;

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

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    Sha256() { if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0); }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void *p, size_t n) { if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex()
    {
        unsigned char d[32];
        std::string out;
        if (!h || BCryptFinishHash(h, d, 32, 0) != 0) return "";
        for (unsigned char b : d) { out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; }
        return out;
    }
};

// A GET of https://github.com<path>, redirects followed (release files come from another host). To memory (small
// files) or, with 'file', streamed to that file while its SHA-256 is worked out.
static bool http_get(const std::wstring &path, std::string *out, HANDLE file, Sha256 *sum)
{
    if (out) out->clear();
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t lenText[32] = L"";
        size = sizeof lenText;
        unsigned long long total = 0;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &size, WINHTTP_NO_HEADER_INDEX))
            total = wcstoull(lenText, NULL, 10);
        if (status == 200) {
            ok = true;
            std::vector<char> buf(1 << 20);
            unsigned long long have = 0;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) { ok = false; break; }
                if (!got) break;
                if (out) out->append(buf.data(), got);
                if (file) {
                    DWORD wrote = 0;
                    if (!WriteFile(file, buf.data(), got, &wrote, NULL) || wrote != got) { ok = false; break; }
                }
                if (sum) sum->add(buf.data(), got);
                have += got;
                if (file && total) InterlockedExchange(&g_ipermille, (LONG)(have * 1000ull / total));
            }
            if (total && have != total) ok = false;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static void make_dirs_for(const std::wstring &file)
{
    for (size_t i = g_exe_dir.size() + 1; (i = file.find(L'\\', i)) != std::wstring::npos; i++) CreateDirectoryW(file.substr(0, i).c_str(), NULL);
}

static bool fail(const std::string &why)
{
    g_ierror = why;
    InterlockedExchange(&g_istate, -1);
    return false;
}

static bool is_hex64(const std::string &t)
{
    if (t.size() != 64) return false;
    for (char ch : t) if (!isxdigit((unsigned char)ch)) return false;
    return true;
}

// The player's own things are never touched by an update, even if a zip held them by mistake.
static bool protected_path(std::string rel)
{
    for (char &c : rel) c = (char)tolower((unsigned char)c);
    static const char *top[] = { "cogbox.ini", "settings.ini", "extra-args.txt", "covers\\" };
    for (const char *t : top) if (rel.compare(0, strlen(t), t) == 0) return true;
    static const char *inner[] = { "\\game\\", "\\user\\", "\\logs\\", "\\shadercache\\" };
    for (const char *t : inner) if (rel.find(t) != std::string::npos) return true;
    return false;
}

static bool install(const std::string &version)
{
    std::wstring probe = g_exe_dir + L"\\cogbox-update-test.tmp";
    HANDLE ph = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (ph == INVALID_HANDLE_VALUE) return fail("Windows doesn't let COGBox change its own folder. Download the update from its page instead.");
    CloseHandle(ph);

    std::wstring base = COGBOX_REPO L"/releases/download/" + wide(version) + L"/COGBox-" + wide(version) + L".zip";
    std::string sum;
    if (!http_get(base + L".sha256", &sum, NULL, NULL) || sum.size() < 64 || !is_hex64(sum.substr(0, 64))) {
        std::string page;
        sum.clear();
        if (http_get(COGBOX_REPO L"/releases/tag/" + wide(version), &page, NULL, NULL)) {
            size_t at = page.find("SHA-256");
            while (at != std::string::npos && sum.empty()) {
                for (size_t i = at; i + 64 <= page.size() && i < at + 400; i++)
                    if (is_hex64(page.substr(i, 64)) && (i == 0 || !isxdigit((unsigned char)page[i - 1]))) { sum = page.substr(i, 64); break; }
                at = page.find("SHA-256", at + 7);
            }
        }
        if (sum.empty()) return fail("The update's checksum couldn't be found. Try again later, or download it from its page.");
    }
    sum = sum.substr(0, 64);
    for (char &ch : sum) ch = (char)tolower((unsigned char)ch);

    std::wstring zipPath = g_exe_dir + L"\\CogBox-update.zip.part";
    HANDLE zf = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (zf == INVALID_HANDLE_VALUE) return fail("Couldn't make room for the download in the COGBox folder.");
    Sha256 h;
    bool got = http_get(base, NULL, zf, &h);
    CloseHandle(zf);
    if (!got) { DeleteFileW(zipPath.c_str()); return fail("The download didn't finish. Check the internet connection (and the free disk space) and try again."); }
    if (h.hex() != sum) { DeleteFileW(zipPath.c_str()); return fail("The download was damaged (its checksum doesn't match). Try again."); }

    InterlockedExchange(&g_istate, 2);
    InterlockedExchange(&g_ipermille, 0);
    mz_zip_archive ar;
    memset(&ar, 0, sizeof ar);
    std::string zipA;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, zipPath.c_str(), -1, NULL, 0, NULL, NULL);
        zipA.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, zipPath.c_str(), -1, &zipA[0], n, NULL, NULL);
        zipA.resize(strlen(zipA.c_str()));
    }
    // miniz opens files with fopen (the ANSI code page): a folder name it can't hold falls back to the short 8.3 path
    std::wstring shortPath(MAX_PATH * 2, L'\0');
    DWORD sn = GetShortPathNameW(zipPath.c_str(), &shortPath[0], (DWORD)shortPath.size());
    if (sn && sn < shortPath.size()) {
        shortPath.resize(sn);
        int n = WideCharToMultiByte(CP_ACP, 0, shortPath.c_str(), -1, NULL, 0, NULL, NULL);
        zipA.assign(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, shortPath.c_str(), -1, &zipA[0], n, NULL, NULL);
        zipA.resize(strlen(zipA.c_str()));
    }
    if (!mz_zip_reader_init_file(&ar, zipA.c_str(), 0)) { DeleteFileW(zipPath.c_str()); return fail("The update couldn't be opened."); }
    mz_uint count = mz_zip_reader_get_num_files(&ar);
    std::vector<std::string> names(count);
    std::string top;
    bool one_top = true, has_root_exe = false;
    for (mz_uint i = 0; i < count; i++) {
        char n[1024];
        mz_zip_reader_get_filename(&ar, i, n, sizeof n);
        names[i] = n;
        std::string first = names[i].substr(0, names[i].find('/') + 1);
        if (i == 0) top = first;
        if (first.empty() || first != top) one_top = false;
        if (names[i] == "CogBox.exe") has_root_exe = true;
    }
    size_t strip = one_top && !has_root_exe ? top.size() : 0;

    struct Done { std::wstring target; bool had_old; };
    std::vector<Done> done;
    auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (it->had_old) MoveFileExW((it->target + L".old-cogbox").c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    bool ok = true;
    for (mz_uint i = 0; i < count && ok; i++) {
        if (mz_zip_reader_is_file_a_directory(&ar, i)) continue;
        std::string rel = names[i].substr(std::min(strip, names[i].size()));
        if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/' || rel.find(':') != std::string::npos) continue;
        for (char &ch : rel) if (ch == '/') ch = '\\';
        if (protected_path(rel)) continue;
        std::wstring target = g_exe_dir + L"\\" + wide(rel), fresh = target + L".new-cogbox";
        make_dirs_for(target);
        size_t size = 0;
        void *p = mz_zip_reader_extract_to_heap(&ar, i, &size, 0);
        if (!p) { ok = fail("The update couldn't be unpacked (" + rel + ")."); break; }
        HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wrote = 0;
        bool written = f != INVALID_HANDLE_VALUE && WriteFile(f, p, (DWORD)size, &wrote, NULL) && wrote == size;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        mz_free(p);
        if (!written) { DeleteFileW(fresh.c_str()); ok = fail("Couldn't write " + rel + " (is the disk full?)."); break; }
        bool had = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (had && !MoveFileExW(target.c_str(), (target + L".old-cogbox").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(fresh.c_str());
            ok = fail("Couldn't replace " + rel + " (is a game still running?).");
            break;
        }
        if (!MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (had) MoveFileExW((target + L".old-cogbox").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            DeleteFileW(fresh.c_str());
            ok = fail("Couldn't replace " + rel + ".");
            break;
        }
        done.push_back({target, had});
        InterlockedExchange(&g_ipermille, (LONG)((i + 1) * 1000ull / count));
    }
    mz_zip_reader_end(&ar);
    DeleteFileW(zipPath.c_str());
    if (!ok) { undo(); return false; }
    InterlockedExchange(&g_istate, 3);
    return true;
}

static std::string g_iversion;
static DWORD WINAPI install_thread(void *) { install(g_iversion); return 0; }

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
            if (depth < 3 && n != L"game" && n != L"user" && n != L"logs" && n != L"shadercache") tidy(dir + L"\\" + n, depth + 1);
        } else if (n.size() > 11 && (n.compare(n.size() - 11, 11, L".old-cogbox") == 0 || n.compare(n.size() - 11, 11, L".new-cogbox") == 0)) {
            DeleteFileW((dir + L"\\" + n).c_str());
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (depth == 0) DeleteFileW((dir + L"\\CogBox-update.zip.part").c_str());
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
