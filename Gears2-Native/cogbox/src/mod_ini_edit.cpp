// COGBox - mod.ini reading / editing for the Fabricator's Creator (see mod_ini_edit.h).
#include "mod_ini_edit.h"
#include "util.h"

#include <algorithm>

namespace {

struct Line {
    std::string text, eol;   // eol: "\r\n", "\n" or "" (the last line may have none)
};

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// text -> lines with their own endings (a lone '\r' is not an ending)
std::vector<Line> split_lines(const std::string &t)
{
    std::vector<Line> v;
    size_t at = 0;
    while (at < t.size()) {
        const size_t n = t.find('\n', at);
        if (n == std::string::npos) { v.push_back({ t.substr(at), "" }); break; }
        const bool cr = n > at && t[n - 1] == '\r';
        v.push_back({ t.substr(at, n - at - (cr ? 1 : 0)), cr ? "\r\n" : "\n" });
        at = n + 1;
    }
    return v;
}

// A section header line "[x]": its lower-case trimmed name.
bool header_of(const std::string &line, std::string &name)
{
    const std::string t = trim(line);
    if (t.empty() || t[0] != '[') return false;
    const size_t e = t.find(']');
    name = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
    return true;
}
// "key = value": key lower-cased and trimmed; eq = the position of '=' in the line. Not for comments and headers.
bool key_of(const std::string &line, std::string &key, std::string &value, size_t &eq)
{
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return false;
    eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = lower(trim(line.substr(0, eq)));
    value = trim(line.substr(eq + 1));
    return true;
}

std::vector<std::string> split_list(const std::string &v)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= v.size()) {
        const size_t b = (std::min)(v.find(',', a), v.size());
        const std::string r = trim(v.substr(a, b - a));
        if (!r.empty() && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
        a = b + 1;
    }
    return out;
}

bool strip_bom(std::string &t)
{
    if (t.compare(0, 3, "\xEF\xBB\xBF") != 0) return false;
    t.erase(0, 3);
    return true;
}

}  // namespace

std::optional<ModIniFields> mod_ini_read(const std::string &folder)
{
    std::string text;
    if (!read_file(join(folder, "mod.ini"), text, 1u << 20)) return std::nullopt;
    strip_bom(text);
    ModIniFields f;
    bool mine = true;   // before any section, and in [Mod]
    for (const Line &l : split_lines(text)) {
        std::string sec, key, value;
        size_t eq;
        if (header_of(l.text, sec)) { mine = sec == "mod"; continue; }
        if (!mine || !key_of(l.text, key, value, eq)) continue;
        if (key == "name") f.name = value;
        else if (key == "game") f.game = lower(value);
        else if (key == "version") f.version = value;
        else if (key == "author") f.author = value;
        else if (key == "description") f.description = value;
        else if (key == "side") f.side = value;
        else if (key == "requires") f.needs = split_list(value);
    }
    return f;
}

bool mod_ini_write(const std::string &folder, const ModIniFields &f)
{
    const std::string path = join(folder, "mod.ini");
    std::string text;
    if (file_exists(path) && !read_file(path, text, 1u << 20)) return false;
    const bool bom = strip_bom(text);
    std::vector<Line> lines = split_lines(text);
    std::string eol = "\r\n";   // a new file: Windows endings; else the first line's
    for (const Line &l : lines) if (!l.eol.empty()) { eol = l.eol; break; }

    std::string needs;
    for (const std::string &n : f.needs) {
        const std::string t = trim(n);
        if (t.empty() || t.find(',') != std::string::npos) continue;   // (modcore splits the list on commas)
        needs += (needs.empty() ? "" : ", ") + t;
    }
    struct Want { const char *key, *lowkey; std::string value; };
    std::vector<Want> want = { { "Name", "name", trim(f.name) }, { "Game", "game", trim(f.game) },
                               { "Version", "version", trim(f.version) }, { "Author", "author", trim(f.author) },
                               { "Description", "description", trim(f.description) }, { "Side", "side", trim(f.side) },
                               { "Requires", "requires", needs } };
    for (Want &w : want)   // one line each: no line breaks inside a value
        for (char &c : w.value) if (c == '\r' || c == '\n') c = ' ';

    // the [Mod] region is lines [begin, end): after its header (added at the top if there is none) to the next header
    size_t begin = 0;
    bool found = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string sec;
        if (header_of(lines[i].text, sec) && sec == "mod") { begin = i + 1; found = true; break; }
    }
    if (!found) {
        lines.insert(lines.begin(), Line{ "[Mod]", eol });
        begin = 1;
    }
    auto region_end = [&]() {
        for (size_t i = begin; i < lines.size(); i++) {
            std::string sec;
            if (header_of(lines[i].text, sec)) return i;
        }
        return lines.size();
    };

    for (const Want &w : want) {
        bool had = false;
        size_t last = begin;   // just after the region's last key line (else after its header)
        for (size_t i = begin; i < region_end();) {
            std::string key, value;
            size_t eq;
            if (!key_of(lines[i].text, key, value, eq)) { i++; continue; }
            if (key != w.lowkey) { last = ++i; continue; }
            had = true;
            if (w.value.empty()) { lines.erase(lines.begin() + (long)i); continue; }
            size_t v = eq + 1;   // keep the key's own spelling and spacing: "Name  =  x" -> "Name  =  new"
            while (v < lines[i].text.size() && (lines[i].text[v] == ' ' || lines[i].text[v] == '\t')) v++;
            lines[i].text = lines[i].text.substr(0, v) + w.value;
            last = ++i;
        }
        if (!had && !w.value.empty()) lines.insert(lines.begin() + (long)last, Line{ std::string(w.key) + " = " + w.value, eol });
    }
    // (an inserted line after an unterminated last one)
    for (size_t i = 0; i + 1 < lines.size(); i++) if (lines[i].eol.empty()) lines[i].eol = eol;

    std::string out = bom ? "\xEF\xBB\xBF" : "";
    for (const Line &l : lines) out += l.text + l.eol;
    return write_file(path, out);
}
