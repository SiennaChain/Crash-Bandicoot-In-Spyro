// The Crash Bandicoot N. Sane Trilogy half of the link: the guest, which owns
// the player. Loaded as xinput9_1_0.dll from the game's folder.
//
// Phase 0 (docs/PHASE0.md): find the player, the model the game draws for
// him, the camera and the clock, once a game frame and on the game's own
// thread, and publish them. Nothing of the game's is changed except the one
// hook that gets us onto that thread.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "clock.h"
#include "config.h"
#include "crashlog.h"
#include "csp_protocol.h"
#include "hook.h"
#include "log.h"
#include "memory.h"
#include "pad.h"
#include "session.h"
#include "shm.h"
#include "standin.h"
#include "steampad.h"

namespace {

using namespace csp;

// Places in the one build session.h names, as distances into the exe
// (addresses) or into an object (fields). Each can be overridden in
// crashspyro.ini under [places] while something is being hunted for.
// docs/PHASE0.md says where each came from and whether it has been seen to
// hold in a running game.
#define CRASH_PLACES(X)             \
  /* the game's frame: bool(client) */ \
  X(frame, 0x169B90)                \
  /* globals */                     \
  X(actorList, 0x1A9E778)           \
  X(cameraSystem, 0x1A890E0)        \
  X(clock, 0x1A61888)               \
  /* classes, by their vtables */   \
  X(clientVtable, 0x13ED108)        \
  X(actorVtable, 0x1592220)         \
  X(behaviorVtable, 0x150DF30)      \
  X(modelVtable, 0x1676CB8)         \
  X(cameraSystemVtable, 0x151D850)  \
  X(cameraVtable, 0x151D390)        \
  X(clockVtable, 0x13F7FB8)         \
  /* the player actor */            \
  X(actorParent, 0x10)              \
  X(actorPosition, 0x20)            \
  X(actorBehavior, 0xB0)            \
  X(behaviorModel, 0x28)            \
  X(modelMatrix, 0x30)              \
  /* the camera */                  \
  X(systemCamera, 0x10)             \
  X(cameraPosition, 0x80)           \
  X(cameraView, 0x180)              \
  X(cameraProjection, 0x1C0)        \
  /* the clock */                   \
  X(clockSimTicks, 0x20)            \
  X(clockPaused, 0x2C)              \
  /* the client */                  \
  X(clientLevelName, 0xB8)

struct Places {
#define X(name, value) uint32_t name = value;
  CRASH_PLACES(X)
#undef X
};

// What the frame routine starts with in the known build. The exe is stored
// encrypted and unpacks itself as it starts, after this DLL is loaded, so
// these bytes turning up is also how we know the code is there to hook.
constexpr char kFramePrefix[] = "48895c240848896c2410488974241848897c242041564883";

HMODULE g_module = nullptr;
std::wstring g_dir;
Places g_places;
SharedMapping g_link;

using FrameFn = bool(__fastcall*)(void* client);
FrameFn g_frame = nullptr;

// Written on the game's thread, read by the worker for the log.
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_flags{0};
std::atomic<uint32_t> g_generation{0};
std::atomic<uintptr_t> g_strangeActor{0};
std::atomic<int> g_padOnlyInFront{1};  // [pad] only_in_front

void LoadPlaces(const Config& config) {
#define X(name, value) g_places.name = config.GetOffset("places", #name, value);
  CRASH_PLACES(X)
#undef X
  g_padOnlyInFront.store(config.GetInt("pad", "only_in_front", 1));
}

// What Crash hears from the controller (docs/DESIGN.md, 4). Both games read
// the one controller whichever window is in front. While Spyro says its
// character is following Crash, the controller is Crash's wherever the
// player is looking; otherwise Crash is like any game, deaf to it unless in
// front.
uint32_t OnPad(uint32_t index, uint32_t result, pad::State* state) {
  // The bridge's own controller, when it is playing one, is all Crash hears.
  if (standin::Fill(index, state)) return 0;
  if (result != 0 || !g_padOnlyInFront.load(std::memory_order_relaxed) || pad::GameInFront()) return result;
  HostState host{};
  const bool linked = g_link.block() && g_link.PeerAlive() && g_link.block()->host.Read(host) && (host.flags & kHostLinked);
  if (!linked) pad::Quiet(state);
  return result;
}

// Text the game keeps as a plain run of characters. Empty unless all of it
// is printable, which a freed or reused buffer rarely is.
bool ReadText(uintptr_t address, char* out, size_t size) {
  char buffer[64] = {};
  if (address < 0x10000 || !mem::Read(address, buffer, sizeof(buffer) - 1)) return false;
  size_t length = 0;
  for (; length < sizeof(buffer) - 1 && buffer[length]; ++length) {
    if (buffer[length] < 32 || buffer[length] > 126) return false;
  }
  if (length == 0 || length >= size) return false;
  std::memcpy(out, buffer, length + 1);
  return true;
}

// The game's thread, straight after one of its frames.
void Sample(uintptr_t client) {
  const Places& p = g_places;
  const uintptr_t base = mem::ImageBase();
  static bool hadPlayer = false;
  static uint32_t generation = 0;

  GuestState s{};
  s.frame = g_frames.fetch_add(1) + 1;
  s.timeUs = NowUs();

  const uintptr_t list = mem::Pointer(base + p.actorList);
  const uintptr_t actor = list ? mem::Pointer(list) : 0;
  if (actor && mem::IsA(actor, p.actorVtable) && mem::Read(actor + p.actorPosition, s.position)) {
    s.flags |= kGuestPlayer;
    uintptr_t parent = 0;
    if (mem::Read(actor + p.actorParent, parent) && parent != 0) s.flags |= kGuestParented;
    const uintptr_t behavior = mem::Pointer(actor + p.actorBehavior);
    const uintptr_t model = mem::IsA(behavior, p.behaviorVtable) ? mem::Pointer(behavior + p.behaviorModel) : 0;
    if (mem::IsA(model, p.modelVtable) && mem::Read(model + p.modelMatrix, s.model)) s.flags |= kGuestModel;
  } else if (actor) {
    // Something is in the player's place that is not the class Crash was
    // when the place was found (Coco, a vehicle level?). Worth knowing.
    uintptr_t vtable = 0;
    if (mem::Read(actor, vtable) && mem::InImage(vtable)) g_strangeActor.store(vtable - base);
  }
  if ((s.flags & kGuestPlayer) && !hadPlayer) ++generation;
  hadPlayer = (s.flags & kGuestPlayer) != 0;
  s.generation = generation;

  const uintptr_t system = mem::Pointer(base + p.cameraSystem);
  const uintptr_t camera = mem::IsA(system, p.cameraSystemVtable) ? mem::Pointer(system + p.systemCamera) : 0;
  if (mem::IsA(camera, p.cameraVtable) && mem::Read(camera + p.cameraPosition, s.cameraPosition) &&
      mem::Read(camera + p.cameraView, s.view) && mem::Read(camera + p.cameraProjection, s.projection)) {
    s.flags |= kGuestCamera;
  }

  const uintptr_t clock = mem::Pointer(base + p.clock);
  uint8_t paused = 0;
  if (mem::IsA(clock, p.clockVtable) && mem::Read(clock + p.clockSimTicks, s.simTicks) &&
      mem::Read(clock + p.clockPaused, paused) && paused <= 1) {
    s.flags |= kGuestClock;
    if (paused) s.flags |= kGuestPaused;
  }

  uintptr_t name = 0;
  if (mem::IsA(client, p.clientVtable) && mem::Read(client + p.clientLevelName, name) &&
      ReadText(name, s.level, sizeof(s.level))) {
    s.flags |= kGuestLevel;
  }

  g_link.block()->guest.Write(s);
  g_link.Frame();
  g_flags.store(s.flags);
  g_generation.store(generation);
}

bool __fastcall OnFrame(void* client) {
  const bool result = g_frame(client);
  Sample(reinterpret_cast<uintptr_t>(client));
  return result;
}

// The same decision for what Crash hears through Steam's controller
// interface, which is how the player's controller actually reaches it.
bool OnSteam(bool, uint64_t) {
  if (!g_padOnlyInFront.load(std::memory_order_relaxed) || pad::GameInFront()) return false;
  HostState host{};
  const bool linked = g_link.block() && g_link.PeerAlive() && g_link.block()->host.Read(host) && (host.flags & kHostLinked);
  return !linked;
}

// ------------------------------------------------------------ staying awake --
//
// Crash takes no notice of any controller while it believes its window is
// not the active one (seen: a press through the stand-in did nothing until
// the window was told it had been activated). In the mod Crash is never the
// window in front, so while it is wanted it is kept believing it is: the
// messages that would tell it otherwise are kept from it.

HWND g_window = nullptr;
WNDPROC g_windowProc = nullptr;
bool g_windowUnicode = true;
std::atomic<bool> g_awake{false};

LRESULT CALLBACK OnWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (g_awake.load(std::memory_order_relaxed)) {
    if ((message == WM_ACTIVATEAPP && !wParam) || (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) ||
        message == WM_KILLFOCUS) {
      return 0;
    }
  }
  return g_windowUnicode ? CallWindowProcW(g_windowProc, window, message, wParam, lParam)
                         : CallWindowProcA(g_windowProc, window, message, wParam, lParam);
}

BOOL CALLBACK FindGameWindow(HWND window, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
  *reinterpret_cast<HWND*>(out) = window;
  return FALSE;
}

// Tells the game's window it has just been activated, or just been left,
// without either being so.
void TellWindow(bool active) {
  if (active) {
    PostMessageW(g_window, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_window, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_window, WM_SETFOCUS, 0, 0);
  } else {
    PostMessageW(g_window, WM_KILLFOCUS, 0, 0);
    PostMessageW(g_window, WM_ACTIVATE, WA_INACTIVE, 0);
    PostMessageW(g_window, WM_ACTIVATEAPP, FALSE, 0);
  }
}

// Called a few times a second with whether Crash is wanted awake.
void KeepAwake(bool wanted) {
  if (!g_window) {
    HWND found = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
    if (!found) return;
    g_windowUnicode = IsWindowUnicode(found) != 0;
    const LONG_PTR ours = reinterpret_cast<LONG_PTR>(&OnWindowMessage);
    g_windowProc = reinterpret_cast<WNDPROC>(g_windowUnicode ? SetWindowLongPtrW(found, GWLP_WNDPROC, ours)
                                                              : SetWindowLongPtrA(found, GWLP_WNDPROC, ours));
    if (!g_windowProc) return;
    g_window = found;
    log::Info("awake: watching the game window's messages");
  }
  if (wanted == g_awake.load()) return;
  const bool inFront = GetForegroundWindow() == g_window;
  if (wanted) {
    g_awake.store(true);
    if (!inFront) TellWindow(true);
  } else {
    g_awake.store(false);
    if (!inFront) TellWindow(false);  // back to the truth
  }
  log::Info("awake: Crash is %s", wanted ? "kept believing it is the active window" : "left to notice which window is active");
}

// ------------------------------------------------------------ getting in --
//
// [start] auto_load = 1: from the title screen to the Crash 3 hub with the
// save in use, with nobody at the controller. The way there, as found by
// trying it (docs/PHASE0.md):
//
//   title ("press A to begin")      A
//   the three games, Crash 1 first  right, right, then A on "Continue"
//
// Both screens are the level "Crash1/C1_StartScreen"; what tells them apart
// is where the camera is. Nothing says which of the three games is picked
// out, so the two rights are done once, from a screen that has just
// appeared, and the level that then loads is checked.

constexpr char kStartScreen[] = "Crash1/C1_StartScreen/C1_StartScreen";
constexpr float kTitleY = -476.5f, kTitleZ = 981.7f;    // the camera at the title
constexpr float kSelectY = -228.6f, kSelectZ = 76.2f;   // and at the choice of game

DWORD WINAPI ChooseWarped(void*) {
  standin::Hold("RIGHT", 150);
  Sleep(750);
  standin::Hold("RIGHT", 150);
  Sleep(750);
  standin::Hold("A", 150);
  return 0;
}

// Called a few times a second. True while there is still something to do.
bool AutoLoad() {
  static DWORD lastPress = 0, atSelectSince = 0, began = GetTickCount();
  static bool chosen = false, over = false;
  if (over) return false;
  const DWORD now = GetTickCount();
  const auto stop = [&](const char* why, const char* level) {
    log::Info("start: %s (%s)", why, level);
    over = true;
    return false;
  };
  if (static_cast<DWORD>(now - began) > 240000) return stop("gave up after four minutes", "");

  GuestState s{};
  if (!g_link.block()->guest.Read(s) || !(s.flags & kGuestLevel)) return true;
  if (std::strcmp(s.level, kStartScreen) != 0) {
    // Somewhere else: wait for the player to exist before judging where.
    if (!(s.flags & kGuestPlayer)) return true;
    return stop(std::strncmp(s.level, "crash3/", 7) == 0 ? "in Crash 3; the controller is the player's again"
                                                          : "ended up somewhere other than Crash 3",
                s.level);
  }
  if (!(s.flags & kGuestCamera) || chosen) return true;

  const auto cameraAt = [&](float y, float z, float within) {
    return std::fabs(s.cameraPosition.y - y) < within && std::fabs(s.cameraPosition.z - z) < within;
  };
  if (cameraAt(kTitleY, kTitleZ, 20.0f)) {
    atSelectSince = 0;
    if (static_cast<DWORD>(now - lastPress) >= 1000) {
      lastPress = now;
      standin::Hold("A", 150);
    }
  } else if (cameraAt(kSelectY, kSelectZ, 3.0f)) {
    if (!atSelectSince) atSelectSince = now;
    // Settled there for a second: the screen is taking presses.
    if (static_cast<DWORD>(now - atSelectSince) >= 1200) {
      chosen = true;
      log::Info("start: at the choice of game; right, right, A");
      CloseHandle(CreateThread(nullptr, 0, ChooseWarped, nullptr, 0, nullptr));
    }
  } else {
    atSelectSince = 0;  // the logo, or the camera on its way between the two
  }
  return true;
}

// One line saying what the bridge can see, written when that changes and
// otherwise every `status_seconds`.
void Report(bool force) {
  static uint32_t lastFlags = ~0u, lastGeneration = ~0u, lastFrames = 0;
  static bool lastPeer = false;
  static uintptr_t lastStrange = 0;
  const uint32_t flags = g_flags.load(), generation = g_generation.load(), frames = g_frames.load();
  const bool peer = g_link.PeerAlive();
  const uintptr_t strange = g_strangeActor.load();
  if (strange != lastStrange) {
    log::Info("the player's place holds an object of another class: vtable exe+0x%llX", static_cast<unsigned long long>(strange));
    lastStrange = strange;
  }
  if (!force && flags == lastFlags && generation == lastGeneration && peer == lastPeer) return;

  GuestState s{};
  g_link.block()->guest.Read(s);
  HostState h{};
  const bool host = peer && g_link.block()->host.Read(h);
  log::Info("frames %u (+%u)  player %s%s gen %u  pos (%.2f, %.2f, %.2f)  model %s  camera %s  clock %s%s  level \"%s\"  |  spyro %s",
            frames, frames - lastFrames, (flags & kGuestPlayer) ? "yes" : "no",
            (flags & kGuestParented) ? " (riding)" : "", generation, s.position.x, s.position.y, s.position.z,
            (flags & kGuestModel) ? "yes" : "no", (flags & kGuestCamera) ? "yes" : "no",
            (flags & kGuestClock) ? "yes" : "no", (flags & kGuestPaused) ? " (paused)" : "",
            (flags & kGuestLevel) ? s.level : "", !peer ? "absent" : host ? "linked" : "there, nothing published yet");
  if (host) {
    log::Info("  spyro says: pawn %s  pos (%.1f, %.1f, %.1f)  yaw %.1f", (h.flags & kHostPawn) ? "yes" : "no",
              h.position.x, h.position.y, h.position.z, h.rotation.y);
  }
  lastFlags = flags;
  lastGeneration = generation;
  lastPeer = peer;
  lastFrames = frames;
}

DWORD WINAPI Run(void*) {
  Config config(g_dir + L"crashspyro.ini");
  config.Changed();
  LoadPlaces(config);

  const uintptr_t frame = mem::ImageBase() + g_places.frame;
  log::Info("exe at 0x%llX, %u bytes; waiting for it to unpack", static_cast<unsigned long long>(mem::ImageBase()), mem::ImageSize());
  // Unpacked is not enough: Steam's wrapper writes the code out first and
  // only then makes it runnable, and a hook put in between is refused.
  const auto ready = [frame] { return mem::StartsWith(frame, kFramePrefix) && mem::IsExecutable(frame); };
  int waited = 0;
  for (; waited < 1200 && !ready(); ++waited) Sleep(50);
  if (!ready()) {
    log::Error("the frame routine is not where it is in the known build (exe+0x%X). Doing nothing.", g_places.frame);
    return 0;
  }
  log::Info("frame routine found after %d ms", waited * 50);

  if (!g_link.Open(kSideCrash)) return 0;
  // Whatever the last run of Crash left in its slot is not this run's.
  g_link.block()->guest.Write(GuestState{});
  pad::SetFilter(&OnPad);
  log::Info("pad: %d of the game's two XInput calls come straight to the bridge", pad::TakeBack());
  standin::DebugPress(config);
  if (!hook::Install(reinterpret_cast<void*>(frame), reinterpret_cast<void*>(&OnFrame),
                     reinterpret_cast<void**>(&g_frame), "Crash's frame")) {
    return 0;
  }

  DWORD lastReport = GetTickCount();
  bool steam = false;
  bool arriving = config.GetInt("start", "auto_load", 0) != 0;
  if (arriving) {
    standin::Present(true);
    log::Info("start: going from the title screen to the Crash 3 hub");
  }
  for (;;) {
    Sleep(250);
    g_link.Beat();
    // Steam is not up until some way into the game's start.
    if (!steam) steam = steampad::Install(&OnSteam);
    {
      // Awake while the controller is Crash's whatever is in front: while
      // Spyro's character is following him, and when the ini says so.
      HostState host{};
      const bool linked = g_link.PeerAlive() && g_link.block()->host.Read(host) && (host.flags & kHostLinked);
      if (arriving && !AutoLoad()) {
        arriving = false;
        standin::Present(false);
      }
      KeepAwake(linked || arriving || config.GetInt("debug", "awake", 0) != 0);
    }
    if (config.Changed()) {
      LoadPlaces(config);
      standin::DebugPress(config);
    }
    const DWORD every = static_cast<DWORD>(config.GetInt("debug", "status_seconds", 10)) * 1000;
    const bool due = every != 0 && static_cast<DWORD>(GetTickCount() - lastReport) >= every;
    Report(due);
    if (due) lastReport = GetTickCount();
  }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(instance);
  g_module = instance;

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(instance, path, MAX_PATH);
  g_dir = path;
  g_dir.erase(g_dir.find_last_of(L"\\/") + 1);

  // Not our game, not our build, or not asked for: be xinput and nothing else.
  const session::Verdict verdict = session::Decide(session::kCrash, g_dir + L"crashspyro.ini");
  if (verdict == session::Verdict::kNotThisExe || verdict == session::Verdict::kNoLauncher) return TRUE;

  log::Init(g_dir + L"crashspyro_crash.log");
  if (verdict == session::Verdict::kOtherBuild) {
    log::Error("this is not the build of Crash the mod was made for. Doing nothing.");
    return TRUE;
  }
  log::Info("Crash-in-Spyro: Crash bridge, protocol v%u", kVersion);
  crashlog::Install();
  CloseHandle(CreateThread(nullptr, 0, Run, nullptr, 0, nullptr));
  return TRUE;
}
