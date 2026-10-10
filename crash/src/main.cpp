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
#include <malloc.h>
#include <string>
#include <vector>

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
  X(clientLevelName, 0xB8)          \
  /* Havok (docs/PHASE1.md) */      \
  X(physicsSystem, 0x1A918C0)       \
  X(systemWorld, 0x38)              \
  X(worldVtable, 0x1854058)         \
  X(worldBodies, 0x20)              \
  X(compressedMeshVtable, 0x18579E8) \
  X(staticCompoundVtable, 0x1856258) \
  X(convexPolytopeVtable, 0x1853BF8) \
  X(bodyCinfoCtor, 0x10D6B30)       \
  X(createBody, 0x10E5CA0)          \
  X(destroyBodies, 0x10E6800)       \
  X(removeBodies, 0x10E64C0)        \
  X(addBodies, 0x10E5EA0)           \
  X(externMeshCtor, 0x1119C10)

struct Places {
#define X(name, value) uint32_t name = value;
  CRASH_PLACES(X)
#undef X
};

// What the frame routine starts with in the known build. The exe is stored
// encrypted and unpacks itself as it starts, after this DLL is loaded, so
// these bytes turning up is also how we know the code is there to hook.
constexpr char kFramePrefix[] = "48895c240848896c2410488974241848897c242041564883";
// And the two of Havok's routines the bridge calls.
constexpr char kBodyCinfoCtorPrefix[] = "48895c2408574883ec20488bd94883c1";
constexpr char kCreateBodyPrefix[] = "44894c24204c8944241853555657415441554156";
constexpr char kDestroyBodiesPrefix[] = "488954241053574155415641574883ec504963d8488bfa44";
constexpr char kRemoveBodiesPrefix[] = "4c8bdc45894b20458943184989531053";
constexpr char kAddBodiesPrefix[] = "4585c00f8e950500004c8bdc45894318";
constexpr char kExternMeshCtorPrefix[] = "48895c24084889742410574883ec300f29742420410fb6d8";

HMODULE g_module = nullptr;
std::wstring g_dir;
Places g_places;
SharedMapping g_link;
GroundMapping g_ground;

using FrameFn = bool(__fastcall*)(void* client);
FrameFn g_frame = nullptr;

// Written on the game's thread, read by the worker for the log.
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_flags{0};
std::atomic<uint32_t> g_generation{0};
std::atomic<uintptr_t> g_strangeActor{0};
std::atomic<int> g_padOnlyInFront{1};  // [pad] only_in_front
std::atomic<int> g_steerBySpyro{1};    // [ground] steer_by_spyro
// [ground] hide_own_level: 0 nothing; 1 the level's still meshes (its ground
// and walls); 2 every still body of the level's (crates and triggers too).
std::atomic<int> g_hideOwnLevel{2};
// The way Crash's own camera looks over the ground, as of his last frame.
std::atomic<float> g_aheadX{0.0f}, g_aheadY{0.0f};

void LoadPlaces(const Config& config) {
#define X(name, value) g_places.name = config.GetOffset("places", #name, value);
  CRASH_PLACES(X)
#undef X
  g_padOnlyInFront.store(config.GetInt("pad", "only_in_front", 1));
  g_steerBySpyro.store(config.GetInt("ground", "steer_by_spyro", 1));
  g_hideOwnLevel.store(config.GetInt("ground", "hide_own_level", 2));
}

// Crash goes where the stick points as *his* camera sees it, and nobody is
// looking through his camera. While he stands on Spyro's ground the stick
// is turned, before he hears it, by the angle between the two cameras, so
// that pushing it away from the player sends him away from the player in
// the picture the player has: Spyro's.
void SteerBySpyro(pad::State* state, float spyroYawDegrees) {
  const float fx = g_aheadX.load(std::memory_order_relaxed), fy = g_aheadY.load(std::memory_order_relaxed);
  if (fx == 0.0f && fy == 0.0f) return;
  const float yaw = spyroYawDegrees * 3.14159265f / 180.0f;
  // Spyro's camera's line of sight and its right, over the ground, in
  // Crash's world: Unreal's (cos, sin) and (-sin, cos) with y the other way.
  const float sx = std::cos(yaw), sy = -std::sin(yaw), rx = -std::sin(yaw), ry = -std::cos(yaw);
  const float x = state->gamepad.leftX, y = state->gamepad.leftY;
  const float wantX = y * sx + x * rx, wantY = y * sy + x * ry;  // where he is to go, in his world
  // The same, as so far along his own camera's line of sight and so far to its right.
  const float ahead = wantX * fx + wantY * fy, right = wantX * fy - wantY * fx;
  const auto fit = [](float v) { return static_cast<int16_t>(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v); };
  state->gamepad.leftX = fit(right);
  state->gamepad.leftY = fit(ahead);
}

// What Crash hears from the controller (docs/DESIGN.md, 4). Both games read
// the one controller whichever window is in front. While Spyro says its
// character is following Crash, the controller is Crash's wherever the
// player is looking; otherwise Crash is like any game, deaf to it unless in
// front.
uint32_t OnPad(uint32_t index, uint32_t result, pad::State* state) {
  // The bridge's own controller, when it is playing one, is all Crash hears.
  const bool ours = standin::Fill(index, state);
  if (!ours && result != 0) return result;
  HostState host{};
  const bool linked = g_link.block() && g_link.PeerAlive() && g_link.block()->host.Read(host) && (host.flags & kHostLinked);
  if (!ours && !linked && g_padOnlyInFront.load(std::memory_order_relaxed) && !pad::GameInFront()) pad::Quiet(state);
  if (linked && (host.flags & kHostGround) && (host.flags & kHostCamera) && g_steerBySpyro.load(std::memory_order_relaxed)) {
    SteerBySpyro(state, host.cameraRotation.y);
  }
  return ours ? 0 : result;
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

// ------------------------------------------------------------------ Havok --
//
// Phase 1's first experiment (docs/PHASE1.md): can the bridge put a body of
// its own into Crash's physics world, and is Crash stopped by it? The body
// is as plain as can be made: a copy, standing still, of Crash's own capsule
// shape, so no shape has to be built. A row of them close together is a
// fence he cannot slip through.
//
// An hknpBody is 0x90 bytes: position at +0x30 (metres; the game's units
// are inches), collision filter +0x44, shape +0x48, motion +0x68 (0 = it
// does not move), material +0x70, and at +0x88 the game's own word for it,
// which for a character is its actor with the low bit set.
constexpr uint32_t kBodySize = 0x90;
struct Body {
  float position[4];
  uint32_t flags;  // 0x400: made, but not in the world
  uint32_t filter;
  uintptr_t shape;
  uint32_t motion;
  uint16_t material;
  uintptr_t userData;
};

bool ReadBody(uintptr_t bodies, uint32_t number, Body& out) {
  uint8_t raw[kBodySize];
  if (!mem::Read(bodies + uintptr_t{kBodySize} * number, raw, sizeof(raw))) return false;
  std::memcpy(out.position, raw + 0x30, sizeof(out.position));
  std::memcpy(&out.flags, raw + 0x40, sizeof(out.flags));
  std::memcpy(&out.filter, raw + 0x44, sizeof(out.filter));
  std::memcpy(&out.shape, raw + 0x48, sizeof(out.shape));
  std::memcpy(&out.motion, raw + 0x68, sizeof(out.motion));
  std::memcpy(&out.material, raw + 0x70, sizeof(out.material));
  std::memcpy(&out.userData, raw + 0x88, sizeof(out.userData));
  return true;
}

// The world, Crash's own body in it (for his shape and where he is), and
// what a still body has to be marked as for him to be stopped by it.
struct Physics {
  uintptr_t world = 0;
  Body crash{};
  uint32_t filter = 0;
  uint16_t material = 0;
  bool likeGround = false;  // a mesh of the level's was found carrying that mark
};

std::atomic<int> g_fenceWanted{0};
std::atomic<int> g_slabWanted{0};
// [debug] slab_ahead, slab_half, slab_top, slab_thick: metres.
std::atomic<float> g_slabAhead{11.0f}, g_slabHalf{10.0f}, g_slabTop{0.5f}, g_slabThick{0.5f};
std::atomic<int> g_meshWanted{0}, g_meshFastTree{0}, g_clearWanted{0};
// [debug] mesh_rise, mesh_run: metres.
std::atomic<float> g_meshRise{1.2f}, g_meshRun{4.0f};
// How the new bodies are handed to the world: createBody's fourth argument.
// 0 makes a body and leaves it out of the world (what the game does for
// things not yet switched on: the first try here, and Crash ran through
// them); 1 puts it in at once; 2 has the world put it in at its next step.
std::atomic<int> g_fenceMode{2};
// What marks a still body as something Crash is stopped by. The walkable
// mesh of both levels looked at carries this.
std::atomic<uint32_t> g_fenceFilter{0x017F0803};

bool FindPhysics(uintptr_t actor, Physics& out) {
  const Places& p = g_places;
  const uintptr_t base = mem::ImageBase();
  if (!mem::StartsWith(base + p.bodyCinfoCtor, kBodyCinfoCtorPrefix) || !mem::StartsWith(base + p.createBody, kCreateBodyPrefix)) {
    log::Error("havok: the routines for making a body are not where they are in the known build");
    return false;
  }
  const uintptr_t system = mem::Pointer(base + p.physicsSystem);
  const uintptr_t world = system ? mem::Pointer(system + p.systemWorld) : 0;
  const uintptr_t bodies = mem::IsA(world, p.worldVtable) ? mem::Pointer(world + p.worldBodies) : 0;
  if (!bodies) {
    log::Error("havok: no physics world found");
    return false;
  }

  Body crash{}, ground{};
  bool haveCrash = false, haveGround = false;
  for (uint32_t number = 1; number < 4096 && !(haveCrash && haveGround); ++number) {
    Body body{};
    if (!ReadBody(bodies, number, body)) break;
    if (!haveCrash && body.shape && body.userData == (actor | 1)) {
      crash = body;
      haveCrash = true;
    }
    if (!haveGround && body.motion == 0 && body.filter == g_fenceFilter.load() && mem::IsA(body.shape, p.compressedMeshVtable)) {
      ground = body;
      haveGround = true;
    }
  }
  if (!haveCrash) {
    log::Error("havok: could not find Crash's body among the world's bodies");
    return false;
  }
  out.world = world;
  out.crash = crash;
  out.likeGround = haveGround;
  // No mesh marked that way here: use the mark anyway, and Crash's own material.
  out.filter = haveGround ? ground.filter : g_fenceFilter.load();
  out.material = haveGround ? ground.material : crash.material;
  return true;
}

// The way Crash's camera looks over the ground, and that turned clockwise.
void CameraAxes(const GuestState& state, float& aheadX, float& aheadY, float& acrossX, float& acrossY) {
  aheadX = -state.view[2];
  aheadY = -state.view[6];
  const float length = std::sqrt(aheadX * aheadX + aheadY * aheadY);
  if (!(state.flags & kGuestCamera) || length < 0.25f) {
    aheadX = 0.0f;
    aheadY = 1.0f;
  } else {
    aheadX /= length;
    aheadY /= length;
  }
  acrossX = aheadY;
  acrossY = -aheadX;
}

// A body that does not move, with `shape`, at `position` (metres), marked as
// `physics` says. Returns its number, or 0x7FFFFFFF.
uint32_t MakeStillBody(const Physics& physics, uintptr_t shape, const float position[4]) {
  using MakeCinfo = void* (*)(void* cinfo);
  using CreateBody = uint32_t* (*)(void* world, uint32_t* out, const void* cinfo, int mode, uint8_t flags);
  const uintptr_t base = mem::ImageBase();
  // hknpBodyCinfo: shape +0x00, motion +0x0C, material +0x12, filter +0x14,
  // position +0x30, orientation +0x40. Havok fills in the rest.
  alignas(16) uint8_t cinfo[0x200] = {};
  reinterpret_cast<MakeCinfo>(base + g_places.bodyCinfoCtor)(cinfo);
  const uint32_t still = 0;
  std::memcpy(cinfo + 0x00, &shape, sizeof(shape));
  std::memcpy(cinfo + 0x0C, &still, sizeof(still));
  std::memcpy(cinfo + 0x12, &physics.material, sizeof(physics.material));
  std::memcpy(cinfo + 0x14, &physics.filter, sizeof(physics.filter));
  std::memcpy(cinfo + 0x30, position, 4 * sizeof(float));
  uint32_t number = 0x7FFFFFFF;
  reinterpret_cast<CreateBody>(base + g_places.createBody)(reinterpret_cast<void*>(physics.world), &number, cinfo, g_fenceMode.load(), 0);
  return number;
}

struct BridgeMesh;

// What the bridge has put into a world, so that it can be taken out again.
// Only ever touched on the game's thread.
struct Made {
  uintptr_t world;
  uint32_t body;
  uintptr_t shape;   // 0 if the shape is not the bridge's to free
  BridgeMesh* mesh;  // for a mesh shape, what it reads its triangles from
};
std::vector<Made> g_made;
// Shapes whose bodies are gone, kept a while longer in case anything of
// Havok's from the step before still looks at them.
struct Dead {
  uintptr_t shape;
  BridgeMesh* mesh;
  uint32_t frame;
};
std::vector<Dead> g_dead;

void FreeShape(uintptr_t shape, BridgeMesh* mesh);

// Takes out of `world` every body the bridge put into it, and forgets the
// ones it put into worlds that are no longer there (leaving a level takes
// the world and all its bodies with it).
int ClearGround(uintptr_t world) {
  const uintptr_t destroy = mem::ImageBase() + g_places.destroyBodies;
  std::vector<uint32_t> numbers;
  for (const Made& made : g_made) {
    if (made.world == world) numbers.push_back(made.body);
    if (made.shape) g_dead.push_back({made.shape, made.mesh, g_frames.load()});
  }
  g_made.clear();
  if (numbers.empty()) return 0;
  if (!mem::StartsWith(destroy, kDestroyBodiesPrefix)) {
    log::Error("havok: the routine for destroying bodies is not where it is in the known build");
    return 0;
  }
  // (world, numbers, how many, 1): the last as the game's own calls have it.
  using DestroyBodies = void (*)(void* world, const uint32_t* numbers, int count, int activation);
  reinterpret_cast<DestroyBodies>(destroy)(reinterpret_cast<void*>(world), numbers.data(), static_cast<int>(numbers.size()), 1);
  return static_cast<int>(numbers.size());
}

// Once a frame: gives back the shapes that have been dead long enough.
void BuryTheDead() {
  constexpr uint32_t kFrames = 120;
  size_t kept = 0;
  for (const Dead& dead : g_dead) {
    if (g_frames.load() - dead.frame < kFrames) {
      g_dead[kept++] = dead;
      continue;
    }
    FreeShape(dead.shape, dead.mesh);
  }
  g_dead.resize(kept);
}

// On the game's thread, between frames, which is when the game makes bodies
// of its own.
void PutUpFence(uintptr_t actor, const GuestState& state) {
  Physics physics;
  if (!FindPhysics(actor, physics)) return;
  float aheadX, aheadY, acrossX, acrossY;
  CameraAxes(state, aheadX, aheadY, acrossX, acrossY);
  const float kAhead = 1.5f, kApart = 0.45f;
  const Body& crash = physics.crash;
  int made = 0;
  uint32_t first = 0;
  for (int k = -4; k <= 4; ++k) {
    const float position[4] = {crash.position[0] + aheadX * kAhead + acrossX * kApart * static_cast<float>(k),
                               crash.position[1] + aheadY * kAhead + acrossY * kApart * static_cast<float>(k), crash.position[2], 0.0f};
    const uint32_t number = MakeStillBody(physics, crash.shape, position);
    if (number != 0x7FFFFFFF) {
      g_made.push_back({physics.world, number, 0, nullptr});
      if (!made) first = number;
      ++made;
    }
  }
  log::Info("havok: %d still copies of Crash's capsule in a row %.1f m ahead of him, %.2f m apart (the first is body %u). "
            "He is at (%.2f, %.2f, %.2f) m; marked with filter 0x%08X, material %u (%s); handed over in mode %d",
            made, kAhead, kApart, first, crash.position[0], crash.position[1], crash.position[2], physics.filter, physics.material,
            physics.likeGround ? "as a mesh of the level's is" : "no mesh of the level's is marked so", g_fenceMode.load());
}

// A box of the bridge's own making, as Havok keeps one: an
// hknpConvexPolytopeShape with eight corners, six sides. The layout is as
// read from one of the game's (docs/PHASE1.md); nothing is copied from the
// game. `halfX`, `halfY`: half its size over the ground; `top`, `bottom`:
// heights of its two flat faces, all in metres from the body's own place.
// The block is never given back: Havok is told it does not own it.
uintptr_t MakeBoxShape(float halfX, float halfY, float top, float bottom) {
  auto* block = static_cast<uint8_t*>(_aligned_malloc(0x190, 16));
  if (!block) return 0;
  std::memset(block, 0, 0x190);
  const auto put16 = [&](int at, uint16_t v) { std::memcpy(block + at, &v, 2); };
  const auto put32 = [&](int at, uint32_t v) { std::memcpy(block + at, &v, 4); };
  const auto putF = [&](int at, float v) { std::memcpy(block + at, &v, 4); };

  const uintptr_t vtable = mem::ImageBase() + g_places.convexPolytopeVtable;
  std::memcpy(block, &vtable, sizeof(vtable));
  put16(0x08, 1);       // one reference
  put16(0x0A, 0);       // "size 0": not Havok's to count references on or to free
  put16(0x10, 0x01C3);  // flags, as a box of the game's has them
  block[0x12] = 0;      // no bits of shape key
  block[0x13] = 1;      // dispatched as a convex polytope
  putF(0x14, 0.01f);    // convex radius
  put32(0x28, 0x1FFF);
  // Four arrays, each given as how many and how far on from here they are.
  put16(0x30, 8);  put16(0x32, 0x20);   // corners, at +0x50
  put32(0x34, 0xFFFFFFFF);
  put16(0x40, 8);  put16(0x42, 0x90);   // sides' planes (six, padded to eight), at +0xD0
  put16(0x44, 6);  put16(0x46, 0x10C);  // sides, at +0x150
  put16(0x48, 24); put16(0x4A, 0x128);  // which corners each side has, at +0x170
  put32(0x4C, 0x3F000014);

  // Corners: x, y, z, and a fourth number that is 0.5 with the corner's
  // number in its lowest bits.
  static const int kCorner[8][3] = {{+1, +1, +1}, {+1, -1, +1}, {+1, +1, -1}, {-1, +1, +1},
                                    {+1, -1, -1}, {-1, +1, -1}, {-1, -1, +1}, {-1, -1, -1}};
  for (int i = 0; i < 8; ++i) {
    const int at = 0x50 + 16 * i;
    putF(at + 0, static_cast<float>(kCorner[i][0]) * halfX);
    putF(at + 4, static_cast<float>(kCorner[i][1]) * halfY);
    putF(at + 8, kCorner[i][2] > 0 ? top : bottom);
    put32(at + 12, 0x3F000000u | static_cast<uint32_t>(i));
  }
  // Planes: a normal and a distance, so that normal . point + distance = 0 on the side.
  const float planes[6][4] = {{0, 0, 1, -top},   {0, 0, -1, bottom}, {1, 0, 0, -halfX},
                              {-1, 0, 0, -halfX}, {0, -1, 0, -halfY}, {0, 1, 0, -halfY}};
  for (int i = 0; i < 6; ++i) std::memcpy(block + 0xD0 + 16 * i, planes[i], 16);
  // The two that pad the six out to eight, as the game's own box has them.
  put32(0x130, 0xFFFFFFFF); put32(0x134, 0xFFFFFFFF);
  put32(0x140, 0xFFFFFFFF); put32(0x144, 0xFFFFFFFF); put32(0x148, 0x3F84E5CA); put32(0x14C, 0x3F000014);
  // Sides: where each one's corners start in the list below, how many, and a flag.
  for (int i = 0; i < 6; ++i) {
    put16(0x150 + 4 * i, static_cast<uint16_t>(4 * i));
    block[0x152 + 4 * i] = 4;
    block[0x153 + 4 * i] = 0x80;
  }
  put32(0x168, 5);
  static const uint8_t kSides[24] = {0, 3, 6, 1, 2, 4, 7, 5, 0, 1, 4, 2, 7, 6, 3, 5, 7, 4, 1, 6, 0, 2, 5, 3};
  std::memcpy(block + 0x170, kSides, sizeof(kSides));
  return reinterpret_cast<uintptr_t>(block);
}

// Phase 1's second experiment: something flat for him to stand on. A wide
// box whose top is a little above the ground he is on, some way ahead.
void LayDownSlab(uintptr_t actor, const GuestState& state) {
  Physics physics;
  if (!FindPhysics(actor, physics)) return;
  float aheadX, aheadY, acrossX, acrossY;
  CameraAxes(state, aheadX, aheadY, acrossX, acrossY);
  const float ahead = g_slabAhead.load(), half = g_slabHalf.load(), top = g_slabTop.load(), thick = g_slabThick.load();
  const uintptr_t shape = MakeBoxShape(half, half, top, top - thick);
  if (!shape) return;
  const Body& crash = physics.crash;
  // The box lies along the world's axes, not the camera's: its middle is put ahead of him.
  const float position[4] = {crash.position[0] + aheadX * ahead, crash.position[1] + aheadY * ahead, crash.position[2], 0.0f};
  const uint32_t number = MakeStillBody(physics, shape, position);
  if (number != 0x7FFFFFFF) g_made.push_back({physics.world, number, shape, nullptr});
  log::Info("havok: a box %.1f m square and %.2f m thick, its top %.2f m above his feet, its middle %.1f m ahead of him at (%.2f, %.2f, %.2f) m: body %u. "
            "Marked with filter 0x%08X, material %u",
            2 * half, thick, top, ahead, position[0], position[1], position[2], number, physics.filter, physics.material);
}

// A triangle mesh of the bridge's own, in the form Havok's
// hknpExternMeshShape asks for (docs/PHASE1.md): a reference-counted object
// with a few questions to answer. The shape keeps only a search tree over
// the triangles and comes back here for their corners, from Havok's worker
// threads while the world steps. So nothing here changes once a shape has
// been made from it.
struct MeshVtable {
  void* (*destroy)(BridgeMesh*, unsigned);
  void (*first)(BridgeMesh*);
  const void* (*classType)(BridgeMesh*);
  void (*deleteThis)(BridgeMesh*);
  int (*triangles)(BridgeMesh*);
  void (*corners)(BridgeMesh*, int index, float* out);  // three corners, four floats each
  uint16_t (*tag)(BridgeMesh*, int index);
  bool (*extra)(BridgeMesh*);
};
struct BridgeMesh {
  const MeshVtable* vtable;
  uint32_t sizeAndReferences;  // size 0: Havok neither counts references on it nor frees it
  uint32_t unused;
  int triangles;
  const float* corners;  // twelve floats a triangle, metres from the body's own place
};
const MeshVtable kMeshVtable = {
    [](BridgeMesh* self, unsigned) -> void* { return self; },
    [](BridgeMesh*) {},
    [](BridgeMesh*) -> const void* { return nullptr; },
    [](BridgeMesh*) {},
    [](BridgeMesh* self) { return self->triangles; },
    [](BridgeMesh* self, int index, float* out) {
      if (index < 0 || index >= self->triangles) index = 0;
      std::memcpy(out, self->corners + 12 * index, 12 * sizeof(float));
    },
    // No tag: the world's codec for them is Havok's null one, so every
    // triangle goes by what its body is marked as.
    [](BridgeMesh*, int) -> uint16_t { return 0xFFFF; },
    [](BridgeMesh*) { return false; },
};

// An hknpExternMeshShape over `count` triangles, built by Havok's own
// constructor, which also builds the search tree. `corners` is copied.
// Neither the shape nor the mesh is ever given back, yet.
uintptr_t MakeMeshShape(const float* corners, int count, BridgeMesh*& meshOut) {
  meshOut = nullptr;
  const uintptr_t make = mem::ImageBase() + g_places.externMeshCtor;
  if (!mem::StartsWith(make, kExternMeshCtorPrefix)) {
    log::Error("havok: the routine for making a mesh shape is not where it is in the known build");
    return 0;
  }
  auto* copy = static_cast<float*>(_aligned_malloc(static_cast<size_t>(count) * 12 * sizeof(float), 16));
  auto* shape = static_cast<uint8_t*>(_aligned_malloc(0x70, 16));
  if (!copy || !shape) return 0;
  std::memcpy(copy, corners, static_cast<size_t>(count) * 12 * sizeof(float));
  std::memset(shape, 0, 0x70);
  auto* mesh = new BridgeMesh{&kMeshVtable, 1, 0, count, copy};
  // (shape, mesh, also build the faster of its two trees, how thin a
  // triangle may be before it is left out of the tree)
  using MakeExternMesh = void* (*)(void* shape, void* mesh, bool fastTree, float tolerance);
  reinterpret_cast<MakeExternMesh>(make)(shape, mesh, g_meshFastTree.load() != 0, 1e-7f);
  // The memory is the bridge's, not Havok's heap's: have Havok not count
  // references on the shape, so that it never tries to free it.
  const uint16_t none = 0;
  std::memcpy(shape + 0x0A, &none, sizeof(none));
  meshOut = mesh;
  return reinterpret_cast<uintptr_t>(shape);
}

// Gives back a shape the bridge made, once nothing of Havok's uses it.
void FreeShape(uintptr_t shape, BridgeMesh* mesh) {
  if (mesh) {
    // Havok's own destructor for the shape, which lets go of the search
    // tree it built. (Asked not to free the shape itself: that is ours.)
    using Destroy = void* (*)(void* shape, unsigned flags);
    const auto* vtable = reinterpret_cast<const Destroy*>(mem::Pointer(shape));
    if (vtable) vtable[0](reinterpret_cast<void*>(shape), 0);
    _aligned_free(const_cast<float*>(mesh->corners));
    delete mesh;
  }
  _aligned_free(reinterpret_cast<void*>(shape));
}

// Phase 1's third experiment: ground made of triangles. A ramp up from the
// floor he is on, starting a metre ahead of him, and a flat top beyond it.
void LayDownMesh(uintptr_t actor, const GuestState& state) {
  Physics physics;
  if (!FindPhysics(actor, physics)) return;
  float aheadX, aheadY, acrossX, acrossY;
  CameraAxes(state, aheadX, aheadY, acrossX, acrossY);
  const float start = 1.0f, run = g_meshRun.load(), rise = g_meshRise.load(), length = run + 4.0f, halfWidth = 3.0f;
  const int along = 8, across = 6;
  const auto height = [&](float a) { return a <= start ? 0.0f : a >= start + run ? rise : rise * (a - start) / run; };
  float corners[along * across * 2 * 12];
  int count = 0;
  const auto corner = [&](float* to, float a, float b) {
    to[0] = aheadX * a + acrossX * b;
    to[1] = aheadY * a + acrossY * b;
    to[2] = height(a);
    to[3] = 0.0f;
  };
  for (int i = 0; i < along; ++i) {
    const float a0 = start + length * static_cast<float>(i) / along, a1 = start + length * static_cast<float>(i + 1) / along;
    for (int j = 0; j < across; ++j) {
      const float b0 = -halfWidth + 2 * halfWidth * static_cast<float>(j) / across, b1 = -halfWidth + 2 * halfWidth * static_cast<float>(j + 1) / across;
      // Two triangles a square, each wound so that its face is the upper side.
      float* t = corners + 12 * count;
      corner(t + 0, a0, b0); corner(t + 4, a0, b1); corner(t + 8, a1, b1);
      corner(t + 12, a0, b0); corner(t + 16, a1, b1); corner(t + 20, a1, b0);
      count += 2;
    }
  }
  BridgeMesh* mesh = nullptr;
  const uintptr_t shape = MakeMeshShape(corners, count, mesh);
  if (!shape) return;
  const Body& crash = physics.crash;
  const float position[4] = {crash.position[0], crash.position[1], crash.position[2], 0.0f};
  const uint32_t number = MakeStillBody(physics, shape, position);
  if (number != 0x7FFFFFFF) g_made.push_back({physics.world, number, shape, mesh});
  log::Info("havok: a mesh of %d triangles, a ramp from %.1f m ahead of him rising %.2f m over %.1f m and then flat for 4 m, %.1f m wide: body %u, shape at %p (%s tree). "
            "He is at (%.2f, %.2f, %.2f) m; marked with filter 0x%08X, material %u",
            count, start, rise, run, 2 * halfWidth, number, reinterpret_cast<void*>(shape), g_meshFastTree.load() ? "with the fast" : "plain",
            position[0], position[1], position[2], physics.filter, physics.material);
}

// ---------------------------------------------------------- Spyro's ground --
//
// While Spyro says its ground is being sent (kHostGround), the latest page
// of it is kept in Crash's world as one still body with a mesh shape, laid
// where the page's join says. The level's own still meshes are taken out of
// the world for as long, so that it is Spyro's ground he stands on and not
// whatever of his own is at the same height; they go back in afterwards.

struct Piece {
  uintptr_t world = 0;
  uint32_t body = 0x7FFFFFFF;
  uintptr_t shape = 0;
  BridgeMesh* mesh = nullptr;
  uint32_t frame = 0;  // when it was superseded
};
struct SpyroGround {
  uint32_t built = 0;      // the generation of the page `now` was made from
  Piece now;
  std::vector<Piece> old;  // superseded, each destroyed a few frames after what replaced it went in
  uintptr_t hiddenWorld = 0;
  std::vector<uint32_t> hidden;  // the level's own, out of the world meanwhile
  DWORD lastWanted = 0;
  std::vector<float> corners, local;
} g_spyroGround;

// Destroys a piece's body if the world it was put in is still the world,
// and has its shape given back later.
void DropPiece(Piece& piece, uintptr_t world) {
  if (piece.body != 0x7FFFFFFF && piece.world == world && world) {
    const uintptr_t destroy = mem::ImageBase() + g_places.destroyBodies;
    if (mem::StartsWith(destroy, kDestroyBodiesPrefix)) {
      using DestroyBodies = void (*)(void* world, const uint32_t* numbers, int count, int activation);
      reinterpret_cast<DestroyBodies>(destroy)(reinterpret_cast<void*>(world), &piece.body, 1, 1);
    }
  }
  if (piece.shape) g_dead.push_back({piece.shape, piece.mesh, g_frames.load()});
  piece = Piece{};
}

void HideOwnLevel(const Physics& physics) {
  SpyroGround& g = g_spyroGround;
  const int how = g_hideOwnLevel.load();
  if (!how || g.hiddenWorld == physics.world) return;
  const Places& p = g_places;
  const uintptr_t remove = mem::ImageBase() + p.removeBodies;
  if (!mem::StartsWith(remove, kRemoveBodiesPrefix)) {
    log::Error("havok: the routine for taking bodies out of the world is not where it is in the known build");
    return;
  }
  const uintptr_t bodies = mem::Pointer(physics.world + p.worldBodies);
  g.hidden.clear();
  for (uint32_t number = 1; number < 4096; ++number) {
    Body body{};
    if (!ReadBody(bodies, number, body)) break;
    if (!body.shape || body.motion != 0 || (body.flags & 0x400) || number == g.now.body) continue;
    bool ours = false;
    for (const Made& made : g_made) ours = ours || (made.world == physics.world && made.body == number);
    if (ours) continue;
    if (how >= 2 || mem::IsA(body.shape, p.compressedMeshVtable) || mem::IsA(body.shape, p.staticCompoundVtable)) g.hidden.push_back(number);
  }
  g.hiddenWorld = physics.world;
  if (g.hidden.empty()) return;
  using RemoveBodies = void (*)(void* world, const uint32_t* numbers, int count, int activation);
  reinterpret_cast<RemoveBodies>(remove)(reinterpret_cast<void*>(physics.world), g.hidden.data(), static_cast<int>(g.hidden.size()), 1);
  log::Info("ground: took the level's own %zu still %s out of the world (the first is body %u)", g.hidden.size(),
            how >= 2 ? "bodies" : "meshes", g.hidden[0]);
}

void ShowOwnLevel(uintptr_t world) {
  SpyroGround& g = g_spyroGround;
  if (g.hiddenWorld == world && world && !g.hidden.empty()) {
    const Places& p = g_places;
    const uintptr_t add = mem::ImageBase() + p.addBodies, bodies = mem::Pointer(world + p.worldBodies);
    // Only the ones that are still there and still out.
    std::vector<uint32_t> back;
    for (const uint32_t number : g.hidden) {
      Body body{};
      if (ReadBody(bodies, number, body) && body.shape && body.motion == 0 && (body.flags & 0x400)) back.push_back(number);
    }
    if (!back.empty() && mem::StartsWith(add, kAddBodiesPrefix)) {
      using AddBodies = void (*)(void* world, const uint32_t* numbers, int count, int mode, uint8_t flags);
      reinterpret_cast<AddBodies>(add)(reinterpret_cast<void*>(world), back.data(), static_cast<int>(back.size()), 2, 0);
    }
    log::Info("ground: put %zu of the level's own %zu bodies back", back.size(), g.hidden.size());
  }
  g.hidden.clear();
  g.hiddenWorld = 0;
}

// Makes the ground the page describes and puts it in; the ground before it
// goes a few frames later, so that he is never standing on nothing.
void BuildGround(const Physics& physics, const GroundPage& page, const float* corners) {
  SpyroGround& g = g_spyroGround;
  const uint32_t count = page.triangles;
  // Unreal's world to Crash's by the page's join, then to Havok: metres
  // (Crash's units are inches), from the body's own place, which is put at
  // the join. Crash's y runs the other way, which turns every triangle
  // over; giving its corners in the other order turns it back.
  constexpr float kInchesPerMetre = 39.37f;
  const float k = 1.0f / (page.scale * kInchesPerMetre);
  const float ox = page.centre.x - page.spyroAnchor.x, oy = page.centre.y - page.spyroAnchor.y, oz = page.centre.z - page.spyroAnchor.z;
  g.local.resize(static_cast<size_t>(count) * 12);
  static const int kOrder[3] = {0, 2, 1};
  for (uint32_t t = 0; t < count; ++t) {
    for (int c = 0; c < 3; ++c) {
      const float* from = corners + static_cast<size_t>(t) * 9 + kOrder[c] * 3;
      float* to = &g.local[static_cast<size_t>(t) * 12 + c * 4];
      to[0] = (from[0] + ox) * k;
      to[1] = -(from[1] + oy) * k;
      to[2] = (from[2] + oz) * k;
      to[3] = 0.0f;
    }
  }
  const uint32_t started = NowUs();
  BridgeMesh* mesh = nullptr;
  const uintptr_t shape = count ? MakeMeshShape(g.local.data(), static_cast<int>(count), mesh) : 0;
  uint32_t number = 0x7FFFFFFF;
  const float position[4] = {page.crashAnchor.x / kInchesPerMetre, page.crashAnchor.y / kInchesPerMetre, page.crashAnchor.z / kInchesPerMetre, 0.0f};
  if (shape) number = MakeStillBody(physics, shape, position);
  const float tookMs = ElapsedUs(started, NowUs()) / 1000.0f;

  if (g.now.body != 0x7FFFFFFF || g.now.shape) {
    g.now.frame = g_frames.load();
    g.old.push_back(g.now);
  }
  g.now = Piece{physics.world, number, shape, mesh, 0};
  log::Info("ground: Spyro's ground, page %u: %u triangles around his (%.0f, %.0f, %.0f) as body %u at (%.2f, %.2f, %.2f) m, %.1f ms to make. "
            "Crash is at (%.2f, %.2f, %.2f) m",
            page.generation, count, page.centre.x, page.centre.y, page.centre.z, number, position[0], position[1], position[2], tookMs,
            physics.crash.position[0], physics.crash.position[1], physics.crash.position[2]);
}

// Once a frame on the game's thread.
void TendGround(uintptr_t actor, const GuestState& s) {
  SpyroGround& g = g_spyroGround;
  const Places& p = g_places;
  const uintptr_t system = mem::Pointer(mem::ImageBase() + p.physicsSystem);
  const uintptr_t found = system ? mem::Pointer(system + p.systemWorld) : 0;
  const uintptr_t world = mem::IsA(found, p.worldVtable) ? found : 0;

  // A world that has gone took its bodies with it.
  if (g.now.world && g.now.world != world) DropPiece(g.now, world);
  if (g.hiddenWorld && g.hiddenWorld != world) ShowOwnLevel(world);
  // What was superseded goes once its successor has been in for a few frames.
  size_t kept = 0;
  for (Piece& piece : g.old) {
    if (piece.world == world && g_frames.load() - piece.frame < 4) g.old[kept++] = piece;
    else DropPiece(piece, world);
  }
  g.old.resize(kept);

  HostState host{};
  const bool wanted = g_ground.open() && g_link.PeerAlive() && g_link.block()->host.Read(host) && (host.flags & kHostGround) &&
                      (s.flags & kGuestPlayer) && world;
  const DWORD now = GetTickCount();
  if (!wanted) {
    // Not at once: one missed heartbeat is not the end of it.
    if ((g.now.shape || g.hiddenWorld) && static_cast<DWORD>(now - g.lastWanted) > 500) {
      ShowOwnLevel(world);
      DropPiece(g.now, world);
      g.built = 0;
      log::Info("ground: Spyro's ground is no longer wanted; taken out");
    }
    return;
  }
  g.lastWanted = now;

  const uint32_t latest = g_ground.Latest();
  if (latest == 0 || latest == g.built) return;
  g.corners.resize(static_cast<size_t>(kGroundMaxTriangles) * 9);
  GroundPage page{};
  if (!g_ground.Read(page, g.corners.data(), kGroundMaxTriangles) || page.generation != latest) return;  // next frame
  g.built = latest;
  // A page made for where Crash was before a level load says nothing now.
  if (page.crashGeneration != s.generation || !(page.scale > 0.0f)) return;
  Physics physics;
  if (!FindPhysics(actor, physics)) return;
  BuildGround(physics, page, g.corners.data());
  HideOwnLevel(physics);
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
  {
    float aheadX, aheadY, acrossX, acrossY;
    CameraAxes(s, aheadX, aheadY, acrossX, acrossY);
    g_aheadX.store(aheadX, std::memory_order_relaxed);
    g_aheadY.store(aheadY, std::memory_order_relaxed);
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

  if ((s.flags & kGuestPlayer) && g_fenceWanted.exchange(0)) PutUpFence(actor, s);
  if ((s.flags & kGuestPlayer) && g_slabWanted.exchange(0)) LayDownSlab(actor, s);
  if ((s.flags & kGuestPlayer) && g_meshWanted.exchange(0)) LayDownMesh(actor, s);
  if (g_clearWanted.exchange(0)) {
    const uintptr_t system = mem::Pointer(base + p.physicsSystem);
    const uintptr_t world = system ? mem::Pointer(system + p.systemWorld) : 0;
    const size_t had = g_made.size();
    const int gone = ClearGround(mem::IsA(world, p.worldVtable) ? world : 0);
    log::Info("havok: took %d of the bridge's %zu bodies out of the world (the rest were in worlds that are gone)", gone, had);
  }
  TendGround(actor, s);
  BuryTheDead();
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
  g_ground.Open();
  // Whatever the last run of Crash left in its slot is not this run's.
  g_link.block()->guest.Write(GuestState{});
  pad::SetFilter(&OnPad);
  log::Info("pad: %d of the game's two XInput calls come straight to the bridge", pad::TakeBack());
  standin::DebugPress(config);
  if (!hook::Install(reinterpret_cast<void*>(frame), reinterpret_cast<void*>(&OnFrame),
                     reinterpret_cast<void**>(&g_frame), "Crash's frame")) {
    return 0;
  }

  std::string fence = config.GetString("debug", "fence"), slab = config.GetString("debug", "slab");
  std::string mesh = config.GetString("debug", "mesh"), clear = config.GetString("debug", "clear");
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
      // [debug] fence = <anything>: each change of the text puts one up.
      const std::string now = config.GetString("debug", "fence");
      g_fenceMode.store(config.GetInt("debug", "fence_mode", 2));
      g_fenceFilter.store(config.GetOffset("debug", "fence_filter", 0x017F0803));
      if (now != fence) {
        fence = now;
        if (!now.empty()) g_fenceWanted.store(1);
      }
      // [debug] slab = <anything>: likewise, one box to stand on.
      const std::string slabNow = config.GetString("debug", "slab");
      g_slabAhead.store(config.GetFloat("debug", "slab_ahead", 11.0f));
      g_slabHalf.store(config.GetFloat("debug", "slab_half", 10.0f));
      g_slabTop.store(config.GetFloat("debug", "slab_top", 0.5f));
      g_slabThick.store(config.GetFloat("debug", "slab_thick", 0.5f));
      if (slabNow != slab) {
        slab = slabNow;
        if (!slabNow.empty()) g_slabWanted.store(1);
      }
      // [debug] mesh = <anything>: likewise, a ramp made of triangles.
      const std::string meshNow = config.GetString("debug", "mesh");
      g_meshRise.store(config.GetFloat("debug", "mesh_rise", 1.2f));
      g_meshRun.store(config.GetFloat("debug", "mesh_run", 4.0f));
      g_meshFastTree.store(config.GetInt("debug", "mesh_fast_tree", 0));
      if (meshNow != mesh) {
        mesh = meshNow;
        if (!meshNow.empty()) g_meshWanted.store(1);
      }
      // [debug] clear = <anything>: takes out everything the bridge put in.
      const std::string clearNow = config.GetString("debug", "clear");
      if (clearNow != clear) {
        clear = clearNow;
        if (!clearNow.empty()) g_clearWanted.store(1);
      }
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
