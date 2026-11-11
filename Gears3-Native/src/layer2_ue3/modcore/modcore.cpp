// modcore (modcore.h): the index of the mods' files, configuration fragments, merged tables of contents, hashes.
// MIT (modcore/LICENSE).
#include "modcore.h"
#include "campaigns.h"
#include <cstring>
#include <set>
#include <sstream>

namespace modcore
{
Index BuildIndex(const std::vector<Manifest>& mods, const std::string& cooked)
{
    Index ix;
    std::map<std::string, std::vector<std::string>> owners;
    for (const Manifest& m : mods)
    {
        if (!m.Active()) continue;
        for (const auto& [rel, path] : Files(m, cooked))
        {
            ix.files[rel] = path;                                   // later (higher priority) wins
            owners[rel].push_back(m.name);
        }
        for (const auto& f : Fragments(m)) ix.fragments.emplace_back(m.name, f);
    }
    for (auto& [rel, who] : owners)
        if (who.size() > 1) ix.conflicts[rel] = who;
    return ix;
}

// ---- configuration ----

namespace
{
    struct Section { std::u16string name; std::vector<std::pair<std::u16string, std::u16string>> keys; };
    struct File { std::u16string name; std::vector<Section> sections; };

    struct Reader
    {
        const std::vector<uint8_t>& d; size_t p = 0; bool ok = true;
        int32_t I32()
        {
            if (p + 4 > d.size()) { ok = false; return 0; }
            const int32_t v = int32_t(uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3]);
            p += 4; return v;
        }
        std::u16string Str()
        {
            const int32_t n = I32();
            std::u16string s;
            if (n > 0)
            {
                if (p + size_t(n) > d.size()) { ok = false; return s; }
                for (int32_t i = 0; i + 1 < n; i++) s += char16_t(d[p + i]);
                p += size_t(n);
            }
            else if (n < 0)
            {
                const size_t units = size_t(-int64_t(n));
                if (p + 2 * units > d.size()) { ok = false; return s; }
                for (size_t i = 0; i + 1 < units; i++) s += char16_t(d[p + 2 * i] | d[p + 2 * i + 1] << 8);
                p += 2 * units;
            }
            return s;
        }
    };

    bool Parse(const std::vector<uint8_t>& d, std::vector<File>& files)
    {
        Reader r{ d };
        const int32_t n = r.I32();
        for (int32_t f = 0; f < n && r.ok; f++)
        {
            File file; file.name = r.Str();
            const int32_t ns = r.I32();
            for (int32_t s = 0; s < ns && r.ok; s++)
            {
                Section sec; sec.name = r.Str();
                const int32_t nk = r.I32();
                for (int32_t k = 0; k < nk && r.ok; k++) { auto key = r.Str(); auto value = r.Str(); sec.keys.emplace_back(std::move(key), std::move(value)); }
                file.sections.push_back(std::move(sec));
            }
            files.push_back(std::move(file));
        }
        return r.ok && r.p == d.size();
    }

    void PutI32(std::vector<uint8_t>& o, int32_t v)
    {
        o.push_back(uint8_t(uint32_t(v) >> 24)); o.push_back(uint8_t(uint32_t(v) >> 16));
        o.push_back(uint8_t(uint32_t(v) >> 8)); o.push_back(uint8_t(v));
    }
    void PutStr(std::vector<uint8_t>& o, const std::u16string& s)
    {
        if (s.empty()) { PutI32(o, 0); return; }
        PutI32(o, -int32_t(s.size() + 1));
        for (char16_t c : s) { o.push_back(uint8_t(c)); o.push_back(uint8_t(c >> 8)); }
        o.push_back(0); o.push_back(0);
    }
    std::vector<uint8_t> Write(const std::vector<File>& files)
    {
        std::vector<uint8_t> o;
        PutI32(o, int32_t(files.size()));
        for (const File& f : files)
        {
            PutStr(o, f.name); PutI32(o, int32_t(f.sections.size()));
            for (const Section& s : f.sections)
            {
                PutStr(o, s.name); PutI32(o, int32_t(s.keys.size()));
                for (const auto& [k, v] : s.keys) { PutStr(o, k); PutStr(o, v); }
            }
        }
        return o;
    }

    // UTF-8 to UTF-16 (fragments are UTF-8 text)
    std::u16string Widen(const std::string& s)
    {
        std::u16string out;
        for (size_t i = 0; i < s.size();)
        {
            const unsigned char c = (unsigned char)s[i];
            uint32_t cp; size_t n;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F); n = 2; }
            else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F); n = 3; }
            else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F); n = 4; }
            else { cp = 0xFFFD; n = 1; }
            if (cp >= 0x10000) { cp -= 0x10000; out += char16_t(0xD800 + (cp >> 10)); out += char16_t(0xDC00 + (cp & 0x3FF)); }
            else out += char16_t(cp);
            i += n;
        }
        return out;
    }
    std::u16string LowerW(std::u16string s)
    {
        for (char16_t& c : s) { if (c >= u'A' && c <= u'Z') c = char16_t(c + 32); if (c == u'/') c = u'\\'; }
        return s;
    }
    std::u16string FirstField(const std::u16string& v) { const auto c = v.find(u','); return c == std::u16string::npos ? v : v.substr(0, c); }

    // one fragment onto the parsed files; returns the number of key lines applied (problems are logged)
    int Apply(std::vector<File>& files, const std::filesystem::path& fragment, const std::string& modName, const Log& log)
    {
        std::ifstream in(fragment, std::ios::binary);
        std::string line;
        File* file = nullptr; Section* sec = nullptr;
        std::vector<std::u16string> replaced;
        int applied = 0, lineNo = 0;
        while (std::getline(in, line))
        {
            lineNo++;
            if (lineNo == 1 && line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) line.erase(0, 3);
            line = Trim(line);
            if (line.empty() || line[0] == ';' || (line[0] == '#' && line.rfind("#####", 0) != 0)) continue;
            auto bad = [&](const char* why) { if (log) log(modName + " " + U8(fragment.filename()) + ":" + std::to_string(lineNo) + ": " + why); };
            if (line.rfind("#####", 0) == 0)
            {
                const std::u16string want = LowerW(Widen(Trim(line.substr(5))));
                file = nullptr;
                for (File& f : files)
                {
                    const std::u16string n = LowerW(f.name);
                    if (n.size() >= want.size() && n.compare(n.size() - want.size(), want.size(), want) == 0) { file = &f; break; }
                }
                if (!file) { files.push_back({ u"..\\..\\GearGame\\Config\\" + Widen(Trim(line.substr(5))), {} }); file = &files.back(); }
                sec = nullptr; continue;
            }
            if (!file) { bad("no '##### <ini file>' line before this"); continue; }
            if (line.front() == '[' && line.back() == ']')
            {
                const std::u16string name = Widen(line.substr(1, line.size() - 2));
                sec = nullptr;
                // UE3's config is case-blind: [Objectives] is the game's [OBJECTIVES] (a second section only differing in
                // case hid the first one's keys from the game: the stock campaign's objectives showed as <?INT?...?>)
                for (Section& s : file->sections) if (LowerW(s.name) == LowerW(name)) { sec = &s; break; }
                if (!sec) { file->sections.push_back({ name, {} }); sec = &file->sections.back(); }
                replaced.clear(); continue;
            }
            const auto eq = line.find('=');
            if (!sec || eq == std::string::npos) { bad("expected [section] or key=value"); continue; }
            const char op = (line[0] == '+' || line[0] == '-' || line[0] == '*') ? line[0] : 0;
            const std::u16string key = Widen(Trim(line.substr(op ? 1 : 0, eq - (op ? 1 : 0)))), value = Widen(line.substr(eq + 1));
            const std::u16string lowKey = LowerW(key);   // keys are case-blind too
            auto& keys = sec->keys;
            auto same = [&](const auto& kv) { return LowerW(kv.first) == lowKey; };
            if (op == '+')
            {
                if (std::find_if(keys.begin(), keys.end(), [&](const auto& kv) { return same(kv) && kv.second == value; }) == keys.end())
                    keys.emplace_back(key, value);
            }
            else if (op == '-')
                keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const auto& kv) { return same(kv) && kv.second == value; }), keys.end());
            else if (op == '*')
            {
                auto it = std::find_if(keys.begin(), keys.end(), [&](const auto& kv) { return same(kv) && FirstField(kv.second) == FirstField(value); });
                if (it == keys.end()) keys.emplace_back(key, value); else it->second = value;
            }
            else
            {
                if (std::find(replaced.begin(), replaced.end(), lowKey) == replaced.end())
                {
                    keys.erase(std::remove_if(keys.begin(), keys.end(), same), keys.end());
                    replaced.push_back(lowKey);
                }
                keys.emplace_back(key, value);
            }
            applied++;
        }
        return applied;
    }
}

std::vector<uint8_t> MergeConfig(const std::vector<uint8_t>& base, const std::vector<std::pair<std::string, std::filesystem::path>>& fragments,
                                 const Log& log)
{
    std::vector<File> files;
    if (base.empty() || !Parse(base, files)) return {};
    for (const auto& [mod, frag] : fragments)
    {
        const int n = Apply(files, frag, mod, log);
        if (log) log("config " + U8(frag.filename()) + " from " + mod + ": " + std::to_string(n) + " lines");
    }
    return Write(files);
}

// ---- table of contents ----

std::string MergeToc(const std::string& toc, const std::map<std::string, TocFile>& modded, bool addNew, const std::string& newFolder,
                     int* changed, int* added)
{
    std::vector<std::string> lines;
    {
        std::istringstream in(toc);
        std::string line;
        while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); lines.push_back(line); }
    }
    std::set<std::string> seen;
    for (auto& line : lines)
    {
        std::istringstream ls(line);
        std::string size, zero, path, md5;
        if (!(ls >> size >> zero >> path >> md5)) continue;
        const std::string name = Lower(path.substr(path.find_last_of('\\') + 1));
        if (auto it = modded.find(name); it != modded.end())
        {
            line = std::to_string(it->second.size) + " 0 " + path + " " + it->second.md5;
            seen.insert(name);
        }
    }
    std::vector<std::string> extra;
    if (addNew)
        for (const auto& [name, f] : modded)
            if (!seen.count(name)) extra.push_back(std::to_string(f.size) + " 0 " + newFolder + (f.name.empty() ? name : f.name) + " " + f.md5);
    lines.insert(lines.begin(), extra.begin(), extra.end());
    if (changed) *changed = int(seen.size());
    if (added) *added = int(extra.size());
    std::string out;
    for (const auto& line : lines) out += line + "\r\n";
    return out;
}

// ---- hashes (RFC 1321 MD5, FIPS 180-1 SHA-1) ----

namespace
{
    struct Md5
    {
        uint32_t h[4] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };
        uint8_t buf[64]; size_t used = 0; uint64_t total = 0;
        static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
        void Block(const uint8_t* p)
        {
            static const uint32_t K[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
            static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
            uint32_t w[16];
            for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            for (int i = 0; i < 64; i++)
            {
                uint32_t f; int g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                const uint32_t t = d; d = c; c = b;
                b = b + Rol(a + f + K[i] + w[g], R[i]);
                a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        }
        void Update(const uint8_t* p, size_t n)
        {
            total += n;
            while (n)
            {
                const size_t k = (std::min)(n, size_t(64) - used);
                memcpy(buf + used, p, k); used += k; p += k; n -= k;
                if (used == 64) { Block(buf); used = 0; }
            }
        }
        std::string Hex()
        {
            const uint64_t bits = total * 8;
            const uint8_t one = 0x80, zero = 0;
            Update(&one, 1);
            while (used != 56) Update(&zero, 1);
            uint8_t len[8];
            for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (8 * i));
            Update(len, 8);
            static const char* hex = "0123456789abcdef";
            std::string s;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) { const uint8_t b = uint8_t(h[i] >> (8 * j)); s += hex[b >> 4]; s += hex[b & 15]; }
            return s;
        }
    };
}

std::string Md5Hex(const uint8_t* data, size_t size) { Md5 m; m.Update(data, size); return m.Hex(); }

std::string Md5HexFile(const std::filesystem::path& file, uint64_t* size)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Md5 m;
    std::vector<char> buf(1 << 20);
    uint64_t n = 0;
    while (in)
    {
        in.read(buf.data(), std::streamsize(buf.size()));
        const auto got = in.gcount();
        if (got <= 0) break;
        m.Update((const uint8_t*)buf.data(), size_t(got));
        n += uint64_t(got);
    }
    if (size) *size = n;
    return m.Hex();
}

std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size)
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    auto rol = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    std::vector<uint8_t> m(data, data + size);
    const uint64_t bits = uint64_t(size) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (8 * i)));
    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(m[off + 4 * i]) << 24 | uint32_t(m[off + 4 * i + 1]) << 16 | uint32_t(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
    return out;
}

// ---- campaigns (campaigns.h) ----

namespace
{
    std::string Narrow(const std::u16string& s)   // the config's names and numbers are ASCII
    {
        std::string o;
        for (char16_t c : s) o += c < 0x80 ? char(c) : '?';
        return o;
    }
    bool EndsWith(const std::string& s, const std::string& end)
    {
        return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    }
}

UsedSlots UsedCampaignSlots(const std::vector<uint8_t>& coalesced)
{
    UsedSlots u;
    std::vector<File> files;
    if (!Parse(coalesced, files)) return u;
    for (const File& f : files)
    {
        if (!EndsWith(Lower(Narrow(f.name)), "xbox360-geargame.ini")) continue;
        for (const Section& s : f.sections)
        {
            const std::string sec = Lower(Narrow(s.name));
            const bool act = EndsWith(sec, " gearcampaignactdata"), chapter = EndsWith(sec, " gearcampaignchapterdata");
            if (!act && !chapter) continue;
            for (const auto& [k, v] : s.keys)
            {
                const std::string key = Lower(Narrow(k));
                if (act && key == "actnum") u.acts.insert(atoi(Narrow(v).c_str()));
                if (chapter && key == "chapternum") u.chapters.insert(atoi(Narrow(v).c_str()));
            }
        }
    }
    return u;
}

std::string LocalizedGearGameFile(const std::vector<uint8_t>& coalesced)
{
    std::vector<File> files;
    if (Parse(coalesced, files))
        for (const File& f : files)
        {
            const std::string name = Narrow(f.name), low = Lower(name);
            const auto slash = low.find_last_of('\\');
            const std::string base = slash == std::string::npos ? low : low.substr(slash + 1);
            if (base.size() == 12 && base.rfind("geargame.", 0) == 0 && base != "geargame.ini") return name.substr(name.size() - 12);
        }
    return "GearGame.int";
}

std::string ActNumeral(int act)
{
    static const char* const r[] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII", "XIII",
                                     "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX" };
    return act >= 0 && act < 20 ? r[act] : std::to_string(act + 1);
}

Allocation AllocateCampaigns(const std::vector<Manifest>& mods, const UsedSlots& used)
{
    Allocation a;
    struct Pending { std::string id; int order; size_t first; PlacedCampaign c; };
    std::vector<Pending> pending;
    for (size_t i = 0; i < mods.size(); i++)
    {
        const Manifest& m = mods[i];
        if (!m.Active() || m.campaign.id.empty()) continue;
        auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& p) { return p.id == m.campaign.id; });
        if (it == pending.end())
        {
            pending.push_back(Pending{ m.campaign.id, -1, i, {} });
            it = pending.end() - 1;
            it->c.id = m.campaign.id;
        }
        PlacedCampaign& c = it->c;
        if (it->order < 0) it->order = m.campaign.order;
        if (c.title.empty()) c.title = m.campaign.title;
        if (c.screenshot.empty()) c.screenshot = m.campaign.screenshot;
        for (const ChapterDecl& d : m.chapters)
        {
            const auto twin = std::find_if(c.chapters.begin(), c.chapters.end(), [&](const PlacedChapter& p) { return p.decl.number == d.number; });
            if (twin != c.chapters.end())
            {
                a.problems.push_back(m.name + ": campaign " + c.id + " chapter " + std::to_string(d.number) + " is also " + twin->mod + "'s - left out");
                continue;
            }
            c.chapters.push_back(PlacedChapter{ 0, d, m.name });
        }
    }
    for (Pending& p : pending) if (p.order < 0) p.order = 100;
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& x, const Pending& y) {
        return x.order != y.order ? x.order < y.order : x.first < y.first; });
    int act = used.acts.empty() ? 0 : *used.acts.rbegin() + 1;
    int slot = kFirstModChapter;
    for (Pending& p : pending)
    {
        PlacedCampaign& c = p.c;
        std::stable_sort(c.chapters.begin(), c.chapters.end(), [](const PlacedChapter& x, const PlacedChapter& y) {
            return x.decl.number < y.decl.number; });
        if (c.title.empty()) c.title = c.id;
        std::vector<PlacedChapter> kept;
        for (PlacedChapter& ch : c.chapters)
        {
            while (slot <= kLastModChapter && used.chapters.count(slot)) slot++;
            if (slot > kLastModChapter)
            {
                a.problems.push_back(ch.mod + ": campaign " + c.id + " chapter " + std::to_string(ch.decl.number) +
                                     ": no free chapter slot (" + std::to_string(kFirstModChapter) + "-" +
                                     std::to_string(kLastModChapter) + " are taken) - left out");
                continue;
            }
            ch.slot = slot++;
            kept.push_back(ch);
        }
        c.chapters = kept;
        if (c.chapters.empty()) continue;
        c.act = act++;
        std::string line = "campaign " + c.id + " '" + c.title + "': act " + ActNumeral(c.act) + " (ActNum " +
                           std::to_string(c.act) + "), chapters " + std::to_string(c.chapters.front().slot);
        if (c.chapters.size() > 1) line += "-" + std::to_string(c.chapters.back().slot);
        a.log.push_back(line);
        a.campaigns.push_back(c);
    }
    return a;
}

std::string CampaignFragment(const Allocation& a, const std::string& localized)
{
    if (a.campaigns.empty()) return {};
    std::ostringstream o;
    o << "; written by the campaign registry (modcore campaigns.h) from the mods' [Campaign] / [Chapter.N]\n";
    o << "##### Xbox360-GearGame.ini\n";
    for (const PlacedCampaign& c : a.campaigns)
    {
        const std::string shot = !c.screenshot.empty() ? c.screenshot : c.chapters.front().decl.screenshot;
        o << "[Act" << c.act + 1 << " GearCampaignActData]\nActNum=" << c.act << "\nbRequiredActForGameCompletion=false\n";
        if (!shot.empty()) o << "ScreenshotPathName=" << shot << "\n";
        for (const PlacedChapter& ch : c.chapters)
        {
            o << "[Chapter" << ch.slot << " GearCampaignChapterData]\nChapterNum=" << ch.slot << "\nActNum=" << c.act
              << "\nMapName=" << ch.decl.map << "\n";
            const std::string cshot = !ch.decl.screenshot.empty() ? ch.decl.screenshot : shot;
            if (!cshot.empty()) o << "ScreenshotPathName=" << cshot << "\n";
            o << "IsAlwaysUnlocked=true\n";
        }
    }
    o << "##### " << localized << "\n";
    for (const PlacedCampaign& c : a.campaigns)
    {
        o << "[Act" << c.act + 1 << " GearCampaignActData]\nListName=" << ActNumeral(c.act) << ": " << c.title
          << "\nDisplayName=" << c.title << "\n";
        for (const PlacedChapter& ch : c.chapters)
        {
            const std::string title = ch.decl.title.empty() ? ch.decl.map : ch.decl.title;
            o << "[Chapter" << ch.slot << " GearCampaignChapterData]\nListName=" << ch.decl.number << ": " << title
              << "\nDisplayName=" << title << "\nChapterNumDesc=Chapter " << ch.decl.number << "\n";
            if (!ch.decl.location.empty()) o << "ChapterLocation=" << ch.decl.location << "\n";
            if (!ch.decl.time.empty()) o << "ChapterTime=" << ch.decl.time << "\n";
        }
    }
    return o.str();
}
}
