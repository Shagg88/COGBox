// COGBox - translations (0.2.0). See i18n.h.
#include "i18n.h"
#include "util.h"

#include <windows.h>
#include <unordered_map>
#include <deque>

namespace {
std::unordered_map<std::string, std::string> g_map;   // English -> translation
std::deque<std::string> g_store;                      // stable storage for the strings tr() hands out
std::unordered_map<std::string, const char *> g_out;  // label (with its ##id) -> translated label
std::string g_code = "en";

std::string unescape(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
        else o += s[i];
    }
    return o;
}
std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

const std::vector<Language> &languages()
{
    static const std::vector<Language> v = {
        { "en", "English" }, { "es", "Espa\xC3\xB1ol" }, { "fr", "Fran\xC3\xA7" "ais" }, { "de", "Deutsch" },
        { "it", "Italiano" }, { "pt-BR", "Portugu\xC3\xAAs (Brasil)" }, { "pl", "Polski" },
        { "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9" },
        { "ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" }, { "zh-Hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87" },
        { "ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4" },
    };
    return v;
}

std::string i18n_auto_code()
{
    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_SPANISH: return "es";
    case LANG_FRENCH: return "fr";
    case LANG_GERMAN: return "de";
    case LANG_ITALIAN: return "it";
    case LANG_PORTUGUESE: return "pt-BR";
    case LANG_POLISH: return "pl";
    case LANG_RUSSIAN: return "ru";
    case LANG_JAPANESE: return "ja";
    case LANG_CHINESE: return "zh-Hans";
    case LANG_KOREAN: return "ko";
    default: return "en";
    }
}

void i18n_load(const std::string &choice)
{
    g_map.clear();
    g_out.clear();
    g_code = choice == "auto" || choice.empty() ? i18n_auto_code() : choice;
    if (g_code == "en") return;
    std::string text;
    if (!read_file(join(join(exe_dir(), "lang"), g_code + ".txt"), text, 8u << 20)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && (unsigned char)line[0] == 0xEF) line = line.substr(3);   // UTF-8 BOM
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find(" = ");
        if (eq == std::string::npos) continue;
        std::string en = unescape(trim(t.substr(0, eq))), tx = unescape(trim(t.substr(eq + 3)));
        if (!en.empty() && !tx.empty()) g_map[en] = tx;
    }
}

std::string i18n_current() { return g_code; }

std::string tr(const std::string &english)
{
    if (g_map.empty()) return english;
    size_t hid = english.find("##");
    std::string text = hid == std::string::npos ? english : english.substr(0, hid);
    auto it = g_map.find(text);
    if (it == g_map.end()) return english;
    return hid == std::string::npos ? it->second : it->second + english.substr(hid);
}

const char *tr(const char *english)
{
    if (g_map.empty() || !english) return english;
    auto o = g_out.find(english);
    if (o != g_out.end()) return o->second;
    g_store.push_back(tr(std::string(english)));   // always our own copy: the caller's text may be a temporary
    g_out[english] = g_store.back().c_str();
    return g_out[english];
}
