// Stands in for the XInput DLL a game loads from its own folder, and hands
// everything on to the real one in the system folder. Built twice: as
// xinput9_1_0.dll for Crash and as xinput1_3.dll for Spyro (which asks for
// its functions by number, hence the two .def files).
//
// CSP_REAL_XINPUT is the system DLL this build stands in for.
#include <windows.h>

#include <atomic>
#include <initializer_list>

#include "log.h"
#include "memory.h"
#include "pad.h"

namespace csp::pad {
namespace {

std::atomic<Filter> g_filter{nullptr};

struct Real {
  HMODULE module = nullptr;
  FARPROC getState = nullptr, setState = nullptr, getCapabilities = nullptr, enable = nullptr,
          getAudioGuids = nullptr, getBattery = nullptr, getKeystroke = nullptr;
  FARPROC byNumber[4] = {};  // 100..103, which have no names
};

// Loaded on the first call, not in DllMain: loading a library from inside
// the loader is asking for a deadlock.
const Real& Get() {
  static const Real real = [] {
    Real r;
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    // The one this build stands in for, then the one every Windows since 8
    // has, which answers the same calls.
    for (const wchar_t* name : {CSP_REAL_XINPUT, L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
      if (length == 0 || length + lstrlenW(name) + 2 > MAX_PATH) break;
      path[length] = L'\\';
      lstrcpyW(path + length + 1, name);
      r.module = LoadLibraryW(path);
      if (r.module) break;
    }
    if (!r.module) return r;
    r.getState = GetProcAddress(r.module, "XInputGetState");
    r.setState = GetProcAddress(r.module, "XInputSetState");
    r.getCapabilities = GetProcAddress(r.module, "XInputGetCapabilities");
    r.enable = GetProcAddress(r.module, "XInputEnable");
    r.getAudioGuids = GetProcAddress(r.module, "XInputGetDSoundAudioDeviceGuids");
    r.getBattery = GetProcAddress(r.module, "XInputGetBatteryInformation");
    r.getKeystroke = GetProcAddress(r.module, "XInputGetKeystroke");
    for (int i = 0; i < 4; ++i) r.byNumber[i] = GetProcAddress(r.module, MAKEINTRESOURCEA(100 + i));
    return r;
  }();
  return real;
}

template <typename Fn>
Fn As(FARPROC p) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(p));
}

}  // namespace

void SetFilter(Filter filter) { g_filter.store(filter); }

// What the exported functions do, as functions with no name outside this
// DLL (see TakeBack in pad.h for why that matters).
__declspec(noinline) DWORD WINAPI GetStateInner(DWORD index, void* state) {
  const auto real = As<DWORD(WINAPI*)(DWORD, void*)>(Get().getState);
  DWORD result = real ? real(index, state) : ERROR_DEVICE_NOT_CONNECTED;
  if (result == 0 && state) {
    // Which way the player's controller comes in decides which filter
    // matters, so it is said once, the first time something is really
    // pressed or pushed.
    static std::atomic<bool> said{false};
    const State* seen = static_cast<const State*>(state);
    if ((seen->gamepad.buttons || seen->gamepad.leftX > 12000 || seen->gamepad.leftX < -12000 || seen->gamepad.leftY > 12000 ||
         seen->gamepad.leftY < -12000) &&
        !said.exchange(true)) {
      log::Info("pad: the player's controller is reaching the game through XInput (controller %lu)", index);
    }
  }
  if (const Filter filter = g_filter.load()) {
    if (state) result = filter(index, result, static_cast<State*>(state));
  }
  return result;
}

__declspec(noinline) DWORD WINAPI SetStateInner(DWORD index, void* vibration) {
  const auto real = As<DWORD(WINAPI*)(DWORD, void*)>(Get().setState);
  return real ? real(index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

int TakeBack() {
  // The game imports from this DLL under the name of the one it stands in for.
  char name[64] = "";
  WideCharToMultiByte(CP_ACP, 0, CSP_REAL_XINPUT, -1, name, sizeof(name), nullptr, nullptr);
  static int taken = 0;
  if (mem::PatchImport(name, "XInputGetState", reinterpret_cast<void*>(&GetStateInner))) ++taken;
  if (mem::PatchImport(name, "XInputSetState", reinterpret_cast<void*>(&SetStateInner))) ++taken;
  return taken;
}

bool GameInFront() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace csp::pad

using csp::pad::As;
using csp::pad::Get;

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD index, void* state) { return csp::pad::GetStateInner(index, state); }

DWORD WINAPI Proxy_XInputSetState(DWORD index, void* vibration) { return csp::pad::SetStateInner(index, vibration); }

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD index, DWORD flags, void* capabilities) {
  const auto real = As<DWORD(WINAPI*)(DWORD, DWORD, void*)>(Get().getCapabilities);
  return real ? real(index, flags, capabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI Proxy_XInputEnable(BOOL enable) {
  if (const auto real = As<void(WINAPI*)(BOOL)>(Get().enable)) real(enable);
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD index, void* render, void* capture) {
  const auto real = As<DWORD(WINAPI*)(DWORD, void*, void*)>(Get().getAudioGuids);
  return real ? real(index, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD index, BYTE type, void* information) {
  const auto real = As<DWORD(WINAPI*)(DWORD, BYTE, void*)>(Get().getBattery);
  return real ? real(index, type, information) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD index, DWORD reserved, void* keystroke) {
  const auto real = As<DWORD(WINAPI*)(DWORD, DWORD, void*)>(Get().getKeystroke);
  return real ? real(index, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

// 100: the same as XInputGetState but with the guide button reported.
DWORD WINAPI Proxy_XInputGetStateEx(DWORD index, void* state) {
  const auto real = As<DWORD(WINAPI*)(DWORD, void*)>(Get().byNumber[0]);
  if (!real) return Proxy_XInputGetState(index, state);
  DWORD result = real(index, state);
  if (const csp::pad::Filter filter = csp::pad::g_filter.load()) {
    if (state) result = filter(index, result, static_cast<csp::pad::State*>(state));
  }
  return result;
}

DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD index, DWORD flags, void* out) {
  const auto real = As<DWORD(WINAPI*)(DWORD, DWORD, void*)>(Get().byNumber[1]);
  return real ? real(index, flags, out) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD index) {
  const auto real = As<DWORD(WINAPI*)(DWORD)>(Get().byNumber[2]);
  return real ? real(index) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputPowerOffController(DWORD index) {
  const auto real = As<DWORD(WINAPI*)(DWORD)>(Get().byNumber[3]);
  return real ? real(index) : ERROR_DEVICE_NOT_CONNECTED;
}

// Something for tools/install.ps1 to tell one of ours from somebody else's
// DLL of the same name before it overwrites or removes anything.
extern const char CrashSpyroBridgeMarker[];
const char CrashSpyroBridgeMarker[] = "CrashSpyroBridgeMarker";

}  // extern "C"
