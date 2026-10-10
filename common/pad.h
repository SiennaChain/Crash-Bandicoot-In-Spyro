// The controller, as each game hears it.
//
// A bridge gets into its game by standing in for the XInput DLL the game
// loads from its own folder (xinput9_1_0.dll for Crash, xinput1_3.dll for
// Spyro; common/proxy_xinput.cpp). That is also the one place every reading
// of the controller passes through, in both games, whichever window is in
// front: XInput does not care about focus. One controller, two games both
// listening: who hears what is decided here.
#pragma once

#include <cstdint>

namespace csp::pad {

// XINPUT_STATE, without pulling in the header that would clash with the
// functions the proxy exports under the same names.
struct Gamepad {
  uint16_t buttons;
  uint8_t leftTrigger, rightTrigger;
  int16_t leftX, leftY, rightX, rightY;
};
struct State {
  uint32_t packet;
  Gamepad gamepad;
};

// Called for every reading the game takes, after the real controller has
// been read, on whichever of the game's threads asked. `result` is what
// XInputGetState returned (0 = a controller is there). May change `state`,
// and returns what the game is to be told: `result`, or 0 to have it believe
// a controller is there when none is.
using Filter = uint32_t (*)(uint32_t index, uint32_t result, State* state);
void SetFilter(Filter filter);

// XINPUT_GAMEPAD_*: the two buttons that are a game's own (its pause and its
// map or menu), as against the ones a character is played with.
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kA = 0x1000;

// Makes a reading say nothing is pressed or pushed, apart from the buttons
// in `keep` and, if `keepRightStick`, the right stick.
inline void Quiet(State* state, uint16_t keep = 0, bool keepRightStick = false) {
  state->gamepad.buttons &= keep;
  state->gamepad.leftTrigger = state->gamepad.rightTrigger = 0;
  state->gamepad.leftX = state->gamepad.leftY = 0;
  if (!keepRightStick) state->gamepad.rightX = state->gamepad.rightY = 0;
}

// Points the game's own calls to XInput straight at the bridge's code, past
// the exported functions they were made to go through. Steam's overlay
// rewrites the start of anything exported as XInputGetState by a DLL called
// xinput1_3: with that in place the game's calls go to Steam and never reach
// the code behind the export, so nothing the bridge does there (the filter,
// the stand-in controller) happens. Returns how many of the game's calls
// were redirected (2 when it works). Safe to call again.
int TakeBack();

// Whether the window in front belongs to this game. A game reads keys only
// while it is in front; a controller it reads always, unless told not to.
bool GameInFront();

}  // namespace csp::pad
