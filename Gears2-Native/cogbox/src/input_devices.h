// COGBox - which keyboard-and-mouse or controller drives each split-screen player (SDK patches 117 + 128, --player_devices).
// The keys are the runtime's rex::input::DeviceKey: "kbm" for the keyboard and mouse, "sdl:<guid>|<path>" for a pad
// (path = SDL_GetGamepadPath, else "#<SDL instance id>"). Auto players take the devices nobody chose, keyboard and mouse
// first, then the pads in order (SDK patch 129): all Auto = keyboard and mouse player 1, the first pad player 2. The
// functions here that need no SDL are pure (tested in test/player_devices_test.cpp); the pad enumeration is in
// input_devices_sdl.cpp.
#pragma once
#include <string>
#include <vector>

namespace cogbox {

// A controller SDL sees. `name` is SDL's own name (kept with a saved choice for "(not connected)"); `label` adds what
// helps tell two apart: "<model> - USB - Serial ...<end>" (the model's short name when known, else SDL's name).
struct DetectedDevice { std::string key, name, label; };

// The keyboard and mouse's key and its name in the lists.
inline const char* KbmKey() { return "kbm"; }
inline const char* KbmLabel() { return "Keyboard & mouse"; }

// The controllers SDL sees now, in SDL's order (cached: the first call looks, RefreshControllers() looks again - the
// Gameplay tab calls it when it opens). The keyboard and mouse is not in the list.
std::vector<DetectedDevice> DetectControllers();
void RefreshControllers();

// One key per player ("" = Auto). `chosen` is shorter or longer than `players`: padded with Auto / cut. A key chosen
// for two players belongs to the LATER one (the earlier player becomes Auto).
std::vector<std::string> ResolvePlayerDevices(const std::vector<std::string>& chosen, int players);

// "--player_devices=a;b" (Auto = "auto") for 2 or more players - always, so the runtime applies Auto's order (keyboard
// and mouse first); "" for one player (the game switches by itself).
std::string PlayerDevicesArg(const std::vector<std::string>& resolved);

// What each player will get, as the runtime does it (SDK patch 129): a chosen device that is plugged in is that
// player's; the Auto players (and those whose chosen pad is not plugged in), lowest first, take the devices nobody
// chose - keyboard and mouse first (when `kbm_on`), then the detected pads in order. "" = nothing left for that player.
std::vector<std::string> EffectiveDevices(const std::vector<std::string>& resolved, const std::vector<DetectedDevice>& detected,
                                          bool kbm_on);

// What a PlayerDeviceN setting keeps: the key, and the name last seen for it (for "(not connected)").
std::string PackPlayerDevice(const std::string& key, const std::string& name);
void UnpackPlayerDevice(const std::string& packed, std::string& key, std::string& name);

struct DeviceChoice { std::string key, label; };
// The dropdown's list: Auto, Keyboard & mouse, the detected controllers by label (a second of the same label reads
// "<label> #2"), then - when the saved key is a pad that is not detected - "<saved name> (not connected)".
std::vector<DeviceChoice> PlayerDeviceChoices(const std::vector<DetectedDevice>& detected, const std::string& savedKey,
                                              const std::string& savedName);

// The list label of a key ("Keyboard & mouse", a detected pad's label, "" when unknown).
std::string DeviceLabel(const std::vector<DeviceChoice>& choices, const std::string& key);

}  // namespace cogbox
