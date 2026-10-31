// COGBox - translations (0.2.0). See i18n.h.
#include "i18n.h"
#include "util.h"

#include <windows.h>
#include <unordered_map>
#include <deque>
#include <algorithm>
#include <cctype>
#include <vector>

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

namespace {
// 0.2.0: pieces of messages built at run time ("Desktop shortcut: " + a game name). When a whole line has no
// translation, the known pieces in it are replaced, longest first, only where they start and end at a word edge.
std::unordered_map<std::string, std::vector<const std::string *>> g_pieces;   // first 3 bytes -> keys, longest first
std::unordered_map<std::string, std::string> g_strCache;

bool word_char(unsigned char c) { return isalnum(c) || c >= 0x80; }

void build_pieces()
{
    g_pieces.clear();
    for (auto &kv : g_map) {
        const std::string &k = kv.first;
        if (k.size() < 4) continue;
        g_pieces[k.substr(0, 3)].push_back(&kv.first);
    }
    for (auto &kv : g_pieces)
        std::sort(kv.second.begin(), kv.second.end(), [](const std::string *a, const std::string *b) { return a->size() > b->size(); });
}

std::string replace_pieces(const std::string &text)
{
    std::string out;
    size_t i = 0;
    bool any = false;
    while (i < text.size()) {
        bool hit = false;
        if (i + 3 <= text.size() && (i == 0 || !word_char((unsigned char)text[i - 1]) || !word_char((unsigned char)text[i]))) {
            auto it = g_pieces.find(text.substr(i, 3));
            if (it != g_pieces.end())
                for (const std::string *k : it->second) {
                    size_t n = k->size();
                    if (text.compare(i, n, *k) != 0) continue;
                    if (i + n < text.size() && word_char((unsigned char)text[i + n]) && word_char((unsigned char)(*k)[n - 1])) continue;
                    out += g_map[*k];
                    i += n;
                    hit = any = true;
                    break;
                }
        }
        if (!hit) out += text[i++];
    }
    return any ? out : text;
}
}  // namespace

const std::vector<Language> &languages()
{
    static const std::vector<Language> v = {
        { "en", "English" }, { "es", "Espa\xC3\xB1ol" }, { "fr", "Fran\xC3\xA7" "ais" }, { "de", "Deutsch" },
        { "it", "Italiano" }, { "pt-BR", "Portugu\xC3\xAAs (Brasil)" }, { "pl", "Polski" }, { "hu", "Magyar" },
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
    case LANG_HUNGARIAN: return "hu";
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
    g_strCache.clear();
    g_pieces.clear();
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
        size_t eq = t.rfind(" = ");   // the last one: English text may hold " = " itself, translations never do
        if (eq == std::string::npos) continue;
        std::string en = unescape(trim(t.substr(0, eq))), tx = unescape(trim(t.substr(eq + 3)));
        if (!en.empty() && !tx.empty()) g_map[en] = tx;
    }
    build_pieces();
}

std::string i18n_current() { return g_code; }

std::string tr(const std::string &english)
{
    if (g_map.empty()) return english;
    auto c = g_strCache.find(english);
    if (c != g_strCache.end()) return c->second;
    size_t hid = english.find("##");
    std::string text = hid == std::string::npos ? english : english.substr(0, hid);
    std::string done;
    auto it = g_map.find(text);
    if (it != g_map.end()) done = it->second;
    else {
        size_t a = text.find_first_not_of(" "), b = text.find_last_not_of(" ");
        auto t2 = a == std::string::npos ? g_map.end() : g_map.find(text.substr(a, b - a + 1));
        done = t2 != g_map.end() ? text.substr(0, a) + t2->second + text.substr(b + 1) : replace_pieces(text);
    }
    if (hid != std::string::npos) done += english.substr(hid);
    if (g_strCache.size() > 4000) g_strCache.clear();
    g_strCache[english] = done;
    return done;
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
