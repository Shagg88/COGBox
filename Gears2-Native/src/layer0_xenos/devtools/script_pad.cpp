// Layer 0 (game-free): scripted gamepad for unattended tests (roadmap Stage 0, layer 0 test tooling).
// --gow2_input_script="35:START, 41:A, 47:DPAD_DOWN+A:0.5" presses those buttons at those seconds after the driver is
// added (game start), each held for the given time (default 0.2 s). The pad shows up as an extra controller; the
// user's own controllers keep working. Off unless the option is set.
// --gow2_input_frames: times and holds count swaps (finished frames) instead of seconds, so a scripted route presses
// its buttons on the same frame every run (needed for repeatable runs, D19). Default hold is then 6 frames.
#include <xenos/dev_tools.h>
#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(gow2_input_script, "", "Gears2",
                      "Scripted gamepad: comma list of seconds:BUTTON[+BUTTON][:hold_seconds]");
REXCVAR_DEFINE_STRING(gow2_input_script2, "", "Gears2",
                      "A second scripted gamepad (player 2: co-op / split screen), same syntax as --gow2_input_script");
REXCVAR_DEFINE_BOOL(gow2_input_frames, false, "Gears2",
                    "--gow2_input_script times and holds count swaps instead of seconds");

namespace {

using namespace rex;
using namespace rex::input;
constexpr DeviceId kScriptPad = static_cast<DeviceId>(0x47325350);   // "G2SP"
constexpr DeviceId kScriptPad2 = static_cast<DeviceId>(0x47325332);  // "G2S2"

struct Step { double at, hold; uint16_t buttons; uint8_t lt, rt; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

uint16_t ButtonBit(const std::string& n) {
  static const struct { const char* name; uint16_t bit; } kMap[] = {
      {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
      {"START", 0x0010},   {"BACK", 0x0020},      {"LS", 0x0040},        {"RS", 0x0080},
      {"LB", 0x0100},      {"RB", 0x0200},        {"A", 0x1000},         {"B", 0x2000},
      {"X", 0x4000},       {"Y", 0x8000}};
  for (auto& m : kMap) if (n == m.name) return m.bit;
  return 0;
}

std::vector<Step> Parse(const std::string& text) {
  std::vector<Step> steps;
  std::stringstream all(text);
  std::string item;
  while (std::getline(all, item, ',')) {
    item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
    if (item.empty()) continue;
    Step s{0, REXCVAR_GET(gow2_input_frames) ? 6.0 : 0.2, 0, 0, 0};
    std::stringstream parts(item);
    std::string at, names, hold;
    std::getline(parts, at, ':'); std::getline(parts, names, ':'); std::getline(parts, hold, ':');
    s.at = std::atof(at.c_str());
    if (!hold.empty()) s.hold = std::atof(hold.c_str());
    std::stringstream ns(names);
    std::string n;
    while (std::getline(ns, n, '+')) {
      // Sticks, fully deflected (notes/64: moving-camera test routes): LUP / LDOWN / LLEFT / LRIGHT, RUP / ... .
      constexpr int16_t kFull = 32767;
      if (n == "LT") s.lt = 255; else if (n == "RT") s.rt = 255;
      else if (n == "LUP") s.ly = kFull; else if (n == "LDOWN") s.ly = -kFull;
      else if (n == "LRIGHT") s.lx = kFull; else if (n == "LLEFT") s.lx = -kFull;
      else if (n == "RUP") s.ry = kFull; else if (n == "RDOWN") s.ry = -kFull;
      else if (n == "RRIGHT") s.rx = kFull; else if (n == "RLEFT") s.rx = -kFull;
      else if (uint16_t b = ButtonBit(n)) s.buttons |= b;
      else REXLOG_WARN("gow2_input_script: unknown button '{}'", n);
    }
    steps.push_back(s);
  }
  return steps;
}

class ScriptPad final : public InputDriver {
 public:
  ScriptPad(std::vector<Step> steps, DeviceId id, const char* name)
      : InputDriver(nullptr, 0), id_(id), name_(name), steps_(std::move(steps)), start_(std::chrono::steady_clock::now()),
        frames_(REXCVAR_GET(gow2_input_frames)) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = id_;
    info.name = name_;
    info.synthetic = false;  // a real guest user, so the game sees it as a controller
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
    const double t = frames_ ? double(XenosSwapCount())
                             : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0; uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    for (size_t i = 0; i < steps_.size(); ++i) {
      const Step& s = steps_[i];
      if (t >= s.at && t < s.at + s.hold) {
        buttons |= s.buttons; lt = std::max(lt, s.lt); rt = std::max(rt, s.rt);
        if (s.lx) lx = s.lx;
        if (s.ly) ly = s.ly;
        if (s.rx) rx = s.rx;
        if (s.ry) ry = s.ry;
        if (!logged_[i]) { logged_[i] = true; REXLOG_INFO("gow2_input_script{}: t={:.1f}{} press 0x{:04X} for {:.1f}", id_ == kScriptPad2 ? "2" : "", t, frames_ ? " frames" : "s", s.buttons, s.hold); }
      }
    }
    const uint64_t sticks = uint64_t(uint16_t(lx)) | uint64_t(uint16_t(ly)) << 16 | uint64_t(uint16_t(rx)) << 32 | uint64_t(uint16_t(ry)) << 48;
    if (buttons != last_ || lt != last_lt_ || rt != last_rt_ || sticks != last_sticks_) {
      ++packet_; last_ = buttons; last_lt_ = lt; last_rt_ = rt; last_sticks_ = sticks;
    }
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = packet_;
      out->gamepad.buttons = buttons;
      out->gamepad.left_trigger = lt;
      out->gamepad.right_trigger = rt;
      out->gamepad.thumb_lx = lx;
      out->gamepad.thumb_ly = ly;
      out->gamepad.thumb_rx = rx;
      out->gamepad.thumb_ry = ry;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01; caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF; caps->gamepad.left_trigger = 0xFF; caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF); caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF); caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == id_ ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == id_ ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  DeviceId id_;
  const char* name_;
  std::vector<Step> steps_;
  std::vector<bool> logged_ = std::vector<bool>(steps_.size(), false);
  std::chrono::steady_clock::time_point start_;
  bool frames_ = false;
  uint32_t packet_ = 1;
  uint16_t last_ = 0;
  uint8_t last_lt_ = 0, last_rt_ = 0;
  uint64_t last_sticks_ = 0;
};

}  // namespace

void XenosAddScriptPadIfRequested(rex::Runtime* runtime) {
  const std::string script = REXCVAR_GET(gow2_input_script);
  const std::string script2 = REXCVAR_GET(gow2_input_script2);
  if ((script.empty() && script2.empty()) || runtime == nullptr || runtime->input_system() == nullptr) return;
  if (REXCVAR_GET(gow2_input_frames)) XenosEnsureObserverHub();  // the swap count lives in the observer hub
  auto* input = static_cast<InputSystem*>(runtime->input_system());
  // Player 1's pad is added first (even with no steps) so it stays the first controller.
  auto steps = Parse(script);
  REXLOG_INFO("gow2_input_script: {} step(s)", steps.size());
  input->AddDriver(std::make_unique<ScriptPad>(std::move(steps), kScriptPad, "Gears2-Native script pad"));
  if (!script2.empty()) {
    auto steps2 = Parse(script2);
    REXLOG_INFO("gow2_input_script2: {} step(s) (a second controller)", steps2.size());
    input->AddDriver(std::make_unique<ScriptPad>(std::move(steps2), kScriptPad2, "Gears2-Native script pad 2"));
  }
}
