// The Spyro Reignited Trilogy half of the link: the host, which is the game
// on screen and owns the world. Loaded as xinput1_3.dll from
// Falcon\Binaries\Win64.
//
// Phase 0 (docs/PHASE0.md):
//   - a thread of its own finds the player's pawn and the camera and
//     publishes them, reading only;
//   - a hook on the engine's ProcessEvent gets the bridge onto the game's
//     thread once a frame, at the pawn's own tick;
//   - the follow test: on a key, the pawn is put where Crash is, every
//     frame, until the key is pressed again;
//   - optionally, pressing the controller's A for the player from the title
//     screen until the game is in a level, so a restart needs no hands.
#include <windows.h>

#include <atomic>
#include <cmath>
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
#include "ue.h"

namespace {

using namespace csp;

// Places in the one build session.h names: distances into the exe, or into
// an Unreal Engine 4.19 object. Each can be overridden in crashspyro.ini
// under [places]. docs/PHASE0.md says where each came from and whether it
// has been seen to hold in a running game.
#define SPYRO_PLACES(X)             \
  /* globals */                     \
  X(world, 0x3415F30)               \
  X(names, 0x35590D8)               \
  /* UObject::ProcessEvent */       \
  X(processEvent, 0x7688B0)         \
  /* UWorld -> the player */        \
  X(worldGameInstance, 0x140)       \
  X(instanceLocalPlayers, 0x38)     \
  X(playerController, 0x30)         \
  X(controllerPawn, 0x388)          \
  X(controllerCameraManager, 0x3A0) \
  /* the pawn */                    \
  X(pawnRoot, 0x158)                \
  X(rootRotation, 0x16C)            \
  X(rootLocation, 0x1A0)            \
  /* APlayerCameraManager */        \
  X(cameraLocation, 0x3A0)          \
  X(cameraRotation, 0x3AC)          \
  X(cameraFov, 0x3B8)

struct Places {
#define X(name, value) uint32_t name = value;
  SPYRO_PLACES(X)
#undef X
};

// What ProcessEvent starts with in the known build.
constexpr char kProcessEventPrefix[] = "4055565741544155415641574881ecf0000000";

HMODULE g_module = nullptr;
std::wstring g_dir;
Places g_places;
SharedMapping g_link;

// From crashspyro.ini, read by the worker and used on the game's thread.
struct Settings {
  std::atomic<int> follow{0};     // [follow] on: changing it switches following on or off
  std::atomic<int> key{VK_F2};    // [follow] key: so does pressing this
  std::atomic<float> scale{0.5f}; // Unreal units per Crash unit (measured: docs/PHASE0.md)
  std::atomic<int> relative{1};   // steps are turned from Crash's camera to Spyro's
  std::atomic<float> yaw{0.0f};   // not relative: Crash's world turned this many degrees about up
  std::atomic<int> flipY{1};      // not relative: Crash's y runs the other way
  std::atomic<float> reach{6000.0f};  // following lets go if Crash goes further than this from where it began
  std::atomic<int> autoContinue{0};   // [start] auto_continue
  std::atomic<int> padOnlyInFront{1}; // [pad] only_in_front
  std::atomic<int> steamCamera{0};    // [pad] steam_camera: Steam's number for the camera stick
} g_settings;

using ProcessEventFn = void(__fastcall*)(void* object, void* function, void* params);
ProcessEventFn g_processEvent = nullptr;

std::atomic<uintptr_t> g_pawn{0};      // found by the worker
std::atomic<int32_t> g_tickName{-1};   // the engine's number for "ReceiveTick"
std::atomic<uint32_t> g_gameTicks{0};
std::atomic<bool> g_following{false};
std::atomic<uint32_t> g_lastGameTick{0};  // GetTickCount() at the pawn's last tick
std::atomic<float> g_cameraYaw{0.0f};     // Spyro's camera, degrees; from the worker
std::atomic<int> g_cue{0};             // a sound owed to the player: 1 following began, 2 it ended

void LoadSettings(const Config& config) {
#define X(name, value) g_places.name = config.GetOffset("places", #name, value);
  SPYRO_PLACES(X)
#undef X
  g_settings.follow.store(config.GetInt("follow", "on", 0));
  g_settings.key.store(config.GetInt("follow", "key", VK_F2));
  g_settings.scale.store(config.GetFloat("follow", "scale", 0.5f));
  g_settings.relative.store(config.GetInt("follow", "relative", 1));
  g_settings.yaw.store(config.GetFloat("follow", "yaw", 0.0f));
  g_settings.flipY.store(config.GetInt("follow", "flip_y", 1));
  g_settings.reach.store(config.GetFloat("follow", "reach", 6000.0f));
  g_settings.autoContinue.store(config.GetInt("start", "auto_continue", 0));
  g_settings.padOnlyInFront.store(config.GetInt("pad", "only_in_front", 1));
  g_settings.steamCamera.store(config.GetInt("pad", "steam_camera", 0));
}

bool Sane(const Vec3& v) {
  // NaN fails every comparison, so this rejects it too.
  const auto ok = [](float f) { return f > -1e8f && f < 1e8f; };
  return ok(v.x) && ok(v.y) && ok(v.z);
}

// Every object the engine makes has its table of virtual functions in the
// exe. Following a pointer that used to be good into something else does not.
bool IsObject(uintptr_t object) {
  uintptr_t vtable = 0;
  return object != 0 && mem::Read(object, vtable) && mem::InImage(vtable);
}

bool PawnLocation(uintptr_t pawn, Vec3& out) {
  const uintptr_t root = IsObject(pawn) ? mem::Pointer(pawn + g_places.pawnRoot) : 0;
  return IsObject(root) && mem::Read(root + g_places.rootLocation, out) && Sane(out);
}

// ------------------------------------------------------ the game's thread --

// Calling one of the engine's functions by name, on the game's thread.
struct Call {
  uintptr_t function = 0;
  alignas(16) uint8_t params[0x200] = {};

  bool Find(uintptr_t object, const char* name) {
    function = ue::FindFunction(object, name);
    return function != 0;
  }
  template <typename T>
  bool Set(const char* name, const T& value) {
    const int32_t at = ue::FindParam(function, name);
    if (at < 0 || at + sizeof(T) > sizeof(params)) return false;
    std::memcpy(params + at, &value, sizeof(T));
    return true;
  }
  template <typename T>
  bool Get(const char* name, T& value) const {
    const int32_t at = ue::FindParam(function, name);
    if (at < 0 || at + sizeof(T) > sizeof(params)) return false;
    std::memcpy(&value, params + at, sizeof(T));
    return true;
  }
  void On(uintptr_t object) {
    g_processEvent(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), params);
  }
};

// The follow test's state. Only ever touched on the game's thread.
struct Follow {
  bool active = false;
  uintptr_t pawn = 0;
  uintptr_t movement = 0;   // the pawn's movement component, switched off while following
  uint32_t generation = 0;  // Crash's, when following began
  Vec3 crash0{}, spyro0{};  // where each was at that moment
  Vec3 crashLast{}, target{};  // relative following: Crash a tick ago, and where the pawn has got to
  // K2_SetActorRotation, likewise.
  uintptr_t setRotation = 0;
  int32_t atRotation = -1, atRotationTeleport = -1;
  // K2_SetActorLocation, looked up once per pawn class.
  uintptr_t setLocationClass = 0, setLocation = 0;
  int32_t atLocation = -1, atSweep = -1, atTeleport = -1;
};

void SetMovementTick(const Follow& f, bool enabled) {
  if (!f.movement || !IsObject(f.movement)) return;
  Call call;
  if (call.Find(f.movement, "SetComponentTickEnabled") && call.Set("bEnabled", static_cast<uint8_t>(enabled))) {
    call.On(f.movement);
  }
}

void Release(Follow& f, uintptr_t pawn, const char* why) {
  // The component belongs to the pawn following began with. If that pawn has
  // gone (a level load), so has the component: leave it be.
  if (pawn == f.pawn) SetMovementTick(f, true);
  log::Info("follow: released (%s)", why);
  f.active = false;
  g_following.store(false);
  g_cue.store(2);
}

bool Engage(Follow& f, uintptr_t pawn, const GuestState& guest) {
  const uintptr_t type = ue::ClassOf(pawn);
  if (type != f.setLocationClass) {
    f.setLocationClass = type;
    f.setLocation = ue::FindFunction(pawn, "K2_SetActorLocation");
    f.atLocation = ue::FindParam(f.setLocation, "NewLocation");
    f.atSweep = ue::FindParam(f.setLocation, "bSweep");
    f.atTeleport = ue::FindParam(f.setLocation, "bTeleport");
    f.setRotation = ue::FindFunction(pawn, "K2_SetActorRotation");
    f.atRotation = ue::FindParam(f.setRotation, "NewRotation");
    f.atRotationTeleport = ue::FindParam(f.setRotation, "bTeleportPhysics");
    uint16_t size = 0;
    mem::Read(f.setLocation + 0x90, size);
    log::Info("follow: pawn is a %s; K2_SetActorLocation at %p, parameters %u bytes (location +%d, sweep +%d, teleport +%d)",
              ue::NameOfObject(type).c_str(), reinterpret_cast<void*>(f.setLocation), size, f.atLocation, f.atSweep, f.atTeleport);
  }
  if (!f.setLocation || f.atLocation < 0 || f.atSweep < 0 || f.atTeleport < 0 || f.atTeleport >= 0x1F0) {
    log::Error("follow: cannot move the pawn: K2_SetActorLocation was not found as expected");
    return false;
  }
  if (!PawnLocation(pawn, f.spyro0)) {
    log::Error("follow: cannot read where the pawn is");
    return false;
  }

  f.movement = 0;
  Call get;
  if (get.Find(pawn, "GetMovementComponent")) {
    get.On(pawn);
    get.Get("ReturnValue", f.movement);
  }
  f.pawn = pawn;
  f.generation = guest.generation;
  f.crash0 = guest.position;
  f.crashLast = guest.position;
  f.target = f.spyro0;
  f.active = true;
  g_following.store(true);
  g_cue.store(1);
  SetMovementTick(f, false);
  log::Info("follow: on. Crash at (%.2f, %.2f, %.2f), pawn at (%.1f, %.1f, %.1f), movement component %s (%s)",
            f.crash0.x, f.crash0.y, f.crash0.z, f.spyro0.x, f.spyro0.y, f.spyro0.z,
            f.movement ? "held" : "not found", f.movement ? ue::NameOfObject(ue::ClassOf(f.movement)).c_str() : "");
  return true;
}

void MoveTo(const Follow& f, uintptr_t pawn, const Vec3& target) {
  Call call;
  call.function = f.setLocation;
  std::memcpy(call.params + f.atLocation, &target, sizeof(target));
  call.params[f.atSweep] = 0;     // no sweep: Spyro's collision must not argue with Crash's position
  call.params[f.atTeleport] = 1;  // and no carried-over physics velocity
  call.On(pawn);
}

void Face(const Follow& f, uintptr_t pawn, float yawDegrees) {
  if (!f.setRotation || f.atRotation < 0 || f.atRotationTeleport < 0 || f.atRotationTeleport >= 0x1F0) return;
  Call call;
  call.function = f.setRotation;
  const Vec3 rotation = {0.0f, yawDegrees, 0.0f};  // pitch, yaw, roll
  std::memcpy(call.params + f.atRotation, &rotation, sizeof(rotation));
  call.params[f.atRotationTeleport] = 1;
  call.On(pawn);
}

// The one place Crash's lengths and axes become Spyro's.
void Place(Follow& f, uintptr_t pawn, const GuestState& guest) {
  const float kPi = 3.14159265f;
  const float scale = g_settings.scale.load();
  if (!g_settings.relative.load()) {
    // Crash's world laid over Spyro's with one fixed turn. Right for one
    // direction of Spyro's camera and backwards for the opposite one.
    const float turn = g_settings.yaw.load() * kPi / 180.0f;
    const float dx = guest.position.x - f.crash0.x;
    const float dy = (guest.position.y - f.crash0.y) * (g_settings.flipY.load() ? -1.0f : 1.0f);
    const float dz = guest.position.z - f.crash0.z;
    MoveTo(f, pawn, {f.spyro0.x + scale * (dx * std::cos(turn) - dy * std::sin(turn)),
                     f.spyro0.y + scale * (dx * std::sin(turn) + dy * std::cos(turn)), f.spyro0.z + scale * dz});
    return;
  }

  // Each step Crash took since the last tick, as he saw it (so far ahead,
  // so far to the right of his camera), taken as Spyro's camera sees it.
  // Pushing the stick away from the player then moves the character away
  // from the player in both games, wherever either camera points. Until
  // Crash is steered by Spyro's camera itself, this is what makes the two
  // agree.
  const float dx = guest.position.x - f.crashLast.x, dy = guest.position.y - f.crashLast.y;
  const float dz = guest.position.z - f.crashLast.z;
  f.crashLast = guest.position;
  // Crash's view matrix turns the world to the camera; the camera looks
  // down its own -z, so its line of sight over the ground is minus the
  // third column. His world is right-handed: screen-right is that turned
  // clockwise.
  float aheadX = -guest.view[2], aheadY = -guest.view[6];
  const float length = std::sqrt(aheadX * aheadX + aheadY * aheadY);
  if ((guest.flags & kGuestCamera) && length > 0.25f) {
    aheadX /= length;
    aheadY /= length;
    const float ahead = dx * aheadX + dy * aheadY, across = dx * aheadY - dy * aheadX;
    // Unreal is left-handed: x ahead, y to the right at yaw 0.
    const float yaw = g_cameraYaw.load() * kPi / 180.0f;
    const float stepX = scale * (ahead * std::cos(yaw) - across * std::sin(yaw));
    const float stepY = scale * (ahead * std::sin(yaw) + across * std::cos(yaw));
    f.target.x += stepX;
    f.target.y += stepY;
    // Turn the character the way it is going, once it is really going.
    if (stepX * stepX + stepY * stepY > 0.5f * 0.5f) Face(f, pawn, std::atan2(stepY, stepX) * 180.0f / kPi);
  }
  f.target.z += scale * dz;
  MoveTo(f, pawn, f.target);
}

// Once a frame, from inside the pawn's own tick.
void GameTick(uintptr_t pawn) {
  g_gameTicks.fetch_add(1, std::memory_order_relaxed);
  g_lastGameTick.store(GetTickCount(), std::memory_order_relaxed);
  g_link.Frame();

  static Follow f;
  static bool wasDown = false;
  static int lastSetting = 0;
  static bool complained = false;
  static DWORD lastToggle = 0;

  bool want = f.active;
  const bool down = (GetAsyncKeyState(g_settings.key.load()) & 0x8000) != 0;
  // One press, one change: a second press (or a bouncing key) within a
  // second of the first undid it before anyone could see it had worked.
  const DWORD now = GetTickCount();
  if (down && !wasDown && static_cast<DWORD>(now - lastToggle) > 1000) {
    want = !f.active;
    lastToggle = now;
  }
  wasDown = down;
  const int setting = g_settings.follow.load();
  if (setting != lastSetting) {
    want = setting != 0;
    lastSetting = setting;
  }

  GuestState guest{};
  const bool linked = g_link.PeerAlive() && g_link.block()->guest.Read(guest);
  const bool player = linked && (guest.flags & kGuestPlayer) && !(guest.flags & kGuestParented);

  if (f.active) {
    if (!want) Release(f, pawn, "asked to");
    else if (pawn != f.pawn) Release(f, pawn, "Spyro has a different pawn");
    else if (!linked) Release(f, pawn, "Crash has gone");
    else if (!player) Release(f, pawn, "Crash has no player to follow");
    else if (guest.generation != f.generation) Release(f, pawn, "Crash changed level");
    else {
      // The test maps Crash's level straight onto Spyro's. A death, a bonus
      // round or a long run takes Crash somewhere that is nowhere in Spyro's.
      const float dx = guest.position.x - f.crash0.x, dy = guest.position.y - f.crash0.y, dz = guest.position.z - f.crash0.z;
      const float reach = g_settings.reach.load();
      if (dx * dx + dy * dy + dz * dz > reach * reach) {
        // Put him back where he started rather than leave him out there.
        MoveTo(f, pawn, f.spyro0);
        Release(f, pawn, "Crash went further than the test reaches");
        lastSetting = setting;
      }
    }
  } else if (want) {
    if (player) {
      Engage(f, pawn, guest);
      complained = false;
    } else if (!complained) {
      log::Info("follow: asked for, but %s", linked ? "Crash has no player on foot yet" : "Crash is not there");
      complained = true;
    }
  }
  if (f.active) Place(f, pawn, guest);
}

void __fastcall OnProcessEvent(void* object, void* function, void* params) {
  g_processEvent(object, function, params);
  // Thousands of these a frame: two comparisons and out, unless this is the
  // player's pawn having just run its tick.
  if (object && function && object == reinterpret_cast<void*>(g_pawn.load(std::memory_order_relaxed)) &&
      *reinterpret_cast<const int32_t*>(static_cast<const char*>(function) + ue::kObjectName) ==
          g_tickName.load(std::memory_order_relaxed)) {
    GameTick(reinterpret_cast<uintptr_t>(object));
  }
}

// Whether Spyro's character is following Crash in a level that is running.
// The pawn's tick stops when Spyro is paused or in a menu, and then Spyro is
// just Spyro and wants its whole controller.
bool Linked() {
  return g_following.load(std::memory_order_relaxed) &&
         static_cast<uint32_t>(GetTickCount() - g_lastGameTick.load(std::memory_order_relaxed)) < 250;
}

// What Spyro hears from the controller (docs/DESIGN.md, 4). Both games read
// the one controller whichever window is in front, so each bridge has to
// decide what its own game is told.
uint32_t OnPad(uint32_t index, uint32_t result, pad::State* state) {
  // The bridge's own controller, when it is playing one (A, on the way in),
  // is all Spyro hears.
  if (standin::Fill(index, state)) return 0;
  if (result != 0) return result;
  if (Linked()) {
    // Crash is the one being played. Spyro keeps its camera and its menus.
    pad::Quiet(state, pad::kStart | pad::kBack, true);
  } else if (g_settings.padOnlyInFront.load(std::memory_order_relaxed) && !pad::GameInFront()) {
    // Not linked: like any game, deaf to the controller unless in front.
    pad::Quiet(state);
  }
  return result;
}

// The same decision for what Spyro hears through Steam's controller
// interface, which is how the player's controller actually reaches it.
bool OnSteam(bool analog, uint64_t action) {
  if (Linked()) {
    // Crash is the one being played; Spyro keeps its camera.
    return !(analog && action == static_cast<uint64_t>(g_settings.steamCamera.load(std::memory_order_relaxed)));
  }
  return g_settings.padOnlyInFront.load(std::memory_order_relaxed) && !pad::GameInFront();
}

DWORD WINAPI PlayCue(void* which) {
  if (reinterpret_cast<intptr_t>(which) == 1) {
    Beep(880, 90);
    Beep(1320, 140);
  } else {
    Beep(1320, 90);
    Beep(660, 160);
  }
  return 0;
}

// ------------------------------------------------------------ the worker --

HostState Sample() {
  const Places& p = g_places;
  HostState s{};
  s.frame = g_gameTicks.load();
  s.timeUs = NowUs();
  uintptr_t pawn = 0;

  const uintptr_t world = mem::Pointer(mem::ImageBase() + p.world);
  if (IsObject(world)) {
    s.world = world;
    const uintptr_t instance = mem::Pointer(world + p.worldGameInstance);
    int32_t players = 0;
    if (IsObject(instance) && mem::Read(instance + p.instanceLocalPlayers + 8, players) && players >= 1 && players <= 4) {
      const uintptr_t list = mem::Pointer(instance + p.instanceLocalPlayers);
      const uintptr_t player = list ? mem::Pointer(list) : 0;
      const uintptr_t controller = IsObject(player) ? mem::Pointer(player + p.playerController) : 0;
      if (IsObject(controller)) {
        const uintptr_t candidate = mem::Pointer(controller + p.controllerPawn);
        const uintptr_t root = IsObject(candidate) ? mem::Pointer(candidate + p.pawnRoot) : 0;
        if (IsObject(root) && mem::Read(root + p.rootLocation, s.position) && Sane(s.position) &&
            mem::Read(root + p.rootRotation, s.rotation) && Sane(s.rotation)) {
          s.flags |= kHostPawn;
          pawn = candidate;
        }
        const uintptr_t manager = mem::Pointer(controller + p.controllerCameraManager);
        if (IsObject(manager) && mem::Read(manager + p.cameraLocation, s.cameraPosition) && Sane(s.cameraPosition) &&
            mem::Read(manager + p.cameraRotation, s.cameraRotation) && Sane(s.cameraRotation) &&
            mem::Read(manager + p.cameraFov, s.cameraFov) && s.cameraFov > 1.0f && s.cameraFov < 179.0f) {
          s.flags |= kHostCamera;
          g_cameraYaw.store(s.cameraRotation.y);
        }
      }
    }
  }
  g_pawn.store(pawn);
  if (Linked()) s.flags |= kHostLinked;
  return s;
}

void Report(const HostState& s, bool force) {
  static uint32_t lastFlags = ~0u, lastFrames = 0, lastTicks = 0;
  static uint64_t lastWorld = ~uint64_t{0};
  static bool lastPeer = false, lastFollowing = false;
  static uintptr_t lastPawn = 0;
  const bool peer = g_link.PeerAlive(), following = g_following.load();
  const uintptr_t pawn = g_pawn.load();
  if (pawn && pawn != lastPawn) {
    log::Info("pawn %p is a %s", reinterpret_cast<void*>(pawn), ue::NameOfObject(ue::ClassOf(pawn)).c_str());
  }
  lastPawn = pawn;
  if (!force && s.flags == lastFlags && s.world == lastWorld && peer == lastPeer && following == lastFollowing) return;

  GuestState g{};
  const bool guest = peer && g_link.block()->guest.Read(g);
  const uint32_t frames = g_link.PeerFrames(), ticks = g_gameTicks.load();
  log::Info("world %s  pawn %s  pos (%.1f, %.1f, %.1f)  rot (%.1f, %.1f, %.1f)  camera %s (%.1f, %.1f, %.1f) yaw %.1f fov %.1f  game ticks %u (+%u)%s  |  crash %s",
            s.world ? "yes" : "no", (s.flags & kHostPawn) ? "yes" : "no", s.position.x, s.position.y, s.position.z,
            s.rotation.x, s.rotation.y, s.rotation.z, (s.flags & kHostCamera) ? "yes" : "no",
            s.cameraPosition.x, s.cameraPosition.y, s.cameraPosition.z, s.cameraRotation.y, s.cameraFov, ticks,
            ticks - lastTicks, following ? "  FOLLOWING" : "",
            !peer ? "absent" : guest ? "linked" : "there, nothing published yet");
  if (guest) {
    log::Info("  crash says: frames %u (+%u)  player %s gen %u  pos (%.2f, %.2f, %.2f)%s  level \"%s\"", frames,
              frames - lastFrames, (g.flags & kGuestPlayer) ? "yes" : "no", g.generation, g.position.x, g.position.y,
              g.position.z, (g.flags & kGuestPaused) ? "  (paused)" : "", (g.flags & kGuestLevel) ? g.level : "");
  }
  lastFlags = s.flags;
  lastWorld = s.world;
  lastPeer = peer;
  lastFollowing = following;
  lastFrames = frames;
  lastTicks = ticks;
}

// Gets the bridge onto the game's thread. The exe unpacks itself after this
// DLL is loaded, and makes the unpacked code runnable later still.
bool HookProcessEvent() {
  const uintptr_t at = mem::ImageBase() + g_places.processEvent;
  uint8_t first = 0;
  // Either the engine's own first bytes, or a jump somebody else (UE4SS) has
  // already put there, which MinHook can chain behind.
  const auto ready = [&] {
    return mem::IsExecutable(at) && (mem::StartsWith(at, kProcessEventPrefix) || (mem::Read(at, first) && first == 0xE9));
  };
  int waited = 0;
  for (; waited < 1200 && !ready(); ++waited) Sleep(50);
  if (!ready()) {
    log::Error("ProcessEvent is not where it is in the known build (exe+0x%X). The bridge will only watch.", g_places.processEvent);
    return false;
  }
  log::Info("ProcessEvent found after %d ms%s", waited * 50, first == 0xE9 ? " (already hooked by something else)" : "");
  return hook::Install(reinterpret_cast<void*>(at), reinterpret_cast<void*>(&OnProcessEvent),
                       reinterpret_cast<void**>(&g_processEvent), "Spyro's ProcessEvent");
}

DWORD WINAPI Run(void*) {
  Config config(g_dir + L"crashspyro.ini");
  config.Changed();
  LoadSettings(config);
  log::Info("exe at 0x%llX, %u bytes", static_cast<unsigned long long>(mem::ImageBase()), mem::ImageSize());

  if (!g_link.Open(kSideSpyro)) return 0;
  // Whatever the last run of Spyro left in its slot is not this run's.
  g_link.block()->host.Write(HostState{});
  ue::Init(g_places.names);
  // Before the game first asks about controllers: it asks once and, told
  // there is none, does not ask again until one is plugged in.
  const bool autoContinue = g_settings.autoContinue.load() != 0;
  standin::Present(autoContinue);
  pad::SetFilter(&OnPad);
  log::Info("pad: %d of the game's two XInput calls come straight to the bridge", pad::TakeBack());
  standin::DebugPress(config);
  if (autoContinue) log::Info("start: pressing A from the title screen until the game is in a level");
  const bool hooked = HookProcessEvent();

  const DWORD started = GetTickCount();
  bool arriving = autoContinue;
  DWORD lastPress = started;
  bool steam = false;
  char analogSeen[400] = "", analogNow[400];
  DWORD lastBeat = 0, lastReport = GetTickCount(), lastNameSearch = 0;
  for (;;) {
    Sleep(4);
    const HostState s = Sample();
    g_link.block()->host.Write(s);

    const DWORD now = GetTickCount();
    if (arriving) {
      const DWORD elapsed = now - started;
      if ((s.flags & kHostPawn) || elapsed > 180000) {
        arriving = false;
        standin::Present(false);
        log::Info("start: %s", (s.flags & kHostPawn) ? "in a level; the controller is the player's again" : "gave up after three minutes");
      } else {
        // A short press every second and a half, once there is a world to press it in.
        if (s.world != 0 && static_cast<DWORD>(now - lastPress) >= 1500) {
          lastPress = now;
          standin::Hold("A", 150);
        }
      }
    }
    if (const int cue = g_cue.exchange(0)) {
      CloseHandle(CreateThread(nullptr, 0, PlayCue, reinterpret_cast<void*>(static_cast<intptr_t>(cue)), 0, nullptr));
    }
    if (static_cast<DWORD>(now - lastBeat) < 250) continue;
    lastBeat = now;
    g_link.Beat();
    if (config.Changed()) {
      LoadSettings(config);
      standin::DebugPress(config);
    }
    // Steam is not up until some way into the game's start.
    if (!steam) steam = steampad::Install(&OnSteam);
    steampad::DescribeAnalog(analogNow, sizeof(analogNow));
    if (std::strcmp(analogNow, analogSeen) != 0 && static_cast<DWORD>(now - lastReport) >= 5000) {
      std::strcpy(analogSeen, analogNow);
      log::Info("steam: sticks and triggers the game asks about: %s", analogSeen);
    }
    // The engine fills its table of names as it starts; ask until it is there.
    if (hooked && g_tickName.load() < 0 && s.world && static_cast<DWORD>(now - lastNameSearch) >= 2000) {
      lastNameSearch = now;
      const int32_t name = ue::FindName("ReceiveTick");
      if (name >= 0) {
        g_tickName.store(name);
        log::Info("the engine's name table is readable: \"ReceiveTick\" is name %d, and name %d reads back as \"%s\"", name,
                  name, ue::NameOf(name).c_str());
      }
    }
    const DWORD every = static_cast<DWORD>(config.GetInt("debug", "status_seconds", 10)) * 1000;
    const bool due = every != 0 && static_cast<DWORD>(now - lastReport) >= every;
    Report(s, due);
    if (due) lastReport = now;
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
  const session::Verdict verdict = session::Decide(session::kSpyro, g_dir + L"crashspyro.ini");
  if (verdict == session::Verdict::kNotThisExe || verdict == session::Verdict::kNoLauncher) return TRUE;

  log::Init(g_dir + L"crashspyro_spyro.log");
  if (verdict == session::Verdict::kOtherBuild) {
    log::Error("this is not the build of Spyro the mod was made for. Doing nothing.");
    return TRUE;
  }
  log::Info("Crash-in-Spyro: Spyro bridge, protocol v1");
  crashlog::Install();
  CloseHandle(CreateThread(nullptr, 0, Run, nullptr, 0, nullptr));
  return TRUE;
}
