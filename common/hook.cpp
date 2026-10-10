#include "hook.h"

#include <mutex>

#include "MinHook.h"
#include "log.h"

namespace csp::hook {
namespace {

bool Ready() {
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] {
    const MH_STATUS status = MH_Initialize();
    ready = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
    if (!ready) log::Error("MinHook would not start: %s", MH_StatusToString(status));
  });
  return ready;
}

}  // namespace

bool Install(void* target, void* detour, void** original, const char* what) {
  if (!Ready()) return false;
  MH_STATUS status = MH_CreateHook(target, detour, original);
  if (status == MH_OK) status = MH_EnableHook(target);
  if (status != MH_OK) {
    log::Error("could not hook %s at %p: %s", what, target, MH_StatusToString(status));
    MH_RemoveHook(target);
    return false;
  }
  log::Info("hooked %s at %p", what, target);
  return true;
}

void Remove(void* target) {
  if (!Ready()) return;
  MH_DisableHook(target);
  MH_RemoveHook(target);
}

}  // namespace csp::hook
