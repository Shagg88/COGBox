// Layer 2 (Unreal Engine 3): developer mode's scripted controller (ue3/script_input.h). Our code (MIT).
#include <ue3/script_input.h>
#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
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

// the movement verbs' live stick and buttons (their threads write, the device reads): a non-zero stick wins over the
// timed events' while it is set
std::atomic<int16_t> g_lx{0}, g_ly{0}, g_rx{0};
std::atomic<uint8_t> g_lt{0}, g_rt{0};
std::atomic<uint16_t> g_buttons{0};
std::atomic<bool> g_wantsChoice{false}, g_choiceOpen{false};

struct Event {
  double at = 0, until = 0;   // seconds since start
  uint16_t buttons = 0;
  int lt = -1, rt = -1;       // -1: not this event's
  bool left = false, right = false;
  int16_t x = 0, y = 0;       // the stick it moves
};

uint16_t ParseButtons(const std::string& s) {
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) return uint16_t(std::strtoul(s.c_str() + 2, nullptr, 16));
  // gears3-pc's bare hex is 4 digits (1000 = A); a name such as A or B is a hex digit too, so only 4 digits count
  if (s.size() == 4 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
    return uint16_t(std::strtoul(s.c_str(), nullptr, 16));
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
    if (const int16_t x = g_lx, y = g_ly; x || y) lx = x, ly = y;
    if (const int16_t x = g_rx; x) rx = x;
    if (const uint8_t v = g_lt; v) lt = v;
    if (const uint8_t v = g_rt; v) rt = v;
    buttons |= g_buttons;
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


// ---- the movement verbs (from gears3-pc's hid.cpp): they read the player through the engine's objects ----------------

namespace obj = ue3::obj;

uint32_t g_namesVar = 0, g_objectsVar = 0;
std::mutex g_drive;   // walkto / route / face / follow take turns: a script's times for them are "not before"

void Pause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint64_t Now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Ready() { return obj::Ready() || (g_namesVar && obj::Init(g_namesVar, g_objectsVar)); }

struct Actors {
  int loc = -1, rot = -1, pawn = -1;
  uint32_t pc = 0;
  bool Find() {
    if (loc >= 0 && pawn >= 0 && pc) return true;
    if (!Ready()) return false;
    const uint32_t actor = obj::FindClass("Actor"), controller = obj::FindClass("Controller");
    pc = obj::FindClass("GearPC");
    if (!actor || !controller || !pc) return false;
    loc = obj::PropertyOffset(actor, "Location");
    rot = obj::PropertyOffset(actor, "Rotation");
    pawn = obj::PropertyOffset(controller, "Pawn");
    return loc >= 0 && rot >= 0 && pawn >= 0;
  }
  // player 1's controller and pawn (the first GearPC with a pawn)
  bool Local(uint32_t& controller, uint32_t& p) {
    controller = p = 0;
    if (!Find()) return false;
    for (uint32_t c : obj::FindInstancesOf(pc))
      if (const uint32_t pw = obj::Read32(c + uint32_t(pawn))) { controller = c; p = pw; return true; }
    return false;
  }
  double X(uint32_t a) const { return obj::ReadFloat(a + uint32_t(loc)); }
  double Y(uint32_t a) const { return obj::ReadFloat(a + uint32_t(loc) + 4); }
  double Z(uint32_t a) const { return obj::ReadFloat(a + uint32_t(loc) + 8); }
  double Yaw(uint32_t controller) const {   // Gears moves relative to the camera: the controller's yaw
    return double(int32_t(obj::Read32(controller + uint32_t(rot) + 4)) & 0xFFFF) / 65536.0 * 2 * 3.14159265358979;
  }
} g_actors;

void Steer(double diff) {   // the left stick toward an angle off the camera's yaw (UE3: +y is right of +x)
  g_ly = int16_t(std::lround(std::cos(diff) * 32000));
  g_lx = int16_t(std::lround(std::sin(diff) * 32000));
}

// "walkto x y [s]": steer player 1 to (x, y); stops within 100 units, when stuck for 3 s, or after the time (20 s)
bool WalkStep(double tx, double ty, double seconds) {
  uint32_t pc = 0, pawn = 0;
  if (!g_actors.Local(pc, pawn)) { REXLOG_WARN("script_input: walkto: no player pawn"); return false; }
  const uint64_t t0 = Now();
  double best = 1e9, x = 0, y = 0;
  uint64_t bestAt = t0;
  bool arrived = false;
  for (;;) {
    if (Now() % 1000 < 50) { uint32_t c, p; if (g_actors.Local(c, p)) pc = c, pawn = p; }   // a new pawn after a respawn
    x = g_actors.X(pawn), y = g_actors.Y(pawn);
    const double dx = tx - x, dy = ty - y, dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 100) { arrived = true; break; }
    if (double(Now() - t0) / 1000.0 > seconds) break;
    if (dist < best - 20) best = dist, bestAt = Now();
    if (Now() - bestAt > 3000) { REXLOG_INFO("script_input: walkto: stuck"); break; }
    Steer(std::atan2(dy, dx) - g_actors.Yaw(pc));
    Pause(50);
  }
  g_lx = 0, g_ly = 0;
  REXLOG_INFO("script_input: walkto {:.0f},{:.0f}: ended at {:.0f},{:.0f} after {:.1f} s", tx, ty, x, y, double(Now() - t0) / 1000.0);
  return arrived;
}

// "face x y": the camera (right stick) turned toward (x, y), within 3 degrees or 5 s
void Face(double tx, double ty) {
  uint32_t pc = 0, pawn = 0;
  if (!g_actors.Local(pc, pawn)) { REXLOG_WARN("script_input: face: no player pawn"); return; }
  const uint64_t t0 = Now();
  double diff = 0;
  while (Now() - t0 < 5000) {
    diff = std::remainder(std::atan2(ty - g_actors.Y(pawn), tx - g_actors.X(pawn)) - g_actors.Yaw(pc), 2 * 3.14159265358979);
    if (std::fabs(diff) < 0.05) break;
    const double amount = std::clamp(std::fabs(diff), 0.35, 1.0) * (diff > 0 ? 1 : -1);   // out of the dead zone
    g_rx = int16_t(std::lround(amount * 32000));
    Pause(30);
  }
  g_rx = 0;
  REXLOG_INFO("script_input: face {:.0f},{:.0f}: {:.1f} degrees off", tx, ty, diff * 57.2958);
}

void Tap(uint16_t b) { g_buttons = b; Pause(200); g_buttons = 0; }
void Hold(int16_t lx, int16_t ly, int ms) { g_lx = lx, g_ly = ly; Pause(ms); g_lx = 0, g_ly = 0; }

// "route x,y;x,y;... [s per point]": the points in order; a point not reached is tried 4 more times (a vault over low
// cover, use, a step to either side, back), then skipped
void Route(const std::string& points, double seconds) {
  if (seconds <= 0) seconds = 20;
  std::vector<std::pair<double, double>> pts;
  std::stringstream ss(points);
  for (std::string item; std::getline(ss, item, ';');)
    if (const size_t c = item.find(','); c != std::string::npos) pts.emplace_back(atof(item.c_str()), atof(item.c_str() + c + 1));
  REXLOG_INFO("script_input: route of {} points", pts.size());
  for (size_t k = 0; k < pts.size(); k++) {
    bool reached = false;
    for (int attempt = 0; attempt < 5 && !reached; attempt++) {
      reached = WalkStep(pts[k].first, pts[k].second, seconds);
      if (reached) break;
      if (attempt == 0) {   // low cover in the way: face it, A into cover, forward + A vaults it, as a player does
        Face(pts[k].first, pts[k].second);
        g_ly = 32000; Pause(300); Tap(0x1000); Pause(700); Tap(0x1000); Pause(1200); g_ly = 0; Pause(300);
        Hold(0, -32000, 700);   // (out of cover, if no vault was there)
        continue;
      }
      Tap(0x4000); Pause(400);   // X: a door's button, a pickup
      if (attempt == 1) { Tap(0x1000); Pause(600); Hold(32000, 0, 1600); }
      else if (attempt == 2) Hold(0, -32000, 1000);
      else Hold(-32000, 0, 800);
      Pause(1000);
    }
    REXLOG_INFO("script_input: route point {}/{} ({:.0f},{:.0f}) {}", k + 1, pts.size(), pts[k].first, pts[k].second,
                reached ? "reached" : "skipped");
  }
  REXLOG_INFO("script_input: route done");
}

// "follow <class part> [s] [distance]": walk after the pawn whose class name has the text (COGDom: the squad leader,
// whose scripted moves lead the way), within the distance (250), for the time (60 s)
void Follow(const std::string& what, double seconds, double keep) {
  if (keep <= 0) keep = 250;
  if (seconds <= 0) seconds = 60;
  const uint32_t gearPawn = Ready() ? obj::FindClass("GearPawn") : 0;
  const uint64_t t0 = Now();
  uint64_t lastLog = 0, lastFind = 0;
  uint32_t pc = 0, pawn = 0, target = 0;
  while (double(Now() - t0) / 1000.0 < seconds) {
    if (Now() - lastFind > 1000) {
      lastFind = Now();
      g_actors.Local(pc, pawn);
      target = 0;
      for (uint32_t p : gearPawn ? obj::FindInstancesOf(gearPawn) : std::vector<uint32_t>{})
        if (p != pawn && obj::ObjectName(obj::ClassOf(p)).find(what) != std::string::npos) { target = p; break; }
    }
    if (!pawn || !target) { g_lx = 0, g_ly = 0; Pause(200); continue; }
    const double dx = g_actors.X(target) - g_actors.X(pawn), dy = g_actors.Y(target) - g_actors.Y(pawn);
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (Now() - lastLog > 5000) {
      lastLog = Now();
      REXLOG_INFO("script_input: follow {}: {:.0f} away", obj::ObjectName(target), dist);
    }
    if (dist < keep) { g_lx = 0, g_ly = 0; Pause(50); continue; }
    Steer(std::atan2(dy, dx) - g_actors.Yaw(pc));
    Pause(50);
  }
  g_lx = 0, g_ly = 0;
  REXLOG_INFO("script_input: follow {}: done", what);
}

// "teleport x y z [class]" (testing): player 1's pawn put there (or the first pawn of that class)
void Teleport(double x, double y, double z, const std::string& cls) {
  uint32_t pc = 0, pawn = 0;
  g_actors.Local(pc, pawn);
  if (!cls.empty() && Ready()) {
    const uint32_t c = obj::FindClass(cls.c_str());
    const std::vector<uint32_t> all = c ? obj::FindInstancesOf(c) : std::vector<uint32_t>{};
    pawn = all.empty() ? 0 : all[0];
  }
  if (!pawn) { REXLOG_WARN("script_input: teleport: no {}", cls.empty() ? "player pawn" : cls); return; }
  const float v[3] = {float(x), float(y), float(z)};
  for (int k = 0; k < 3; k++) {
    uint32_t u;
    std::memcpy(&u, &v[k], 4);
    obj::Write32(pawn + uint32_t(g_actors.loc) + 4 * k, u);
  }
  REXLOG_INFO("script_input: teleport {}to {:.0f},{:.0f},{:.0f}", cls.empty() ? "" : cls + " ", x, y, z);
}

// "pos": player 1's position in the log (to plan a route)
void Position() {
  uint32_t pc = 0, pawn = 0;
  if (!g_actors.Local(pc, pawn)) {
    const uint32_t cls = obj::Ready() ? obj::FindClass("GearPC") : 0;
    REXLOG_INFO("script_input: pos: no player pawn (objects {}, GearPC {:08X} x{}, offsets {} {} {})", obj::Ready(), cls,
                cls ? obj::FindInstancesOf(cls).size() : 0, g_actors.loc, g_actors.rot, g_actors.pawn);
    return;
  }
  REXLOG_INFO("script_input: pos {:.0f},{:.0f},{:.0f} yaw {:.0f}", g_actors.X(pawn), g_actors.Y(pawn), g_actors.Z(pawn),
              g_actors.Yaw(pc) * 57.2958);
}

// one movement line, at its time (on a thread of its own; the driving ones take turns)
void RunAction(const std::string& verb, const std::string& args) {
  std::stringstream in(args);
  if (verb == "pos") { Position(); return; }
  if (verb == "teleport") {
    double x = 0, y = 0, z = 0;
    std::string cls;
    in >> x >> y >> z >> cls;
    Teleport(x, y, z, cls);
    return;
  }
  // waits for the path choice (5 min), then its side's trigger until it closes; not one of the driving verbs: the walk
  // that brings the player to the choice runs meanwhile
  if (verb == "choose") {
    std::string side;
    in >> side;
    const bool left = side == "L" || side == "l";
    g_wantsChoice = true;
    const uint64_t t0 = Now();
    while (!g_choiceOpen && Now() - t0 < 300000) Pause(100);
    if (!g_choiceOpen) REXLOG_WARN("script_input: choose {}: no path choice came", side);
    for (int k = 0; k < 10 && g_choiceOpen; k++) {
      (left ? g_lt : g_rt) = 255; Pause(300); (left ? g_lt : g_rt) = 0; Pause(700);
    }
    REXLOG_INFO("script_input: choose {}: {} after {:.1f} s", side, g_choiceOpen ? "still open" : "chosen", double(Now() - t0) / 1000.0);
    g_wantsChoice = false;
    return;
  }
  std::lock_guard drive(g_drive);
  if (verb == "walkto") { double x = 0, y = 0, s = 20; in >> x >> y >> s; WalkStep(x, y, s); }
  else if (verb == "face") { double x = 0, y = 0; in >> x >> y; Face(x, y); }
  else if (verb == "route") { std::string pts; double s = 0; in >> pts >> s; Route(pts, s); }
  else if (verb == "follow") { std::string what; double s = 0, keep = 0; in >> what >> s >> keep; Follow(what, s, keep); }
}

}  // namespace

bool Ue3ScriptInputWantsPathChoice() { return g_wantsChoice; }
void Ue3ScriptInputPathChoiceOpen(bool open) { g_choiceOpen = open; }

void Ue3InstallScriptInputIfRequested(rex::Runtime* runtime, uint32_t names_var, uint32_t objects_var) {
  const std::string path = REXCVAR_GET(cogbox_input_script);
  if (path.empty() || !runtime) return;
  auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system());
  std::ifstream in(rex::to_path(path));
  if (!input || !in) {
    REXLOG_WARN("script_input: {} not read - no scripted controller", path);
    return;
  }
  std::vector<Event> events;
  g_namesVar = names_var, g_objectsVar = objects_var;
  struct Note { double at; std::string verb, text; };   // log / quit / the movement verbs
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
    } else if (verb == "capture") {   // gears3-pc's mouse capture: nothing to do here
      continue;
    } else if (verb == "log" || verb == "choose" || verb == "quit" || verb == "pos" || verb == "teleport" || verb == "walkto" ||
               verb == "face" || verb == "route" || verb == "follow") {
      std::string text;
      std::getline(ls, text);
      notes.push_back({t, verb, text});
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
  REXLOG_INFO("script_input: {} controller event(s), {} timed action(s) from {}{}", events.size(), notes.size(), path,
              bad ? " (" + std::to_string(bad) + " line(s) not understood)" : std::string());
  if (!notes.empty())
    std::thread([notes, start] {
      for (const Note& note : notes) {
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(note.at)));
        if (note.verb == "log" || note.verb == "quit") REXLOG_INFO("script_input: {:.1f} s {}{}", note.at, note.verb, note.text);
        if (note.verb == "quit") {
          rex::FlushLogging();
          TerminateProcess(GetCurrentProcess(), 0);
        }
        if (note.verb != "log" && note.verb != "quit")
          std::thread([note] { RunAction(note.verb, note.text); }).detach();
      }
    }).detach();
}
