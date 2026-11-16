// CogBox - the Fabricator's transfers (see fabricator_net.h). The server's side: tools/master-server/src/fabricator
// (routes.ts: /uploads, /uploads/:id/chunks/:n, /uploads/:id/screens/:pos, /uploads/:id/finish, /files/:token).
#include "fabricator_net.h"
#include "util.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX   // std::min / std::max (build.sh sets it; the test build line does not)
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>

using json = nlohmann::json;

namespace {
constexpr DWORD kTimeoutMs = 30000;
constexpr size_t kPiece = 64 * 1024;           // sent and received this much at a time (progress and cancel in between)
constexpr size_t kMaxAnswer = 1u << 20;        // an answer read into memory (JSON): at most this much
constexpr size_t kMaxScreen = 5u << 20;        // the server takes up to 4 MB
constexpr int64_t kMaxChunk = 256ll << 20;   // a chunk is held in memory: the server's setting is 16 MiB
constexpr int kMaxRetries = 10;                // busy / rate limited / out-of-step answers in one upload

// ---- SHA-256 with Windows' BCrypt (update.cpp's Hash) ----
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    Sha256() { if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0); }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* p, size_t n) { if (h && n) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex() {
        unsigned char d[32];
        std::string out;
        if (!h || BCryptFinishHash(h, d, 32, 0) != 0) return "";
        for (unsigned char c : d) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
        return out;
    }
};

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ---- one HTTP request, its body sent and its answer read in pieces ----
struct Request {
    std::string method, path;
    std::wstring headers;                  // extra ones, each ending in \r\n
    const SocialAccount* account = nullptr;
    const char* body = nullptr;            // sent kPiece at a time
    size_t body_len = 0;
    FabProgress* progress = nullptr;       // while sending: done grows, cancel stops the request
    // When set: asked once with a 200 / 206 answer's status and Content-Range whether the body goes to sink (true) or
    // is read as text like any other answer. sink returns false to stop (cancelled, disk full).
    std::function<bool(int status, const std::string& content_range)> begin;
    std::function<bool(const char*, size_t)> sink;
};
struct Response {
    int status = 0;            // 0: no answer
    std::string body;          // the answer as text, when it did not go to the sink
    bool cancelled = false;    // progress->cancel stopped the send
    bool stopped = false;      // the sink stopped the read
    bool complete = false;     // the whole answer was read
    bool too_big = false;      // a text answer over kMaxAnswer: not read further, not to be trusted
};

Response send_request(const ServerUrl& server, const Request& q) {
    Response res;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return res;
    WinHttpSetTimeouts(s, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    HINTERNET c = WinHttpConnect(s, server.host.c_str(), (INTERNET_PORT)server.port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, widen(q.method).c_str(), widen(q.path).c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, server.secure ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    if (r) {
        std::wstring headers = q.headers;
        if (q.account) headers += L"Authorization: COGBox " + widen(q.account->uuid + ":" + q.account->token) + L"\r\n";
        bool ok = WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                                     WINHTTP_NO_REQUEST_DATA, 0, (DWORD)q.body_len, 0) != FALSE;
        for (size_t at = 0; ok && at < q.body_len;) {
            if (q.progress && q.progress->cancel) { res.cancelled = true; ok = false; break; }
            const DWORD n = (DWORD)std::min(kPiece, q.body_len - at);
            DWORD put = 0;
            ok = WinHttpWriteData(r, q.body + at, n, &put) && put == n;
            at += n;
            if (ok && q.progress) q.progress->done += n;
        }
        if (ok && WinHttpReceiveResponse(r, NULL)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)status;
            bool to_sink = false;
            if (q.sink && (status == 200 || status == 206)) {
                wchar_t range[128] = L"";
                DWORD len = sizeof range;
                if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range, &len,
                                         WINHTTP_NO_HEADER_INDEX))
                    range[0] = 0;
                to_sink = !q.begin || q.begin((int)status, narrow(range));
                if (!to_sink) res.stopped = true;   // refused: the body is not read at all
            }
            std::string piece(kPiece, '\0');
            while (!res.stopped) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) break;
                if (avail == 0) { res.complete = true; break; }
                if (!WinHttpReadData(r, &piece[0], (DWORD)std::min<size_t>(avail, kPiece), &got) || got == 0) break;
                if (to_sink) {
                    if (!q.sink(piece.data(), got)) { res.stopped = true; break; }
                } else {
                    res.body.append(piece.data(), got);
                    if (res.body.size() > kMaxAnswer) { res.too_big = true; break; }
                }
            }
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return res;
}

// What an answer that is not a success means, for the creator: the server's problems / error, else its status.
std::string problem_of(const Response& r) {
    if (r.status == 0) return "no answer from the server: check the connection and try again";
    if (r.too_big) return "the server's answer is too large (" + std::to_string(r.status) + ")";
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object()) {
        if (j.contains("problems") && j["problems"].is_array()) {
            std::string out;
            for (const auto& p : j["problems"])
                if (p.is_string()) out += (out.empty() ? "" : "\n") + p.get<std::string>();
            if (!out.empty()) return out;
        }
        for (const char* k : { "error", "problem" })
            if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
    }
    return "the server answered " + std::to_string(r.status);
}

// ---- uploads.json: the sessions that have not finished ----
std::mutex g_state_mutex;   // uploads.json, read and written whole
std::mutex g_dir_mutex;     // g_state_dir
std::string g_state_dir;

std::string uploads_file() { return join(fab_state_dir(), "uploads.json"); }

json load_uploads() {
    std::string text;
    json j = read_file(uploads_file(), text) ? json::parse(text, nullptr, false) : json();
    if (!j.is_object() || !j.contains("uploads") || !j["uploads"].is_array()) j = json{ { "uploads", json::array() } };
    return j;
}
// util's write_file (a temporary file, then replaced). Its temporary name is fixed (<file>.tmp-cogbox), not per
// process: two launchers sharing one folder could collide, which only costs a resume (left as it is). False when
// uploads.json could not be written: the upload still goes on, it just cannot be resumed after a stop.
bool save_uploads(const json& j) {
    make_dirs(fab_state_dir());
    return write_file(uploads_file(), j.dump(2));
}

std::string server_key(const ServerUrl& s) {
    return std::string(s.secure ? "https://" : "http://") + narrow(s.host) + ":" + std::to_string(s.port);
}

struct Session {
    std::string id;
    int64_t chunk_bytes = 0, chunks = 0, chunks_done = 0;
};
// What makes a remembered session the same upload: the server, the account, the file (path, size, SHA-256), the mod
// and the meta text.
struct UploadKey {
    std::string server, account, path, sha256, meta_sha256;
    int64_t size = 0, mod_id = 0;
    bool same(const json& e) const {
        return e.is_object() && e.value("server", "") == server && e.value("account", "") == account && e.value("path", "") == path &&
               e.value("size", (int64_t)-1) == size && e.value("sha256", "") == sha256 && e.value("meta_sha256", "") == meta_sha256 &&
               e.value("mod_id", (int64_t)-1) == mod_id;
    }
};

// The remembered session for this upload (id empty when none). Sessions of the same file that no longer match it (the
// file or its details changed) are returned in stale, and forgotten.
Session find_upload(const UploadKey& k, std::vector<std::string>* stale) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    json j = load_uploads();
    Session s;
    json kept = json::array();
    for (const auto& e : j["uploads"]) {
        if (k.same(e) && s.id.empty()) {
            s.id = e.value("id", "");
            s.chunk_bytes = e.value("chunk_bytes", (int64_t)0);
            kept.push_back(e);
        } else if (e.is_object() && e.value("server", "") == k.server && e.value("account", "") == k.account &&
                   e.value("path", "") == k.path) {
            stale->push_back(e.value("id", ""));
        } else {
            kept.push_back(e);
        }
    }
    if (kept.size() != j["uploads"].size()) {
        j["uploads"] = kept;
        save_uploads(j);
    }
    return s;
}
// False when uploads.json could not be written: the upload goes on, but cannot be resumed after a stop.
bool remember_upload(const UploadKey& k, const Session& s) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    json j = load_uploads();
    json e = { { "id", s.id }, { "server", k.server }, { "account", k.account }, { "path", k.path }, { "size", k.size },
               { "sha256", k.sha256 }, { "meta_sha256", k.meta_sha256 }, { "mod_id", k.mod_id }, { "chunk_bytes", s.chunk_bytes },
               { "started", unix_now() } };
    j["uploads"].push_back(e);
    return save_uploads(j);
}
// False when uploads.json could not be written: the entry stays, and the next upload of that file finds the server no
// longer has the session (404) and forgets it then.
bool forget_upload(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    json j = load_uploads();
    json kept = json::array();
    for (const auto& e : j["uploads"])
        if (!e.is_object() || e.value("id", "") != id) kept.push_back(e);
    j["uploads"] = kept;
    return save_uploads(j);
}

// The number of chunks a file of size bytes has (0 for a chunk size outside 1 .. kMaxChunk).
int64_t chunks_for(int64_t size, int64_t chunk_bytes) {
    return chunk_bytes > 0 && chunk_bytes <= kMaxChunk ? (size + chunk_bytes - 1) / chunk_bytes : 0;
}

// Waits ms milliseconds in short steps; false when cancel was set meanwhile.
bool wait_unless_cancelled(const std::atomic<bool>& cancel, int ms) {
    for (int t = 0; t < ms; t += 50) {
        if (cancel) return false;
        Sleep(50);
    }
    return !cancel;
}

// An upload id goes into a path: only the server's characters (A-Z a-z 0-9 _ -).
bool safe_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 &&
           std::all_of(id.begin(), id.end(), [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; });
}

HANDLE open_read(const std::string& path) {
    return CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool read_at(HANDLE h, int64_t at, char* buf, size_t n) {
    LARGE_INTEGER pos;
    pos.QuadPart = at;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
    size_t have = 0;
    while (have < n) {
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(n - have, 1u << 30);
        if (!ReadFile(h, buf + have, want, &got, nullptr) || got == 0) return false;
        have += got;
    }
    return true;
}

// A file's first size bytes added to sha, read 1 MiB at a time. False when it cannot be read or cancel was set
// (cancelled set then).
bool hash_into(HANDLE h, int64_t size, Sha256& sha, const std::atomic<bool>& cancel, bool* cancelled) {
    std::string buf(1u << 20, '\0');
    for (int64_t at = 0; at < size;) {
        if (cancel) { *cancelled = true; return false; }
        const size_t n = (size_t)std::min<int64_t>((int64_t)buf.size(), size - at);
        if (!read_at(h, at, &buf[0], n)) return false;
        sha.add(buf.data(), n);
        at += (int64_t)n;
    }
    return true;
}

const char* image_type(const std::string& bytes) {
    if (bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "image/png";
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8 && (unsigned char)bytes[2] == 0xFF)
        return "image/jpeg";
    return "application/octet-stream";
}

struct FileCloser {
    HANDLE h;
    ~FileCloser() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

enum class Step { Ok, Gone, Fail };   // Gone: the session expired or is unknown - a new one is started
}  // namespace

std::string fab_state_dir() {
    std::lock_guard<std::mutex> lock(g_dir_mutex);
    return g_state_dir.empty() ? join(exe_dir(), "fabricator") : g_state_dir;
}
void fab_set_state_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_dir_mutex);
    g_state_dir = dir;
}

HttpResult fab_call(const ServerUrl& server, const std::string& method, const std::string& path, const std::string& json_body,
                    const SocialAccount* account) {
    Request q;
    q.method = method;
    q.path = path;
    q.account = account;
    q.body = json_body.data();
    q.body_len = json_body.size();
    if (!json_body.empty()) q.headers = L"Content-Type: application/json\r\n";
    Response r = send_request(server, q);
    HttpResult out;
    if (r.too_big) return out;   // status 0: no usable answer (a cut JSON text is never handed on)
    out.status = r.status;
    out.body = std::move(r.body);
    return out;
}

HttpResult fab_get_file(const ServerUrl& server, const std::string& path, size_t max_bytes) {
    Request q;
    q.method = "GET";
    q.path = path;
    std::string got;
    bool over = false;
    q.sink = [&](const char* p, size_t n) {
        if (got.size() + n > max_bytes) { over = true; return false; }
        got.append(p, n);
        return true;
    };
    Response r = send_request(server, q);
    HttpResult out;
    if (over || r.too_big || (r.status == 200 && !r.complete)) return out;
    out.status = r.status;
    out.body = r.status == 200 ? std::move(got) : std::move(r.body);
    return out;
}

std::string fab_upload(const ServerUrl& server, const SocialAccount& account, const std::string& meta_json, int64_t mod_id,
                       const std::string& zip_path, const std::vector<std::string>& screenshot_paths, FabProgress& progress,
                       int64_t* mod_out) {
    if (mod_out) *mod_out = 0;
    progress.done = 0;
    const json meta = json::parse(meta_json, nullptr, false);
    if (!meta.is_object()) return "the mod's details are not valid JSON";
    FileCloser zip{ open_read(zip_path) };
    LARGE_INTEGER sz{};
    if (zip.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(zip.h, &sz) || sz.QuadPart <= 0) return "the mod's zip cannot be read";
    const int64_t size = sz.QuadPart;
    progress.total = size;

    UploadKey key;
    key.server = server_key(server);
    key.account = account.uuid;
    key.path = zip_path;
    key.size = size;
    key.mod_id = mod_id;
    {
        Sha256 m;
        m.add(meta_json.data(), meta_json.size());
        key.meta_sha256 = m.hex();
        Sha256 f;
        bool cancelled = false;
        if (!hash_into(zip.h, size, f, progress.cancel, &cancelled)) return cancelled ? "cancelled" : "the mod's zip cannot be read";
        key.sha256 = f.hex();
    }
    const std::string base = "/api/v1/fabricator/uploads";
    auto call = [&](const char* method, const std::string& path, const std::string& body) {
        Request q;
        q.method = method;
        q.path = path;
        q.account = &account;
        q.body = body.data();
        q.body_len = body.size();
        if (!body.empty()) q.headers = L"Content-Type: application/json\r\n";
        return send_request(server, q);
    };

    std::string buf;   // one chunk
    int retries = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        // the session: the remembered one when the server still has it, else a new one
        std::vector<std::string> stale;
        Session s = find_upload(key, &stale);
        for (const std::string& id : stale)
            if (safe_id(id)) call("DELETE", base + "/" + id, "");   // best effort: it expires anyway
        if (!s.id.empty() && (!safe_id(s.id) || s.chunk_bytes <= 0 || s.chunk_bytes > kMaxChunk)) { forget_upload(s.id); s = Session(); }
        if (!s.id.empty()) {
            Response st = call("GET", base + "/" + s.id, "");
            json j = json::parse(st.body, nullptr, false);
            const int64_t want = chunks_for(size, s.chunk_bytes);
            if (st.status == 200 && j.is_object() && j.value("chunks", (int64_t)0) == want && want > 0 &&
                j.value("chunks_done", (int64_t)-1) >= 0 && j.value("chunks_done", (int64_t)-1) <= want) {
                s.chunks = want;
                s.chunks_done = j.value("chunks_done", (int64_t)0);
            } else if (st.status == 200) {
                // a session that does not fit this file (another chunk size, a count that does not add up): a new one
                call("DELETE", base + "/" + s.id, "");
                forget_upload(s.id);
                s = Session();
            } else if (st.status == 404 || st.status == 410) {
                forget_upload(s.id);
                s = Session();
            } else {
                return problem_of(st);
            }
        }
        if (s.id.empty()) {
            json body = { { "meta", meta }, { "size", size }, { "sha256", key.sha256 } };
            body["mod_id"] = mod_id > 0 ? json(mod_id) : json(nullptr);
            Response st = call("POST", base, body.dump());
            json j = json::parse(st.body, nullptr, false);
            if (st.status != 200 || !j.is_object()) return problem_of(st);
            s.id = j.value("id", "");
            s.chunk_bytes = j.value("chunk_bytes", (int64_t)0);
            s.chunks = j.value("chunks", (int64_t)0);
            if (!safe_id(s.id) || s.chunks <= 0 || s.chunks != chunks_for(size, s.chunk_bytes))
                return "the server's answer to a new upload is not understood";
            remember_upload(key, s);   // when this fails the upload still goes on: it just cannot be resumed
        }
        const std::string at = base + "/" + s.id;

        // the screenshots (sent again on a resume: the server keeps the newest one at each place)
        Step step = Step::Ok;
        std::string problem;
        for (size_t i = 0; i < screenshot_paths.size() && step == Step::Ok; i++) {
            if (progress.cancel) return "cancelled";
            std::string img;
            if (!read_file(screenshot_paths[i], img, kMaxScreen)) return "screenshot " + std::to_string(i + 1) + " cannot be read (or is over 4 MB)";
            Request q;
            q.method = "PUT";
            q.path = at + "/screens/" + std::to_string(i);
            q.account = &account;
            q.headers = L"Content-Type: " + widen(image_type(img)) + L"\r\n";
            q.body = img.data();
            q.body_len = img.size();
            Response r = send_request(server, q);
            if (r.status == 204) continue;
            if (r.status == 404 || r.status == 410) step = Step::Gone;
            else { step = Step::Fail; problem = "screenshot " + std::to_string(i + 1) + ": " + problem_of(r); }
        }

        // the chunks, in order, from the first one the server does not have
        int64_t n = s.chunks_done;
        progress.done = std::min(size, n * s.chunk_bytes);
        if (step == Step::Ok && n < s.chunks) buf.resize((size_t)s.chunk_bytes);
        while (step == Step::Ok && n < s.chunks) {
            if (progress.cancel) return "cancelled";
            const int64_t from = n * s.chunk_bytes;
            if (n < 0 || from >= size) return "the upload got out of step with the server: try again";
            const size_t len = (size_t)std::min(s.chunk_bytes, size - from);   // 1 .. chunk_bytes
            if (!read_at(zip.h, from, &buf[0], len)) return "the mod's zip cannot be read";
            Sha256 h;
            h.add(buf.data(), len);
            Request q;
            q.method = "PUT";
            q.path = at + "/chunks/" + std::to_string(n);
            q.account = &account;
            q.headers = L"Content-Type: application/octet-stream\r\nx-chunk-sha256: " + widen(h.hex()) + L"\r\n";
            q.body = buf.data();
            q.body_len = len;
            q.progress = &progress;
            Response r = send_request(server, q);
            if (r.cancelled) {
                progress.done = from;
                return "cancelled";
            }
            if (r.status == 204) {
                n++;
                progress.done = std::min(size, n * s.chunk_bytes);
                continue;
            }
            progress.done = from;
            json j = json::parse(r.body, nullptr, false);
            if (r.status == 404 || r.status == 410) { step = Step::Gone; break; }
            if ((r.status == 409 || r.status == 429) && retries++ < kMaxRetries) {
                if (j.is_object() && j.contains("chunks_done") && j["chunks_done"].is_number_integer()) {
                    n = std::max<int64_t>(0, std::min(s.chunks, j["chunks_done"].get<int64_t>()));   // out of step: the server says where
                    progress.done = std::min(size, n * s.chunk_bytes);
                } else if (!wait_unless_cancelled(progress.cancel, r.status == 429 ? 2000 : 500)) {   // busy, or damaged on the way
                    return "cancelled";
                }
                continue;
            }
            step = Step::Fail;
            problem = problem_of(r);
        }
        if (step == Step::Gone) { forget_upload(s.id); continue; }
        if (step == Step::Fail) return problem;   // the session is kept: the next try goes on from here

        // publish
        if (progress.cancel) return "cancelled";
        Response fin = call("POST", at + "/finish", "");
        json fj = json::parse(fin.body, nullptr, false);
        if (fin.status == 200 && fj.is_object()) {
            forget_upload(s.id);
            if (mod_out) *mod_out = fj.value("mod_id", (int64_t)0);
            progress.done = size;
            return "";
        }
        if (fin.status == 404 || fin.status == 410) { forget_upload(s.id); continue; }
        if (fin.status == 422) {
            // refused: the session's chunks would stay on the server until it expires
            call("DELETE", at, "");
            forget_upload(s.id);
        }
        return problem_of(fin);
    }
    return "the upload expired on the server: try again";
}

std::string fab_full_url(const std::string& server_url, const std::string& url) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return url;
    if (url.empty() || url[0] != '/') return "";
    std::string base = server_url;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) base = "http://" + base;
    const size_t slash = base.find('/', base.find("://") + 3);
    if (slash != std::string::npos) base = base.substr(0, slash);
    return base + url;
}

namespace {
// <dest>.part.json says which file <dest>.part is the start of: {"sha256", "size"}. A part file without it, or with
// another file's, is not gone on from (an older version's bytes would only end in a hash mismatch).
bool part_is_for(const std::string& note, const std::string& sha256, int64_t size) {
    std::string text;
    if (!read_file(note, text, 4096)) return false;
    json j = json::parse(text, nullptr, false);
    return j.is_object() && j.value("sha256", "") == sha256 && j.value("size", (int64_t)-1) == size;
}
void drop_part(const std::string& part, const std::string& note) {
    DeleteFileW(widen(part).c_str());
    DeleteFileW(widen(note).c_str());
}
// "bytes <start>-<end>/<total>" read into start and total; false when it is not that.
bool parse_content_range(const std::string& range, int64_t* start, int64_t* total) {
    if (range.rfind("bytes ", 0) != 0) return false;
    char* end = nullptr;
    *start = _strtoi64(range.c_str() + 6, &end, 10);
    if (end == range.c_str() + 6 || *end != '-') return false;
    const size_t slash = range.find('/');
    if (slash == std::string::npos || slash + 1 >= range.size()) return false;
    *total = _strtoi64(range.c_str() + slash + 1, &end, 10);
    return end != range.c_str() + slash + 1 && *end == 0;
}
}  // namespace

std::string fab_download(const std::string& url, const std::string& dest_path, const std::string& sha256, int64_t size,
                         FabProgress& progress) {
    progress.done = 0;
    progress.total = size;
    if (size <= 0 || sha256.size() != 64) return "the download's size or hash is missing";
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return "the download address is not a full address";
    auto server = parse_server_url(url);
    const size_t slash = url.find('/', url.find("://") + 3);
    if (!server || slash == std::string::npos) return "the download address is not a full address";
    const std::string path = url.substr(slash);
    const std::string want = lower(sha256);
    const std::string part = dest_path + ".part", note = dest_path + ".part.json";
    const std::wstring wpart = widen(part);

    // A second round only after the server could not go on from the part file: then from the start.
    for (int attempt = 0; attempt < 2; attempt++) {
        // what an earlier try left for this same file: hashed again, then only the rest is asked for
        auto sha = std::make_unique<Sha256>();
        int64_t have = 0;
        if (file_exists(part) && !part_is_for(note, want, size)) drop_part(part, note);
        if (file_exists(part)) {
            bool ok = false, cancelled = false;
            {
                FileCloser f{ open_read(part) };
                LARGE_INTEGER sz{};
                if (f.h != INVALID_HANDLE_VALUE && GetFileSizeEx(f.h, &sz) && sz.QuadPart <= size) {
                    ok = hash_into(f.h, sz.QuadPart, *sha, progress.cancel, &cancelled);
                    if (ok) have = sz.QuadPart;
                }
            }
            if (cancelled) return "cancelled";
            if (!ok) {   // unreadable, or longer than the file: from the start
                drop_part(part, note);
                sha = std::make_unique<Sha256>();
                have = 0;
            }
        }
        progress.done = have;

        if (have < size) {
            HANDLE out = INVALID_HANDLE_VALUE;
            bool write_failed = false, wrong_part = false;
            int64_t got = have;
            Request q;
            q.method = "GET";
            q.path = path;
            if (have > 0) q.headers = L"Range: bytes=" + std::to_wstring(have) + L"-\r\n";
            q.begin = [&](int status, const std::string& range) {
                int64_t start = 0, total = 0;
                // a 206 must go on exactly where the part file ends, and be of the file announced
                if (status == 206 && (!parse_content_range(range, &start, &total) || start != have || total != size)) {
                    wrong_part = true;
                    return false;
                }
                if (start == 0) {   // the whole file (a 200, or a part file that was empty): from the start
                    got = 0;
                    sha = std::make_unique<Sha256>();
                    progress.done = 0;
                    if (!write_file(note, json{ { "sha256", want }, { "size", size } }.dump())) { write_failed = true; return false; }
                }
                out = CreateFileW(wpart.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, start == 0 ? CREATE_ALWAYS : OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
                if (out == INVALID_HANDLE_VALUE) { write_failed = true; return false; }
                LARGE_INTEGER end;
                end.QuadPart = start;
                if (!SetFilePointerEx(out, end, nullptr, FILE_BEGIN) || !SetEndOfFile(out)) { write_failed = true; return false; }
                return true;
            };
            q.sink = [&](const char* p, size_t n) {
                if (progress.cancel) return false;
                if (got + (int64_t)n > size) { got += (int64_t)n; return false; }   // more than announced: not this file
                DWORD put = 0;
                if (!WriteFile(out, p, (DWORD)n, &put, nullptr) || put != n) { write_failed = true; return false; }
                sha->add(p, n);
                got += (int64_t)n;
                progress.done = got;
                return true;
            };
            Response r = send_request(*server, q);
            if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            if (write_failed) return "the download cannot be written (is the disk full?)";
            if (r.status == 410) return "address expired: " + problem_of(r) + " - ask for a new download address";
            if (r.status == 416 || wrong_part) {
                drop_part(part, note);   // the server cannot go on from the part file: from the start
                continue;
            }
            if (r.status != 200 && r.status != 206) return r.status == 0 ? problem_of(r) : "download: " + problem_of(r);
            if (got > size) {
                drop_part(part, note);
                return "hash mismatch";
            }
            if (progress.cancel && got < size) return "cancelled";
            if (got < size) return "the download stopped early: try again (it goes on from where it stopped)";
        }

        if (sha->hex() != want) {
            drop_part(part, note);
            return "hash mismatch";
        }
        if (!MoveFileExW(wpart.c_str(), widen(dest_path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return "the downloaded file cannot be put in place";
        DeleteFileW(widen(note).c_str());
        progress.done = size;
        return "";
    }
    return "the server could not go on from the partly downloaded file: try again";
}
