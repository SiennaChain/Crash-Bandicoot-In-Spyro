// Crash's picture, mixed into Spyro's (docs/PHASE2.md): just before each of
// Spyro's frames is shown, what Crash's game drew of Crash is drawn over it.
#pragma once

#include "shm.h"

namespace csp::picture {

// From the bridge's own thread, now and then, until it says true.
bool Install(SharedMapping* link);

// Whether to draw it at all (only while Crash is on Spyro's ground), and
// whether to draw the whole of Crash's picture small in a corner as well,
// for looking at what is arriving.
void Show(bool crash, bool corner);

// [picture] near, bias: the near plane Spyro's engine draws with (its depth
// is near / distance), and how much nearer than Spyro's scene Crash has to
// be before he is hidden by it, both in Unreal's units.
void Depth(float nearPlane, float bias);

}  // namespace csp::picture
