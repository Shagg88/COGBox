// CogBox test of dlc_dupes (GitHub #283: a DLC package another installed package already covers).
//   g++ -std=c++17 -O1 -Wall -Wextra -I../src dlc_dupes_test.cpp ../src/dlc_dupes.cpp ../src/x360fs.cpp -o dlc_dupes_test
//   ./dlc_dupes_test [work_dir]      (work_dir is emptied first and removed at the end)
// Checked: identical packages keep one; a bundle covers the single packs inside it; the same paths and sizes with other
// contents, a partial overlap and an empty package are never marked; paths compare without case; a chain of
// supersets ends at one kept package; and, over many random sets, a package is only ever covered by one that is kept
// (so removing every marked package never removes the last copy of any file).
// r22 review: a package folder that is a link, or a link inside a package, never takes part (B1); names that differ
// only in case make a package ineligible and content keys follow the real spelling (B2); a removal needs whole-file
// SHA-1s of both packages, so a difference outside the marker's samples is caught (R2); an incomplete scan (limit,
// unreadable folder, odd file) is never marked (R3); verify() passes only the shown pairs still marked the same way (R4).
#include "dlc_dupes.h"

#include <cstdio>
#include <set>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <fstream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using dlcdup::Package;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// in-memory packages: path -> contents
struct Mem { std::string name; std::map<std::string, std::string> files; };

static std::vector<int> run(const std::vector<Mem> &m)
{
    std::vector<Package> pk(m.size());
    for (size_t i = 0; i < m.size(); i++) {
        pk[i].name = m[i].name;
        for (auto &kv : m[i].files) { pk[i].files.push_back({ kv.first, kv.second.size(), "" }); pk[i].bytes += kv.second.size(); }
    }
    return dlcdup::find_covered(pk, [&](int a, const dlcdup::File &fa, int b, const dlcdup::File &fb) {
        return fa.path == fb.path && m[a].files.at(fa.path) == m[b].files.at(fb.path);
    });
}

static void write(const fs::path &p, const std::string &s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

int main(int argc, char **argv)
{
    // 1. two identical packages (a re-release under another content ID): the first by name stays
    {
        std::vector<Mem> m = { { "BBB", { { "dlc/skin.ini", "omen" }, { "dlc/skin.xxx", "0123456789" } } },
                               { "AAA", { { "dlc/skin.ini", "omen" }, { "dlc/skin.xxx", "0123456789" } } } };
        auto c = run(m);
        CHECK(c[1] == -1 && c[0] == 1);
    }
    // 2. a bundle holding two single packs: both packs are covered by the bundle, the bundle stays
    {
        std::vector<Mem> m = { { "PACK1", { { "a.ini", "chrome" } } },
                               { "BUNDLE", { { "a.ini", "chrome" }, { "b.ini", "vintage" }, { "c.ini", "flame" } } },
                               { "PACK2", { { "b.ini", "vintage" } } } };
        auto c = run(m);
        CHECK(c[0] == 1 && c[2] == 1 && c[1] == -1);
    }
    // 3. the same paths and sizes, other contents (another version of the same pack): not a duplicate
    {
        std::vector<Mem> m = { { "OLD", { { "dlc.ini", "version1" } } }, { "NEW", { { "dlc.ini", "version2" } } } };
        auto c = run(m);
        CHECK(c[0] == -1 && c[1] == -1);
    }
    // 4. a partial overlap: neither covers the other
    {
        std::vector<Mem> m = { { "X", { { "common.ini", "c" }, { "x.xxx", "xx" } } }, { "Y", { { "common.ini", "c" }, { "y.xxx", "yyy" } } } };
        auto c = run(m);
        CHECK(c[0] == -1 && c[1] == -1);
    }
    // 5. an empty package is never marked and covers nothing
    {
        std::vector<Mem> m = { { "EMPTY", {} }, { "EMPTY2", {} }, { "P", { { "p.ini", "p" } } } };
        auto c = run(m);
        CHECK(c[0] == -1 && c[1] == -1 && c[2] == -1);
    }
    // 6. a chain (A in B in C) and B's twin: everything ends at C, the only package kept
    {
        std::vector<Mem> m = { { "A", { { "1", "a" } } }, { "B", { { "1", "a" }, { "2", "bb" } } }, { "B2", { { "1", "a" }, { "2", "bb" } } },
                               { "C", { { "1", "a" }, { "2", "bb" }, { "3", "ccc" } } } };
        auto c = run(m);
        CHECK(c[3] == -1 && c[0] == 3 && c[1] == 3 && c[2] == 3);
    }
    // 7. random sets: a package is only covered by a kept one, and every file of a covered one is in its cover
    {
        std::mt19937 rng(283);
        const char *contents[] = { "a", "b", "cc", "dd" };
        for (int round = 0; round < 3000; round++) {
            std::vector<Mem> m(2 + rng() % 7);
            for (size_t i = 0; i < m.size(); i++) {
                m[i].name = "P" + std::to_string(i);
                for (int f = 0; f < 5; f++) if (rng() % 2) m[i].files["f" + std::to_string(f)] = contents[rng() % 4];
            }
            auto c = run(m);
            for (size_t i = 0; i < m.size(); i++) {
                if (c[i] < 0) continue;
                CHECK(c[c[i]] == -1);
                CHECK(!m[i].files.empty());
                for (auto &kv : m[i].files) {
                    auto it = m[c[i]].files.find(kv.first);
                    CHECK(it != m[c[i]].files.end() && it->second == kv.second);
                }
            }
        }
    }
    // 8. on disk: scan + content_key, the way the launcher calls them (paths without case; a big file is sampled)
    {
        fs::path w = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "cogbox_dlc_dupes_test";
        std::error_code ec;
        fs::remove_all(w, ec);
        write(w / "SINGLE" / "GearGame" / "DLC" / "Skin.ini", "[Skin]\nName=Omen\n");
        write(w / "BUNDLE" / "geargame" / "dlc" / "skin.ini", "[Skin]\nName=Omen\n");
        write(w / "BUNDLE" / "geargame" / "dlc" / "other.ini", "[Skin]\nName=Chrome\n");
        write(w / "OTHER" / "GearGame" / "DLC" / "Skin.ini", "[Skin]\nName=Gold\n");   // same size, other contents
        std::vector<std::string> names = { "SINGLE", "BUNDLE", "OTHER" };
        std::vector<Package> pk(names.size());
        for (size_t i = 0; i < names.size(); i++) { pk[i].name = names[i]; CHECK(dlcdup::scan(w / names[i], pk[i])); }
        CHECK(pk[0].files.size() == 1 && pk[0].files[0].path == "geargame/dlc/skin.ini");
        CHECK(pk[1].files.size() == 2 && pk[1].bytes == pk[0].bytes + 19);
        CHECK(pk[0].files[0].disk == "GearGame/DLC/Skin.ini");
        auto c = dlcdup::find_covered(pk, [&](int a, const dlcdup::File &fa, int b, const dlcdup::File &fb) {
            const std::string ka = dlcdup::content_key(w / names[a] / fa.disk, fa.size), kb = dlcdup::content_key(w / names[b] / fb.disk, fb.size);
            return !ka.empty() && ka == kb;
        });
        CHECK(c[0] == 1 && c[1] == -1 && c[2] == -1);
        CHECK(dlcdup::content_key(w / "missing", 5).empty());
        Package none;
        CHECK(!dlcdup::scan(w / "missing", none));
        // a file over 64 MB (sparse): a change inside a sample is seen
        const uint64_t big = (70ull << 20) + 123;
        auto make_big = [&](const fs::path &p, uint64_t at, char v) {
            { std::ofstream o(p, std::ios::binary); }
            fs::resize_file(p, big);
            std::fstream o(p, std::ios::binary | std::ios::in | std::ios::out);
            o.seekp((std::streamoff)at);
            o.put(v);
        };
        make_big(w / "big1", big / 2, 'x');
        make_big(w / "big2", big / 2, 'y');
        make_big(w / "big3", big / 2, 'x');
        const std::string k1 = dlcdup::content_key(w / "big1", big), k2 = dlcdup::content_key(w / "big2", big), k3 = dlcdup::content_key(w / "big3", big);
        CHECK(!k1.empty() && k1 != k2 && k1 == k3);
        fs::remove_all(w, ec);
    }
    // 9. r22 review cases, on disk
    {
        fs::path w = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "cogbox_dlc_dupes_test") / "r22";
        std::error_code ec;
        fs::remove_all(w, ec);
        using Dirs = std::vector<std::pair<std::string, fs::path>>;
        auto dirs_of = [&](std::vector<std::string> names) { Dirs d; for (auto &n : names) d.push_back({ n, w / n }); return d; };
        auto cover_name = [](const Dirs &d, const std::vector<int> &c, size_t i) { return c[i] < 0 ? std::string("-") : d[c[i]].first; };
        dlcdup::KeyCache kc;
        std::vector<Package> pk;
        // B1: a package folder that is a symlink to another (the link sorts first): neither is marked
        write(w / "b1" / "ZREAL" / "dlc" / "a.ini", "real contents");
        fs::create_directory_symlink(w / "b1" / "ZREAL", w / "b1" / "ALINK", ec);
        if (!ec) {
            Dirs d = { { "ALINK", w / "b1" / "ALINK" }, { "ZREAL", w / "b1" / "ZREAL" } };
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(c[0] == -1 && c[1] == -1);
            CHECK(!pk[0].eligible && pk[0].files.empty() && pk[1].eligible);
            CHECK(dlcdup::is_link(w / "b1" / "ALINK") && !dlcdup::is_link(w / "b1" / "ZREAL"));
            CHECK(dlcdup::same_physical(w / "b1" / "ALINK", w / "b1" / "ZREAL") == 1);
            // even if it were shown as a pair, verify never passes it (either way round)
            auto v = dlcdup::verify(d, { { "ZREAL", "ALINK" }, { "ALINK", "ZREAL" } }, kc);
            CHECK(v.ok.empty() && v.skipped.size() == 2);
            // a link inside a package (a file and a folder): that package is not eligible
            write(w / "b1" / "IN" / "dlc" / "a.ini", "real contents");
            fs::create_symlink(w / "b1" / "ZREAL" / "dlc" / "a.ini", w / "b1" / "IN" / "dlc" / "b.ini", ec);
            write(w / "b1" / "IN2" / "dlc" / "a.ini", "real contents");
            fs::create_directory_symlink(w / "b1" / "ZREAL" / "dlc", w / "b1" / "IN2" / "more", ec);
            write(w / "b1" / "BIG" / "dlc" / "a.ini", "real contents");
            write(w / "b1" / "BIG" / "dlc" / "b.ini", "real contents");
            write(w / "b1" / "BIG" / "more" / "a.ini", "real contents");
            Dirs d2 = { { "IN", w / "b1" / "IN" }, { "IN2", w / "b1" / "IN2" }, { "BIG", w / "b1" / "BIG" } };
            auto c2 = dlcdup::mark(d2, kc, pk);
            CHECK(!pk[0].eligible && !pk[1].eligible && pk[2].eligible);
            CHECK(c2[0] == -1 && c2[1] == -1 && c2[2] == -1);
            CHECK(pk[1].files.size() == 1);   // the link to a folder is not followed
        } else {
            std::printf("note: symlinks not available here (%s), B1 link cases skipped\n", ec.message().c_str());
            ec.clear();
        }
        // B1: one file under two names (a hard link) is not a second copy - verify refuses it
        write(w / "hl" / "A" / "x.ini", "hard linked");
        fs::create_directories(w / "hl" / "B");
        fs::create_hard_link(w / "hl" / "A" / "x.ini", w / "hl" / "B" / "x.ini", ec);
        if (!ec) {
            Dirs d = { { "A", w / "hl" / "A" }, { "B", w / "hl" / "B" } };
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(c[1] == 0);   // the marker sees the same contents
            auto v = dlcdup::verify(d, { { "B", "A" } }, kc);
            CHECK(v.ok.empty() && v.skipped.size() == 1);
        } else ec.clear();
        // B2: Foo.ini and foo.ini with other contents in one package (case-sensitive file systems: Proton / Wine)
        write(w / "b2" / "A" / "Foo.ini", "XXXX");
        write(w / "b2" / "A" / "foo.ini", "YYYY");
        write(w / "b2" / "B" / "foo.ini", "XXXX");
        write(w / "b2" / "B" / "pad.ini", "padding");
        if (fs::exists(w / "b2" / "A" / "Foo.ini") && fs::file_size(w / "b2" / "A" / "foo.ini") == 4 && dlcdup::content_key(w / "b2" / "A" / "foo.ini", 4) != dlcdup::content_key(w / "b2" / "A" / "Foo.ini", 4)) {
            Dirs d = dirs_of({ "b2/A", "b2/B" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(!pk[0].eligible && c[0] == -1 && c[1] == -1);
            CHECK(pk[0].files.size() == 2);
            // the cache follows the real spelling: the two names give their own keys
            CHECK(kc.key(w / "b2" / "A" / "Foo.ini", 4) != kc.key(w / "b2" / "A" / "foo.ini", 4));
            auto v = dlcdup::verify(d, { { "b2/A", "b2/B" } }, kc);
            CHECK(v.ok.empty());
        } else std::printf("note: case-insensitive file system here, B2 collision case skipped\n");
        // R1: the content keys are cached; a second marking reads nothing again, a changed file is read again
        write(w / "r1" / "A" / "a.ini", "same");
        write(w / "r1" / "B" / "a.ini", "same");
        {
            dlcdup::KeyCache k1;
            Dirs d = dirs_of({ "r1/A", "r1/B" });
            auto c = dlcdup::mark(d, k1, pk);
            CHECK(c[1] == 0);
            const size_t r = k1.reads();
            CHECK(r == 2);
            c = dlcdup::mark(d, k1, pk);
            CHECK(c[1] == 0 && k1.reads() == r);
        }
        // R2: two 70 MB files that differ outside the marker's samples: marked, but verify (whole files) refuses
        const uint64_t big = (70ull << 20) + 123;
        auto make_big = [&](const fs::path &p, uint64_t at, char v) {
            fs::create_directories(p.parent_path());
            { std::ofstream o(p, std::ios::binary); }
            fs::resize_file(p, big);
            std::fstream o(p, std::ios::binary | std::ios::in | std::ios::out);
            o.seekp((std::streamoff)at);
            o.put(v);
        };
        make_big(w / "r2" / "OLD" / "Textures.tfc", 10ull << 20, 'a');
        make_big(w / "r2" / "NEW" / "Textures.tfc", 10ull << 20, 'b');
        make_big(w / "r2" / "SAME" / "Textures.tfc", 10ull << 20, 'a');
        {
            Dirs d = dirs_of({ "r2/NEW", "r2/OLD", "r2/SAME" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(c[0] == -1 && c[1] == 0 && c[2] == 0);   // the marker can't tell them apart (samples)
            uint64_t last = 0, total = 0;
            auto v = dlcdup::verify(d, { { "r2/OLD", "r2/NEW" }, { "r2/SAME", "r2/NEW" } }, kc, [&](uint64_t dn, uint64_t t, const std::string &) {
                last = dn; total = t;
                return true;
            });
            CHECK(v.ok.empty() && v.skipped.size() == 2 && !v.cancelled);
            CHECK(total == 4 * big && last == total);
            // with SAME kept (NEW gone), OLD and SAME really are identical: verify passes them
            Dirs d2 = dirs_of({ "r2/OLD", "r2/SAME" });
            c = dlcdup::mark(d2, kc, pk);
            CHECK(c[0] == -1 && c[1] == 0);
            v = dlcdup::verify(d2, { { "r2/SAME", "r2/OLD" } }, kc);
            CHECK(v.ok.size() == 1 && v.ok[0].name == "r2/SAME" && v.ok[0].cover == "r2/OLD");
            // cancelled: nothing passes
            v = dlcdup::verify(d2, { { "r2/SAME", "r2/OLD" } }, kc, [](uint64_t, uint64_t, const std::string &) { return false; });
            CHECK(v.cancelled && v.ok.empty());
        }
        fs::remove_all(w / "r2", ec);
        // R3: any limit / error / odd file makes a package ineligible (never marked)
        write(w / "r3" / "COVER" / "a.ini", "a");
        write(w / "r3" / "COVER" / "b.ini", "b");
        write(w / "r3" / "COVER" / "c.ini", "c");
        write(w / "r3" / "MANY" / "a.ini", "a");
        write(w / "r3" / "MANY" / "b.ini", "b");
        {
            Package p;
            CHECK(dlcdup::scan(w / "r3" / "MANY", p, 1) && !p.eligible);
            CHECK(dlcdup::scan(w / "r3" / "MANY", p, 2) && p.eligible && p.files.size() == 2);
        }
#ifndef _WIN32
        write(w / "r3" / "FIFO" / "a.ini", "a");
        if (mkfifo((w / "r3" / "FIFO" / "pipe").c_str(), 0600) == 0) {
            Dirs d = dirs_of({ "r3/COVER", "r3/FIFO" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(!pk[1].eligible && c[1] == -1);
        }
        write(w / "r3" / "LOCKED" / "a.ini", "a");
        write(w / "r3" / "LOCKED" / "sub" / "b.ini", "b");
        if (geteuid() != 0 && chmod((w / "r3" / "LOCKED" / "sub").c_str(), 0) == 0) {
            Dirs d = dirs_of({ "r3/COVER", "r3/LOCKED" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(!pk[1].eligible && c[1] == -1);   // the unreadable folder is not skipped silently
            chmod((w / "r3" / "LOCKED" / "sub").c_str(), 0700);
        } else std::printf("note: running as root, unreadable-folder case skipped\n");
#endif
        {
            Dirs d = dirs_of({ "r3/COVER", "r3/MANY" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(c[1] == 0);   // without a limit hit, MANY is in COVER
        }
        // R4: only the shown pairs, and only while still marked with the same cover
        write(w / "r4" / "BUNDLE" / "a.ini", "a");
        write(w / "r4" / "BUNDLE" / "b.ini", "b");
        write(w / "r4" / "BUNDLE" / "c.ini", "c");
        write(w / "r4" / "P1" / "a.ini", "a");
        write(w / "r4" / "P2" / "b.ini", "b");
        write(w / "r4" / "P3" / "c.ini", "c");
        {
            Dirs d = dirs_of({ "r4/BUNDLE", "r4/P1", "r4/P2", "r4/P3" });
            auto c = dlcdup::mark(d, kc, pk);
            CHECK(cover_name(d, c, 1) == "r4/BUNDLE" && cover_name(d, c, 2) == "r4/BUNDLE" && cover_name(d, c, 3) == "r4/BUNDLE");
            // shown: P1 and P2 (P3 came later / wasn't shown); P2 changes before Remove; P9 is gone; P1 with a wrong cover
            write(w / "r4" / "P2" / "b.ini", "B");
            auto v = dlcdup::verify(d, { { "r4/P1", "r4/BUNDLE" }, { "r4/P2", "r4/BUNDLE" }, { "r4/P9", "r4/BUNDLE" }, { "r4/P3", "r4/P1" } }, kc);
            CHECK(v.ok.size() == 1 && v.ok[0].name == "r4/P1" && v.ok[0].cover == "r4/BUNDLE");
            CHECK(v.skipped.size() == 3);
            for (const auto &o : v.ok) CHECK(o.name != "r4/P3");
            // a bigger package added after the question: the shown cover is no longer the cover - skipped
            write(w / "r4" / "MEGA" / "a.ini", "a");
            write(w / "r4" / "MEGA" / "b.ini", "b");
            write(w / "r4" / "MEGA" / "c.ini", "c");
            write(w / "r4" / "MEGA" / "d.ini", "d");
            Dirs d2 = dirs_of({ "r4/BUNDLE", "r4/MEGA", "r4/P1" });
            v = dlcdup::verify(d2, { { "r4/P1", "r4/BUNDLE" } }, kc);
            CHECK(v.ok.empty() && v.skipped.size() == 1);
            // the cover itself is never passed as a duplicate
            v = dlcdup::verify(d2, { { "r4/MEGA", "r4/BUNDLE" }, { "r4/BUNDLE", "r4/MEGA" } }, kc);
            CHECK(v.ok.size() == 1 && v.ok[0].name == "r4/BUNDLE" && v.ok[0].cover == "r4/MEGA");
        }
        fs::remove_all(w, ec);
    }
    std::printf(g_fail ? "dlc_dupes_test: %d FAILED\n" : "dlc_dupes_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
