// Crash's picture, mixed into Spyro's (docs/PHASE2.md): just before each of
// Spyro's frames is shown, what Crash's game drew of Crash is drawn over it.
#pragma once

#include "shm.h"

namespace csp::picture {

// The camera the frame about to be shown was drawn through, and where the
// pawn's feet were in that frame (`haveFeet` false if it is not following).
// False if the camera cannot be told.
using CameraFn = bool (*)(Vec3& position, Vec3& rotation, float& fov, Vec3& feet, bool& haveFeet);

// From the bridge's own thread, now and then, until it says true. `camera`
// is asked from the thread that shows frames.
bool Install(SharedMapping* link, CameraFn camera);

// Whether to draw it at all (only while Crash is on Spyro's ground), and
// whether to draw the whole of Crash's picture small in a corner as well,
// for looking at what is arriving.
void Show(bool crash, bool corner);

// [picture] near, bias: the near plane Spyro's engine draws with (its depth
// is near / distance), and how much nearer than Spyro's scene Crash has to
// be before he is hidden by it, both in Unreal's units.
void Depth(float nearPlane, float bias);

// [picture] shadow, steady, size: how dark the shadow under Crash is (0 =
// none, 1 = black); whether his picture is moved to where the camera of the
// frame being shown would see him rather than drawn where it was drawn; and
// how big he is drawn, about his feet (1 = as Crash's game drew him). Size
// changes only how he looks: how fast he runs, how high he jumps and what
// stops him stay as they are.
void Looks(float shadow, bool steady, float size);

}  // namespace csp::picture
