// CogBox - the Fabricator's transfers (the store's network layer): signed-in calls to the master server, a resumable
// chunked upload (publishing a mod) and a resumable download of a mod's zip. Blocking: a worker thread calls them and
// the page reads FabProgress. Nothing here logs a token, an upload id or a download address.
// test/fabricator_net_test.cpp runs them against a local master server.
#pragma once
#include "social_net.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One call to /api/v1/fabricator/... (or any path on the server): 30 s timeouts, a JSON body when one is given, the
// account's "Authorization: COGBox <uuid>:<token>" header when one is given. status 0: no answer.
HttpResult fab_call(const ServerUrl& server, const std::string& method, const std::string& path, const std::string& json_body,
                    const SocialAccount* account);

// GET path (a store picture, /api/v1/fabricator/images/<key>) read whole into body, whatever its type, at most max_bytes:
// a longer answer, or none, is status 0. No account.
HttpResult fab_get_file(const ServerUrl& server, const std::string& path, size_t max_bytes);

// Bytes done of total (upload: the zip's bytes the server holds; download: the bytes in the file). cancel: the transfer
// stops at its next step and answers "cancelled" (what was done is kept for the next try). An upload first reads the
// whole zip once for its SHA-256 (the server wants it before the first chunk): done stays at 0 meanwhile, because done
// is what the server holds and a bar that filled for the hash would have to jump back.
struct FabProgress {
    std::atomic<int64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
    std::atomic<int> finishing_polls{0};   // an upload: how often the server answered "still checking" (all sent by then)
};

// Publishes zip_path (a new mod when mod_id is 0, else a new version of that mod) with meta_json (the upload's meta
// object: name, game, type, version, short_desc, long_desc, tags, notes ...) and 1-6 screenshots (PNG / JPEG).
// The zip is read a chunk at a time, never whole. An upload that stopped (cancelled, closed, offline) is remembered in
// <state dir>\uploads.json and the next call for the same file (same path, size and SHA-256) on the same server goes on
// from the chunks the server already has; an expired session starts again. Once every chunk is there, POST finish starts
// the server's checks and GET /uploads/:id is asked every 2 s until it says done (published) or failed (the problems;
// the session is then deleted). A lost answer to the finish is followed by that GET before anything else; a remembered
// session the server is finishing, or has published, is followed and never sent again. A 429 waits for its Retry-After
// (2 s without one, 60 s at most a time, 30 min in all) and is not counted as a failure. "" on success (*mod_out = the
// mod's id), else the problems, one per line ("cancelled" when cancelled). Whether the session is kept for a resume
// afterwards: fab_upload_remembered. A failed finish the server marks retryable (its own error, a restart) keeps the
// session: publishing again finishes it again. Before starting a session for a new version, and whenever a remembered
// session is gone, GET /creator/mods is asked whether this zip (its SHA-256) is live already in that mod (a new mod:
// the creator's mod of this name); if so that is the success, and nothing is uploaded again.
std::string fab_upload(const ServerUrl& server, const SocialAccount& account, const std::string& meta_json, int64_t mod_id,
                       const std::string& zip_path, const std::vector<std::string>& screenshot_paths, FabProgress& progress,
                       int64_t* mod_out);

// Whether a session of this zip on this server and account is remembered (a publish of it again resumes).
bool fab_upload_remembered(const ServerUrl& server, const SocialAccount& account, const std::string& zip_path);
// Gives up the remembered sessions of this zip: each is deleted on the server (it stops counting toward the creator's
// daily uploads and quota) and forgotten. A session the server is still finishing answers 409 and is kept (false then).
bool fab_upload_discard(const ServerUrl& server, const SocialAccount& account, const std::string& zip_path);

// Downloads url (a full address: fab_full_url) to dest_path through <dest>.part, going on from an existing part file
// with a Range request (the part is hashed again first). <dest>.part.json beside it holds the sha256 and size the part
// belongs to: a part of any other file (or without one) is deleted and the download starts at 0, and so is one the
// server's 206 does not continue exactly (start or total size differ). The file is renamed to dest only when its size and SHA-256
// match; on a mismatch the part file is deleted and "hash mismatch" returned. An expired address or a mod taken down
// answers a problem starting "address expired" (ask POST /mods/:id/download for a new one). "" on success.
std::string fab_download(const std::string& url, const std::string& dest_path, const std::string& sha256, int64_t size,
                         FabProgress& progress);

// The download answer's url made a full address: a path ("/api/v1/fabricator/files/...") goes after server_url's
// scheme, host and port; a full http(s) address is kept.
std::string fab_full_url(const std::string& server_url, const std::string& url);

// Where uploads.json is kept: <COGBox>\fabricator (the launcher's folder) unless set (tests).
std::string fab_state_dir();
void fab_set_state_dir(const std::string& dir);
// Tests only: for a request drop(method, path) answers true for, the request is sent and its answer thrown away (as if
// the connection broke before the answer came). Empty: none.
void fab_set_test_drop_answer(std::function<bool(const std::string& method, const std::string& path)> drop);
// Tests only: every text answer (status != 0) is passed to f, which may replace its status and body. Empty: none.
void fab_set_test_rewrite_answer(std::function<void(const std::string& method, const std::string& path, int& status, std::string& body)> f);
