#include "steampad.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "log.h"
#include "memory.h"

namespace csp::steampad {
namespace {

// ISteamController, version 006: which entry of its table of methods each
// of these is. Read out of both games' own steam_api64.dll, whose plain-C
// wrappers are "fetch the table, jump to entry N" (docs/PHASE0.md).
constexpr int kGetDigitalActionData = 0x68 / 8;
constexpr int kGetAnalogActionData = 0x80 / 8;

// As Steam declares them. A method returning one of these is handed, after
// `this`, the place to write it.
struct Digital {
  bool state;
  bool active;
};
struct Analog {
  int32_t mode;
  float x, y;
  bool active;
};
using DigitalFn = Digital* (*)(void* self, Digital* out, uint64_t controller, uint64_t action);
using AnalogFn = Analog* (*)(void* self, Analog* out, uint64_t controller, uint64_t action);

DigitalFn g_realDigital = nullptr;
AnalogFn g_realAnalog = nullptr;
std::atomic<Policy> g_policy{nullptr};
std::atomic<bool> g_installed{false};

// The analog actions seen. Written from the game's thread, read by the
// worker for the log; a torn number in a log line does no harm.
struct Seen {
  std::atomic<uint64_t> action{0};
  std::atomic<int32_t> mode{0};
  std::atomic<float> x{0}, y{0};
};
Seen g_seen[12];

void Note(uint64_t action, const Analog& data) {
  for (Seen& seen : g_seen) {
    uint64_t held = seen.action.load(std::memory_order_relaxed);
    if (held == 0 && seen.action.compare_exchange_strong(held, action)) held = action;
    if (held != action) continue;
    seen.mode.store(data.mode, std::memory_order_relaxed);
    if (std::fabs(data.x) > seen.x.load(std::memory_order_relaxed)) seen.x.store(std::fabs(data.x), std::memory_order_relaxed);
    if (std::fabs(data.y) > seen.y.load(std::memory_order_relaxed)) seen.y.store(std::fabs(data.y), std::memory_order_relaxed);
    return;
  }
}

// Which way the player's controller comes in decides which filter matters,
// so it is said once, the first time something is really pressed or pushed.
std::atomic<bool> g_saidUsed{false};
void SayUsed(const char* what) {
  if (!g_saidUsed.exchange(true)) log::Info("steam: the player's controller is reaching the game through Steam's interface (%s)", what);
}

Digital* OnDigital(void* self, Digital* out, uint64_t controller, uint64_t action) {
  Digital* result = g_realDigital(self, out, controller, action);
  if (out && out->state) SayUsed("a button");
  const Policy policy = g_policy.load(std::memory_order_relaxed);
  if (out && policy && policy(false, action)) out->state = false;
  return result;
}

Analog* OnAnalog(void* self, Analog* out, uint64_t controller, uint64_t action) {
  Analog* result = g_realAnalog(self, out, controller, action);
  if (!out) return result;
  Note(action, *out);
  if (std::fabs(out->x) > 0.4f || std::fabs(out->y) > 0.4f) SayUsed("a stick");
  const Policy policy = g_policy.load(std::memory_order_relaxed);
  if (policy && policy(true, action)) out->x = out->y = 0.0f;
  return result;
}

}  // namespace

bool Install(Policy policy) {
  g_policy.store(policy);
  if (g_installed.load()) return true;

  const HMODULE api = GetModuleHandleW(L"steam_api64.dll");
  if (!api) return false;
  const auto user = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(api, "SteamAPI_GetHSteamUser")));
  const auto pipe = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(api, "SteamAPI_GetHSteamPipe")));
  const auto create = reinterpret_cast<void* (*)(const char*)>(reinterpret_cast<void*>(GetProcAddress(api, "SteamInternal_CreateInterface")));
  const auto controllerOf = reinterpret_cast<void* (*)(void*, int, int, const char*)>(
      reinterpret_cast<void*>(GetProcAddress(api, "SteamAPI_ISteamClient_GetISteamController")));
  if (!user || !pipe || !create || !controllerOf) return false;
  // Both are zero until the game has started Steam up.
  const int steamUser = user(), steamPipe = pipe();
  if (!steamUser || !steamPipe) return false;
  void* client = create("SteamClient017");
  void* controller = client ? controllerOf(client, steamUser, steamPipe, "SteamController006") : nullptr;
  if (!controller) return false;

  // The interface is one object shared by everything in the process that
  // asks for it, so replacing its methods here replaces them for the game.
  // What is there now is learnt before it is replaced: the game may call in
  // between.
  void** methods = *reinterpret_cast<void***>(controller);
  g_realDigital = reinterpret_cast<DigitalFn>(methods[kGetDigitalActionData]);
  g_realAnalog = reinterpret_cast<AnalogFn>(methods[kGetAnalogActionData]);
  if (!mem::PatchVtable(controller, kGetDigitalActionData, reinterpret_cast<void*>(&OnDigital)) ||
      !mem::PatchVtable(controller, kGetAnalogActionData, reinterpret_cast<void*>(&OnAnalog))) {
    log::Error("steam: could not get between the game and Steam's controller interface");
    return false;
  }
  g_installed.store(true);
  log::Info("steam: between the game and Steam's controller interface (%p)", controller);
  return true;
}

void DescribeAnalog(char* out, int size) {
  int used = 0;
  out[0] = 0;
  for (const Seen& seen : g_seen) {
    const uint64_t action = seen.action.load(std::memory_order_relaxed);
    if (!action || used >= size - 60) break;
    used += std::snprintf(out + used, size - used, "%s#%llu mode %d max (%.2f, %.2f)", used ? "; " : "",
                          static_cast<unsigned long long>(action), seen.mode.load(), seen.x.load(), seen.y.load());
  }
}

}  // namespace csp::steampad
