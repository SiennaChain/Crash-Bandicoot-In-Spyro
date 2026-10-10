// Writes a line to the bridge log when the game hits a fatal exception: what
// kind, where (module + offset), and for an access violation which address
// it touched. The bridge changes game memory, so when a game dies we need to
// know whether we did it, not guess.
//
// It only observes. The exception carries on to whoever handles it.
#pragma once

namespace csp::crashlog {

void Install();

}  // namespace csp::crashlog
