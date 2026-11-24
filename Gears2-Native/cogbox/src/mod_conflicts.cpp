// COGBox - mod conflicts: the pure part (see mod_conflicts.h). Portable: no Windows, no App.
#include "mod_conflicts.h"
#include "mod_file_rules.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string key_of(const std::string &rel)   // as the runtime's RelKey: '\', ASCII lower case
{
    std::string k = rel;
    for (char &c : k) c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    while (!k.empty() && k[0] == '\\') k.erase(0, 1);
    return k;
}
bool ends_with(const std::string &s, const char *t)
{
    const size_t n = strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}
// mod_file_is_program for a lower-case path, through a set (it is asked for every file of every mod)
bool is_program(const std::string &k)
{
    static const std::unordered_set<std::string> exts(mod_file_program_exts().begin(), mod_file_program_exts().end());
    size_t end = k.size();
    while (end && (k[end - 1] == '.' || k[end - 1] == ' ')) end--;
    const size_t dot = k.find_last_of(".\\", end ? end - 1 : 0);
    if (dot == std::string::npos || k[dot] != '.' || end == 0) return false;
    return exts.count(k.substr(dot + 1, end - dot - 1)) > 0;
}

// a pair's tally: the count and the first `max` names in name order (kept small while tens of thousands go by)
struct Tally {
    uint32_t count = 0;
    std::vector<std::string> winner, loser;   // each side's own paths
};
void keep_first(std::vector<std::string> &v, const std::string &s, size_t max)
{
    if (max == 0) return;
    if (v.size() >= max && !(s < v.back())) return;
    v.insert(std::upper_bound(v.begin(), v.end(), s), s);
    if (v.size() > max) v.pop_back();
}

// one mod's place on a path or hash: its index and the path in it
struct Owner { size_t mod; const std::string *rel; };

template <class Key>
void tally(const std::unordered_map<Key, std::vector<Owner>> &owners, std::map<std::pair<size_t, size_t>, Tally> &pairs, size_t max)
{
    for (const auto &kv : owners) {
        const std::vector<Owner> &o = kv.second;
        if (o.size() < 2) continue;
        const Owner &w = o.back();   // the last in load order wins
        for (size_t i = 0; i + 1 < o.size(); i++) {
            Tally &t = pairs[{ w.mod, o[i].mod }];
            t.count++;
            keep_first(t.winner, *w.rel, max);
            keep_first(t.loser, *o[i].rel, max);
        }
    }
}

}  // namespace

std::string mod_conflict_game_path(const std::string &rel, const ModConflictRules &r)
{
    std::string k = key_of(rel);
    if (k.empty()) return {};
    if (r.fragments && (k == "config" || k.rfind("config\\", 0) == 0)) return {};   // fragments: merged, not replaced
    if (is_program(k)) return {};   // (the launcher's list, wider than the runtime's .xex .xexp .dll .exe: see the .h)
    if (k.find('\\') == std::string::npos) {
        // a file at the mod's top: its own (mod.ini, read-me, fabricator.json), or a bare Xbox 360 file (modcore GamePath)
        if (r.cooked.empty()) return {};
        const size_t dot = k.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : k.substr(dot + 1);
        if (ext == "xxx" || ext == "upk" || ext == "tfc" || (ext == "bin" && k.rfind("coalesced_", 0) == 0)) k = "geargame\\" + r.cooked + "\\" + k;
        else if (ext == "bik") k = "geargame\\movies\\" + k;
        else return {};
    }
    const size_t cut = k.find('\\');
    const std::string leaf = k.substr(cut + 1);
    if (leaf.find('\\') == std::string::npos && leaf.find("toc") != std::string::npos && ends_with(leaf, ".txt")) return {};   // its own TOC
    if (!r.game_tops.empty() && !r.game_tops.count(k.substr(0, cut))) return {};   // a folder the game hasn't: not used
    return k;
}

bool mod_conflict_texture_hash(const std::string &rel, uint64_t &hash)
{
    if (rel.size() < 20) return false;
    {   // most files aren't: the name's end first, before any copy
        const char *e = rel.c_str() + rel.size() - 4;
        if (e[0] != '.' || (e[1] | 0x20) != 'd' || (e[2] | 0x20) != 'd' || (e[3] | 0x20) != 's') return false;
    }
    const std::string k = key_of(rel);
    const size_t cut = k.find_last_of('\\');
    const std::string leaf = cut == std::string::npos ? k : k.substr(cut + 1);
    if (leaf.size() < 20 || !ends_with(leaf, ".dds") || (leaf[16] != '_' && leaf[16] != '.')) return false;
    uint64_t h = 0;
    for (size_t i = 0; i < 16; i++) {
        const char c = leaf[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        h = (h << 4) | (uint64_t)d;
    }
    // the folders the renderer walks: at most 6 deep, never into dump, .build or replace_<variant>
    int depth = 0;
    for (size_t at = 0; cut != std::string::npos && at <= cut;) {
        size_t e = k.find('\\', at);
        const std::string part = k.substr(at, e - at);
        if (part == "dump" || part == ".build" || part.rfind("replace_", 0) == 0 || ++depth > 6) return false;
        at = e + 1;
    }
    hash = h;
    return true;
}

std::vector<ModConflicts> mod_conflicts(const std::vector<ModFileList> &mods, const ModConflictRules &r)
{
    std::vector<ModConflicts> out(mods.size());
    std::unordered_map<std::string, std::vector<Owner>> files;
    std::unordered_map<uint64_t, std::vector<Owner>> textures;
    size_t total = 0;
    for (const ModFileList &m : mods) total += m.list().size();
    files.reserve(total);
    for (size_t i = 0; i < mods.size(); i++) {
        for (const std::string &rel : mods[i].list()) {
            uint64_t h = 0;
            if (mod_conflict_texture_hash(rel, h)) {   // a pack texture (even in a mod the game leaves out)
                std::vector<Owner> &o = textures[h];
                if (o.empty() || o.back().mod != i) o.push_back({ i, &rel });   // (twice in one pack: its first)
                continue;
            }
            if (!mods[i].overlay) continue;
            std::string k = mod_conflict_game_path(rel, r);
            if (k.empty()) continue;
            std::vector<Owner> &o = files[std::move(k)];
            if (o.empty() || o.back().mod != i) o.push_back({ i, &rel });   // (a bare file and its placed twin: one)
        }
    }
    std::map<std::pair<size_t, size_t>, Tally> fp, tp;   // (winner, loser)
    tally(files, fp, r.max_names);
    tally(textures, tp, r.max_names);
    auto put = [&](const std::map<std::pair<size_t, size_t>, Tally> &pairs, std::vector<ModConflictPair> ModConflicts::*win,
                   std::vector<ModConflictPair> ModConflicts::*lose) {
        for (const auto &kv : pairs) {   // (in winner, then loser order: each list ends up in the other's load order)
            const size_t w = kv.first.first, l = kv.first.second;
            (out[w].*win).push_back({ l, mods[l].name, kv.second.count, kv.second.winner });
            (out[l].*lose).push_back({ w, mods[w].name, kv.second.count, kv.second.loser });
        }
    };
    put(fp, &ModConflicts::replaces, &ModConflicts::replaced_by);
    put(tp, &ModConflicts::tex_wins, &ModConflicts::tex_loses);
    for (ModConflicts &c : out)
        for (auto *v : { &c.replaces, &c.replaced_by, &c.tex_wins, &c.tex_loses })
            std::sort(v->begin(), v->end(), [](const ModConflictPair &x, const ModConflictPair &y) { return x.other < y.other; });
    return out;
}
