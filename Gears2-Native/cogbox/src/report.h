// COGBox - a problem report: the last play's log, crash report and dump, the settings and a description of the PC in
// one zip, made on the player's computer and never sent anywhere. The player attaches it to a GitHub issue themselves.
#pragma once
#include <string>
#include <vector>

struct ReportFile {
    std::string name;   // its name in the zip ("logs/gow2.log")
    std::string path;   // where it is (UTF-8)
    bool text = true;   // text files have the Windows user folder, user name and PC name taken out; others get x's
};

// The PC: Windows, processor, memory, graphics adapters, then 'displays' (lines the caller makes), display scale
std::string report_system_text(const std::string &displays);
std::vector<std::string> report_gpu_names();   // the graphics adapters' names (no software adapters)
// 0.6.5 (cold starts): the graphics adapters with their driver versions, sorted, one line - changes when a driver is
// updated or a graphics card changed (what makes the game's saved pipelines unusable).
std::string report_graphics_fingerprint();
// 0.6.2 (GitHub #304): the graphics cards the game can be told to use (Graphics tab, Graphics card): hardware DXGI adapters
// only (no software adapters, no Microsoft Basic Render / Display), each name once, with its video memory of its own in MB.
struct GpuCard { std::string name; unsigned mb = 0; };
std::vector<GpuCard> report_gpu_cards();
bool cpu_has_avx2();
// The Windows user folder, user name and PC name replaced in text
std::string report_anonymise(const std::string &text);
// Writes the zip: "report.txt" with 'system', then the files that exist.
bool report_write(const std::vector<ReportFile> &files, const std::string &system, const std::string &zipPath, std::string &err);
