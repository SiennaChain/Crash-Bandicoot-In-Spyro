// The controller, as each game really hears it.
//
// Both games ask Steam for its own controller interface ("SteamController006",
// what Steam calls Steam Input) and read the player's controller through
// that. While Steam is handing a controller to a game that way it shows the
// game nothing through XInput, so the XInput stand-in (pad.h) never sees the
// real sticks at all: only what the bridge itself presses.
//
// This gets between a game and that interface, by replacing the two methods
// the game reads buttons and sticks with, so that a bridge can have its game
// told "nothing pressed, sticks centred" whenever the controller is not that
// game's to hear.
#pragma once

#include <cstdint>

namespace csp::steampad {

// Asked for every reading Steam is about to give the game, on whichever of
// the game's threads asked. `analog`: a stick or trigger, otherwise a button.
// `action`: Steam's number for the thing the game asked about (a game asks by
// what it is for, "jump", not by which button). Return true to have the game
// told it is not pressed, or centred.
using Policy = bool (*)(bool analog, uint64_t action);

// False until Steam is up in this process, which is some way into a game's
// start; call it again until it is true. Once true, `policy` is in force.
bool Install(Policy policy);

// One line for the log: the sticks and triggers the game has asked Steam
// about so far, each with the largest values seen. For finding out which
// number is which. Empty if there are none yet.
void DescribeAnalog(char* out, int size);

}  // namespace csp::steampad
