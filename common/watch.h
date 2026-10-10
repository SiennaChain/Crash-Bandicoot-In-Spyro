// A tool for finding things out, not part of what the mod does when played:
// which of the game's code writes to a given address.
//
// The processor can be told to stop whenever an address is written (a data
// breakpoint). A stop arrives as an exception on the thread that did the
// writing, just after the instruction that did it; the bridge notes where
// that was and lets the thread carry on.
#pragma once

#include <cstdint>

namespace csp::watch {

// Watches `size` bytes (1, 2, 4 or 8) at `address` for writes, on every
// thread the game has now, for `seconds`. Then stops watching and logs each
// place a write came from, the most frequent first, with what called it.
// Takes that long to return: call it from the bridge's own thread.
void Writers(uintptr_t address, int size, int seconds);

}  // namespace csp::watch
