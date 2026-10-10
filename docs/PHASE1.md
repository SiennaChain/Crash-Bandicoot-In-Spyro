# Phase 1: ground

Goal (from `docs/DESIGN.md`): Spyro's collision reaches Crash's movement;
Crash's own level stops mattering; Crash steers by Spyro's camera. Done when,
from Spyro's window, Crash runs on Spyro's ground, is stopped by Spyro's
wall, and jumps onto and falls off Spyro's ledge, all with Crash's physics.

**Where it stands (2026-10-10):** reconnaissance only. Nothing below changes
either game; all of it was read from the running game from outside.

## What is known about Crash's side

Crash's physics is Havok `hk_2014.1.0-r1`, the newer kind (classes beginning
`hknp`). Havok's classes carry the compiler's type information, so their
vtables can be found by name (`tools\probe\vtables.py`) and objects of them
found in the running game (`tools\probe\findobjects.ps1`, then
`tools\probe\peek.ps1` to look inside one). The game's own classes were
built without it.

| Class | vtable |
|---|---|
| `hknpWorld` | `exe+0x1854058` |
| `hknpCharacterRigidBody` | `exe+0x1856538` |
| `hknpCharacterProxy` | `exe+0x1856DB8` |
| `hknpCompressedMeshShape` | `exe+0x18579E8` |
| `hknpExternMeshShape` | `exe+0x1857168` |
| `hknpStaticCompoundShape` | `exe+0x1856258` |
| `hknpCapsuleShape` | `exe+0x18559E8` |
| `hknpSphereShape` | `exe+0x1855C18` |
| `hknpConvexPolytopeShape` | `exe+0x1853BF8` |
| `hknpTriangleShape` | `exe+0x1853CF0` |

**Crash is a Havok character rigid body.** In Toad Village there is one
`hknpWorld`, one `hknpCharacterRigidBody` and no `hknpCharacterProxy`. The
character body object holds:

| Offset | What |
|---|---|
| `+0x10` | its body's number in the world (1) |
| `+0x18` | its shape, an `hknpCapsuleShape` |
| `+0x20` | the `hknpWorld` |
| `+0x28` | an object of the game's (vtable `exe+0x13E4FA0`), presumably what Havok calls back |
| `+0x38` | up: (0, 0, 1) |

The body itself is found from the shape: the one object on the heap that
points at that capsule and is not the character body. Bodies are 0x90 bytes
each in one array (other bodies seen were exact multiples of 0x90 away), a
body's shape is at `+0x48`, and its transform is at `+0x00`: three columns
of rotation, then the position at `+0x30`.

**That body is Crash, and Havok works in metres while the game works in
inches.** With Crash's actor at (603.83, 8335.76, 428.56) the body was at
(15.3372, 211.728, 10.8854): each is the actor's number divided by 39.37, to
four figures. So one unit of Crash's is an inch as far as his physics is
concerned, whatever it is worth in Spyro's world (0.5 of Spyro's units,
`docs/PHASE0.md`).

**He is collided with the level by Havok, against shapes in that world.**
Copies of his capsule's address on the game's worker-thread stacks sit in
records beside the player actor's class (`exe+0x1592220`) and beside
`hknpCompressedMeshShape`, `hknpStaticCompoundShape` and `hknpTriangleShape`
objects. So the level he stands on is ordinary bodies with mesh shapes in the
same world. That is the easier of the two cases this could have been: if
Spyro's ground is put into that world as bodies of its own, Crash should
stand on it, with no query of his to find and intercept.

**Corrected:** an earlier note (in `docs/PHASE0.md`, since removed) said the
game "already makes an `hknpExternMeshShape`". It does not. The one hit for
that class is an entry in a table of vtables beside the world (the next
things in it are other classes' vtables), not a shape; the same goes for one
of the two capsule "objects". The class's code is in the exe, since its
vtable is; whether the code that *builds* one is, is not known.

The game's own classes around the player's movement, by name only:
`CBehaviorPhysicsCapsuleComponent`, `CBehaviorPhysicsPencilComponent`,
`CBasePhysicsComponent`, `CBaseMovementController`,
`CGroundLocomotionHandler`, `CAirLocomotionBehaviorData`,
`CIceLocomotionHandler`, `CDetectGroundHandler`, `CClampToGroundComponent`,
`CDisableLevelBorderCollisionHandler`.

## What is known about Spyro's side

Spyro's collision is PhysX 3, in DLLs beside the game
(`Engine\Binaries\ThirdParty\PhysX3\Win64\VS2015\PhysX3_x64.dll` and
friends) that export their functions by name, and the matching headers are
in the installed engine source
(`Engine\Source\ThirdParty\PhysX3\PhysX_3.4\Include`). Nothing has been read
out of it yet.

The engine moves the world's origin about: the same spot in Sunny Villa has
read (-299705, -297964, 266) on one load and (295, -297964, 266) on another.
Whatever carries Spyro's shapes across has to carry the origin they were
read under.

## To do, in order

1. Crash side: how a body gets into the world. Find `hknpWorld`'s routine
   for making a body and what a body needs filled in (shape, position,
   "static", collision filter), by reading how the level's own static bodies
   were made.
2. Crash side: the cheapest shape that can carry Spyro's triangles. In order
   of preference: one `hknpExternMeshShape` (if what builds its tree is in
   the exe), many `hknpTriangleShape` or `hknpConvexPolytopeShape` bodies,
   or the level's own compressed mesh rewritten in place.
3. Prove it small: one flat square under Crash, somewhere his own level has
   nothing, and see him stand on it.
4. Spyro side: read the shapes near the pawn out of PhysX, on the game's
   thread, with the origin.
5. Join them: Spyro's shapes, scaled by two (the inverse of 0.5) and then
   into metres, mirrored for handedness, into Crash's world around where he
   is; his own level's bodies taken out or moved away.
6. Steer Crash by Spyro's camera: write Spyro's view into Crash's camera
   object, which is also what Phase 2 draws from. Until then the follow's
   camera-relative translation stands in.
