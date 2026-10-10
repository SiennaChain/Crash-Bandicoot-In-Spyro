// Putting a routine of ours in front of one of the game's, by address.
// MinHook does the work (third_party/minhook); this only keeps its set-up in
// one place and its failures in the log.
#pragma once

namespace csp::hook {

// Makes every call to `target` arrive at `detour` instead. `*original` is
// then how the detour (or anyone) still calls the game's own routine. The
// detour must take and return exactly what the game's routine does. False,
// with the reason logged under `what`, if it could not be done; the game's
// code is then untouched.
bool Install(void* target, void* detour, void** original, const char* what);

// Puts the game's code back as it was. The detour may still be running on
// another thread when this returns; do not unload the DLL on the strength of
// it.
void Remove(void* target);

}  // namespace csp::hook
