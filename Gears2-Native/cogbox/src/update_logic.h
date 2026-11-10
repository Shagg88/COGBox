// COGBox updates - the parts that need no Windows (so test/update_logic_test.cpp runs them on Linux): where releases
// come from, version numbers, reading SourceForge's file lists, checksum files and download pages.
#pragma once
#include <string>
#include <vector>

namespace cogbox_update {

// ---- WHERE UPDATES COME FROM: the one place to change ------------------------------------------------------------
// Asked in this order; the first that answers with a release wins. 0.4.9: GitHub (Shagg88) was suspended on Nov 7 2026,
// so SourceForge is first and GitHub stays as the fallback in case the account comes back.
enum class SourceKind { SourceForge, GitHub, GitLab };
struct Source {
    SourceKind kind;
    const char *name;      // for messages / logs
    const char *project;   // SourceForge: project name ("cogbox"); GitHub: "/owner/repo"; GitLab: "owner/repo"
};
static const Source kSources[] = {
    // 0.5.0 (Shagg 9 Nov): GitLab is COGBox's home and update source: releases at gitlab.com/shagg88/cogbox, the files
    // in its generic package registry (packages/generic/cogbox/<version>/COGBox-<version>.7z + .sha256). SourceForge
    // stays as the fallback while it still exists (a source that doesn't answer is simply skipped).
    { SourceKind::GitLab, "GitLab", "shagg88/cogbox" },
    { SourceKind::SourceForge, "SourceForge", "cogbox" },
};
static const int kSourceCount = (int)(sizeof kSources / sizeof kSources[0]);

// the page players can download from by hand (the first source's)
std::string source_page(const Source &s);

// ---- versions ------------------------------------------------------------------------------------------------------
// a newer than b ("0.4.10" > "0.4.9"; a leading v is ignored; up to 4 parts); false if either isn't a number
bool version_newer(const std::string &a, const std::string &b);
// "X.Y.Z" (1 to 4 numeric parts) of a release file name, wherever it sits: "/0.4.9/COGBox-0.4.9.7z" -> "0.4.9".
// Only COGBox-<version>.7z and COGBox-<version>.zip count (not .7z.sha256, README.md, other names). ext gets ".7z"/".zip".
std::string version_from_name(const std::string &name, std::string *ext = nullptr);

// ---- SourceForge ---------------------------------------------------------------------------------------------------
struct SfFile {
    std::string version;   // "0.4.9"
    std::string path;      // "/0.4.9/COGBox-0.4.9.7z" (inside the project's Files)
    std::string ext;       // ".7z" or ".zip"
    std::string md5;       // lower-case hex, "" if the listing didn't give one
};
// Every release file named in a SourceForge listing: best_release.json ({"release": {"filename": "/0.4.9/COGBox-0.4.9.7z",
// "md5sum": ...}, "platform_releases": {"windows": {...}}}) or the files RSS (rss?path=/: <item><title>/0.4.9/COGBox-0.4.9.7z
// </title>... <media:hash algo="md5">...</media:hash></item>). The md5 is taken only from the same JSON object / RSS item.
std::vector<SfFile> sf_parse_listing(const std::string &text);
// the newest release in one or more listings (.7z over .zip of the same version; an md5 from any listing is kept)
bool sf_pick_latest(const std::vector<SfFile> &files, SfFile *out);
std::string sf_best_release_url(const std::string &project);   // https://sourceforge.net/projects/<p>/best_release.json
std::string sf_rss_url(const std::string &project);            // https://sourceforge.net/projects/<p>/rss?path=/
// where to download <path> from, in the order to try: the files/.../download page (redirects to a mirror), then
// downloads.sourceforge.net/project/... (the same mirror redirect without the project page in between)
std::vector<std::string> sf_download_urls(const std::string &project, const std::string &path);

// ---- downloads -----------------------------------------------------------------------------------------------------
// the first 64-hex-digit word of a .sha256 file ("<hash>  COGBox-0.4.9.7z" or the hash alone), lower case; "" if none
std::string parse_sha256_text(const std::string &text);
// whether a download's first bytes are a web page (SourceForge's "your download will start shortly" page) and not a file
bool looks_like_html(const std::string &head);
// The address an HTML download page sends the browser on to: <meta http-equiv="refresh" content="5; url=...">, else the
// first link to a *.dl.sourceforge.net mirror or with use_mirror=. &amp; decoded; relative addresses made absolute with
// base. "" if none.
std::string html_next_url(const std::string &html, const std::string &base);
// a redirect's Location made absolute against the address it came from
std::string resolve_location(const std::string &base, const std::string &location);
// https://host[:port]/path?query -> host, port (443 default), path ("/..." with query). Only https is accepted.
bool split_https_url(const std::string &url, std::string *host, int *port, std::string *path);
// whether a file starts like the archive its name says (7z: 37 7A BC AF 27 1C; zip: PK)
bool archive_magic_ok(const std::string &head, const std::string &ext);

// ---- installing ----------------------------------------------------------------------------------------------------
// rel: an archive entry's path relative to the COGBox folder ('\\' separated). True for the player's own things, which
// an update never writes even if an archive held them: CogBox.ini, settings.ini, extra-args*.txt, covers\, _old-files\,
// achievements\, and any game, user, logs, shadercache, mods or saves folder.
bool update_protected_path(std::string rel);

} // namespace cogbox_update
