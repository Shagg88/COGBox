// COGBox - small helpers (see util.h)
#include "util.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <algorithm>

std::wstring widen(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring exe_dir_w()
{
    static std::wstring dir;
    if (dir.empty()) {
        std::wstring buf(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        buf.resize(n);
        size_t slash = buf.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? L"." : buf.substr(0, slash);
    }
    return dir;
}

std::string exe_dir() { return narrow(exe_dir_w()); }

bool file_exists(const std::string &p)
{
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::string &p)
{
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string &p)
{
    if (p.empty() || dir_exists(p)) return true;
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos && slash > 2) make_dirs(p.substr(0, slash));
    return CreateDirectoryW(widen(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool read_file(const std::string &p, std::string &out, size_t limit)
{
    out.clear();
    HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || (uint64_t)sz.QuadPart > limit) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}

bool write_file(const std::string &p, const std::string &data)
{
    std::wstring w = widen(p), tmp = w + L".tmp-cogbox";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size());
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

uint64_t file_size(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

std::string join(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string now_stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string today_text()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    snprintf(b, sizeof b, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return b;
}

int64_t unix_now() { return (int64_t)time(nullptr); }

std::string human_duration(int64_t s)
{
    if (s < 60) return "under a minute";
    int64_t m = s / 60, h = m / 60;
    m %= 60;
    char b[48];
    if (h == 0) snprintf(b, sizeof b, "%d min", (int)m);
    else if (m == 0) snprintf(b, sizeof b, "%d h", (int)h);
    else snprintf(b, sizeof b, "%d h %d min", (int)h, (int)m);
    return b;
}

std::string human_bytes(uint64_t n)
{
    char b[32];
    if (n >= (1ull << 30)) snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.0f MB", n / 1048576.0);
    else snprintf(b, sizeof b, "%.0f KB", n / 1024.0);
    return b;
}

// ---- Ini -------------------------------------------------------------------------------------------------------------
static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool is_section(const std::string &line, std::string *name)
{
    std::string t = trim(line);
    if (t.size() < 2 || t[0] != '[' || t.back() != ']') return false;
    if (name) *name = trim(t.substr(1, t.size() - 2));
    return true;
}

static bool split_kv(const std::string &line, std::string &k, std::string &v)
{
    std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    k = trim(t.substr(0, eq));
    v = trim(t.substr(eq + 1));
    return !k.empty();
}

static bool ieq(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
}

bool Ini::load(const std::string &p)
{
    path = p;
    lines.clear();
    std::string data;
    if (!read_file(p, data, 8u << 20)) return false;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data.erase(0, 3);
    size_t at = 0;
    while (at <= data.size()) {
        size_t nl = data.find('\n', at);
        std::string l = data.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    return true;
}

bool Ini::save() const
{
    std::string out;
    for (auto &l : lines) out += l + "\r\n";
    return write_file(path, out);
}

std::string Ini::get(const std::string &sec, const std::string &key, const std::string &def) const
{
    std::string cur, k, v;
    for (auto &l : lines) {
        if (is_section(l, &cur)) continue;
        if (ieq(cur, sec) && split_kv(l, k, v) && ieq(k, key)) return v;
    }
    return def;
}

int Ini::geti(const std::string &sec, const std::string &key, int def) const
{
    std::string v = get(sec, key, "");
    if (v.empty()) return def;
    char *e = nullptr;
    long n = strtol(v.c_str(), &e, 10);
    return e && *e == 0 ? (int)n : def;
}

int64_t Ini::geti64(const std::string &sec, const std::string &key, int64_t def) const
{
    std::string v = get(sec, key, "");
    if (v.empty()) return def;
    char *e = nullptr;
    long long n = strtoll(v.c_str(), &e, 10);
    return e && *e == 0 ? (int64_t)n : def;
}

void Ini::set(const std::string &sec, const std::string &key, const std::string &value)
{
    std::string cur, k, v;
    int secEnd = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (is_section(lines[i], &cur)) { if (ieq(cur, sec)) secEnd = (int)i + 1; continue; }
        if (ieq(cur, sec)) {
            if (split_kv(lines[i], k, v) && ieq(k, key)) { lines[i] = key + " = " + value; return; }
            if (!trim(lines[i]).empty()) secEnd = (int)i + 1;
        }
    }
    if (secEnd < 0) {
        if (!lines.empty()) lines.push_back("");
        lines.push_back("[" + sec + "]");
        lines.push_back(key + " = " + value);
        return;
    }
    lines.insert(lines.begin() + secEnd, key + " = " + value);
}

void Ini::remove(const std::string &sec, const std::string &key)
{
    std::string cur, k, v;
    for (size_t i = 0; i < lines.size(); i++) {
        if (is_section(lines[i], &cur)) continue;
        if (ieq(cur, sec) && split_kv(lines[i], k, v) && ieq(k, key)) { lines.erase(lines.begin() + i); return; }
    }
}

std::vector<std::pair<std::string, std::string>> Ini::section(const std::string &sec) const
{
    std::vector<std::pair<std::string, std::string>> out;
    std::string cur, k, v;
    for (auto &l : lines) {
        if (is_section(l, &cur)) continue;
        if (ieq(cur, sec) && split_kv(l, k, v)) out.push_back({k, v});
    }
    return out;
}

void Ini::clear_section(const std::string &sec)
{
    std::string cur, k, v;
    for (size_t i = 0; i < lines.size();) {
        if (is_section(lines[i], &cur)) { i++; continue; }
        if (ieq(cur, sec) && split_kv(lines[i], k, v)) lines.erase(lines.begin() + i);
        else i++;
    }
}

// ---- Windows' own boxes ---------------------------------------------------------------------------------------------
void shell_open(const std::string &target)
{
    ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static std::string pick(const wchar_t *title, const wchar_t *filter, bool folder)
{
    std::string result;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&d))) {
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        d->SetTitle(title);
        std::vector<COMDLG_FILTERSPEC> specs;
        if (filter && !folder)
            for (const wchar_t *p = filter; *p;) {
                const wchar_t *name = p; p += wcslen(p) + 1;
                if (!*p) break;
                const wchar_t *spec = p; p += wcslen(p) + 1;
                specs.push_back({name, spec});
            }
        if (!specs.empty()) d->SetFileTypes((UINT)specs.size(), specs.data());
        if (SUCCEEDED(d->Show(nullptr))) {
            IShellItem *it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = narrow(p); CoTaskMemFree(p); }
                it->Release();
            }
        }
        d->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

std::string pick_folder(const wchar_t *title) { return pick(title, nullptr, true); }
std::string pick_file(const wchar_t *title, const wchar_t *filter) { return pick(title, filter, false); }

std::string desktop_dir()
{
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) r = narrow(p);
    if (p) CoTaskMemFree(p);
    return r.empty() ? exe_dir() : r;
}
