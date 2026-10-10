// Whether the mod is to do anything in this run of a game.
//
// Each bridge is a stand-in for a DLL the game loads from its own folder, so
// the game loads it every time it starts, including when somebody just wants
// to play Crash or Spyro. Neither is to notice. A bridge does nothing at all
// (no thread, no hook, nothing of the game's changed) unless both of these
// hold:
//
//   The exe it finds itself in is the one it was made for: by name, and by
//   the two numbers in an exe's header that tell one build of it from
//   another. Every address the bridge knows is a place in that build and in
//   no other; in another they are somebody else's memory.
//
//   The launcher started it. The launcher holds a named object (kSessionName)
//   from before it starts the first game until the last has gone.
//
// crashspyro.ini can waive either, for working on the mod:
//   [bridge] without_launcher = 1    any_exe = 1
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace csp::session {

struct ExeId {
  uint32_t stamp = 0;      // when it was linked, as the linker wrote it
  uint32_t imageSize = 0;  // how much memory it takes up loaded
};
inline bool operator==(const ExeId& a, const ExeId& b) { return a.stamp == b.stamp && a.imageSize == b.imageSize; }
inline bool operator!=(const ExeId& a, const ExeId& b) { return !(a == b); }

struct Game {
  const wchar_t* exe;
  ExeId id;
};
// The Steam builds every address was found in (Crash: build 2964270, exe
// SHA-256 170c61d3...; Spyro: build 3849171, exe SHA-256 996a7789...).
constexpr Game kCrash = {L"CrashBandicootNSaneTrilogy.exe", {0x5B50A076, 0x01D74000}};
constexpr Game kSpyro = {L"Spyro-Win64-Shipping.exe", {0x5CE389A2, 0x03A38000}};

// The numbers of an exe on disk. False if it cannot be read or is no exe.
bool OfFile(const std::wstring& path, ExeId* id);

// The launcher's side. Null if another launcher already holds it.
HANDLE Hold();

enum class Verdict { kRun, kNotThisExe, kNoLauncher, kOtherBuild };
// A bridge's side, asked once, as the game loads it. `ini` is the bridge's
// crashspyro.ini. It is called from DllMain, so nothing in it goes further
// than kernel32.
Verdict Decide(const Game& game, const std::wstring& ini);

}  // namespace csp::session
