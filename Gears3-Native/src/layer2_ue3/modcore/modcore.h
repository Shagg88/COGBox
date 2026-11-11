#pragma once
// modcore: Gears of War (Unreal Engine 3, Xbox 360) mod rules, independent of any runtime. MIT (modcore/LICENSE).
//
// A mod is a folder with a mod.ini and game files at the paths the disc uses (docs/MODDING.md). This header reads the
// manifests and decides the load order and which file wins; modcore.cpp merges mods' configuration fragments into the
// packed configuration (Coalesced_<LANG>.bin), writes a merged table of contents (Xbox360TOC*.txt) and hashes files.
// Serving the files to the game (a file system hook, an overlay device, override lists) and patching the executable's
// SHA-1 table are the runtime's: modcore never touches the game folder.
//
//   [mod]                                  (COGBox's [Mod] Name / Version / Author / Description read the same)
//   name, version, author, description     free text
//   priority = 0                           load order: lower first; equal: by name; the last one wins a file
//   enabled = true
//   side = everyone | host | client        who needs the mod in multiplayer (below; default everyone)
//   requires = Other Mod, Another          mods that must be enabled too (by name or folder), else this one is off
//   game = gears3                          the game it is made for (gears3 if not given); others are turned off
//   content = MPCH_COGMarcusG2MP_SF, ...   the game's own content packages the mod's levels use that the campaign
//                                          does not load (multiplayer characters...)
//
// side: "client" changes only what this player sees or hears (menus, HUD, text): never compared. "host" changes the
// rules the host's game applies (game modes' settings): it acts in matches you host and is not compared. "everyone"
// (packages, maps, weapon stats, anything both ends simulate): every player of a multiplayer session must have the
// same "everyone" mods; their fingerprint (ForMultiplayer) is compared when a player joins.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace modcore
{
    enum class Side { Everyone, Host, Client };

    inline const char* SideName(Side s) { return s == Side::Client ? "client" : s == Side::Host ? "host" : "everyone"; }

    // A campaign a mod adds (mod API level 1, campaigns.h): [Campaign] and its [Chapter.N] sections. Slots (act and
    // chapter numbers) are given out by AllocateCampaigns, never written by the mod.
    struct ChapterDecl
    {
        int number = 0;                              // N of [Chapter.N]: its place in the campaign and its "Chapter N"
        std::string map, title, location, time, screenshot;
    };
    struct CampaignDecl
    {
        std::string id;                              // stable: chapters of one campaign may come from several mods
        std::string title, screenshot;
        int squad_catch_up = 0;                      // SquadCatchUp = <distance>: an engine-difference adapter (0: off).
                                                     // A squad AI stuck that far from player 1 is brought behind them
                                                     // (converted levels whose AI paths do not hold, ue3/squad_catch_up)
        int max_players = 0;                         // MaxPlayers = 1-4: co-op players its chapters take (0: the
                                                     // game's 4; a campaign whose squad is two declares 2)
        int order = -1;                              // where it sorts among campaigns: the first Order any of its mods
                                                     // gives (100 when none does), then the mods' load order
    };

    struct Manifest
    {
        std::string folder;                          // the mod's folder name
        std::string name, version, author, description, game = "gears3";
        int priority = 0;
        bool enabled = true;
        Side side = Side::Everyone;
        std::vector<std::string> needs;          // requires = ...
        std::vector<std::string> content;        // content = ... (package names; chapters' ContentPackages join it)
        int api = 0;                                 // api = the mod API level it was made for (0: before campaigns)
        CampaignDecl campaign;                       // [Campaign] (id empty: none)
        std::vector<ChapterDecl> chapters;           // [Chapter.N], in N order
        // [ClassDefaults] Class.Property = value: a class default the game uses while one of the mods' campaign
        // chapters is loaded (an engine-difference adapter, ue3/mod_chapters; bool, int, float or byte properties)
        struct ClassDefault { std::string cls, prop, value; };
        std::vector<ClassDefault> class_defaults;
        std::vector<std::string> warnings;           // keys not understood, chapters left out (the mod still loads)
        std::string problem;                         // why a mod that is enabled in its mod.ini is not loaded
        std::filesystem::path dir;
        bool loose = false;                          // files placed directly in mods\ (no mod.ini)
        bool Active() const { return enabled && problem.empty(); }
    };

    inline std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    inline std::string Trim(const std::string& s)
    {
        const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    inline Manifest Read(const std::filesystem::path& dir)
    {
        Manifest m;
        m.dir = dir;
        m.folder = m.name = dir.filename().string();
        std::ifstream in(dir / "mod.ini");
        std::string line;
        bool first = true;
        // [Campaign] and [Chapter.N] hold the campaign's keys; any other section (or none) the mod's, as before them
        enum { ModKeys, CampaignKeys, ChapterKeys, ClassDefaultKeys, IgnoredKeys } in_section = ModKeys;
        size_t chapter = 0;                          // m.chapters index while in a [Chapter.N]
        auto list_into = [](const std::string& value, std::vector<std::string>& list) {
            size_t a = 0;
            while (a <= value.size())
            {
                const size_t b = (std::min)(value.find(',', a), value.size());
                const std::string r = Trim(value.substr(a, b - a));
                if (!r.empty() && std::find(list.begin(), list.end(), r) == list.end()) list.push_back(r);
                a = b + 1;
            }
        };
        while (std::getline(in, line))
        {
            if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB) line.erase(0, 3);   // UTF-8 BOM
            first = false;
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[')
            {
                const std::string sec = Lower(Trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
                in_section = ModKeys;
                if (sec == "campaign") in_section = CampaignKeys;
                else if (sec == "classdefaults") in_section = ClassDefaultKeys;
                else if (sec.rfind("chapter.", 0) == 0)
                {
                    const std::string num = sec.substr(8);
                    const int n = atoi(num.c_str());
                    if (n <= 0 || num.empty() || num.find_first_not_of("0123456789") != std::string::npos)
                    {
                        m.warnings.push_back("[" + line.substr(1, line.find(']') - 1) + "]: a chapter's number must be 1 or more - left out");
                        in_section = IgnoredKeys;
                    }
                    else
                    {
                        chapter = m.chapters.size();
                        for (size_t i = 0; i < m.chapters.size(); i++) if (m.chapters[i].number == n) chapter = i;
                        if (chapter == m.chapters.size()) { m.chapters.push_back(ChapterDecl{}); m.chapters.back().number = n; }
                        in_section = ChapterKeys;
                    }
                }
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1)), low = Lower(value);
            if (in_section == CampaignKeys)
            {
                if (key == "id") m.campaign.id = low;
                else if (key == "title") m.campaign.title = value;
                else if (key == "order") m.campaign.order = atoi(value.c_str());
                else if (key == "screenshot") m.campaign.screenshot = value;
                else if (key == "squadcatchup") m.campaign.squad_catch_up = (std::max)(0, atoi(value.c_str()));
                else if (key == "maxplayers")
                {
                    const int n = atoi(value.c_str());
                    if (n >= 1 && n <= 4) m.campaign.max_players = n;
                    else m.warnings.push_back("[Campaign] MaxPlayers = " + value + ": must be 1 to 4 - ignored");
                }
                else m.warnings.push_back("[Campaign] " + Trim(line.substr(0, eq)) + ": not a campaign key - ignored");
                continue;
            }
            if (in_section == IgnoredKeys) continue;
            if (in_section == ClassDefaultKeys)
            {
                const std::string raw = Trim(line.substr(0, eq));
                const size_t dot = raw.find('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 >= raw.size() || value.empty())
                    m.warnings.push_back("[ClassDefaults] " + raw + ": must be Class.Property = value - ignored");
                else
                    m.class_defaults.push_back({raw.substr(0, dot), raw.substr(dot + 1), value});
                continue;
            }
            if (in_section == ChapterKeys)
            {
                ChapterDecl& c = m.chapters[chapter];
                if (key == "map") c.map = value;
                else if (key == "title") c.title = value;
                else if (key == "location") c.location = value;
                else if (key == "time") c.time = value;
                else if (key == "screenshot") c.screenshot = value;
                else if (key == "contentpackages") list_into(value, m.content);
                else m.warnings.push_back("[Chapter." + std::to_string(c.number) + "] " + Trim(line.substr(0, eq)) + ": not a chapter key - ignored");
                continue;
            }
            if (key == "api") m.api = atoi(value.c_str());
            else if (key == "name") m.name = value;
            else if (key == "version") m.version = value;
            else if (key == "author") m.author = value;
            else if (key == "description") m.description = value;
            else if (key == "priority") m.priority = atoi(value.c_str());
            else if (key == "enabled") m.enabled = !(low == "false" || low == "0" || low == "no");
            else if (key == "side") m.side = low == "client" ? Side::Client : low == "host" || low == "server" ? Side::Host : Side::Everyone;
            else if (key == "game") m.game = low.empty() ? "gears3" : low;
            else if (key == "requires" || key == "content")
            {
                auto& list = key == "content" ? m.content : m.needs;
                size_t a = 0;
                while (a <= value.size())
                {
                    const size_t b = (std::min)(value.find(',', a), value.size());
                    const std::string r = Trim(value.substr(a, b - a));
                    if (!r.empty()) list.push_back(r);
                    a = b + 1;
                }
            }
        }
        std::sort(m.chapters.begin(), m.chapters.end(), [](const ChapterDecl& a, const ChapterDecl& b) { return a.number < b.number; });
        for (size_t i = 0; i < m.chapters.size();)
        {
            const ChapterDecl& c = m.chapters[i];
            std::string why;
            if (c.map.empty()) why = "has no Map";
            else if (m.campaign.id.empty()) why = "is in no campaign ([Campaign] Id = ...)";
            if (why.empty()) { i++; continue; }
            m.warnings.push_back("[Chapter." + std::to_string(c.number) + "] " + why + " - left out");
            m.chapters.erase(m.chapters.begin() + long(i));
        }
        return m;
    }

    inline void CheckProblems(std::vector<Manifest>& mods, const std::string& game);

    // every mod under a mods folder, in load order, with each one's problem set: made for another game, or a required
    // mod that is missing or off. Order: by priority, then name ("(loose files)" sorts by that name); or, when `order`
    // is given (a launcher's list of folder or mod names, first loaded first, as COGBox keeps it), the listed mods in
    // that order and the rest after them by priority and name. Mods made folder-only (no mod.ini) count as mods when
    // `folderMods` is set (COGBox: mod.ini is optional); else their files are "(loose files)".
    inline std::vector<Manifest> Scan(const std::filesystem::path& root, const std::string& game = "gears3",
                                      const std::vector<std::string>* order = nullptr, bool folderMods = false)
    {
        std::vector<Manifest> mods;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return mods;
        bool loose = false;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
        {
            if (e.is_directory(ec) && (folderMods || std::filesystem::is_regular_file(e.path() / "mod.ini", ec))) mods.push_back(Read(e.path()));
            else loose = true;
        }
        if (loose) { Manifest m; m.name = m.folder = "(loose files)"; m.dir = root; m.loose = true; mods.push_back(m); }
        auto rank = [&](const Manifest& m) -> size_t {
            if (!order) return SIZE_MAX;
            for (size_t i = 0; i < order->size(); i++)
                if (Lower((*order)[i]) == Lower(m.folder) || Lower((*order)[i]) == Lower(m.name)) return i;
            return SIZE_MAX;
        };
        std::stable_sort(mods.begin(), mods.end(), [&](const Manifest& a, const Manifest& b) {
            const size_t ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            return a.priority != b.priority ? a.priority < b.priority : a.name < b.name; });
        CheckProblems(mods, game);
        return mods;
    }

    // mods given as an explicit list of folders, already in load order (a launcher's list: COGBox's --ue3_mods), each
    // read from its mod.ini when it has one (a folder alone is a mod named after it), with problems set as Scan sets them
    inline std::vector<Manifest> FromFolders(const std::vector<std::filesystem::path>& dirs, const std::string& game = "gears3")
    {
        std::vector<Manifest> mods;
        for (const auto& d : dirs) mods.push_back(Read(d));
        CheckProblems(mods, game);
        return mods;
    }

    // made for another game, or a required mod that is missing or off
    inline void CheckProblems(std::vector<Manifest>& mods, const std::string& game)
    {
        for (auto& m : mods)
            if (m.game != game)
                m.problem = "made for " + std::string(m.game == "gears3" ? "Gears of War 3" : m.game == "gears2" ? "Gears of War 2" : m.game == "gears1" ? "Gears of War" : m.game);
        // requirements, repeated until nothing changes (a mod turned off can turn off the mods that need it)
        for (bool changed = true; changed;)
        {
            changed = false;
            for (auto& m : mods)
            {
                if (!m.Active()) continue;
                for (const auto& r : m.needs)
                {
                    const auto it = std::find_if(mods.begin(), mods.end(), [&](const Manifest& o) {
                        return Lower(o.name) == Lower(r) || Lower(o.folder) == Lower(r); });
                    if (it == mods.end()) m.problem = "needs the mod " + r + ", which is not installed";
                    else if (!it->Active()) m.problem = "needs the mod " + r + ", which is off";
                    if (!m.problem.empty()) { changed = true; break; }
                }
            }
        }
    }

    // where a mod's file goes in the game, from its path in the mod (lower case, backslashes). Mods made for the Xbox
    // 360 are often shared as the bare files, or inside the folders of the game's installation: a path holding a
    // GearGame folder is taken from there; a bare package, configuration or texture cache goes to the cooked folder
    // (GearGame\CookedXbox360; Gears 2: CookedXenon), a bare movie to GearGame\Movies. Empty: not a game file
    // (mod.ini, config fragments, TOCs, read-me files, programs).
    inline std::string GamePath(std::string rel, const std::string& cooked = "cookedxbox360")
    {
        rel = Lower(rel);
        if (rel == "mod.ini" || rel.rfind("config\\", 0) == 0) return {};
        const auto dot = rel.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : rel.substr(dot + 1);
        if (ext == "xex" || ext == "xexp" || ext == "exe" || ext == "dll") return {};   // programs never come from a mod
        if (const auto g = rel.rfind("geargame\\", 0) == 0 ? 0 : rel.find("\\geargame\\"); g != std::string::npos)
            rel = rel.substr(g == 0 ? 0 : g + 1);
        else
        {
            const std::string name = rel.substr(rel.find_last_of('\\') + 1);
            if (ext == "xxx" || ext == "upk" || ext == "tfc" || (ext == "bin" && name.rfind("coalesced_", 0) == 0)) rel = "geargame\\" + cooked + "\\" + name;
            else if (ext == "bik") rel = "geargame\\movies\\" + name;
            else return {};
        }
        if (rel.rfind("geargame\\xbox360toc", 0) == 0 && rel.size() > 4 && rel.substr(rel.size() - 4) == ".txt") return {};   // the runtime builds the TOC
        return rel;
    }

    // a mod's files: (game path, file on disk). The loose files do not descend into the manifest mods' folders.
    inline std::vector<std::pair<std::string, std::filesystem::path>> Files(const Manifest& m, const std::string& cooked = "cookedxbox360")
    {
        std::vector<std::pair<std::string, std::filesystem::path>> out;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(m.dir, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec) break;
            if (m.loose && it.depth() == 0 && it->is_directory(ec) && std::filesystem::is_regular_file(it->path() / "mod.ini", ec))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string game = GamePath(std::filesystem::relative(it->path(), m.dir, ec).string(), cooked);
            if (!game.empty()) out.emplace_back(game, it->path());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // a mod's configuration fragments (config\*.ini), by name
    inline std::vector<std::filesystem::path> Fragments(const Manifest& m)
    {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        if (m.loose) return out;
        for (const auto& e : std::filesystem::directory_iterator(m.dir / "config", ec))
            if (e.is_regular_file(ec) && Lower(e.path().extension().string()) == ".ini") out.push_back(e.path());
        std::sort(out.begin(), out.end());
        return out;
    }

    // what a multiplayer session compares: the "everyone" mods that are on, in load order. The fingerprint is FNV-1a
    // (64-bit) over "name|version|path:size;..." per mod (game files and fragments), so two players with the same mods
    // match whatever their folders are called; 0 when there are none. summary: "Name 1.0, Other 2" for messages.
    struct NetSet { uint64_t fingerprint = 0; std::string summary; };

    inline NetSet ForMultiplayer(const std::vector<Manifest>& mods)
    {
        NetSet s;
        std::string text;
        for (const auto& m : mods)
        {
            if (!m.Active() || m.side != Side::Everyone) continue;
            std::error_code ec;
            text += m.name + "|" + m.version + "|";
            for (const auto& [game, file] : Files(m)) text += game + ":" + std::to_string(std::filesystem::file_size(file, ec)) + ";";
            for (const auto& f : Fragments(m)) text += "config\\" + Lower(f.filename().string()) + ":" + std::to_string(std::filesystem::file_size(f, ec)) + ";";
            text += "\n";
            s.summary += (s.summary.empty() ? "" : ", ") + m.name + (m.version.empty() ? "" : " " + m.version);
        }
        if (text.empty()) return s;
        uint64_t h = 0xCBF29CE484222325ull;
        for (unsigned char c : text) { h ^= c; h *= 0x100000001B3ull; }
        s.fingerprint = h ? h : 1;
        return s;
    }

    // ---- modcore.cpp ----

    using Log = std::function<void(const std::string&)>;

    // The active mods' files (game path -> the winning file: the last mod in load order) and fragments (load order),
    // and the paths more than one mod provides (game path -> mod names, load order).
    struct Index
    {
        std::map<std::string, std::filesystem::path> files;
        std::vector<std::pair<std::string, std::filesystem::path>> fragments;   // (mod name, config\*.ini)
        std::map<std::string, std::vector<std::string>> conflicts;
    };
    Index BuildIndex(const std::vector<Manifest>& mods, const std::string& cooked = "cookedxbox360");

    // Gears 3 reads every .ini from Coalesced_<LANG>.bin (big-endian int32 file count; per file a string name and
    // int32 section count; per section a string name and int32 key count; per key two strings; a string is int32 n then
    // n ANSI bytes, or -n UTF-16LE units, NUL included). Merge applies fragments in order to `base` (the disc's file or
    // a mod's whole file): '##### <ini file>' picks the packed file whose name ends with it, then [Section] and
    //   Key=v   replaces every value of Key (the first such line in the section; later plain lines add values)
    //   +Key=v  adds v unless present        -Key=v  removes v
    //   *Key=v  replaces the value with the same first field ("(Identifier=X,...)": a menu entry), else adds v
    // Sections and files that do not exist are created; the result is written in UTF-16 throughout. Empty when `base`
    // is not a packed configuration.
    std::vector<uint8_t> MergeConfig(const std::vector<uint8_t>& base,
                                     const std::vector<std::pair<std::string, std::filesystem::path>>& fragments, const Log& log = {});

    // A table of contents (Xbox360TOC*.txt: "<size> <uncompressed size> <path> <md5>" per line) with the lines of the
    // files in `modded` (lower-case file name -> size and MD5 of what will be served) replaced, and, when `addNew` (the
    // main TOC; the language TOCs list localized files only), lines added for names it lacks, under `newFolder`
    // ("..\GearGame\CookedXbox360\"). Lines end in CRLF as the disc's do.
    struct TocFile { uint64_t size = 0; std::string md5; std::string name; };
    std::string MergeToc(const std::string& toc, const std::map<std::string, TocFile>& modded, bool addNew,
                         const std::string& newFolder = "..\\GearGame\\CookedXbox360\\", int* changed = nullptr, int* added = nullptr);

    // hashes (no platform library needed)
    std::string Md5Hex(const uint8_t* data, size_t size);
    std::string Md5HexFile(const std::filesystem::path& file, uint64_t* size);   // empty when it cannot be read
    std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size);
}
