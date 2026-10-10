// What the two games say to each other, and the one block of memory they say
// it in. Both bridges are C++ and both include this file, so there is one
// definition and no second copy to keep in step.
//
// Spyro Reignited Trilogy is the host: it is the game on screen and it owns
// the world. Crash Bandicoot N. Sane Trilogy is the guest: it runs unseen and
// owns the player. docs/DESIGN.md has who owns what.
//
// Platform neutral on purpose (no windows.h), so tests/ can build it anywhere.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace csp {

constexpr wchar_t kMappingName[] = L"Local\\CrashSpyro_link_v1";
// Held by the launcher for as long as a session lasts; see common/session.h.
constexpr wchar_t kSessionName[] = L"Local\\CrashSpyro_session";

constexpr uint32_t kMagic = 0x4C505343;  // "CSPL"
constexpr uint32_t kMagicInitializing = 1;
// Bump on any change to the layout below. A bridge refuses a block of another
// version rather than read it wrongly.
constexpr uint32_t kVersion = 2;

enum Side : uint32_t { kSideSpyro = 0, kSideCrash = 1, kSideCount = 2 };
constexpr Side Other(Side s) { return s == kSideSpyro ? kSideCrash : kSideSpyro; }

struct Vec3 {
  float x, y, z;
};

struct PeerInfo {
  std::atomic<uint32_t> pid;
  std::atomic<uint32_t> heartbeatMs;  // GetTickCount() when it last beat
  std::atomic<uint32_t> beats;
  // Counts the game's own frames, from the game's own thread. A heartbeat
  // from a helper thread would keep a hung game looking alive; this cannot.
  std::atomic<uint32_t> frames;
};

struct Header {
  std::atomic<uint32_t> magic;
  uint32_t version;
  uint32_t size;
  uint32_t reserved;
  PeerInfo peers[kSideCount];
};

// A peer is alive if it has ever beaten and did so within the last two
// seconds. Unsigned subtraction, so the 49-day wrap of GetTickCount is fine.
constexpr uint32_t kHeartbeatTimeoutMs = 2000;
inline bool HeartbeatAlive(uint32_t heartbeatMs, uint32_t beats, uint32_t nowMs) {
  return beats != 0 && static_cast<uint32_t>(nowMs - heartbeatMs) < kHeartbeatTimeoutMs;
}

// Microseconds between two values of the shared clock (common/clock.h),
// which wraps every 71 minutes. Signed: `later` may be a touch earlier.
inline int32_t ElapsedUs(uint32_t earlier, uint32_t later) {
  return static_cast<int32_t>(later - earlier);
}

// One writer, any number of readers, never a lock: the writer makes `seq`
// odd, writes, and makes it even again; a reader that saw an odd number or a
// different number afterwards read half of one value and half of the next,
// and tries again.
template <typename T>
struct Slot {
  std::atomic<uint32_t> seq;
  uint32_t reserved;
  T value;

  void Write(const T& v) {
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_release);
    std::memcpy(static_cast<void*>(&value), &v, sizeof(T));
    seq.store(s + 2, std::memory_order_release);
  }

  // False if nothing has been written yet, or the writer would not hold still.
  bool Read(T& out) const {
    for (int attempt = 0; attempt < 16; ++attempt) {
      const uint32_t before = seq.load(std::memory_order_acquire);
      if (before == 0) return false;
      if (before & 1) continue;
      std::memcpy(static_cast<void*>(&out), &value, sizeof(T));
      if (seq.load(std::memory_order_acquire) == before) return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------- guest ----

enum GuestFlags : uint32_t {
  kGuestPlayer = 1u << 0,    // the player object was found and is the type expected
  kGuestParented = 1u << 1,  // it is riding something; `position` is relative to that
  kGuestModel = 1u << 2,     // `model` is filled in
  kGuestCamera = 1u << 3,    // `cameraPosition`, `view` and `projection` are filled in
  kGuestClock = 1u << 4,     // `simTicks` and kGuestPaused mean something
  kGuestPaused = 1u << 5,    // Crash's gameplay clock is stopped (menu, loading)
  kGuestLevel = 1u << 6,     // `level` is filled in
};

// Crash, as of the end of one of its game frames. Lengths are in Crash's own
// units and axes; turning them into Spyro's is the host's job, in one place.
struct GuestState {
  uint32_t flags;
  uint32_t frame;       // counts Crash's game frames since the bridge hooked in
  uint32_t timeUs;      // shared clock at the moment it was taken
  uint32_t simTicks;    // Crash's own count of gameplay ticks
  // Goes up each time the player object is lost and found again, which is
  // what a level load looks like from here. A position from one generation
  // says nothing about where the player is in the next.
  uint32_t generation;
  uint32_t reserved;
  Vec3 position;        // the player actor's own position
  float model[16];      // the matrix the game drew the player's model with
  Vec3 cameraPosition;
  float view[16];       // Crash's camera: world to camera
  float projection[16];
  char level[64];       // e.g. "crash3/l301_toadvillage/l301_toadvillage"
};

// ----------------------------------------------------------------- host ----

enum HostFlags : uint32_t {
  kHostPawn = 1u << 0,    // the player's pawn was found; `position` etc. are filled in
  kHostCamera = 1u << 1,  // `camera*` are filled in
  // Spyro's character is being put where Crash is, right now, in a level
  // that is running (not paused, not in a menu). While this is set the
  // controller's movement and buttons are Crash's, whichever window is in
  // front.
  kHostLinked = 1u << 2,
  // Spyro's ground is being sent for Crash to stand on (the ground mapping,
  // below), the two worlds laid over each other rigidly. With kHostLinked,
  // Crash steers by Spyro's camera, the one the player is looking through.
  // Without it (Spyro paused, or in a menu) Crash keeps the ground and
  // stands still: he hears no controller until Spyro is running again.
  kHostGround = 1u << 3,
};

// Spyro, sampled once a frame. Unreal's units (centimetres) and axes
// (x forward, y right, z up; rotations in degrees as pitch, yaw, roll).
struct HostState {
  uint32_t flags;
  uint32_t frame;
  uint32_t timeUs;
  uint32_t reserved;
  uint64_t world;       // which world this is; changes on every level load
  Vec3 position;        // the pawn's root component, in the world
  Vec3 rotation;        // pitch, yaw, roll
  Vec3 cameraPosition;
  Vec3 cameraRotation;
  float cameraFov;      // horizontal, degrees
  uint32_t reserved2;
};

// -------------------------------------------------------------- picture ----

// Crash's picture, for Spyro to mix into its own (docs/PHASE2.md). The
// pixels are not here: they are in a texture on the graphics card, which
// Direct3D lets a second process open by the handle given. Red, green and
// blue are the picture as Crash's game drew it; the fourth channel is how
// far from the camera what is drawn there is, in Unreal's units.
enum PictureFlags : uint32_t {
  kPictureDepth = 1u << 0,  // the fourth channel does hold distances
};

struct GuestPicture {
  uint64_t handle;     // 0 = there is none
  uint32_t width, height;
  uint32_t frame;      // counts the pictures drawn
  uint32_t flags;
  float farDistance;   // a distance at or beyond this is "nothing of Crash's here"
  uint32_t reserved;
};

// ---------------------------------------------------------------- block ----

struct SharedBlock {
  Header header;
  Slot<GuestState> guest;      // written by Crash only
  Slot<HostState> host;        // written by Spyro only
  Slot<GuestPicture> picture;  // written by Crash only
};

constexpr uint32_t kMappingSize = 64 * 1024;

// --------------------------------------------------------------- ground ----
//
// Spyro's collision near the player, as triangles, for Crash to stand on.
// Far too big for the block above, and written seldom, so it has a mapping
// of its own: a header and two pages. Spyro fills the page that is not the
// latest and then names it the latest; Crash copies the latest and checks,
// by the page's own sequence number, that it was not rewritten meanwhile.

constexpr wchar_t kGroundMappingName[] = L"Local\\CrashSpyro_ground_v1";
constexpr uint32_t kGroundMagic = 0x47505343;  // "CSPG"
constexpr uint32_t kGroundVersion = 1;
constexpr uint32_t kGroundMaxTriangles = 300000;
constexpr uint32_t kGroundNone = 0xFFFFFFFF;

struct GroundHeader {
  std::atomic<uint32_t> magic;
  uint32_t version;
  uint32_t size;
  std::atomic<uint32_t> latest;  // 0 or 1; kGroundNone until a page is whole
};

struct GroundPage {
  std::atomic<uint32_t> seq;  // odd while it is being written
  uint32_t generation;        // one more each time Spyro fills a page
  uint32_t triangles;
  uint32_t crashGeneration;   // GuestState::generation the join was made under
  uint64_t world;             // HostState::world it was read from
  // Where it was gathered around, in Unreal's world and units. The corners
  // that follow are measured from here.
  Vec3 centre;
  float reach;
  // The join. This point of Unreal's world ...
  Vec3 spyroAnchor;
  float scale;                // Unreal units per Crash unit
  // ... is this point of Crash's, and Crash's y runs the other way:
  //   crash = crashAnchor + (dx, -dy, dz) / scale,  d = spyro - spyroAnchor
  Vec3 crashAnchor;
  uint32_t reserved;
  // then `triangles` times nine floats: three corners, x y z each
};

constexpr uint32_t kGroundPagesAt = 4096;
constexpr uint32_t kGroundPageBytes = (static_cast<uint32_t>(sizeof(GroundPage)) + kGroundMaxTriangles * 36 + 4095) & ~4095u;
constexpr uint32_t kGroundMappingSize = kGroundPagesAt + 2 * kGroundPageBytes;

// Spyro's point `s` (Unreal's world) in Crash's world, by a page's join.
inline Vec3 GroundToCrash(const GroundPage& page, const Vec3& s) {
  return {page.crashAnchor.x + (s.x - page.spyroAnchor.x) / page.scale, page.crashAnchor.y - (s.y - page.spyroAnchor.y) / page.scale,
          page.crashAnchor.z + (s.z - page.spyroAnchor.z) / page.scale};
}

static_assert(sizeof(Vec3) == 12, "Vec3 is three packed floats");
static_assert(sizeof(PeerInfo) == 16, "PeerInfo layout");
static_assert(sizeof(Header) == 48, "Header layout");
static_assert(sizeof(GuestState) == 304, "GuestState layout");
static_assert(sizeof(HostState) == 80, "HostState layout");
static_assert(sizeof(GuestPicture) == 32, "GuestPicture layout");
static_assert(sizeof(SharedBlock) <= kMappingSize, "block must fit the mapping");
static_assert(sizeof(GroundHeader) == 16, "GroundHeader layout");
static_assert(sizeof(GroundPage) == 72, "GroundPage layout");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "atomics must be plain words to share them");

}  // namespace csp
