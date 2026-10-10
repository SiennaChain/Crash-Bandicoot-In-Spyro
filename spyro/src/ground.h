// Spyro's collision, read out of the engine's physics (PhysX 3.4) as plain
// triangles: what Crash is to stand on (docs/PHASE1.md).
//
// Everything here is asked of PhysX through its own interfaces, on the
// game's thread, under the scene's read lock. Nothing is changed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "csp_protocol.h"

namespace ground {

struct Options {
  float reach = 6000.0f;   // how far out from the centre, along x and along y
  float up = 4000.0f;      // how far above it
  float down = 4000.0f;    // and below
  uint32_t maxTriangles = csp::kGroundMaxTriangles;
  // Unreal's collision channel the shapes must block (2 is Pawn), and
  // whether to take the shapes meant for simple collision (what the
  // engine's own characters are moved against) or the complex ones.
  uint32_t channel = 2;
  bool complex = false;
};

struct Stats {
  int scenes = 0;
  int touched = 0;  // shapes whose bounds reach into the box
  int used = 0;
  int meshes = 0, convexes = 0, boxes = 0, spheres = 0, capsules = 0, heightFields = 0, others = 0;
  int notSolid = 0;  // left out: does not block the channel, or is the other kind of collision
  // Left out: blocks pawns, but the player's own pawn does not block its
  // kind, and the engine goes by whichever of the two is the more lenient
  // (so the pawn passes through: pickups).
  int passedThrough = 0;
  bool pawnFound = false;    // the player's own capsule was among the shapes
  uint32_t pawnBlocks = 0;   // and this is the bit for each channel it blocks
  int characters = 0;  // left out: is itself a pawn
  uint32_t triangles = 0;
  bool full = false;  // stopped at maxTriangles
  std::string kinds;  // the commonest kinds of shape touched, for the log
};

// Appends to `out` nine floats a triangle: three corners, each as how far
// it is from `centre` in Unreal's world. False if the physics is not there.
bool Gather(const csp::Vec3& centre, const Options& options, std::vector<float>& out, Stats& stats);

// The highest point of `triangles` (as Gather gives them) straight below
// (x, y, z), those being measured from the same centre. False if none.
bool Below(const std::vector<float>& triangles, float x, float y, float z, float& height);

}  // namespace ground
