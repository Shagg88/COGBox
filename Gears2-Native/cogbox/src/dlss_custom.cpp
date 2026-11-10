// CogBox 0.4.9: the player's own official nvngx_dlss.dll (dlss_custom.h).
#include "dlss_custom.h"

#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

namespace cogbox {

namespace {

const char *const kDll = "nvngx_dlss.dll";
const char *const kMarker = "custom-dlss.txt";   // in the backup folder: the sha256 of the player's file we copied

std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string dir_of(const std::string &p) { size_t i = p.find_last_of("\\/"); return i == std::string::npos ? std::string() : p.substr(0, i); }
std::string name_of(const std::string &p) { size_t i = p.find_last_of("\\/"); return i == std::string::npos ? p : p.substr(i + 1); }

std::string sha256_of(const std::string &path)
{
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD n = 0;
        bool good = true;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, NULL) && n) good &= BCryptHashData(h, buf.data(), n, 0) == 0;
        unsigned char d[32];
        if (good && BCryptFinishHash(h, d, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
            out = hex;
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// The version resource: FileVersion numbers + FileDescription.
bool version_of(const std::string &path, std::string &ver, std::string &desc)
{
    const std::wstring w = widen(path);
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(w.c_str(), &dummy);
    if (!size) return false;
    std::vector<char> info(size);
    if (!GetFileVersionInfoW(w.c_str(), 0, size, info.data())) return false;
    VS_FIXEDFILEINFO *fi = nullptr;
    UINT n = 0;
    if (!VerQueryValueW(info.data(), L"\\", (void **)&fi, &n) || !fi || fi->dwSignature != 0xFEEF04BD) return false;
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
             LOWORD(fi->dwFileVersionLS));
    ver = b;
    struct Lang { WORD lang, cp; } *langs = nullptr;
    std::vector<std::wstring> blocks;
    if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &n) && langs)
        for (UINT i = 0; i < n / sizeof(Lang); i++) {
            wchar_t t[16];
            swprintf(t, 16, L"%04x%04x", langs[i].lang, langs[i].cp);
            blocks.push_back(t);
        }
    blocks.push_back(L"040904b0");
    blocks.push_back(L"040904e4");
    for (const std::wstring &bl : blocks) {
        wchar_t *v = nullptr;
        UINT len = 0;
        if (VerQueryValueW(info.data(), (L"\\StringFileInfo\\" + bl + L"\\FileDescription").c_str(), (void **)&v, &len) && v && len) {
            desc = narrow(std::wstring(v));
            break;
        }
    }
    return true;
}

// A PE file for x64 (the games are 64-bit).
bool is_x64_dll(const std::string &path)
{
    FILE *f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    unsigned char h[0x400] = {};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 0x40 || h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe = h[0x3C] | h[0x3D] << 8 | h[0x3E] << 16 | (uint32_t)h[0x3F] << 24;
    if (pe + 24 > n || memcmp(h + pe, "PE\0\0", 4) != 0) return false;
    const uint16_t machine = h[pe + 4] | h[pe + 5] << 8, chars = h[pe + 22] | h[pe + 23] << 8;
    return machine == 0x8664 && (chars & 0x2000);   // IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_DLL
}

// The revocation check itself could not be done (offline, no CRL / OCSP answer) - not a revoked certificate.
bool revocation_unknown(LONG r)
{
    return r == (LONG)CERT_E_REVOCATION_FAILURE || r == (LONG)CRYPT_E_REVOCATION_OFFLINE || r == (LONG)CRYPT_E_NO_REVOCATION_CHECK;
}

// Authenticode: a valid embedded signature (WinVerifyTrust) whose signer certificate's organisation is NVIDIA
// Corporation - package_cogbox.ps1's rule (Get-AuthenticodeSignature Valid + O=NVIDIA Corporation).
// Review: the whole chain is checked for revocation. A revoked certificate (CERT_E_REVOKED) is rejected; when the check
// can't be done (offline), the signature is verified again without it and that answer is used.
bool signed_by_nvidia(const std::string &path, std::string &why)
{
    const std::wstring w = widen(path);
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = w.c_str();
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (revocation_unknown(r)) {
        wd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
        wd.hWVTStateData = nullptr;
        wd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        wd.dwStateAction = WTD_STATEACTION_VERIFY;
        r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    }
    bool ok = false;
    if (r != ERROR_SUCCESS) {
        char b[96];
        snprintf(b, sizeof b, r == (LONG)CERT_E_REVOKED || r == (LONG)CRYPT_E_REVOKED
                                  ? "its signing certificate has been revoked (0x%08lX)"
                                  : "its digital signature is missing or not valid (0x%08lX)", (unsigned long)r);
        why = b;
    } else {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        wchar_t org[256] = {};
        if (pc && pc->pCert)
            CertGetNameStringW(pc->pCert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, org, 256);
        std::wstring o(org);
        ok = o == L"NVIDIA Corporation";
        if (!ok) why = "it is signed, but not by NVIDIA (" + (o.empty() ? std::string("unknown signer") : narrow(o)) + ")";
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return ok;
}

// <COGBox>\_old-files\dlss-backup\<game folder>\<port folder> for the exe's folder
std::string backup_dir_for(const std::string &exe)
{
    const std::string port = dir_of(exe), root = exe_dir();
    std::string rel;
    if (lower(port).compare(0, root.size() + 1, lower(root) + "\\") == 0) rel = port.substr(root.size() + 1);
    else rel = name_of(dir_of(port)) + "\\" + name_of(port);
    return join(join(join(root, "_old-files"), "dlss-backup"), rel);
}

// Review: unique per call - the time stamp plus milliseconds and a counter (two moves in one second no longer share it).
std::string replaced_dir()
{
    static unsigned counter = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "-%03u-%u", (unsigned)t.wMilliseconds, ++counter);
    return join(join(join(exe_dir(), "_old-files"), "dlss-replaced"), now_stamp() + b);
}

bool move_file(const std::string &from, const std::string &to)
{
    make_dirs(dir_of(to));
    if (MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return true;
    return false;
}

std::string err_text() { return "error " + std::to_string(GetLastError()); }

// 0.4.9 (shared049): the release keeps COGBox's nvngx_dlss.dll once, in <COGBox>\shared; the game's NGX looks beside
// the exe first, then there. So a player's file beside the exe wins, and taking it away puts COGBox's back in use.
std::string shared_dll() { return join(join(exe_dir(), "shared"), kDll); }

}  // namespace

DlssFileInfo dlss_check_file(const std::string &path)
{
    DlssFileInfo r;
    std::string desc;
    if (!file_exists(path)) { r.why = "the file isn't there"; return r; }
    if (!is_x64_dll(path)) { r.why = "it isn't a 64-bit Windows DLL"; return r; }
    if (!version_of(path, r.version, desc)) { r.why = "it has no version information - not an NVIDIA DLSS file"; return r; }
    const std::string d = lower(desc);
    if (d.find("nvidia dlss") == std::string::npos) { r.why = "it isn't NVIDIA DLSS (it says \"" + desc + "\")"; return r; }
    if (d.find("dlss-g") != std::string::npos || d.find("dlssg") != std::string::npos || d.find("dlss-d") != std::string::npos ||
        d.find("dlssd") != std::string::npos) {
        r.why = "it is NVIDIA's " + desc + ", not DLSS Super Resolution (nvngx_dlss.dll)";
        return r;
    }
    if (!signed_by_nvidia(path, r.why)) return r;
    r.ok = true;
    return r;
}

DlssState dlss_state(const std::string &exe)
{
    static std::string lastExe;
    static uint64_t lastSize = 0, lastTime = 0, lastBackupTime = 0;
    static DlssState last;
    DlssState s;
    if (exe.empty()) return s;
    const std::string cur = join(dir_of(exe), kDll), bdir = backup_dir_for(exe), bak = join(bdir, kDll);
    WIN32_FILE_ATTRIBUTE_DATA fa = {}, fb = {};
    const bool have = GetFileAttributesExW(widen(cur).c_str(), GetFileExInfoStandard, &fa);
    const bool haveB = GetFileAttributesExW(widen(bak).c_str(), GetFileExInfoStandard, &fb);
    const uint64_t size = have ? ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) : 0;
    const uint64_t t = have ? ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) : 0;
    const uint64_t tb = haveB ? ((uint64_t)fb.ftLastWriteTime.dwHighDateTime << 32 | fb.ftLastWriteTime.dwLowDateTime) : 1;
    if (exe == lastExe && size == lastSize && t == lastTime && tb == lastBackupTime) return last;
    const std::string shared = shared_dll();
    const bool haveShared = file_exists(shared);
    s.present = have || haveShared;
    s.backupDir = bdir;
    std::string desc;
    if (have) version_of(cur, s.version, desc);
    else if (haveShared) version_of(shared, s.version, desc);   // shared049: COGBox's own, in <COGBox>\shared
    if (haveB) version_of(bak, s.backupVersion, desc);
    else if (haveShared) version_of(shared, s.backupVersion, desc);
    if (haveB || haveShared) {
        std::string mark;
        read_file(join(bdir, kMarker), mark, 4096);
        while (!mark.empty() && (mark.back() == '\r' || mark.back() == '\n' || mark.back() == ' ')) mark.pop_back();
        // custom only while the file beside the exe is still the one we copied (an update unzipped over it puts
        // COGBox's own back, and then the backup is just old)
        s.custom = have && !mark.empty() && sha256_of(cur) == mark;
    }
    lastExe = exe, lastSize = size, lastTime = t, lastBackupTime = tb, last = s;
    return s;
}

bool dlss_install_custom(const std::string &exe, const std::string &src, std::string &msg)
{
    // Review: the source is hashed around validation (the file checked is the file hashed), and the copy must match it.
    const std::string srcHash = sha256_of(src);
    if (srcHash.empty()) { msg = "Not used: couldn't read the file."; return false; }
    const DlssFileInfo info = dlss_check_file(src);
    if (!info.ok) { msg = "Not used: " + info.why + "."; return false; }
    if (sha256_of(src) != srcHash) { msg = "Not used: the file changed while it was being checked."; return false; }
    const std::string port = dir_of(exe), cur = join(port, kDll), bdir = backup_dir_for(exe), bak = join(bdir, kDll);
    if (lower(src) == lower(cur)) { msg = "That is already the file beside the game."; return false; }
    const DlssState st = dlss_state(exe);
    const std::string aside = replaced_dir();
    std::string prevCustom;   // a custom file we moved aside (put back on a rollback)
    // 1. COGBox's own file to the backup folder (only if it isn't already a custom one: then COGBox's is backed up)
    if (file_exists(cur)) {
        if (st.custom) {
            prevCustom = join(aside, kDll);
            if (!move_file(cur, prevCustom)) { msg = "Couldn't move the current DLSS file aside (" + err_text() + ") - is the game running?"; return false; }
        } else {
            if (file_exists(bak) && !move_file(bak, join(aside, "old-backup-" + std::string(kDll)))) {
                msg = "Couldn't move an old backup aside (" + err_text() + ")."; return false;
            }
            if (!move_file(cur, bak)) { msg = "Couldn't back up COGBox's DLSS file (" + err_text() + ") - is the game running?"; return false; }
        }
    }
    // 2. the player's copy beside the exe
    // rollback: the previous custom file (if we moved one aside) or COGBox's own back beside the exe
    auto put_back = [&] {
        if (file_exists(cur)) DeleteFileW(widen(cur).c_str());
        if (!prevCustom.empty()) move_file(prevCustom, cur);
        else if (file_exists(bak)) move_file(bak, cur);
    };
    if (!CopyFileW(widen(src).c_str(), widen(cur).c_str(), TRUE)) {
        const std::string e = err_text();
        put_back();
        msg = "Couldn't copy your DLSS file (" + e + "). The previous file is back in place.";
        return false;
    }
    if (sha256_of(cur) != srcHash) {
        put_back();
        msg = "Not used: the copied file doesn't match the file that was checked. The previous file is back in place.";
        return false;
    }
    make_dirs(bdir);   // shared049: nothing was backed up when COGBox's file is the shared one
    write_file(join(bdir, kMarker), srcHash + "\r\n");   // the validated hash, not a re-read
    write_file(join(bdir, "README.txt"), "COGBox's own nvngx_dlss.dll for this game, kept while your own DLSS file is in use.\r\n"
                                          "Use \"Restore COGBox's DLSS\" in the launcher (Graphics tab) to put it back.\r\n");
    msg = "Your DLSS file " + info.version + " is in use for this game (COGBox's is backed up). At your own risk - "
          "\"Restore COGBox's DLSS\" puts it back.";
    return true;
}

bool dlss_restore(const std::string &exe, std::string &msg)
{
    const std::string port = dir_of(exe), cur = join(port, kDll), bdir = backup_dir_for(exe), bak = join(bdir, kDll);
    const DlssState st = dlss_state(exe);
    const std::string aside = replaced_dir();
    if (!file_exists(bak) && file_exists(shared_dll())) {
        // shared049: COGBox's file is the one in <COGBox>\shared - moving the player's file away from the exe is enough
        if (!st.custom) { msg = "COGBox's DLSS file is already in use for this game."; return true; }
        if (!move_file(cur, join(aside, kDll))) { msg = "Couldn't move your DLSS file aside (" + err_text() + ") - is the game running?"; return false; }
        move_file(join(bdir, kMarker), join(aside, kMarker));
        msg = "COGBox's DLSS file is back for this game (yours was moved to _old-files\\dlss-replaced).";
        return true;
    }
    if (!file_exists(bak)) { msg = "There is no backup of COGBox's DLSS file for this game - it is already in use."; return false; }
    if (!st.custom && file_exists(cur)) {
        // COGBox's file is already beside the exe (an update put it back): the backup is old - move it aside
        move_file(bak, join(aside, "old-backup-" + std::string(kDll)));
        move_file(join(bdir, kMarker), join(aside, kMarker));
        msg = "COGBox's DLSS file is already in use for this game.";
        return true;
    }
    if (file_exists(cur) && !move_file(cur, join(aside, kDll))) { msg = "Couldn't move your DLSS file aside (" + err_text() + ") - is the game running?"; return false; }
    if (!move_file(bak, cur)) { msg = "Couldn't put COGBox's DLSS file back (" + err_text() + ")."; return false; }
    move_file(join(bdir, kMarker), join(aside, kMarker));
    msg = "COGBox's DLSS file is back for this game" + std::string(st.custom ? " (yours was moved to _old-files\\dlss-replaced)." : ".");
    return true;
}

}  // namespace cogbox
