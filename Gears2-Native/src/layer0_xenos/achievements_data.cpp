// Layer 0 (game-free): COGBox's achievement data - see xenos/achievements_data.h. Standard C++17 only. Our code (MIT).
#include <xenos/achievements_data.h>

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
  std::filesystem::remove(path, ec);
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
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

}  // namespace xenos::ach
