// Layer 0 (game-free): COGBox's in-game text in the launcher's language - see xenos/ui_lang.h. Our code (MIT).
#include <xenos/ui_lang.h>
#include <xenos/achievement_popup_assets.h>  // XenosCogboxDir

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

// The launcher looks for this description's start ("COGBox in-game language") in the exe before it passes the option.
REXCVAR_DEFINE_STRING(cogbox_ui_lang, "", "COGBox",
                      "COGBox in-game language: the launcher's language code (de, es, fr, hu, it, ja, ko, pl, pt-BR, ru, "
                      "zh-Hans); its <COGBox>\\lang\\<code>.txt translates the in-game menu, Achievements page and popups. "
                      "Empty or en = English");

namespace {
std::once_flag g_once;
std::unordered_map<std::string, std::string> g_map;  // never changed after the load: the pointers stay valid
std::string g_code = "en";

std::string Unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; ++i; }
    else o += s[i];
  }
  return o;
}
std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool WordChar(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

void Load() {
  const std::string code = REXCVAR_GET(cogbox_ui_lang);
  if (code.empty() || code == "en") return;
  for (char c : code)  // a code only (it becomes a file name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return;
  const std::filesystem::path file = XenosCogboxDir() / "lang" / (code + ".txt");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    REXLOG_WARN("cogbox_ui_lang: no {} - English", rex::path_to_utf8(file));
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);  // UTF-8 BOM
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t eq = t.rfind(" = ");  // the last one, as the launcher reads it
    if (eq == std::string::npos) continue;
    std::string en = Unescape(Trim(t.substr(0, eq))), tx = Unescape(Trim(t.substr(eq + 3)));
    if (!en.empty() && !tx.empty()) g_map[std::move(en)] = std::move(tx);
  }
  g_code = code;
  REXLOG_INFO("cogbox_ui_lang: {} ({} strings)", code, g_map.size());
}
}  // namespace

void XenosUiLangInit() { std::call_once(g_once, Load); }
bool XenosUiLangActive() {
  XenosUiLangInit();
  return !g_map.empty();
}
std::string XenosUiLangCode() {
  XenosUiLangInit();
  return g_code;
}

const char* XenosTr(const char* english) {
  if (!english) return english;
  XenosUiLangInit();
  if (g_map.empty()) return english;
  const auto it = g_map.find(english);
  return it == g_map.end() ? english : it->second.c_str();
}
std::string XenosTr(const std::string& english) { return XenosTr(english.c_str()); }

std::string XenosTrWords(const std::string& text, std::initializer_list<const char*> words) {
  XenosUiLangInit();
  if (g_map.empty()) return text;
  std::string out = text;
  for (const char* w : words) {
    const auto it = g_map.find(w);
    if (it == g_map.end()) continue;
    const std::string key = w;
    for (size_t at = out.find(key); at != std::string::npos; at = out.find(key, at)) {
      const bool start = at == 0 || !WordChar(static_cast<unsigned char>(out[at - 1]));
      const bool end = at + key.size() >= out.size() || !WordChar(static_cast<unsigned char>(out[at + key.size()]));
      if (!start || !end) { at += key.size(); continue; }
      out.replace(at, key.size(), it->second);
      at += it->second.size();
    }
  }
  return out;
}

std::string XenosUiLangAllText() {
  XenosUiLangInit();
  std::string all;
  for (const auto& kv : g_map) all += kv.second;
  return all;
}
