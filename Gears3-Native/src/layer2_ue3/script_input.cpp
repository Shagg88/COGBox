// Layer 2 (Unreal Engine 3): developer mode's scripted controller (ue3/script_input.h). Our code (MIT).
#include <ue3/script_input.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

REXCVAR_DEFINE_STRING(cogbox_input_script, "", "COGBox",
                      "Developer mode: a text file of timed controller events played into player 1's controller "
                      "(ue3/script_input.h)");

namespace {

using namespace rex;   // X_STATUS, X_RESULT and the X_ERROR_ / X_STATUS_ values
using namespace rex::input;   // the XInput types and XINPUT_ values
using rex::input::DeviceId;
using rex::input::DeviceInfo;

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x5C121700ull);

struct Event {
  double at = 0, until = 0;   // seconds since start
  uint16_t buttons = 0;
  int lt = -1, rt = -1;       // -1: not this event's
  bool left = false, right = false;
  int16_t x = 0, y = 0;       // the stick it moves
};

uint16_t ParseButtons(const std::string& s) {
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) return uint16_t(std::strtoul(s.c_str() + 2, nullptr, 16));
  static const struct { const char* name; uint16_t mask; } kNames[] = {
      {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
      {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000},
      {"X", 0x4000}, {"Y", 0x8000}};
  uint16_t mask = 0;
  std::stringstream in(s);
  for (std::string part; std::getline(in, part, '+');) {
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    for (const auto& n : kNames)
      if (part == n.name) mask |= n.mask;
  }
  return mask;
}

int16_t Axis(double v) { return int16_t(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0)); }

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  ScriptInputDriver(std::vector<Event> events, std::chrono::steady_clock::time_point start)
      : InputDriver(nullptr, 0), events_(std::move(events)), start_(start) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kScriptDevice;
    info.name = "Scripted controller (developer mode)";
    info.subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
    info.synthetic = true;   // player 1's, merged with the real devices (as keyboard and mouse are)
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    for (const Event& e : events_) {
      if (now < e.at || now >= e.until) continue;
      buttons |= e.buttons;
      if (e.lt >= 0) lt = uint8_t(e.lt);
      if (e.rt >= 0) rt = uint8_t(e.rt);
      if (e.left) lx = e.x, ly = e.y;
      if (e.right) rx = e.x, ry = e.y;
    }
    std::memset(out, 0, sizeof(*out));
    out->packet_number = uint32_t(++packet_);
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = lt;
    out->gamepad.right_trigger = rt;
    out->gamepad.thumb_lx = lx;
    out->gamepad.thumb_ly = ly;
    out->gamepad.thumb_rx = rx;
    out->gamepad.thumb_ry = ry;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = XINPUT_DEVTYPE_GAMEPAD;
      caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kScriptDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }

 private:
  std::vector<Event> events_;
  std::chrono::steady_clock::time_point start_;
  uint32_t packet_ = 0;
};

}  // namespace

void Ue3InstallScriptInputIfRequested(rex::Runtime* runtime) {
  const std::string path = REXCVAR_GET(cogbox_input_script);
  if (path.empty() || !runtime) return;
  auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system());
  std::ifstream in(rex::to_path(path));
  if (!input || !in) {
    REXLOG_WARN("script_input: {} not read - no scripted controller", path);
    return;
  }
  std::vector<Event> events;
  struct Note { double at; std::string text; bool quit; };
  std::vector<Note> notes;
  int bad = 0, n = 0;
  for (std::string line; std::getline(in, line);) {
    n++;
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::stringstream ls(line);
    double t = 0;
    std::string verb;
    if (!(ls >> t >> verb)) continue;
    std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Event e;
    e.at = t;
    if (verb == "pad") {
      std::string b;
      double ms = 150;
      ls >> b >> ms;
      e.buttons = ParseButtons(b);
      e.until = t + ms / 1000.0;
      if (!e.buttons) { bad++; REXLOG_WARN("script_input: line {}: no button in '{}'", n, b); continue; }
    } else if (verb == "trig") {
      std::string side;
      double ms = 200;
      int value = 255;
      ls >> side >> ms >> value;
      (side == "L" || side == "l" ? e.lt : e.rt) = std::clamp(value, 0, 255);
      e.until = t + ms / 1000.0;
    } else if (verb == "stick" || verb == "look") {
      std::string side = "R";
      double x = 0, y = 0, ms = 500;
      if (verb == "stick") ls >> side >> x >> y >> ms;
      else ls >> y >> ms;
      (side == "L" || side == "l" ? e.left : e.right) = true;
      e.x = Axis(x);
      e.y = Axis(y);
      e.until = t + ms / 1000.0;
    } else if (verb == "log" || verb == "quit") {
      std::string text;
      std::getline(ls, text);
      notes.push_back({t, text, verb == "quit"});
      continue;
    } else {
      bad++;
      REXLOG_WARN("script_input: line {}: unknown verb '{}'", n, verb);
      continue;
    }
    events.push_back(e);
  }
  const auto start = std::chrono::steady_clock::now();
  input->AddDriver(std::make_unique<ScriptInputDriver>(events, start));
  REXLOG_INFO("script_input: {} controller event(s), {} note(s) from {}{}", events.size(), notes.size(), path,
              bad ? " (" + std::to_string(bad) + " line(s) not understood)" : std::string());
  if (!notes.empty())
    std::thread([notes, start] {
      for (const Note& note : notes) {
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(note.at)));
        REXLOG_INFO("script_input: {:.1f} s{}{}", note.at, note.quit ? " quit" : ":", note.text);
        if (note.quit) {
          rex::FlushLogging();
          TerminateProcess(GetCurrentProcess(), 0);
        }
      }
    }).detach();
}
