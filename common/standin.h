// A controller the bridge plays itself, for getting through a game's menus
// with nobody at them. It reaches the game through the XInput stand-in
// (pad.h), which the real controller does not (steampad.h says why).
#pragma once

#include <cstdint>
#include <string>

#include "config.h"
#include "pad.h"

namespace csp::standin {

// While on, the game is told a controller is connected even when nothing is
// being held. A game that is told there is none may stop asking.
void Present(bool on);

// Holds `what` for `ms`, then lets go, without waiting for it. `what` is one
// or more of UP DOWN LEFT RIGHT START BACK LB RB A B X Y, LS_UP LS_DOWN
// LS_LEFT LS_RIGHT and RS_ the same for a stick pushed right over, and
// LS:x,y or RS:x,y for one held anywhere (each from -1 to 1, right and up),
// joined by '+'. False if it is not understood.
bool Hold(const std::string& what, uint32_t ms);

// For a bridge's pad filter. If the stand-in is in use on this controller
// number, `state` becomes what it is holding and the answer is true: the
// game is to be told a controller is there (return 0 to it).
bool Fill(uint32_t index, pad::State* state);

// [debug] press = <what> [ms] [anything]: a Hold each time the line's text
// changes, so a counter on the end repeats a press. Call once at the start
// (which only notes the text as it is) and after every re-read of the ini.
void DebugPress(const Config& config);

}  // namespace csp::standin
