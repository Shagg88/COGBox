// COGBox updates - Windows-free parts (see update_logic.h). Checked by test/update_logic_test.cpp.
#include "update_logic.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

namespace cogbox_update {

std::string source_page(const Source &s)
{
    if (s.kind == SourceKind::SourceForge) return std::string("https://sourceforge.net/projects/") + s.project + "/files/";
    if (s.kind == SourceKind::GitLab) return std::string("https://gitlab.com/") + s.project + "/-/releases";
    return std::string("https://github.com") + s.project + "/releases/latest";
}

bool version_newer(const std::string &a, const std::string &b)
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
        if (!*p) return false;   // "1.2" vs "1.2.1": b has more (non-zero handled by the next round being 0 vs y)
        if (!*q) { while (*p == '0' || *p == '.') p++; return *p >= '1' && *p <= '9'; }
    }
    return false;
}

static bool ieq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// version + extension right at s (after "COGBox-"); returns the length used, 0 if not a release name
static size_t version_at(const char *s, std::string *ver, std::string *ext)
{
    const char *p = s;
    int parts = 0;
    std::string v;
    while (isdigit((unsigned char)*p)) {
        while (isdigit((unsigned char)*p)) v += *p++;
        parts++;
        bool ext7z = ieq(p, ".7z", 3) && !isalnum((unsigned char)p[3]);   // "0.4.9.7z": the 7 is the extension's
        if (*p == '.' && isdigit((unsigned char)p[1]) && parts < 4 && !ext7z) { v += *p++; continue; }
        break;
    }
    if (parts < 1) return 0;
    const char *e = nullptr;
    if (ieq(p, ".7z", 3)) e = ".7z";
    else if (ieq(p, ".zip", 4)) e = ".zip";
    if (!e) return 0;
    p += strlen(e);
    if (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') return 0;   // .7z.sha256, .7z.001, .zipx ...
    if (ver) *ver = v;
    if (ext) *ext = e;
    return (size_t)(p - s);
}

std::string version_from_name(const std::string &name, std::string *ext)
{
    for (size_t at = 0; (at = name.find("COGBox-", at)) != std::string::npos; at++) {
        if (at > 0 && (isalnum((unsigned char)name[at - 1]))) continue;
        std::string v, e;
        if (version_at(name.c_str() + at + 7, &v, &e)) { if (ext) *ext = e; return v; }
    }
    return "";
}

static bool is_hex(char c) { return isxdigit((unsigned char)c) != 0; }

// the first run of exactly n hex digits in s[from, to)
static std::string hex_run(const std::string &s, size_t from, size_t to, size_t n)
{
    to = std::min(to, s.size());
    for (size_t i = from; i + n <= to; i++) {
        if (!is_hex(s[i]) || (i > 0 && is_hex(s[i - 1]))) continue;
        size_t j = i;
        while (j < to && is_hex(s[j])) j++;
        if (j - i == n) {
            std::string h = s.substr(i, n);
            for (char &c : h) c = (char)tolower((unsigned char)c);
            return h;
        }
        i = j;
    }
    return "";
}

std::vector<SfFile> sf_parse_listing(const std::string &raw)
{
    std::string text = raw;   // JSON may write / as \/
    for (size_t at; (at = text.find("\\/")) != std::string::npos;) text.erase(at, 1);
    std::vector<SfFile> out;
    for (size_t at = 0; (at = text.find("COGBox-", at)) != std::string::npos; at += 7) {
        if (at > 0 && isalnum((unsigned char)text[at - 1])) continue;
        SfFile f;
        size_t len = version_at(text.c_str() + at + 7, &f.version, &f.ext);
        if (!len) continue;
        size_t end = at + 7 + len;
        // the whole word the name is the end of: a path ("/0.4.9/COGBox-0.4.9.7z") or an address (".../files/0.4.9/...")
        size_t start = at;
        while (start > 0 && !strchr("\"'[]<> \t\r\n,", text[start - 1])) start--;
        std::string word = text.substr(start, end - start);
        size_t files = word.find("/files/");
        if (word.compare(0, 4, "http") == 0) {
            if (files == std::string::npos) continue;   // a mirror's address etc.: not a path in Files
            word = word.substr(files + 6);
        }
        if (word.empty() || word[0] != '/') word = "/" + word;
        f.path = word;
        // the md5 of the same RSS item / JSON object
        size_t item = text.rfind("<item", at), itemEnd = text.rfind("</item>", at);
        size_t segA, segB;
        if (item != std::string::npos && (itemEnd == std::string::npos || itemEnd < item)) {
            segA = item;
            segB = text.find("</item>", at);
        } else {
            size_t ob = text.rfind('{', at);
            segA = ob == std::string::npos ? 0 : ob;
            segB = text.find('}', at);
        }
        if (segB == std::string::npos) segB = text.size();
        size_t m = text.find("md5", segA);
        if (m != std::string::npos && m < segB) f.md5 = hex_run(text, m + 3, segB, 32);
        bool merged = false;
        for (auto &o : out)
            if (o.path == f.path) { if (o.md5.empty()) o.md5 = f.md5; merged = true; }
        if (!merged) out.push_back(f);
    }
    return out;
}

bool sf_pick_latest(const std::vector<SfFile> &files, SfFile *out)
{
    const SfFile *best = nullptr;
    for (auto &f : files) {
        if (!best || version_newer(f.version, best->version) ||
            (!version_newer(best->version, f.version) && best->ext != ".7z" && f.ext == ".7z"))
            best = &f;
    }
    if (!best) return false;
    *out = *best;
    if (out->md5.empty())
        for (auto &f : files) if (f.path == best->path && !f.md5.empty()) out->md5 = f.md5;
    return true;
}

std::string sf_best_release_url(const std::string &project) { return "https://sourceforge.net/projects/" + project + "/best_release.json"; }
std::string sf_rss_url(const std::string &project) { return "https://sourceforge.net/projects/" + project + "/rss?path=/"; }

std::vector<std::string> sf_download_urls(const std::string &project, const std::string &path)
{
    std::string p = path.empty() || path[0] != '/' ? "/" + path : path;
    return { "https://sourceforge.net/projects/" + project + "/files" + p + "/download",
             "https://downloads.sourceforge.net/project/" + project + p };
}

std::string parse_sha256_text(const std::string &text)
{
    if (looks_like_html(text)) return "";
    return hex_run(text, 0, text.size(), 64);
}

bool looks_like_html(const std::string &head)
{
    size_t i = 0;
    if (head.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < head.size() && isspace((unsigned char)head[i])) i++;
    if (i >= head.size() || head[i] != '<') return false;
    std::string h = head.substr(i, 64);
    for (char &c : h) c = (char)tolower((unsigned char)c);
    return h.compare(0, 9, "<!doctype") == 0 || h.compare(0, 5, "<html") == 0 || h.compare(0, 5, "<head") == 0 ||
           h.compare(0, 4, "<!--") == 0 || h.compare(0, 5, "<meta") == 0;
}

static std::string html_unescape(std::string s)
{
    static const char *from[] = { "&amp;", "&#38;", "&#x26;", "&quot;", "&#39;" }, *to[] = { "&", "&", "&", "\"", "'" };
    for (int k = 0; k < 5; k++)
        for (size_t at; (at = s.find(from[k])) != std::string::npos;) s.replace(at, strlen(from[k]), to[k]);
    return s;
}

std::string resolve_location(const std::string &base, const std::string &loc)
{
    if (loc.compare(0, 8, "https://") == 0 || loc.compare(0, 7, "http://") == 0) return loc;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return loc;
    if (loc.compare(0, 2, "//") == 0) return base.substr(0, scheme + 1) + loc;
    size_t hostEnd = base.find('/', scheme + 3);
    std::string origin = hostEnd == std::string::npos ? base : base.substr(0, hostEnd);
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    std::string dir = hostEnd == std::string::npos ? "/" : base.substr(hostEnd, base.find_first_of("?#", hostEnd) - hostEnd);
    dir = dir.substr(0, dir.rfind('/') + 1);
    return origin + dir + loc;
}

std::string html_next_url(const std::string &html, const std::string &base)
{
    std::string low = html;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    // <meta http-equiv="refresh" content="5; url=https://...">
    for (size_t at = 0; (at = low.find("http-equiv", at)) != std::string::npos; at++) {
        size_t tagA = low.rfind('<', at), tagB = low.find('>', at);
        if (tagA == std::string::npos || tagB == std::string::npos) break;
        std::string tag = low.substr(tagA, tagB - tagA);
        if (tag.find("refresh") == std::string::npos) continue;
        size_t u = tag.find("url=");
        if (u == std::string::npos) continue;
        size_t a = tagA + u + 4, b = a;
        if (a < html.size() && (html[a] == '\'' || html[a] == '"')) a++, b++;
        while (b < tagB && !strchr("\"'> ", html[b])) b++;
        std::string url = html_unescape(html.substr(a, b - a));
        if (!url.empty()) return resolve_location(base, url);
    }
    // else a link to a mirror
    for (const char *key : { ".dl.sourceforge.net/", "use_mirror=" }) {
        size_t at = low.find(key);
        while (at != std::string::npos) {
            size_t a = at;
            while (a > 0 && !strchr("\"'= <>", html[a - 1])) a--;
            size_t b = at;
            while (b < html.size() && !strchr("\"' <>", html[b])) b++;
            std::string url = html_unescape(html.substr(a, b - a));
            if (url.compare(0, 4, "http") == 0 || url[0] == '/') return resolve_location(base, url);
            at = low.find(key, at + 1);
        }
    }
    return "";
}

bool split_https_url(const std::string &url, std::string *host, int *port, std::string *path)
{
    if (url.compare(0, 8, "https://") != 0) return false;
    size_t h = 8, slash = url.find_first_of("/?#", h);
    std::string hp = url.substr(h, slash == std::string::npos ? std::string::npos : slash - h);
    if (hp.empty() || hp.find('@') != std::string::npos) return false;
    int prt = 443;
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        prt = atoi(hp.c_str() + colon + 1);
        hp = hp.substr(0, colon);
        if (prt <= 0 || prt > 65535) return false;
    }
    std::string pth = slash == std::string::npos ? "/" : url.substr(slash);
    size_t hash = pth.find('#');
    if (hash != std::string::npos) pth = pth.substr(0, hash);
    if (pth.empty() || pth[0] != '/') pth = "/" + pth;
    if (host) *host = hp;
    if (port) *port = prt;
    if (path) *path = pth;
    return true;
}

bool archive_magic_ok(const std::string &head, const std::string &ext)
{
    if (ext == ".7z") return head.size() >= 6 && memcmp(head.data(), "7z\xBC\xAF\x27\x1C", 6) == 0;
    if (ext == ".zip") return head.size() >= 2 && head[0] == 'P' && head[1] == 'K';
    return false;
}

bool update_protected_path(std::string rel)
{
    for (char &c : rel) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
    static const char *top[] = { "cogbox.ini", "settings.ini", "extra-args", "covers\\", "_old-files\\", "achievements\\" };
    for (const char *t : top) if (rel.compare(0, strlen(t), t) == 0) return true;
    static const char *inner[] = { "\\game\\", "\\user\\", "\\logs\\", "\\shadercache\\", "\\mods\\", "\\saves\\" };
    std::string withRoot = "\\" + rel;   // a game, user, logs or shadercache folder at the top counts too (review 13)
    for (const char *t : inner) if (withRoot.find(t) != std::string::npos) return true;
    return false;
}

} // namespace cogbox_update
