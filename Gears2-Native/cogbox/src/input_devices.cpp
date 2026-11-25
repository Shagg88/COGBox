// COGBox - per-player input devices: the pure part (see input_devices.h).
#include "input_devices.h"

#include <algorithm>
#include <map>

namespace cogbox {

std::vector<std::string> ResolvePlayerDevices(const std::vector<std::string>& chosen, int players) {
    std::vector<std::string> out;
    if (players < 0) players = 0;
    for (int i = 0; i < players; ++i) out.push_back(i < (int)chosen.size() ? chosen[(size_t)i] : std::string());
    for (int i = players - 1; i >= 0; --i)   // the later pick wins
        if (!out[(size_t)i].empty())
            for (int j = 0; j < i; ++j) if (out[(size_t)j] == out[(size_t)i]) out[(size_t)j].clear();
    return out;
}

std::string PlayerDevicesArg(const std::vector<std::string>& resolved) {
    if (resolved.size() < 2) return std::string();
    std::string s = "--player_devices=";
    for (size_t i = 0; i < resolved.size(); ++i) s += (i ? ";" : "") + (resolved[i].empty() ? std::string("auto") : resolved[i]);
    return s;
}

std::vector<std::string> EffectiveDevices(const std::vector<std::string>& resolved, const std::vector<DetectedDevice>& detected,
                                          bool kbm_on) {
    std::vector<std::string> out(resolved.size());
    std::vector<bool> has(resolved.size(), false);
    auto present = [&](const std::string& k) {
        if (k == KbmKey()) return kbm_on;
        return std::any_of(detected.begin(), detected.end(), [&](const DetectedDevice& d) { return d.key == k; });
    };
    for (size_t p = 0; p < resolved.size(); ++p)   // pass 1: chosen devices that are there
        if (!resolved[p].empty() && present(resolved[p])) { out[p] = resolved[p]; has[p] = true; }
    std::vector<std::string> pool;   // devices nobody chose: keyboard and mouse first, then the pads in order
    auto named = [&](const std::string& k) { return std::find(resolved.begin(), resolved.end(), k) != resolved.end(); };
    if (kbm_on && !named(KbmKey())) pool.push_back(KbmKey());
    for (const DetectedDevice& d : detected) if (!named(d.key)) pool.push_back(d.key);
    size_t next = 0;
    for (int pass = 0; pass < 2; ++pass)   // players whose chosen pad is missing, then the Auto players, lowest first
        for (size_t p = 0; p < resolved.size() && next < pool.size(); ++p) {
            if (has[p] || resolved[p].empty() != (pass == 1)) continue;
            out[p] = pool[next++];
            has[p] = true;
        }
    return out;
}

static const char kSep[] = " ## ";   // a key never holds this (guid and a device path)

std::string PackPlayerDevice(const std::string& key, const std::string& name) {
    if (key.empty()) return std::string();
    return name.empty() ? key : key + kSep + name;
}

void UnpackPlayerDevice(const std::string& packed, std::string& key, std::string& name) {
    const size_t at = packed.find(kSep);
    key = packed.substr(0, at);
    name = at == std::string::npos ? std::string() : packed.substr(at + sizeof(kSep) - 1);
}

std::vector<DeviceChoice> PlayerDeviceChoices(const std::vector<DetectedDevice>& detected, const std::string& savedKey,
                                              const std::string& savedName) {
    std::vector<DeviceChoice> out = { { "", "Auto" }, { KbmKey(), KbmLabel() } };
    std::map<std::string, int> seen;
    for (const DetectedDevice& d : detected) {
        const std::string base = d.label.empty() ? d.name : d.label;
        const int n = ++seen[base];
        out.push_back({ d.key, n > 1 ? base + " #" + std::to_string(n) : base });
    }
    if (!savedKey.empty() && std::none_of(out.begin(), out.end(), [&](const DeviceChoice& c) { return c.key == savedKey; }))
        out.push_back({ savedKey, (savedName.empty() ? std::string("Controller") : savedName) + " (not connected)" });
    return out;
}

std::string DeviceLabel(const std::vector<DeviceChoice>& choices, const std::string& key) {
    for (const DeviceChoice& c : choices) if (!key.empty() && c.key == key) return c.label;
    return std::string();
}

}  // namespace cogbox
