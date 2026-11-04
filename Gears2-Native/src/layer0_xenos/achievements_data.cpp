// Layer 0 (game-free): COGBox's achievement data - see xenos/achievements_data.h. Standard C++17 only. Our code (MIT).
#include <xenos/achievements_data.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace xenos::ach {

const std::array<GameDef, 4> kGames = {{
    {"Gears1", 0x4D5307D5, "Gears of War"},
    {"Gears2", 0x4D53082D, "Gears of War 2"},
    {"Gears3", 0x4D5308AB, "Gears of War 3"},
    {"Judgment", 0x4D530A26, "Gears of War: Judgment"},
}};

const GameDef* FindGame(uint32_t title_id) {
  for (const GameDef& g : kGames) {
    if (g.title_id == title_id) return &g;
  }
  return nullptr;
}

namespace {

constexpr const char kCatalogHeader[] = "# COGBox achievement catalog ";

std::string Hex8(uint32_t v) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}

std::string Escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c != '\r') out += c;
  }
  return out;
}

std::string Unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[i + 1];
      if (n == '\\') { out += '\\'; ++i; continue; }
      if (n == 'n') { out += '\n'; ++i; continue; }
    }
    out += s[i];
  }
  return out;
}

// Whole-string unsigned number in the given base; false on anything else.
bool ParseU32(const std::string& s, int base, uint32_t* out) {
  if (s.empty() || s.size() > 20) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, base);
  if (end != s.c_str() + s.size() || v > 0xFFFFFFFFull || s[0] == '-' || s[0] == '+') return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// The text split into lines, without their \r\n / \n.
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == text.size()) break;
    start = end + 1;
  }
  return lines;
}

}  // namespace

std::string FormatCatalog(const Catalog& c) {
  std::string out = std::string(kCatalogHeader) + "1\n";
  out += "title=" + Hex8(c.title_id) + "\n";
  out += "game=" + Escape(c.game) + "\n";
  out += "language=" + std::to_string(c.language) + "\n";
  for (const CatalogEntry& e : c.achievements) {
    out += "[achievement]\n";
    out += "id=" + std::to_string(e.id) + "\n";
    out += "gamerscore=" + std::to_string(e.gamerscore) + "\n";
    out += "flags=" + std::to_string(e.flags) + "\n";
    out += "image=" + std::to_string(e.image_id) + "\n";
    out += "label=" + Escape(e.label) + "\n";
    out += "description=" + Escape(e.description) + "\n";
    out += "unachieved=" + Escape(e.unachieved) + "\n";
  }
  return out;
}

std::optional<Catalog> ParseCatalog(const std::string& text) {
  const std::vector<std::string> lines = Lines(text);
  if (lines.empty() || lines[0].compare(0, sizeof(kCatalogHeader) - 1, kCatalogHeader) != 0) return std::nullopt;
  Catalog c;
  bool have_title = false;
  std::optional<CatalogEntry> entry;
  auto finish = [&] {
    if (entry && entry->id != 0) c.achievements.push_back(std::move(*entry));
    entry.reset();
  };
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.empty() || line[0] == '#') continue;
    if (line == "[achievement]") {
      finish();
      entry.emplace();
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (!entry) {
      if (key == "title") have_title = ParseU32(value, 16, &c.title_id);
      else if (key == "game") c.game = Unescape(value);
      else if (key == "language") ParseU32(value, 10, &c.language);
      continue;
    }
    if (key == "id") ParseU32(value, 10, &entry->id);
    else if (key == "gamerscore") ParseU32(value, 10, &entry->gamerscore);
    else if (key == "flags") ParseU32(value, 10, &entry->flags);
    else if (key == "image") ParseU32(value, 10, &entry->image_id);
    else if (key == "label") entry->label = Unescape(value);
    else if (key == "description") entry->description = Unescape(value);
    else if (key == "unachieved") entry->unachieved = Unescape(value);
  }
  finish();
  if (!have_title) return std::nullopt;
  return c;
}

std::filesystem::path TitleDir(const std::filesystem::path& cogbox_dir, uint32_t title_id) {
  return cogbox_dir / "achievements" / Hex8(title_id);
}
std::filesystem::path CatalogPath(const std::filesystem::path& cogbox_dir, uint32_t title_id) {
  return TitleDir(cogbox_dir, title_id) / "catalog.txt";
}
std::filesystem::path IconPath(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id) {
  return TitleDir(cogbox_dir, title_id) / "icons" / (std::to_string(id) + ".png");
}
std::filesystem::path UnlockPath(const std::filesystem::path& cogbox_dir, const GameDef& game) {
  return cogbox_dir / game.folder / "user" / "achievements" / (Hex8(game.title_id) + ".toml");
}
std::filesystem::path RevealedPath(const std::filesystem::path& cogbox_dir) {
  return cogbox_dir / "achievements" / "revealed.txt";
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& bytes) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f) return false;
  }
  // Rename over the old file in one step (MoveFileEx with replace on Windows). Only if that fails: remove the old file
  // and try again - and never delete the new copy once the old one is gone, so nothing is ever lost.
  std::filesystem::rename(tmp, path, ec);
  if (!ec) return true;
  ec.clear();
  if (!std::filesystem::remove(path, ec) || ec) {
    std::filesystem::remove(tmp, ec);  // the old file is still there
    return false;
  }
  std::filesystem::rename(tmp, path, ec);
  return !ec;  // on failure <path>.tmp keeps the new content
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::optional<Catalog> LoadCatalog(const std::filesystem::path& cogbox_dir, uint32_t title_id) {
  std::string text;
  if (!ReadFile(CatalogPath(cogbox_dir, title_id), &text)) return std::nullopt;
  auto c = ParseCatalog(text);
  if (!c || c->title_id != title_id) return std::nullopt;
  return c;
}

namespace {

std::string Trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// 64-bit decimal, whole string.
bool ParseU64(const std::string& s, uint64_t* out) {
  if (s.empty() || s.size() > 20 || s[0] < '0' || s[0] > '9') return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (end != s.c_str() + s.size()) return false;
  *out = v;
  return true;
}

}  // namespace

Unlocks ParseUnlocks(const std::string& toml) {
  Unlocks unlocks;
  std::optional<uint32_t> current;
  for (const std::string& raw : Lines(toml)) {
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#') continue;
    const std::string prefix = "[unlocked.";
    if (line.compare(0, prefix.size(), prefix) == 0 && line.back() == ']') {
      std::string key = line.substr(prefix.size(), line.size() - prefix.size() - 1);
      if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
      uint32_t id = 0;
      current.reset();
      if (ParseU32(key, 10, &id) && id != 0) {
        unlocks.emplace(id, 0);
        current = id;
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (key == "filetime" && current) {
      uint64_t t = 0;
      if (ParseU64(value, &t)) unlocks[*current] = t;
    } else if (key == "unlocked" && value.size() >= 2 && value.front() == '[' && value.back() == ']') {
      std::string list = value.substr(1, value.size() - 2);
      size_t start = 0;
      while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        uint32_t id = 0;
        if (ParseU32(Trim(list.substr(start, comma - start)), 10, &id) && id != 0) unlocks.emplace(id, 0);
        start = comma + 1;
      }
      current.reset();
    }
  }
  return unlocks;
}

Unlocks LoadUnlocks(const std::filesystem::path& cogbox_dir, const GameDef& game) {
  std::string text;
  if (!ReadFile(UnlockPath(cogbox_dir, game), &text)) return {};
  return ParseUnlocks(text);
}

RevealedSet ParseRevealed(const std::string& text) {
  RevealedSet set;
  for (const std::string& raw : Lines(text)) {
    const std::string line = Trim(raw);
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    uint32_t title = 0, id = 0;
    if (ParseU32(line.substr(0, colon), 16, &title) && ParseU32(line.substr(colon + 1), 10, &id) && id != 0) {
      set.insert({title, id});
    }
  }
  return set;
}

RevealedSet LoadRevealed(const std::filesystem::path& cogbox_dir) {
  std::string text;
  if (!ReadFile(RevealedPath(cogbox_dir), &text)) return {};
  return ParseRevealed(text);
}

bool AddRevealed(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id) {
  RevealedSet set = LoadRevealed(cogbox_dir);
  set.insert({title_id, id});
  std::string out;
  for (const auto& [t, i] : set) out += Hex8(t) + ":" + std::to_string(i) + "\n";
  return WriteFileAtomic(RevealedPath(cogbox_dir), out);
}

namespace {
NoRarity g_no_rarity;
RarityProvider* g_rarity = nullptr;

// Unlock-time order: true when a comes first.
bool TimeBefore(const Achievement& a, const Achievement& b) {
  if (a.unlocked != b.unlocked) return a.unlocked;
  if (a.unlocked) {
    if ((a.unlocked_at == 0) != (b.unlocked_at == 0)) return a.unlocked_at != 0;
    if (a.unlocked_at != b.unlocked_at) return a.unlocked_at > b.unlocked_at;
  }
  if (a.game_index != b.game_index) return a.game_index < b.game_index;
  return a.list_order < b.list_order;
}
}  // namespace

RarityProvider& CurrentRarityProvider() { return g_rarity ? *g_rarity : g_no_rarity; }
void SetRarityProvider(RarityProvider* provider) { g_rarity = provider; }

uint32_t PercentByGp(const Totals& t) {
  if (t.gp_total == 0) return 0;
  return static_cast<uint32_t>(uint64_t(t.gp_earned) * 100 / t.gp_total);
}

GameAchievements BuildGame(const GameDef& game, const std::optional<Catalog>& catalog, const Unlocks& unlocks,
                           const RevealedSet& revealed, RarityProvider& rarity,
                           const std::filesystem::path& cogbox_dir) {
  GameAchievements out;
  out.game = &game;
  if (!catalog) return out;
  out.loaded = true;
  uint32_t game_index = 0;
  for (size_t i = 0; i < kGames.size(); ++i) {
    if (kGames[i].title_id == game.title_id) game_index = static_cast<uint32_t>(i);
  }
  for (size_t i = 0; i < catalog->achievements.size(); ++i) {
    const CatalogEntry& e = catalog->achievements[i];
    Achievement a;
    a.title_id = game.title_id;
    a.id = e.id;
    a.gamerscore = e.gamerscore;
    a.label = e.label;
    a.description = e.description;
    a.unachieved = e.unachieved;
    std::error_code ec;
    const std::filesystem::path icon = IconPath(cogbox_dir, game.title_id, e.id);
    if (std::filesystem::exists(icon, ec)) a.icon = icon;
    a.secret = (e.flags & kFlagShowUnachieved) == 0;
    a.revealed = revealed.count({game.title_id, e.id}) != 0;
    const auto u = unlocks.find(e.id);
    a.unlocked = u != unlocks.end();
    a.unlocked_at = a.unlocked ? u->second : 0;
    a.rarity = rarity.Get(game.title_id, e.id);
    a.game_index = game_index;
    a.list_order = static_cast<uint32_t>(i);
    out.totals.total += 1;
    out.totals.gp_total += a.gamerscore;
    if (a.unlocked) {
      out.totals.unlocked += 1;
      out.totals.gp_earned += a.gamerscore;
    }
    out.achievements.push_back(std::move(a));
  }
  return out;
}

GameAchievements LoadGame(const std::filesystem::path& cogbox_dir, const GameDef& game, const RevealedSet& revealed,
                          RarityProvider& rarity) {
  return BuildGame(game, LoadCatalog(cogbox_dir, game.title_id), LoadUnlocks(cogbox_dir, game), revealed, rarity,
                   cogbox_dir);
}

std::vector<GameAchievements> LoadAllGames(const std::filesystem::path& cogbox_dir, RarityProvider& rarity) {
  const RevealedSet revealed = LoadRevealed(cogbox_dir);
  std::vector<GameAchievements> games;
  for (const GameDef& g : kGames) games.push_back(LoadGame(cogbox_dir, g, revealed, rarity));
  return games;
}

Totals SumTotals(const std::vector<GameAchievements>& games) {
  Totals t;
  for (const GameAchievements& g : games) {
    if (!g.loaded) continue;
    t.unlocked += g.totals.unlocked;
    t.total += g.totals.total;
    t.gp_earned += g.totals.gp_earned;
    t.gp_total += g.totals.gp_total;
  }
  return t;
}

void SortAchievements(std::vector<Achievement>& list, SortOrder order) {
  if (order == SortOrder::kUnlockTime) {
    std::stable_sort(list.begin(), list.end(), TimeBefore);
    return;
  }
  std::stable_sort(list.begin(), list.end(), [](const Achievement& a, const Achievement& b) {
    if (a.rarity.known != b.rarity.known) return a.rarity.known;
    if (a.rarity.known && a.rarity.percent != b.rarity.percent) return a.rarity.percent < b.rarity.percent;
    return TimeBefore(a, b);
  });
}

}  // namespace xenos::ach
