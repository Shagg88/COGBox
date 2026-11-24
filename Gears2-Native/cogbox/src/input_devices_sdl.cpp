// COGBox - per-player input devices: the controller list from SDL3 (see input_devices.h). The key is built as the
// runtime's DeviceKey does (sdk-patch117): "sdl:<guid>|<path>", or "sdl:<guid>|#<instance id>" when SDL has no path.
// The label is what helps tell two pads apart (GitHub #420, Flippage): the model's short name from the USB ids, USB /
// Bluetooth, and the last characters of the serial number. A pad's shell colour is not something SDL reports.
#include "input_devices.h"

#include <SDL3/SDL.h>


namespace cogbox {

static std::vector<DetectedDevice> g_pads;
static bool g_looked = false;

// Models by USB vendor / product id, for pads whose SDL name doesn't say (a few common ones).
static const char* ModelName(Uint16 vendor, Uint16 product) {
    struct M { Uint16 v, p; const char* name; };
    static const M k[] = {
        { 0x054C, 0x0CE6, "DualSense" }, { 0x054C, 0x0DF2, "DualSense Edge" }, { 0x054C, 0x05C4, "DualShock 4" },
        { 0x054C, 0x09CC, "DualShock 4" }, { 0x045E, 0x0B12, "Xbox Series X|S" }, { 0x045E, 0x0B13, "Xbox Series X|S" },
        { 0x045E, 0x0B20, "Xbox Series X|S" }, { 0x045E, 0x0B21, "Xbox Series X|S" }, { 0x045E, 0x0B22, "Xbox Elite Series 2" },
        { 0x045E, 0x0B00, "Xbox Elite Series 2" }, { 0x045E, 0x0B05, "Xbox Elite Series 2" }, { 0x045E, 0x02E3, "Xbox Elite" },
        { 0x045E, 0x02EA, "Xbox One" }, { 0x045E, 0x02DD, "Xbox One" }, { 0x045E, 0x02E0, "Xbox One" }, { 0x045E, 0x02FD, "Xbox One" },
        { 0x045E, 0x028E, "Xbox 360" }, { 0x057E, 0x2009, "Switch Pro" }, { 0x28DE, 0x1205, "Steam Deck" },
        { 0x28DE, 0x1102, "Steam Controller" }, { 0x28DE, 0x1142, "Steam Controller" },
    };
    for (const M& m : k) if (m.v == vendor && m.p == product) return m.name;
    return nullptr;
}

// "<model> - USB - Serial ...<end>" (Flippage, 25 Nov: short; no battery): the model's short name when the USB ids say
// it (else SDL's name), how it's connected, and the end of the serial number - what tells two of the same pad apart.
static std::string Label(SDL_JoystickID id, const std::string& name) {
    const char* model = ModelName(SDL_GetGamepadVendorForID(id), SDL_GetGamepadProductForID(id));
    std::string label = model ? std::string(model) : name;
    // Opened briefly for what only an open pad reports; one the launcher already has open is shared, not taken.
    if (SDL_Gamepad* g = SDL_OpenGamepad(id)) {
        const SDL_JoystickConnectionState c = SDL_GetGamepadConnectionState(g);
        if (c == SDL_JOYSTICK_CONNECTION_WIRED) label += " - USB";
        else if (c == SDL_JOYSTICK_CONNECTION_WIRELESS) label += " - Bluetooth";
        if (const char* serial = SDL_GetGamepadSerial(g)) {
            const std::string sn = serial;
            const size_t keep = sn.find('-') != std::string::npos ? 5 : 4;   // "aa-bb-...-ee-ff" ends in a whole pair
            if (sn.size() > keep) label += " - Serial ..." + sn.substr(sn.size() - keep);
            else if (!sn.empty()) label += " - Serial " + sn;
        }
        SDL_CloseGamepad(g);
    }
    return label;
}

void RefreshControllers() {
    g_pads.clear();
    g_looked = true;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; ++i) {
        char guid[33] = {};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
        const char* path = SDL_GetGamepadPathForID(ids[i]);
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        DetectedDevice d;
        d.key = std::string("sdl:") + guid + "|" + (path && *path ? std::string(path) : "#" + std::to_string((unsigned long long)ids[i]));
        d.name = name && *name ? name : "Controller";
        d.label = Label(ids[i], d.name);
        g_pads.push_back(std::move(d));
    }
    SDL_free(ids);
}

std::vector<DetectedDevice> DetectControllers() {
    if (!g_looked) RefreshControllers();
    return g_pads;
}

}  // namespace cogbox
