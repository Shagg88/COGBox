// CogBox - the launcher's side of the online account (see social_net.h)
#include "social_net.h"
#include "util.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace {
std::string str(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return {};
    if (j[key].is_string()) return j[key].get<std::string>();
    if (j[key].is_number_unsigned()) return std::to_string(j[key].get<unsigned long long>());
    return {};
}
std::optional<SocialAccount> account_from(const json& j, bool need_token) {
    SocialAccount a;
    a.uuid = str(j, "uuid");
    a.token = str(j, "token");
    a.name = str(j, "display_name");
    a.discriminator = str(j, "discriminator");
    a.xuid = str(j, "xuid");
    if (a.uuid.empty() || (need_token && a.token.empty())) return std::nullopt;
    return a;
}
std::filesystem::path identity_file(const std::string& dir) {
    return std::filesystem::path(widen(dir)) / L"identity.json";
}

// 0.6.2 (GitHub #302 hardening): OneDrive, antivirus and backup tools hold a file open for a moment while they scan or
// sync it. Their lock ends in milliseconds; a read or a swap-in that gives up at the first refusal makes the launcher
// think the account is gone (and a new account is a new #number). So both retry a few times, 15 ms longer each time
// (about 0.4 s in all), and only then report "busy" - which the launcher answers by trying again later, never by
// making a new account.
bool lock_like(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_USER_MAPPED_FILE ||
           e == ERROR_BUSY || e == ERROR_DELETE_PENDING;
}
constexpr int kTries = 8;

enum class Read { Ok, Missing, Busy };
// identity.json whole. Missing: there is no file (nothing else can be meant by those two errors). Busy: there is one and
// it could not be read in kTries tries.
Read read_identity_text(const std::filesystem::path& file, std::string* text) {
    for (int k = 0; k < kTries; k++) {
        if (k) Sleep(15 * k);
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return Read::Missing;
            continue;   // locked, or any other refusal: again a moment later
        }
        std::string got;
        char buf[4096];
        DWORD n = 0;
        bool ok = true;
        while (true) {
            if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) { ok = false; break; }
            if (!n) break;
            got.append(buf, n);
            if (got.size() > (1u << 20)) break;   // an account file is a few hundred bytes
        }
        CloseHandle(h);
        if (!ok) continue;
        *text = std::move(got);
        return Read::Ok;
    }
    return Read::Busy;
}

std::atomic<unsigned> g_tmp_serial{0};
// A whole new identity.json, never half of one: the text goes to a file of its own in the same folder (so the swap stays
// on one volume), is flushed to disk, and then moved over the real name in one step with MoveFileExW. A game or an
// antivirus scan that has identity.json open at that moment makes the move fail with a sharing violation: tried again a
// few times. replace=false (a new account): the move does not replace, so it fails when a file is there - a game that
// made its own a moment earlier keeps it - and nothing is retried for that.
bool put_identity_text(const std::filesystem::path& file, const std::string& text, bool replace) {
    std::filesystem::path tmp = file;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(g_tmp_serial.fetch_add(1));
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int k = 0; k < kTries && h == INVALID_HANDLE_VALUE; k++) {
        if (k) Sleep(15 * k);
        h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && !lock_like(GetLastError())) break;
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) {
        ok = false;
        for (int k = 0; k < kTries; k++) {
            if (k) Sleep(15 * k);
            if (MoveFileExW(tmp.c_str(), file.c_str(), (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH)) { ok = true; break; }
            const DWORD e = GetLastError();
            if (!replace && (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)) break;   // a file is there: it stays
            if (!lock_like(e)) break;
            if (replace && e == ERROR_ACCESS_DENIED) {   // a read-only identity.json (a copy, a game) takes the new one too
                const DWORD at = GetFileAttributesW(file.c_str());
                if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(file.c_str(), at & ~FILE_ATTRIBUTE_READONLY);
            }
        }
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}
}  // namespace

std::optional<SocialAccount> read_account(const std::string& identity_dir_utf8, bool* busy) {
    if (busy) *busy = false;
    std::string text;
    const Read r = read_identity_text(identity_file(identity_dir_utf8), &text);
    if (r == Read::Busy && busy) *busy = true;
    if (r != Read::Ok) return std::nullopt;
    return account_from(json::parse(text, nullptr, false), true);
}

std::optional<SocialAccount> parse_claim(const std::string& body) {
    return account_from(json::parse(body, nullptr, false), true);
}

bool write_account_exclusive(const std::string& identity_dir_utf8, const SocialAccount& a) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(widen(identity_dir_utf8)), ec);
    const json j = { { "uuid", a.uuid }, { "token", a.token }, { "display_name", a.name },
                     { "discriminator", a.discriminator }, { "xuid", a.xuid } };
    // Created only when there is none (the games make theirs the same way, with fopen "wx"). 0.6.2 (GitHub #302): not
    // _wfopen "wx" here - the launcher links the system msvcrt.dll, whose fopen predates the C11 "x" flag (Wine's
    // ignores it and overwrites). The text goes to a file of its own first and is moved to identity.json without
    // replacing (Windows refuses when one is there: the same promise as CREATE_NEW, and nobody ever reads half a file).
    return put_identity_text(identity_file(identity_dir_utf8), j.dump(2), false);
}

std::optional<SocialAccount> read_account_partial(const std::string& identity_dir_utf8, bool* busy) {
    if (busy) *busy = false;
    std::string text;
    const Read r = read_identity_text(identity_file(identity_dir_utf8), &text);
    if (r == Read::Busy && busy) *busy = true;
    if (r != Read::Ok) return std::nullopt;
    return account_from(json::parse(text, nullptr, false), false);
}

bool write_account_replace(const std::string& identity_dir_utf8, const SocialAccount& a) {
    const json j = { { "uuid", a.uuid }, { "token", a.token }, { "display_name", a.name },
                     { "discriminator", a.discriminator }, { "xuid", a.xuid } };
    return put_identity_text(identity_file(identity_dir_utf8), j.dump(2), true);
}

bool set_account_aside(const std::string& identity_dir_utf8, const std::string& suffix) {
    const std::filesystem::path file = identity_file(identity_dir_utf8);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    for (int k = 0; k < 100; k++) {   // never over an earlier one (two in one second: -2, -3 ...)
        std::filesystem::path aside = file;
        aside += widen("." + suffix + stamp + (k ? "-" + std::to_string(k + 1) : std::string()));
        if (MoveFileExW(file.c_str(), aside.c_str(), 0)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    std::error_code ec;
    return !std::filesystem::exists(file, ec);
}

void clear_account_read_only(const std::string& identity_dir_utf8) {
    const std::wstring f = identity_file(identity_dir_utf8).wstring();
    const DWORD at = GetFileAttributesW(f.c_str());
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(f.c_str(), at & ~FILE_ATTRIBUTE_READONLY);
}

int account_file_age_seconds(const std::string& identity_dir_utf8) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(identity_file(identity_dir_utf8).c_str(), GetFileExInfoStandard, &d)) return -1;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long a = (unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;
    const unsigned long long b = (unsigned long long)now.dwHighDateTime << 32 | now.dwLowDateTime;
    if (a > b + 600000000ULL) return 1 << 30;   // over a minute in the future: a clock set back - counts as old
    return b <= a ? 0 : (int)std::min<unsigned long long>((b - a) / 10000000ULL, 1u << 30);
}

bool account_dir_writable(const std::string& identity_dir_utf8) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(widen(identity_dir_utf8)), ec);
    const std::filesystem::path probe = std::filesystem::path(widen(identity_dir_utf8)) / L"identity.write-test.tmp";
    FILE* f = _wfopen(probe.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fputs("x", f) >= 0;
    std::fclose(f);
    std::filesystem::remove(probe, ec);
    return ok;
}

bool update_account_name(const std::string& identity_dir_utf8, const std::string& name, const std::string& discriminator) {
    const std::filesystem::path file = identity_file(identity_dir_utf8);
    std::string text;
    if (read_identity_text(file, &text) != Read::Ok) return false;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    j["display_name"] = name;
    j["discriminator"] = discriminator;
    // a new file, then swapped in: a game reading it at that moment sees the old or the new one, never half of one
    return put_identity_text(file, j.dump(2), true);
}

std::optional<ServerUrl> parse_server_url(const std::string& url) {
    ServerUrl u;
    std::string rest = url;
    u.port = 80;
    if (rest.rfind("https://", 0) == 0) {
        u.secure = true;
        u.port = 443;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    const size_t colon = rest.find(':');
    std::string host = rest.substr(0, colon);
    if (colon != std::string::npos) {
        const std::string port = rest.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        u.port = std::stoi(port);
    }
    if (host.empty() || u.port <= 0 || u.port > 65535) return std::nullopt;
    u.host = widen(host);
    return u;
}

std::string stun_address(const std::string& url) {
    auto u = parse_server_url(url);
    return u ? narrow(u->host) + ":3478" : std::string();
}

HttpResult social_http(const ServerUrl& server, const SocialCall& call, const SocialAccount* account) {
    HttpResult res;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return res;
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    HINTERNET c = WinHttpConnect(s, server.host.c_str(), (INTERNET_PORT)server.port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, widen(call.method).c_str(), widen(call.path).c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, server.secure ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    if (r) {
        std::wstring headers;
        if (account) headers += L"Authorization: COGBox " + widen(account->uuid + ":" + account->token) + L"\r\n";
        if (!call.body.empty()) headers += L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                               call.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)call.body.data(),
                               (DWORD)call.body.size(), (DWORD)call.body.size(), 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)status;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
                res.body.append(chunk.data(), got);
                if (res.body.size() > (1u << 20)) break;   // the friends list is small: never more than 1 MB
            }
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return res;
}

int local_utc_minutes() {
    TIME_ZONE_INFORMATION z{};
    const DWORD r = GetTimeZoneInformation(&z);
    return -(int(z.Bias) + (r == TIME_ZONE_ID_DAYLIGHT ? int(z.DaylightBias) : int(z.StandardBias)));
}
