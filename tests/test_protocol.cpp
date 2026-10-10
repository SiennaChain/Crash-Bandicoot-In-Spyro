// Tests of the shared block's own logic. They say nothing about either game:
// a passing run here is not evidence that anything works in one.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "csp_protocol.h"

using namespace csp;

static int g_failed = 0;
#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
      ++g_failed;                                                           \
    }                                                                       \
  } while (0)

static void Heartbeat() {
  CHECK(!HeartbeatAlive(1000, 0, 1000));  // never beat
  CHECK(HeartbeatAlive(1000, 1, 1000));
  CHECK(HeartbeatAlive(1000, 1, 2999));
  CHECK(!HeartbeatAlive(1000, 1, 3000));
  CHECK(HeartbeatAlive(0xFFFFFF00u, 5, 0x00000010u));  // across the wrap
}

static void Elapsed() {
  CHECK(ElapsedUs(100, 350) == 250);
  CHECK(ElapsedUs(350, 100) == -250);
  CHECK(ElapsedUs(0xFFFFFFF0u, 0x10u) == 0x20);  // across the wrap
}

static void SlotBasics() {
  static Slot<GuestState> slot;  // static: zeroed, as a fresh mapping is
  GuestState out{};
  CHECK(!slot.Read(out));  // nothing written yet

  GuestState in{};
  in.frame = 7;
  in.position = {1.0f, 2.0f, 3.0f};
  std::strcpy(in.level, "crash3/l301_toadvillage/l301_toadvillage");
  slot.Write(in);
  CHECK(slot.Read(out));
  CHECK(out.frame == 7 && out.position.y == 2.0f);
  CHECK(std::strcmp(out.level, in.level) == 0);
}

// A reader must never see half of one value and half of the next. Every
// field of a written value carries the same number, so a torn read shows.
static void SlotUnderLoad() {
  struct Wide {
    uint32_t words[64];
  };
  static Slot<Wide> slot;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    Wide w;
    for (uint32_t n = 1; !stop.load(); ++n) {
      for (uint32_t& word : w.words) word = n;
      slot.Write(w);
      // A game writes once a frame. A writer that never paused at all would
      // leave a reader no gap to read in, which is how this kind of slot
      // behaves and not what is being tested.
      for (volatile int spin = 0; spin < 400; spin = spin + 1) {
      }
    }
  });

  // A thread can take milliseconds to start; reading before it has written
  // anything would be two million quick "nothing yet"s and no test at all.
  while (slot.seq.load() == 0) std::this_thread::yield();

  int torn = 0, read = 0;
  for (int i = 0; i < 2000000; ++i) {
    Wide w;
    if (!slot.Read(w)) continue;
    ++read;
    for (uint32_t word : w.words) {
      if (word != w.words[0]) {
        ++torn;
        break;
      }
    }
  }
  stop.store(true);
  writer.join();
  CHECK(read > 0);
  CHECK(torn == 0);
}

int main() {
  Heartbeat();
  Elapsed();
  SlotBasics();
  SlotUnderLoad();
  if (g_failed) {
    std::printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  std::printf("protocol v%u: all checks passed (block %zu bytes, guest %zu, host %zu)\n", kVersion,
              sizeof(SharedBlock), sizeof(GuestState), sizeof(HostState));
  return 0;
}
