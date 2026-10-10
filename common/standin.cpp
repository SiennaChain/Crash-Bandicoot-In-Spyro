#include "standin.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>

#include "log.h"

namespace csp::standin {
namespace {

std::atomic<uint16_t> g_buttons{0};
std::atomic<int> g_x{0}, g_y{0};    // left stick, -32767..32767
std::atomic<int> g_rx{0}, g_ry{0};  // right stick
std::atomic<bool> g_present{false};

bool Parse(const std::string& text, uint16_t& buttons, int& x, int& y, int& rx, int& ry) {
  static const struct {
    const char* name;
    uint16_t button;
    int x, y, rx, ry;
  } kNames[] = {{"UP", 0x0001, 0, 0, 0, 0},     {"DOWN", 0x0002, 0, 0, 0, 0},    {"LEFT", 0x0004, 0, 0, 0, 0},    {"RIGHT", 0x0008, 0, 0, 0, 0},
                {"START", 0x0010, 0, 0, 0, 0},  {"BACK", 0x0020, 0, 0, 0, 0},    {"LB", 0x0100, 0, 0, 0, 0},      {"RB", 0x0200, 0, 0, 0, 0},
                {"A", 0x1000, 0, 0, 0, 0},      {"B", 0x2000, 0, 0, 0, 0},       {"X", 0x4000, 0, 0, 0, 0},       {"Y", 0x8000, 0, 0, 0, 0},
                {"LS_UP", 0, 0, 32767, 0, 0},   {"LS_DOWN", 0, 0, -32767, 0, 0}, {"LS_LEFT", 0, -32767, 0, 0, 0}, {"LS_RIGHT", 0, 32767, 0, 0, 0},
                {"RS_UP", 0, 0, 0, 0, 32767},   {"RS_DOWN", 0, 0, 0, 0, -32767}, {"RS_LEFT", 0, 0, 0, -32767, 0}, {"RS_RIGHT", 0, 0, 0, 32767, 0}};
  buttons = 0;
  x = y = rx = ry = 0;
  for (size_t at = 0; at <= text.size();) {
    const size_t plus = text.find('+', at);
    const std::string name = text.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
    bool known = false;
    // A stick held anywhere: LS:0.5,-0.87 is half right and most of the way down.
    if (name.size() > 3 && (name[0] == 'L' || name[0] == 'R') && name[1] == 'S' && name[2] == ':') {
      char* end = nullptr;
      const double across = std::strtod(name.c_str() + 3, &end);
      if (end && *end == ',') {
        const double up = std::strtod(end + 1, &end);
        if (end && *end == 0 && across >= -1.0 && across <= 1.0 && up >= -1.0 && up <= 1.0) {
          (name[0] == 'L' ? x : rx) = static_cast<int>(across * 32767.0);
          (name[0] == 'L' ? y : ry) = static_cast<int>(up * 32767.0);
          known = true;
        }
      }
    }
    for (const auto& entry : kNames) {
      if (name != entry.name) continue;
      buttons |= entry.button;
      if (entry.x) x = entry.x;
      if (entry.y) y = entry.y;
      if (entry.rx) rx = entry.rx;
      if (entry.ry) ry = entry.ry;
      known = true;
    }
    if (!known) return false;
    if (plus == std::string::npos) break;
    at = plus + 1;
  }
  return true;
}

struct Press {
  uint16_t buttons;
  int x, y, rx, ry;
  DWORD ms;
};

DWORD WINAPI HoldThenLetGo(void* given) {
  Press* press = static_cast<Press*>(given);
  g_buttons.store(press->buttons);
  g_x.store(press->x);
  g_y.store(press->y);
  g_rx.store(press->rx);
  g_ry.store(press->ry);
  Sleep(press->ms);
  g_buttons.store(0);
  g_x.store(0);
  g_y.store(0);
  g_rx.store(0);
  g_ry.store(0);
  delete press;
  return 0;
}

}  // namespace

void Present(bool on) { g_present.store(on); }

bool Hold(const std::string& what, uint32_t ms) {
  auto* press = new Press{0, 0, 0, 0, 0, ms};
  if (!Parse(what, press->buttons, press->x, press->y, press->rx, press->ry)) {
    delete press;
    return false;
  }
  CloseHandle(CreateThread(nullptr, 0, HoldThenLetGo, press, 0, nullptr));
  return true;
}

bool Fill(uint32_t index, pad::State* state) {
  const uint16_t buttons = g_buttons.load(std::memory_order_relaxed);
  const int x = g_x.load(std::memory_order_relaxed), y = g_y.load(std::memory_order_relaxed);
  const int rx = g_rx.load(std::memory_order_relaxed), ry = g_ry.load(std::memory_order_relaxed);
  if (index != 0 || !(g_present.load(std::memory_order_relaxed) || buttons || x || y || rx || ry)) return false;
  // A new number every reading: a game may skip a reading whose number has
  // not changed.
  static std::atomic<uint32_t> packet{0};
  *state = {};
  state->packet = packet.fetch_add(1) + 1;
  state->gamepad.buttons = buttons;
  state->gamepad.leftX = static_cast<int16_t>(x);
  state->gamepad.leftY = static_cast<int16_t>(y);
  state->gamepad.rightX = static_cast<int16_t>(rx);
  state->gamepad.rightY = static_cast<int16_t>(ry);
  return true;
}

void DebugPress(const Config& config) {
  static bool primed = false;
  static std::string last;
  const std::string now = config.GetString("debug", "press");
  if (!primed || now == last) {
    primed = true;
    last = now;
    return;
  }
  last = now;
  if (now.empty()) return;
  const size_t space = now.find(' ');
  const std::string what = now.substr(0, space);
  uint32_t ms = 150;
  if (space != std::string::npos) {
    const int given = std::atoi(now.c_str() + space + 1);
    if (given > 0 && given <= 20000) ms = static_cast<uint32_t>(given);
  }
  if (Hold(what, ms)) {
    log::Info("debug: holding %s for %u ms", what.c_str(), ms);
  } else {
    log::Info("debug: do not know \"%s\"", what.c_str());
  }
}

}  // namespace csp::standin
