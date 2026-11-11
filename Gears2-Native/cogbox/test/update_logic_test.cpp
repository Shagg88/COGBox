// CogBox test of the 0.4.9 updater's Windows-free parts: version numbers, SourceForge listings (best_release.json +
// files RSS), .sha256 files, download pages, and .7z unpacking through the LZMA SDK.
//   gcc -O2 -c -DZ7_EXTRACT_ONLY ../third_party/lzma/*.c   (into some folder)
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src update_logic_test.cpp ../src/update_logic.cpp ../src/archive7z.cpp <lzma .o> -o update_logic_test
//   ./update_logic_test [fixtures dir]     (default: update_fixtures next to this file; made by update_fixtures/make.sh)
// Without the fixtures the .7z checks are skipped (and said so).
#include "update_logic.h"
#include "archive7z.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

using namespace cogbox_update;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a == _b) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); } } while (0)

static void versions()
{
    CHECK(version_newer("0.4.9", "0.4.8"));
    CHECK(!version_newer("0.4.8", "0.4.8"));
    CHECK(!version_newer("0.4.8", "0.4.9"));
    CHECK(version_newer("0.4.10", "0.4.9"));
    CHECK(version_newer("v1.0.0", "0.9.9"));
    CHECK(version_newer("0.5", "0.4.9"));
    CHECK(version_newer("0.4.8.1", "0.4.8"));
    CHECK(!version_newer("0.4.8", "0.4.8.1"));
    CHECK(!version_newer("0.4.8.0", "0.4.8"));
    CHECK(!version_newer("", "0.4.8"));
    CHECK(!version_newer("latest", "0.4.8"));
    std::string ext;
    CHECK_EQ(version_from_name("/0.4.9/COGBox-0.4.9.7z", &ext), "0.4.9");
    CHECK_EQ(ext, ".7z");
    CHECK_EQ(version_from_name("COGBox-0.4.7.zip", &ext), "0.4.7");
    CHECK_EQ(ext, ".zip");
    CHECK_EQ(version_from_name("COGBox-0.4.10.7z"), "0.4.10");
    CHECK_EQ(version_from_name("COGBox-0.4.9.7z.sha256"), "");
    CHECK_EQ(version_from_name("/0.4.9/README.md"), "");
    CHECK_EQ(version_from_name("NotCOGBox-0.4.9.7z"), "");
    CHECK_EQ(version_from_name("COGBox-0.4.9.7z.001"), "");
    CHECK_EQ(version_from_name("COGBox-.7z"), "");
    CHECK_EQ(version_from_name("cogbox-0.4.9-src.7z"), "");
}

// shaped like SourceForge's answers (best_release.json: "release" + "platform_releases"; files RSS with media:hash)
static const char kBestRelease[] =
    "{\"release\": {\"filename\": \"/0.4.8/COGBox-0.4.8.7z\", \"url\": \"https://sourceforge.net/projects/cogbox/files/0.4.8/"
    "COGBox-0.4.8.7z/download\", \"bytes\": 151283712, \"date\": \"2026-11-08 04:30:12\", \"md5sum\": "
    "\"0123456789ABCDEF0123456789abcdef\", \"mime_type\": \"application/x-7z-compressed; charset=binary\", \"sf_platform_default\": "
    "[\"windows\"]}, \"platform_releases\": {\"windows\": {\"filename\": \"\\/0.4.8\\/COGBox-0.4.8.7z\", \"md5sum\": "
    "\"0123456789abcdef0123456789abcdef\"}, \"mac\": {\"filename\": \"/0.4.8/README.md\", \"md5sum\": "
    "\"ffffffffffffffffffffffffffffffff\"}, \"linux\": null}}";
static const char kRss[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<rss xmlns:media=\"http://video.search.yahoo.com/mrss/\" version=\"2.0\"><channel>"
    "<title>COGBox</title><link>https://sourceforge.net</link>\n"
    "<item><title><![CDATA[/0.4.9/COGBox-0.4.9.7z.sha256]]></title><link>https://sourceforge.net/projects/cogbox/files/0.4.9/"
    "COGBox-0.4.9.7z.sha256/download</link><media:content url=\"https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.7z"
    ".sha256/download\" filesize=\"81\"><media:hash algo=\"md5\">11111111111111111111111111111111</media:hash></media:content></item>\n"
    "<item><title><![CDATA[/0.4.9/COGBox-0.4.9.7z]]></title><link>https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.7z"
    "/download</link><media:content url=\"https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.7z/download\" "
    "type=\"application/x-7z-compressed; charset=binary\" filesize=\"150000000\"><media:hash algo=\"md5\">"
    "22222222222222222222222222222222</media:hash></media:content></item>\n"
    "<item><title><![CDATA[/0.4.9/COGBox-0.4.9.zip]]></title><link>https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.zip"
    "/download</link><media:content url=\"x\"><media:hash algo=\"md5\">33333333333333333333333333333333</media:hash></media:content></item>\n"
    "<item><title><![CDATA[/0.4.8/COGBox-0.4.8.7z]]></title><link>https://sourceforge.net/projects/cogbox/files/0.4.8/COGBox-0.4.8.7z"
    "/download</link><media:content url=\"y\"><media:hash algo=\"md5\">44444444444444444444444444444444</media:hash></media:content></item>\n"
    "<item><title><![CDATA[/0.4.8/README.md]]></title><link>https://sourceforge.net/projects/cogbox/files/0.4.8/README.md/download</link></item>\n"
    "</channel></rss>";

static void listings()
{
    auto br = sf_parse_listing(kBestRelease);
    CHECK(br.size() == 1);
    if (!br.empty()) {
        CHECK_EQ(br[0].path, "/0.4.8/COGBox-0.4.8.7z");
        CHECK_EQ(br[0].version, "0.4.8");
        CHECK_EQ(br[0].md5, "0123456789abcdef0123456789abcdef");
    }
    SfFile f;
    CHECK(sf_pick_latest(br, &f) && f.version == "0.4.8");

    auto rss = sf_parse_listing(kRss);
    CHECK(rss.size() == 3);   // 0.4.9 .7z, 0.4.9 .zip, 0.4.8 .7z (not the .sha256, not README.md)
    CHECK(sf_pick_latest(rss, &f));
    CHECK_EQ(f.path, "/0.4.9/COGBox-0.4.9.7z");   // .7z over .zip of the same version
    CHECK_EQ(f.md5, "22222222222222222222222222222222");   // its own item's, not the .sha256 item's before it
    for (auto &x : rss) if (x.path == "/0.4.8/COGBox-0.4.8.7z") CHECK_EQ(x.md5, "44444444444444444444444444444444");

    // both listings together: 0.4.9 from the RSS even if best_release.json still says 0.4.8 (default flag not moved yet)
    auto all = br;
    all.insert(all.end(), rss.begin(), rss.end());
    CHECK(sf_pick_latest(all, &f) && f.version == "0.4.9");

    CHECK(sf_parse_listing("<html><body>Error 404</body></html>").empty());
    CHECK(sf_parse_listing("").empty());
    CHECK(!sf_pick_latest({}, &f));
    // a newest README default (Shagg uploads README.md last) gives nothing, not a wrong version
    CHECK(sf_parse_listing("{\"release\": {\"filename\": \"/0.4.9/README.md\", \"md5sum\": \"ab\"}}").empty());

    auto urls = sf_download_urls("cogbox", "/0.4.9/COGBox-0.4.9.7z");
    CHECK(urls.size() == 2);
    CHECK_EQ(urls[0], "https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.7z/download");
    CHECK_EQ(urls[1], "https://downloads.sourceforge.net/project/cogbox/0.4.9/COGBox-0.4.9.7z");
    CHECK_EQ(sf_best_release_url("cogbox"), "https://sourceforge.net/projects/cogbox/best_release.json");
    CHECK_EQ(sf_rss_url("cogbox"), "https://sourceforge.net/projects/cogbox/rss?path=/");
    CHECK_EQ(source_page(kSources[0]), "https://github.com/Shagg88/COGBox/releases/latest");   // 0.6.0: GitHub first
    CHECK(kSources[0].kind == SourceKind::GitHub && kSourceCount == 3 && kSources[1].kind == SourceKind::GitLab
          && kSources[2].kind == SourceKind::SourceForge);
    CHECK_EQ(source_page(kSources[1]), "https://gitlab.com/shagg88/cogbox/-/releases");
    CHECK_EQ(source_page(kSources[2]), "https://sourceforge.net/projects/cogbox/files/");
}

static void checksums_and_pages()
{
    const std::string h = "528b48c0d3de0705b4db655625521932e6a76a9d486ccb40d1e67ffade957223";
    CHECK_EQ(parse_sha256_text(h + "  COGBox-0.4.8.7z\n"), h);
    CHECK_EQ(parse_sha256_text(h + "\r\n"), h);
    CHECK_EQ(parse_sha256_text("528B48C0D3DE0705B4DB655625521932E6A76A9D486CCB40D1E67FFADE957223 *COGBox-0.4.8.7z"), h);
    CHECK_EQ(parse_sha256_text("\xEF\xBB\xBF" + h), h);
    CHECK_EQ(parse_sha256_text(h + "0"), "");           // 65 digits: not a sha256
    CHECK_EQ(parse_sha256_text(h.substr(0, 63)), "");
    CHECK_EQ(parse_sha256_text("<!DOCTYPE html><html>" + h + "</html>"), "");   // an interstitial page holding hex
    CHECK_EQ(parse_sha256_text(""), "");

    CHECK(looks_like_html("<!DOCTYPE html>\n<html>"));
    CHECK(looks_like_html("\r\n  <HTML lang=en>"));
    CHECK(!looks_like_html("7z\xBC\xAF\x27\x1C"));
    CHECK(!looks_like_html("PK\x03\x04"));
    CHECK(!looks_like_html(h));

    const std::string page = "https://sourceforge.net/projects/cogbox/files/0.4.9/COGBox-0.4.9.7z/download";
    CHECK_EQ(html_next_url("<html><head><meta http-equiv=\"refresh\" content=\"5; url=https://downloads.sourceforge.net/project/"
                           "cogbox/0.4.9/COGBox-0.4.9.7z?ts=123&amp;use_mirror=netix&amp;r=\"></head></html>", page),
             "https://downloads.sourceforge.net/project/cogbox/0.4.9/COGBox-0.4.9.7z?ts=123&use_mirror=netix&r=");
    CHECK_EQ(html_next_url("<html><META HTTP-EQUIV='Refresh' CONTENT='0;URL=/projects/cogbox/files/x/download?use_mirror=a'></html>", page),
             "https://sourceforge.net/projects/cogbox/files/x/download?use_mirror=a");
    CHECK_EQ(html_next_url("<html><a class=\"direct-download\" href=\"https://netix.dl.sourceforge.net/project/cogbox/0.4.9/"
                           "COGBox-0.4.9.7z?viasf=1\">direct link</a></html>", page),
             "https://netix.dl.sourceforge.net/project/cogbox/0.4.9/COGBox-0.4.9.7z?viasf=1");
    CHECK_EQ(html_next_url("<html><body>nothing here</body></html>", page), "");

    CHECK_EQ(resolve_location("https://a.b/x/y?z", "/p"), "https://a.b/p");
    CHECK_EQ(resolve_location("https://a.b/x/y?z", "q"), "https://a.b/x/q");
    CHECK_EQ(resolve_location("https://a.b/x", "//c.d/e"), "https://c.d/e");
    CHECK_EQ(resolve_location("https://a.b/x", "https://m.dl.sourceforge.net/f"), "https://m.dl.sourceforge.net/f");

    std::string host, path;
    int port = 0;
    CHECK(split_https_url("https://netix.dl.sourceforge.net/project/cogbox/a.7z?viasf=1", &host, &port, &path));
    CHECK_EQ(host, "netix.dl.sourceforge.net");
    CHECK(port == 443);
    CHECK_EQ(path, "/project/cogbox/a.7z?viasf=1");
    CHECK(split_https_url("https://h:8443", &host, &port, &path) && port == 8443 && path == "/");
    CHECK(!split_https_url("http://insecure.example/x", &host, &port, &path));
    CHECK(!split_https_url("https://user@evil/x", &host, &port, &path));
    CHECK(!split_https_url("ftp://x/y", &host, &port, &path));

    CHECK(archive_magic_ok(std::string("7z\xBC\xAF\x27\x1C\x00\x04", 8), ".7z"));
    CHECK(!archive_magic_ok("<!DOCTYPE html>", ".7z"));
    CHECK(archive_magic_ok("PK\x03\x04", ".zip"));
    CHECK(!archive_magic_ok("PK\x03\x04", ".7z"));
}

static std::string sha256_hex(const unsigned char *d, size_t n);

static void sevenzip(const std::string &dir)
{
    {
        std::ifstream probe(dir + "/solid-lzma2.7z");
        if (!probe) { std::printf("SKIP .7z checks: no fixtures in %s (update_fixtures/make.sh needs 7-Zip / p7zip)\n", dir.c_str()); return; }
    }
    const std::map<std::string, std::string> want = {
        { "COGBox-0.4.9/CogBox.exe", "58fb963b09a0aeafa4f2114e69f9c0e0c08639a1b5c5d4b81ec9421b253084aa" },
        { "COGBox-0.4.9/README.txt", "06386a610da7cb0304653d53ba4b8ab01285b3f6fd3e382d9f938d17691f6aa8" },
        { "COGBox-0.4.9/empty.bin", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "COGBox-0.4.9/licenses/\xC3\xBCml\xC3\xA4ut-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88.txt",
          "" /* checked below by its content */ },
    };
    for (const char *name : { "solid-lzma2.7z", "bcj2.7z" }) {
        Archive7z a;
        bool ok = a.open(dir + "/" + name);
        CHECK(ok);
        if (!ok) { std::printf("  %s: %s\n", name, a.error().c_str()); continue; }
        int files = 0, dirs = 0;
        bool sawEmptyDir = false;
        for (unsigned i = 0; i < a.count(); i++) {
            std::string n = a.name(i);
            if (a.is_dir(i)) { dirs++; if (n == "COGBox-0.4.9/emptydir") sawEmptyDir = true; continue; }
            files++;
            const unsigned char *d = nullptr;
            size_t sz = 0;
            bool got = a.extract(i, &d, &sz);
            CHECK(got);
            CHECK(sz == a.size(i));
            auto w = want.find(n);
            CHECK(w != want.end());
            if (w == want.end() || !got) { std::printf("  unexpected %s\n", n.c_str()); continue; }
            if (n.find("licenses/") != std::string::npos) CHECK(sz == 13 && std::memcmp(d, "unicode name\n", 13) == 0);
            else CHECK_EQ(sha256_hex(d, sz), w->second);
        }
        CHECK(files == 4);
        CHECK(dirs == 3);
        CHECK(sawEmptyDir);
    }
    {   // a flipped byte in the packed data: the CRC check catches it (or the header is refused) - never silently wrong
        Archive7z a;
        bool bad = false;
        if (!a.open(dir + "/corrupt.7z")) bad = true;
        else
            for (unsigned i = 0; i < a.count(); i++) {
                if (a.is_dir(i)) continue;
                const unsigned char *d;
                size_t sz;
                if (!a.extract(i, &d, &sz)) { bad = true; break; }
            }
        CHECK(bad);
    }
    {
        Archive7z a;
        CHECK(!a.open(dir + "/does-not-exist.7z"));
        std::ofstream(dir + "/../update_fixtures_tmp_notes.txt") << "not a 7z";
        CHECK(!a.open(dir + "/../update_fixtures_tmp_notes.txt"));
        std::remove((dir + "/../update_fixtures_tmp_notes.txt").c_str());
    }
}

// ---- a small SHA-256 for the test (the launcher uses Windows' BCrypt) --------------------------------------------
static std::string sha256_hex(const unsigned char *data, size_t len)
{
    static const unsigned k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    unsigned h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::string m((const char *)data, len);
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    unsigned long long bits = (unsigned long long)len * 8;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (i * 8));
    auto ror = [](unsigned x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        unsigned w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (unsigned)(unsigned char)m[off + 4 * i] << 24 | (unsigned)(unsigned char)m[off + 4 * i + 1] << 16 |
                   (unsigned)(unsigned char)m[off + 4 * i + 2] << 8 | (unsigned char)m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        unsigned a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            unsigned t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            unsigned t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : std::string(__FILE__).substr(0, std::string(__FILE__).find_last_of('/') + 1) + "update_fixtures";
    CHECK_EQ(sha256_hex((const unsigned char *)"abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    versions();
    listings();
    checksums_and_pages();
    sevenzip(dir);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
