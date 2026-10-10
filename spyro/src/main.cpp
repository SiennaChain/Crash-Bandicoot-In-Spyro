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
//
// Phase 1 (docs/PHASE1.md):
//   - while following, Spyro's collision around the pawn is read out of the
//     engine's physics and sent to Crash to stand on, and the two worlds
//     are laid over each other rigidly.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "clock.h"
#include "config.h"
#include "crashlog.h"
#include "csp_protocol.h"
#include "ground.h"
#include "hook.h"
#include "log.h"
#include "memory.h"
#include "pad.h"
#include "picture.h"
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
GroundMapping g_ground;

// From crashspyro.ini, read by the worker and used on the game's thread.
struct Settings {
  std::atomic<int> follow{0};     // [follow] on: changing it switches following on or off
  std::atomic<int> key{VK_F2};    // [follow] key: so does pressing this
  std::atomic<float> scale{0.5f}; // Unreal units per Crash unit (measured: docs/PHASE0.md)
  std::atomic<float> behind{20.0f};  // [follow] behind_ms: how far behind Crash's newest frame the pawn is kept
  // [follow] ground: Spyro's collision goes to Crash and the pawn is put
  // exactly where Crash is in it. Without it, the Phase 0 test below.
  std::atomic<int> ground{1};
  std::atomic<float> groundReach{6000.0f}, groundUp{3000.0f}, groundDown{3000.0f};  // [ground] reach, up, down
  std::atomic<int> groundComplex{0};  // [ground] complex: the detailed collision rather than the simple
  std::atomic<int> pictureShow{1};    // [picture] show: Crash drawn into Spyro's picture while he is on its ground
  std::atomic<int> pictureCorner{0};  // [picture] corner: the whole of Crash's picture, small, in a corner
  std::atomic<int> hideSpyro{1};      // [picture] hide_spyro: Spyro's own character is not drawn while Crash is
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
std::atomic<bool> g_groundMode{false}; // following, with Spyro's ground being sent
std::atomic<uintptr_t> g_cameraManager{0};  // found by the worker
std::atomic<int> g_groundDumpWanted{0};  // [debug] ground: write what would be sent to a file

void LoadSettings(const Config& config) {
#define X(name, value) g_places.name = config.GetOffset("places", #name, value);
  SPYRO_PLACES(X)
#undef X
  g_settings.follow.store(config.GetInt("follow", "on", 0));
  g_settings.key.store(config.GetInt("follow", "key", VK_F2));
  g_settings.scale.store(config.GetFloat("follow", "scale", 0.5f));
  g_settings.behind.store(config.GetFloat("follow", "behind_ms", 20.0f));
  g_settings.ground.store(config.GetInt("follow", "ground", 1));
  g_settings.groundReach.store(config.GetFloat("ground", "reach", 6000.0f));
  g_settings.groundUp.store(config.GetFloat("ground", "up", 3000.0f));
  g_settings.groundDown.store(config.GetFloat("ground", "down", 3000.0f));
  g_settings.groundComplex.store(config.GetInt("ground", "complex", 0));
  g_settings.pictureShow.store(config.GetInt("picture", "show", 1));
  g_settings.pictureCorner.store(config.GetInt("picture", "corner", 0));
  g_settings.hideSpyro.store(config.GetInt("picture", "hide_spyro", 1));
  picture::Depth(config.GetFloat("picture", "near", 5.0f), config.GetFloat("picture", "bias", 2.0f));
  picture::Looks(config.GetFloat("picture", "shadow", 0.45f), config.GetInt("picture", "steady", 1) != 0, config.GetFloat("picture", "size", 0.9f));
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

// What the frame the engine is about to show was drawn with: noted on the
// game's thread once a frame (GameTick), read by the thread that shows
// frames. The game's thread is a frame ahead of what is being shown, and
// the pawn's tick comes before the camera is moved for the new frame: at
// that moment the camera manager still holds the camera of the frame being
// drawn, and the pawn is still where the last tick put it.
struct Shown {
  Vec3 cameraPosition{}, cameraRotation{};
  float cameraFov = 0.0f;
  Vec3 feet{};
  bool haveCamera = false, haveFeet = false;
};
std::atomic_flag g_shownBusy = ATOMIC_FLAG_INIT;
Shown g_shown;

bool CameraNow(Vec3& position, Vec3& rotation, float& fov, Vec3& feet, bool& haveFeet) {
  while (g_shownBusy.test_and_set(std::memory_order_acquire)) {
  }
  const Shown shown = g_shown;
  g_shownBusy.clear(std::memory_order_release);
  position = shown.cameraPosition;
  rotation = shown.cameraRotation;
  fov = shown.cameraFov;
  feet = shown.feet;
  haveFeet = shown.haveFeet;
  return shown.haveCamera;
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
  // With Spyro's ground being sent: where it was last gathered around, and
  // the point of Spyro's ground that is Crash's `crash0` (the ground under
  // the pawn when following began).
  bool ground = false;
  Vec3 centre{}, spyroAnchor{};
  bool hidden = false;  // Spyro's character has been hidden for Crash's sake
  // Where Crash is taken to be at each tick of Spyro's (Place). His last
  // few frames, each with the time on his game's own clock, and the moment
  // on that clock that where he was over the ground, and how high, belong
  // to (not always the frame's own: see Place); how far that clock is behind
  // the shared one, and how fast that is changing; and Spyro's own ticks,
  // as they came and made even.
  struct Seen {
    uint32_t gameUs, flatUs, riseUs;
    Vec3 at;
  };
  bool tracking = false;
  Seen seen[6]{};
  int seenCount = 0;
  uint32_t crashFrame = 0;
  uint32_t lead = 0;
  float drift = 0.0f;  // microseconds a frame of Crash's
  uint32_t tickWall = 0, tickAt = 0;
  float tickEvery = 16667.0f;
  // After being let go: when (0 = not lately), and where the pawn was then.
  DWORD letGo = 0;
  Vec3 leftAt{};
  // K2_SetActorRotation, likewise.
  uintptr_t setRotation = 0;
  int32_t atRotation = -1, atRotationTeleport = -1;
  // K2_SetActorLocation, looked up once per pawn class.
  uintptr_t setLocationClass = 0, setLocation = 0;
  int32_t atLocation = -1, atSweep = -1, atTeleport = -1;
};

// Spyro's character, not drawn (and drawn again): Crash is the one to see.
void HidePawn(uintptr_t pawn, bool hidden) {
  Call call;
  if (IsObject(pawn) && call.Find(pawn, "SetActorHiddenInGame") && call.Set("bNewHidden", static_cast<uint8_t>(hidden))) call.On(pawn);
}

void SetMovementTick(const Follow& f, bool enabled) {
  if (!f.movement || !IsObject(f.movement)) return;
  Call call;
  if (call.Find(f.movement, "SetComponentTickEnabled") && call.Set("bEnabled", static_cast<uint8_t>(enabled))) {
    call.On(f.movement);
  }
}

// Spyro's collision around `centre`, as ground.h gives it.
bool GatherGround(const Vec3& centre, std::vector<float>& triangles, ground::Stats& stats, uint32_t& tookUs) {
  ground::Options options;
  options.reach = g_settings.groundReach.load();
  options.up = g_settings.groundUp.load();
  options.down = g_settings.groundDown.load();
  options.complex = g_settings.groundComplex.load() != 0;
  triangles.clear();
  const uint32_t started = NowUs();
  const bool ok = ground::Gather(centre, options, triangles, stats);
  tookUs = static_cast<uint32_t>(ElapsedUs(started, NowUs()));
  if (!ok) log::Error("ground: the engine's physics could not be asked");
  return ok;
}

void DescribeGround(const char* what, const Vec3& centre, const ground::Stats& stats, uint32_t tookUs) {
  log::Info("ground: %s around (%.0f, %.0f, %.0f): %u triangles%s from %d of %d shapes in %d scenes "
            "(%d meshes, %d convex, %d boxes, %d balls, %d capsules; left out %d not solid to a pawn, %d the player passes through, "
            "%d pawns, %d height fields, %d other; the player's capsule %s, blocking 0x%08X), %.1f ms",
            what, centre.x, centre.y, centre.z, stats.triangles, stats.full ? " (FULL: more were left out)" : "", stats.used, stats.touched,
            stats.scenes, stats.meshes, stats.convexes, stats.boxes, stats.spheres, stats.capsules, stats.notSolid, stats.passedThrough,
            stats.characters, stats.heightFields, stats.others, stats.pawnFound ? "found" : "NOT found", stats.pawnBlocks,
            tookUs / 1000.0f);
}

// Sends what was gathered around `centre` to Crash, under the join `f` holds.
void PublishGround(const Follow& f, const Vec3& centre, const std::vector<float>& triangles) {
  static uint32_t generation = GetTickCount() | 1;  // not a number an earlier run of Spyro left behind
  GroundPage page{};
  page.generation = ++generation;
  page.crashGeneration = f.generation;
  page.world = mem::Pointer(mem::ImageBase() + g_places.world);
  page.centre = centre;
  page.reach = g_settings.groundReach.load();
  page.spyroAnchor = f.spyroAnchor;
  page.scale = g_settings.scale.load();
  page.crashAnchor = f.crash0;
  g_ground.Publish(page, triangles.data(), static_cast<uint32_t>(triangles.size() / 9));
}

// Has Spyro's movement stand still: whatever speed it thinks it has is
// thrown away. Seen: let go after being carried about, the character flew
// off at 15,000 units a second and did not stop.
void StopMovement(const Follow& f, Vec3* was) {
  if (!f.movement || !IsObject(f.movement)) return;
  const int32_t at = ue::FindProperty(f.movement, "Velocity");
  if (at >= 0) {
    if (was) mem::Read(f.movement + at, *was);
    const Vec3 still{};
    std::memcpy(reinterpret_cast<void*>(f.movement + at), &still, sizeof(still));
  }
  Call stop;
  if (stop.Find(f.movement, "StopMovementImmediately")) stop.On(f.movement);
}

void Release(Follow& f, uintptr_t pawn, const char* why) {
  // The component belongs to the pawn following began with. If that pawn has
  // gone (a level load), so has the component: leave it be.
  Vec3 speed{};
  f.letGo = 0;
  if (pawn == f.pawn) {
    StopMovement(f, &speed);
    SetMovementTick(f, true);
    if (f.hidden) HidePawn(pawn, false);
    // And for a while afterwards it is watched: see GameTick.
    if (PawnLocation(pawn, f.leftAt)) f.letGo = GetTickCount() | 1;
  }
  f.hidden = false;
  log::Info("follow: released (%s); Spyro's movement thought its speed was (%.0f, %.0f, %.0f)", why, speed.x, speed.y, speed.z);
  f.active = false;
  f.ground = false;
  g_groundMode.store(false);
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
  f.tracking = false;
  f.generation = guest.generation;
  f.crash0 = guest.position;
  f.crashLast = guest.position;
  f.target = f.spyro0;
  f.ground = false;
  if (g_settings.ground.load() && g_ground.open()) {
    static std::vector<float> triangles;
    static bool waiting = false;
    ground::Stats stats;
    uint32_t tookUs = 0;
    if (!GatherGround(f.spyro0, triangles, stats, tookUs)) return false;
    // Crash's position is his feet; the pawn's is its middle. The join is
    // made at the ground under the pawn, so there has to be some: there is
    // none while a level is still arriving.
    float below = 0.0f;
    if (!ground::Below(triangles, 0.0f, 0.0f, 0.0f, below)) {
      if (!waiting) log::Info("follow: waiting, there being no ground under the pawn yet (%u triangles within reach)", stats.triangles);
      waiting = true;
      return false;
    }
    waiting = false;
    DescribeGround("gathered", f.spyro0, stats, tookUs);
    if (!stats.kinds.empty()) log::Info("ground: shapes touched: %s", stats.kinds.c_str());
    f.spyroAnchor = {f.spyro0.x, f.spyro0.y, f.spyro0.z + below};
    f.centre = f.spyro0;
    f.ground = true;
    PublishGround(f, f.centre, triangles);
    log::Info("ground: the join: Spyro's (%.1f, %.1f, %.1f), the ground under the pawn, is Crash's (%.2f, %.2f, %.2f)", f.spyroAnchor.x,
              f.spyroAnchor.y, f.spyroAnchor.z, f.crash0.x, f.crash0.y, f.crash0.z);
  }
  g_groundMode.store(f.ground);
  f.active = true;
  g_following.store(true);
  g_cue.store(1);
  SetMovementTick(f, false);
  if (f.ground && g_settings.hideSpyro.load() && g_settings.pictureShow.load()) {
    HidePawn(pawn, true);
    f.hidden = true;
  }
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
  if (f.ground) {
    // The two worlds laid over each other as the ground's join has them:
    // Crash's y runs the other way, and that is all.
    //
    // Where he is at this tick is not where his newest frame has him. His
    // frames and Spyro's are both sixty a second and not in step, and
    // neither comes evenly: his game makes up a late frame's time in the
    // next one's step (GuestState::gameUs), and Spyro's ticks come 18, 18
    // and 14 thousandths of a second apart though its pictures are shown
    // evenly. Put where each frame of his had him, the pawn moves in fits
    // and starts, Spyro's camera after it, and that is what the player sees
    // as him juddering. So both clocks are made even first, and he is put
    // where his own frames, by his own clock, say he was a moment ago
    // ([follow] behind_ms: far enough back that there is nearly always a
    // frame of his on either side to go between).
    const uint32_t wall = NowUs();
    if (!f.tracking) {
      f.tracking = true;
      f.seenCount = 0;
      f.tickAt = wall;
      f.tickEvery = 16667.0f;
    } else {
      // Spyro's ticks: as far apart as they are on the whole, and pulled a
      // little towards when each really came.
      const float came = static_cast<float>(ElapsedUs(f.tickWall, wall));
      f.tickEvery += 0.02f * (std::fmin(std::fmax(came, f.tickEvery * 0.5f), f.tickEvery * 2.0f) - f.tickEvery);
      f.tickAt += static_cast<uint32_t>(std::lround(f.tickEvery));
      const int32_t late = ElapsedUs(f.tickAt, wall);
      if (late > 12000 || late < -12000) {
        f.tickAt = wall;  // a frame was dropped, or they are not coming evenly at all
      } else {
        f.tickAt += static_cast<uint32_t>(std::lround(0.05f * static_cast<float>(late)));
      }
    }
    f.tickWall = wall;

    if (guest.frame != f.crashFrame || f.seenCount == 0) {
      f.crashFrame = guest.frame;
      const uint32_t lead = guest.timeUs - guest.gameUs;
      Follow::Seen* last = f.seenCount ? &f.seen[f.seenCount - 1] : nullptr;
      const int32_t step = last ? ElapsedUs(last->gameUs, guest.gameUs) : 0;
      const float dx = last ? guest.position.x - last->at.x : 0.0f, dy = last ? guest.position.y - last->at.y : 0.0f,
                  dz = last ? guest.position.z - last->at.z : 0.0f;
      if (!last || step <= 0 || step > 250000 || dx * dx + dy * dy + dz * dz > 150.0f * 150.0f) {
        // His first frame, or his clock is stopped (his game is paused), or
        // stopped for a while, or he has been put somewhere else: nothing
        // before this frame says anything about now.
        f.seen[0] = {guest.gameUs, guest.gameUs, guest.gameUs, guest.position};
        f.seenCount = 1;
        f.lead = lead;
        f.drift = 0.0f;
      } else {
        // When this frame's place belongs to. Its own time, nearly always.
        // But after a late frame his game takes a double step, and not
        // everything of his takes it at once: running, he goes twice as far
        // in that frame; in the air, he rises or falls a single step in it
        // and the double in the frame after. So what moved a single step in
        // a long frame is put one usual step after the frame before, and
        // which it was is told by which leaves him moving more as he was.
        // (Over the ground and up and down are told apart, each for itself.)
        int32_t usual = step;
        for (int i = 1; i < f.seenCount; ++i) usual = std::min(usual, ElapsedUs(f.seen[i - 1].gameUs, f.seen[i].gameUs));
        uint32_t flatUs = guest.gameUs, riseUs = guest.gameUs;
        if (step - usual > 2000 && f.seenCount >= 2) {
          const Follow::Seen& before = f.seen[f.seenCount - 2];
          const uint32_t early = last->gameUs + static_cast<uint32_t>(usual);
          const float flatBefore = static_cast<float>(ElapsedUs(before.flatUs, last->flatUs)), riseBefore = static_cast<float>(ElapsedUs(before.riseUs, last->riseUs));
          const float flatLate = static_cast<float>(ElapsedUs(last->flatUs, guest.gameUs)), flatEarly = static_cast<float>(ElapsedUs(last->flatUs, early));
          const float riseLate = static_cast<float>(ElapsedUs(last->riseUs, guest.gameUs)), riseEarly = static_cast<float>(ElapsedUs(last->riseUs, early));
          if (flatBefore > 0.0f && flatEarly > 0.0f && flatLate > 0.0f) {
            const float wasX = (last->at.x - before.at.x) / flatBefore, wasY = (last->at.y - before.at.y) / flatBefore;
            const auto off = [&](float over) { return std::hypot(dx / over - wasX, dy / over - wasY); };
            if (off(flatEarly) < off(flatLate)) flatUs = early;
          }
          if (riseBefore > 0.0f && riseEarly > 0.0f && riseLate > 0.0f) {
            const float was = (last->at.z - before.at.z) / riseBefore;
            if (std::fabs(dz / riseEarly - was) < std::fabs(dz / riseLate - was)) riseUs = early;
          }
        }
        const int most = static_cast<int>(sizeof(f.seen) / sizeof(f.seen[0]));
        if (f.seenCount == most) {
          for (int i = 1; i < most; ++i) f.seen[i - 1] = f.seen[i];
          --f.seenCount;
        }
        f.seen[f.seenCount++] = {guest.gameUs, flatUs, riseUs, guest.position};
        // How far his clock is behind the shared one: followed slowly, since
        // any one frame of his may be ten thousandths of a second late.
        const float error = static_cast<float>(static_cast<int32_t>(lead - f.lead));
        if (std::fabs(error) > 60000.0f) {
          f.lead = lead;
          f.drift = 0.0f;
        } else {
          f.drift += 0.0001f * error;
          f.lead += static_cast<uint32_t>(std::lround(0.02f * error + f.drift));
        }
      }
    }

    // The moment wanted, on his clock, and where his frames have him then:
    // between the two of them either side of it, or, later than his newest
    // (it is late), carried on as he was going for a little while and no
    // further.
    const float behind = std::fmin(std::fmax(g_settings.behind.load(), 0.0f), 60.0f);
    const uint32_t wanted = f.tickAt - f.lead - static_cast<uint32_t>(behind * 1000.0f);
    struct Span {
      int from, to;
      float along;
    };
    const auto span = [&](uint32_t Follow::Seen::*when) -> Span {
      const int newest = f.seenCount - 1;
      if (newest < 1) return {0, 0, 0.0f};
      const int32_t past = ElapsedUs(f.seen[newest].*when, wanted);
      if (past >= 0) {
        const float step = static_cast<float>(ElapsedUs(f.seen[newest - 1].*when, f.seen[newest].*when));
        return {newest - 1, newest, 1.0f + static_cast<float>(past > 30000 ? 30000 : past) / std::fmax(step, 1000.0f)};
      }
      for (int i = newest; i > 0; --i) {
        const int32_t into = ElapsedUs(f.seen[i - 1].*when, wanted);
        if (into < 0) continue;
        return {i - 1, i, static_cast<float>(into) / std::fmax(static_cast<float>(ElapsedUs(f.seen[i - 1].*when, f.seen[i].*when)), 1000.0f)};
      }
      return {0, 0, 0.0f};
    };
    const Span flat = span(&Follow::Seen::flatUs), rise = span(&Follow::Seen::riseUs);
    const auto along = [](float a, float b, float t) { return a + (b - a) * t; };
    const Vec3 crash = {along(f.seen[flat.from].at.x, f.seen[flat.to].at.x, flat.along), along(f.seen[flat.from].at.y, f.seen[flat.to].at.y, flat.along),
                        along(f.seen[rise.from].at.z, f.seen[rise.to].at.z, rise.along)};
    const Vec3 to = {f.spyro0.x + scale * (crash.x - f.crash0.x), f.spyro0.y - scale * (crash.y - f.crash0.y), f.spyro0.z + scale * (crash.z - f.crash0.z)};
    const float stepX = to.x - f.target.x, stepY = to.y - f.target.y;
    f.target = to;
    MoveTo(f, pawn, to);
    if (stepX * stepX + stepY * stepY > 0.5f * 0.5f) Face(f, pawn, std::atan2(stepY, stepX) * 180.0f / kPi);
    // He has gone far enough from where the ground was last read: read it again.
    const float reach = g_settings.groundReach.load(), tall = std::fmin(g_settings.groundUp.load(), g_settings.groundDown.load());
    if (std::fabs(to.x - f.centre.x) > reach * 0.4f || std::fabs(to.y - f.centre.y) > reach * 0.4f || std::fabs(to.z - f.centre.z) > tall * 0.4f) {
      static std::vector<float> triangles;
      ground::Stats stats;
      uint32_t tookUs = 0;
      if (GatherGround(to, triangles, stats, tookUs)) {
        f.centre = to;
        PublishGround(f, f.centre, triangles);
        DescribeGround("gathered again", to, stats, tookUs);
      }
    }
    return;
  }
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
  {
    // For the thread that shows frames: see Shown.
    Shown shown;
    const uintptr_t manager = g_cameraManager.load(std::memory_order_relaxed);
    const Places& p = g_places;
    shown.haveCamera = IsObject(manager) && mem::Read(manager + p.cameraLocation, shown.cameraPosition) && Sane(shown.cameraPosition) &&
                       mem::Read(manager + p.cameraRotation, shown.cameraRotation) && Sane(shown.cameraRotation) &&
                       mem::Read(manager + p.cameraFov, shown.cameraFov) && shown.cameraFov > 1.0f && shown.cameraFov < 179.0f;
    if (f.active && f.ground && pawn == f.pawn) {
      shown.feet = {f.target.x, f.target.y, f.target.z - (f.spyro0.z - f.spyroAnchor.z)};
      shown.haveFeet = true;
    }
    while (g_shownBusy.test_and_set(std::memory_order_acquire)) {
    }
    g_shown = shown;
    g_shownBusy.clear(std::memory_order_release);
  }
  static bool wasDown = false;
  static int lastSetting = 0;
  static bool complained = false;
  static DWORD lastToggle = 0, lastTry = 0;
  // Whether following is asked for. It stays asked for through whatever
  // interrupts it (Spyro going to another level, Crash being restarted) and
  // begins again by itself when it can; only the key or the setting ends it.
  static bool want = false;

  const bool down = (GetAsyncKeyState(g_settings.key.load()) & 0x8000) != 0;
  // One press, one change: a second press (or a bouncing key) within a
  // second of the first undid it before anyone could see it had worked.
  const DWORD now = GetTickCount();
  if (down && !wasDown && static_cast<DWORD>(now - lastToggle) > 1000) {
    want = !want;
    lastToggle = now;
    if (!want && !f.active) log::Info("follow: no longer asked for");
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
  // Whether Crash is standing still: the join is made between where the two
  // are standing, and means nothing made in the middle of a jump or a fall.
  static Vec3 crashWas{};
  static DWORD crashMoved = 0;
  if (player && (std::fabs(guest.position.x - crashWas.x) > 0.05f || std::fabs(guest.position.y - crashWas.y) > 0.05f ||
                 std::fabs(guest.position.z - crashWas.z) > 0.05f)) {
    crashWas = guest.position;
    crashMoved = now;
  }
  const bool standing = player && static_cast<DWORD>(now - crashMoved) >= 300;

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
      if (!f.ground && dx * dx + dy * dy + dz * dz > reach * reach) {
        // Put him back where he started rather than leave him out there.
        MoveTo(f, pawn, f.spyro0);
        Release(f, pawn, "Crash went further than the test reaches");
        want = false;
      }
    }
  }
  // Not `else`: a pawn that has just been let go because Spyro changed
  // level is taken up again in the new one, at this very tick if it can be.
  if (!f.active && want && static_cast<DWORD>(now - lastTry) >= 250) {
    lastTry = now;
    if (standing) {
      if (Engage(f, pawn, guest)) complained = false;
    } else if (!complained) {
      log::Info("follow: asked for, but %s", player ? "Crash is not standing still yet" : linked ? "Crash has no player on foot yet" : "Crash is not there");
      complained = true;
    }
  }
  if (f.active) Place(f, pawn, guest);

  // For three seconds after letting go, Spyro's character is watched. If it
  // has been thrown (more than 1,500 units from where it was left, which
  // nothing of the game's own does that fast) it is put back and stopped.
  if (!f.active && f.letGo && pawn == f.pawn) {
    Vec3 here{};
    if (static_cast<DWORD>(now - f.letGo) > 3000) {
      f.letGo = 0;
    } else if (PawnLocation(pawn, here)) {
      const float dx = here.x - f.leftAt.x, dy = here.y - f.leftAt.y, dz = here.z - f.leftAt.z;
      if (dx * dx + dy * dy + dz * dz > 1500.0f * 1500.0f) {
        log::Error("follow: Spyro's character was thrown %.0f units after being let go; put back where it was left", std::sqrt(dx * dx + dy * dy + dz * dz));
        MoveTo(f, pawn, f.leftAt);
        StopMovement(f, nullptr);
      }
    }
  }

  // [debug] ground: what would be sent, written to a file to be looked at.
  if (g_groundDumpWanted.exchange(0)) {
    Vec3 here{};
    std::vector<float> triangles;
    ground::Stats stats;
    uint32_t tookUs = 0;
    if (PawnLocation(pawn, here) && GatherGround(here, triangles, stats, tookUs)) {
      DescribeGround("for the file,", here, stats, tookUs);
      if (!stats.kinds.empty()) log::Info("ground: shapes touched: %s", stats.kinds.c_str());
      float below = 0.0f;
      if (ground::Below(triangles, 0.0f, 0.0f, 0.0f, below)) log::Info("ground: the ground under the pawn is %.1f below its middle", -below);
      else log::Info("ground: there is NO ground under the pawn");
      const std::wstring path = g_dir + L"crashspyro_ground.bin";
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        // Count, the centre, then nine floats a triangle measured from it.
        const uint32_t count = static_cast<uint32_t>(triangles.size() / 9);
        DWORD written = 0;
        WriteFile(file, &count, sizeof(count), &written, nullptr);
        WriteFile(file, &here, sizeof(here), &written, nullptr);
        WriteFile(file, triangles.data(), static_cast<DWORD>(triangles.size() * sizeof(float)), &written, nullptr);
        CloseHandle(file);
        log::Info("ground: wrote %u triangles to crashspyro_ground.bin", count);
      }
    }
  }
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
          g_cameraManager.store(manager, std::memory_order_relaxed);
        }
      }
    }
  }
  g_pawn.store(pawn);
  // Ground is "being sent" for as long as following with it is on, also
  // while Spyro is paused or in a menu and the pawn is not ticking: Crash
  // keeps it under his feet and simply hears no controller meanwhile.
  if (g_groundMode.load(std::memory_order_relaxed)) s.flags |= kHostGround;
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
  g_ground.Open();
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

  std::string dump = config.GetString("debug", "ground");
  const DWORD started = GetTickCount();
  bool arriving = autoContinue;
  DWORD lastPress = started;
  bool steam = false, picture = false;
  char analogSeen[400] = "", analogNow[400];
  DWORD lastBeat = 0, lastReport = GetTickCount(), lastNameSearch = 0;
  for (;;) {
    Sleep(4);
    const HostState s = Sample();
    g_link.block()->host.Write(s);
    picture::Show(g_settings.pictureShow.load() != 0 && (s.flags & kHostLinked) && (s.flags & kHostGround), g_settings.pictureCorner.load() != 0);

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
      const std::string dumpNow = config.GetString("debug", "ground");
      if (dumpNow != dump) {
        dump = dumpNow;
        if (!dumpNow.empty()) g_groundDumpWanted.store(1);
      }
    }
    // Steam is not up until some way into the game's start.
    if (!steam) steam = steampad::Install(&OnSteam);
    // Nor is Direct3D there to be hooked until the game has begun to draw.
    if (!picture && s.world != 0) picture = picture::Install(&g_link, &CameraNow);
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
  log::Info("Crash-in-Spyro: Spyro bridge, protocol v%u", kVersion);
  crashlog::Install();
  CloseHandle(CreateThread(nullptr, 0, Run, nullptr, 0, nullptr));
  return TRUE;
}
