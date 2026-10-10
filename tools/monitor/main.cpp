// Prints what the two bridges are saying to each other, from outside both
// games. For checking the link without reading two log files:
//
//   csp_monitor            one line a second until Ctrl+C
//   csp_monitor 10         for ten seconds
//
// It only opens the shared block; it never creates it, so it cannot make a
// bridge think the other game is there.
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "csp_protocol.h"

using namespace csp;

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 0;

  const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
  if (!mapping) {
    std::printf("no shared block: neither bridge is running\n");
    return 1;
  }
  const auto* block = static_cast<const SharedBlock*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kMappingSize));
  if (!block) {
    std::printf("could not map the shared block: %lu\n", GetLastError());
    return 1;
  }
  if (block->header.magic.load() != kMagic || block->header.version != kVersion) {
    std::printf("shared block is protocol v%u, this monitor is v%u\n", block->header.version, kVersion);
    return 1;
  }

  uint32_t lastFrames[kSideCount] = {};
  for (int tick = 0; seconds == 0 || tick <= seconds; ++tick) {
    const DWORD now = GetTickCount();
    bool alive[kSideCount];
    uint32_t rate[kSideCount];
    for (int side = 0; side < kSideCount; ++side) {
      const PeerInfo& peer = block->header.peers[side];
      alive[side] = peer.pid.load() != 0 && HeartbeatAlive(peer.heartbeatMs.load(), peer.beats.load(), now);
      const uint32_t frames = peer.frames.load();
      rate[side] = frames - lastFrames[side];
      lastFrames[side] = frames;
    }

    HostState h{};
    GuestState g{};
    const bool host = block->host.Read(h), guest = block->guest.Read(g);
    if (tick > 0) {  // the first pass has no rate to show yet
      std::printf("SPYRO %s %3u/s", alive[kSideSpyro] ? "up  " : "down", rate[kSideSpyro]);
      if (host) {
        std::printf("  pawn %c (%10.1f %10.1f %8.1f) yaw %6.1f  cam %c yaw %6.1f fov %4.1f", (h.flags & kHostPawn) ? 'y' : 'n',
                    h.position.x, h.position.y, h.position.z, h.rotation.y, (h.flags & kHostCamera) ? 'y' : 'n',
                    h.cameraRotation.y, h.cameraFov);
      }
      std::printf("\nCRASH %s %3u/s", alive[kSideCrash] ? "up  " : "down", rate[kSideCrash]);
      if (guest) {
        std::printf("  player %c gen %u (%9.2f %9.2f %8.2f)%s%s  cam %c (%.1f %.1f %.1f)  ticks %u  %s", (g.flags & kGuestPlayer) ? 'y' : 'n',
                    g.generation, g.position.x, g.position.y, g.position.z, (g.flags & kGuestParented) ? " riding" : "",
                    (g.flags & kGuestPaused) ? " paused" : "", (g.flags & kGuestCamera) ? 'y' : 'n', g.cameraPosition.x,
                    g.cameraPosition.y, g.cameraPosition.z, g.simTicks, (g.flags & kGuestLevel) ? g.level : "");
      }
      std::printf("\n");
      std::fflush(stdout);
    }
    if (seconds == 0 || tick < seconds) Sleep(1000);
  }
  return 0;
}
