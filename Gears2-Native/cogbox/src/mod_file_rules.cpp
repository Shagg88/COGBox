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
