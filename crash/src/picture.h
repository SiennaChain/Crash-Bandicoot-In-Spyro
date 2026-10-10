// Crash's picture, sent to Spyro (docs/PHASE2.md): each frame, what Crash's
// game drew and how far away each part of it is, in one texture that
// Spyro's process can open.
#pragma once

#include "shm.h"

namespace csp::picture {

// From the bridge's own thread, now and then, until it says true: Direct3D
// is not there to be hooked until the game has started drawing.
bool Install(SharedMapping* link);

// From the game's thread, each frame: the projection its camera has
// (sixteen floats, as the game keeps it), and how many of Unreal's units
// one of Crash's is worth, for the distances.
void SetCamera(const float projection[16], float unrealPerUnit);

// [picture] send. Off: no picture is drawn and Spyro is told there is none.
void Enable(bool on);

// [picture] depth_pick: which of the depth buffers of the picture's size
// the game clears in a frame is the scene's, counting from 0 in the order
// it clears them.
void PickDepth(int which);

}  // namespace csp::picture
