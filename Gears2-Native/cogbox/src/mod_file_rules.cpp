// COGBox - the program files a mod may not bring (see mod_file_rules.h).
#include "mod_file_rules.h"

#include <algorithm>

const std::vector<std::string> &mod_file_program_exts()
{
    static const std::vector<std::string> exts = {
        "exe", "dll", "xex", "xexp", "bat", "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "msi", "msp",
        "mst", "scr", "com", "lnk", "url", "pif", "hta", "reg", "asi", "sys", "cpl", "ocx", "jar", "appx", "msix",
        "appxbundle", "msixbundle", "scf", "inf", "chm", "gadget", "application", "settingcontent-ms", "msc",
        "zip", "7z", "rar", "cab",   // nested archives
        // review r369 M3: more code the system loads or runs (drivers, codecs, EFI, PowerShell data / console files,
        // scriptlets, Windows Script, ClickOnce / search / library shortcuts, update and troubleshooter packages)
        "ax", "drv", "efi", "mui", "ps1xml", "psd1", "psc1", "psc2", "msh", "msh1", "msh2", "mshxml", "msh1xml",
        "msh2xml", "ws", "wsc", "sct", "vb", "appref-ms", "library-ms", "search-ms", "searchconnector-ms", "msu",
        "diagcab", "appinstaller", "jnlp", "xll", "xbap", "website",
        // ... and the other archives and disk images a program could hide in
        "tar", "gz", "tgz", "bz2", "tbz2", "xz", "txz", "lz", "lzma", "zst", "arj", "lzh", "ace", "iso", "img", "vhd",
        "vhdx", "wim", "dmg",
        // review r369 F2 (security audit): documents that carry macros or code, themes, remote-desktop and web files,
        // Python, databases, certificates, Excel web queries, and Linux launchers (for Wine / Proton players)
        "docm", "dotm", "xlsm", "xltm", "xlam", "pptm", "potm", "ppam", "ppsm", "sldm", "doc", "dot", "xls", "xlt", "ppt",
        "pot", "pps", "one", "theme", "themepack", "deskthemepack", "msstyles", "rdp", "py", "pyw", "pyz", "pyc", "htm",
        "html", "xhtml", "mht", "mhtml", "svg", "mdb", "accde", "accdb", "adp", "cer", "crt", "der", "p7b", "iqy", "slk",
        "desktop", "sh",
    };
    return exts;
}

bool mod_file_is_program(const std::string &name)
{
    std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = base.substr(dot + 1);
    for (char &c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    const std::vector<std::string> &v = mod_file_program_exts();
    return std::find(v.begin(), v.end(), ext) != v.end();
}

bool mod_file_is_device_name(const std::string &component)
{
    std::string stem = component;
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) stem.pop_back();
    stem = stem.substr(0, stem.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char &c : stem) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$") return true;
    if (stem.size() < 4 || (stem.compare(0, 3, "com") != 0 && stem.compare(0, 3, "lpt") != 0)) return false;
    const std::string d = stem.substr(3);
    return (d.size() == 1 && d[0] >= '1' && d[0] <= '9') || d == "\xC2\xB9" || d == "\xC2\xB2" || d == "\xC2\xB3";   // or superscript 1 2 3
}
