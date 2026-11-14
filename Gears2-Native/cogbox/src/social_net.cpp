// CogBox - the launcher's side of the online account (see social_net.h)
#include "social_net.h"
#include "util.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

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
}  // namespace

std::optional<SocialAccount> read_account(const std::string& identity_dir_utf8) {
    std::ifstream in(identity_file(identity_dir_utf8), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    return account_from(j, true);
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
    // ignores it and overwrites); CREATE_NEW is the same promise from Windows itself.
    HANDLE h = CreateFileW(identity_file(identity_dir_utf8).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string text = j.dump(2);
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    CloseHandle(h);
    return ok;
}

std::optional<SocialAccount> read_account_partial(const std::string& identity_dir_utf8) {
    std::ifstream in(identity_file(identity_dir_utf8), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return account_from(json::parse(ss.str(), nullptr, false), false);
}

bool write_account_replace(const std::string& identity_dir_utf8, const SocialAccount& a) {
    const std::filesystem::path file = identity_file(identity_dir_utf8);
    const json j = { { "uuid", a.uuid }, { "token", a.token }, { "display_name", a.name },
                     { "discriminator", a.discriminator }, { "xuid", a.xuid } };
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool set_account_aside(const std::string& identity_dir_utf8, const std::string& suffix) {
    const std::filesystem::path file = identity_file(identity_dir_utf8);
    std::filesystem::path aside = file;
    aside += widen("." + suffix);
    MoveFileExW(file.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
    std::error_code ec;
    return !std::filesystem::exists(file, ec);
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
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return false;
    j["display_name"] = name;
    j["discriminator"] = discriminator;
    // a new file, then swapped in: a game reading it at that moment sees the old or the new one, never half of one
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
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
