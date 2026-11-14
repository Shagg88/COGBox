// CogBox test of dlc_dupes (GitHub #283: a DLC package another installed package already covers).
//   g++ -std=c++17 -O1 -Wall -Wextra -I../src dlc_dupes_test.cpp ../src/dlc_dupes.cpp ../src/x360fs.cpp -o dlc_dupes_test
//   ./dlc_dupes_test [work_dir]      (work_dir is emptied first and removed at the end)
// Checked: identical packages keep one; a bundle covers the single packs inside it; the same paths and sizes with other
// contents, a partial overlap and an empty package are never marked; paths compare without case; a chain of
// supersets ends at one kept package; and, over many random sets, a package is only ever covered by one that is kept
// (so removing every marked package never removes the last copy of any file).
#include "dlc_dupes.h"

#include <cstdio>
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
    std::printf(g_fail ? "dlc_dupes_test: %d FAILED\n" : "dlc_dupes_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
