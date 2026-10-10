# Phase 1: ground

Goal (from `docs/DESIGN.md`): Spyro's collision reaches Crash's movement;
Crash's own level stops mattering; Crash steers by Spyro's camera. Done when,
from Spyro's window, Crash runs on Spyro's ground, is stopped by Spyro's
wall, and jumps onto and falls off Spyro's ledge, all with Crash's physics.

**Where it stands (2026-10-10):** the bridge can put bodies of its own into
Crash's physics world and Crash is stopped by them (the fence, below). Not
yet: a shape that can carry Spyro's triangles, anything from Spyro's side,
and taking Crash's own level away.

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

## Making a body

Read from the code that makes Crash's own character body (found by what
refers to its vtable: `tools\probe\xrefs.py`, then `tools\probe\disasm.ps1`)
and from one of the game's own calls.

| What | Where | Notes |
|---|---|---|
| The physics world | `[[exe+0x1A918C0] + 0x38]` | the game's physics system object, then its `hknpWorld` |
| The world's bodies | world `+0x20` | one array, 0x90 bytes a body, numbered from 0 |
| `hknpBodyCinfo`'s constructor | `exe+0x10D6B30`, starts `48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 83 c1` | fills in a blank description of a body |
| `hknpWorld::createBody` | `exe+0x10E5CA0`, starts `44 89 4c 24 20 4c 89 44 24 18 53 55 56 57 41 54 41 55 41 56` | `(world, out number, description, mode, flags)` |
| `hknpWorld::addBodies` | `exe+0x10E5EA0` | `(world, numbers, how many, mode, flags)`; what `createBody` calls when its mode is not 0 |

The description (`hknpBodyCinfo`), as far as it is used here: shape at
`+0x00`, motion `+0x0C` (0 for a body that does not move), quality `+0x10`,
material `+0x12`, collision filter `+0x14`, flags `+0x18`, position `+0x30`,
orientation `+0x40`.

A body in the array (`hknpBody`): transform at `+0x00` with the position at
`+0x30`, flags `+0x40`, collision filter `+0x44`, shape `+0x48`, its box in
the world's search structure `+0x50`, motion `+0x68`, material `+0x70`,
quality `+0x72`, and at `+0x88` the game's own word for it: for Crash, his
actor's address with the low bit set, which is how the bridge finds his body
without assuming it is number 1.

**The mode matters.** 0 makes the body and leaves it out of the world's
search structure: it exists, has flag 0x400 and an empty box, and nothing
touches it. That is what the game does for things not yet switched on, and
what the first try here did: nine bodies made, and Crash ran straight
through where they were. 1 puts the body in at once. 2 has the world put it
in at its next step, which is what the bridge uses, since it is called
between frames and cannot know what the world is in the middle of.

**The filter matters.** A still body Crash is stopped by is marked
`0x017F0803`; the walkable mesh of Toad Village, of Orient Express and of
the hub all are. (His own body is `0x01FD2038`. `0x017D1041`, tried first
because the first mesh found in the hub had it, is something else.) What
the bits mean is not known.

## The fence

`[debug] fence = <anything>` in Crash's ini: each change of the text has
the bridge, at the end of the game's next frame, put nine still copies of
Crash's own capsule shape in a row 1.5 m ahead of him as his camera sees it,
0.45 m apart. His own shape, so that nothing has to be built; a row, so that
he cannot slip between. `fence_mode` and `fence_filter` choose the two
things above.

Seen 2026-10-10 in the Crash 3 hub, the bridge holding his stick forward
for 0.7 s each time and nobody at the controller:

| | Went ahead | Went sideways |
|---|---|---|
| Nothing in the way | 306 units | 26 |
| Fence, mode 0 (not in the world) | 295 | 0 |
| Fence, mode 2 | **9** | **69** |

With the fence in the world he is stopped within nine units and slides
along it, as he would along a wall; pushed again he comes off its end and
runs on. The game did not crash, in either try.

So the mechanism Phase 1 rests on works: a body the bridge makes is a body
Crash's physics respects.

What it does not show: that he can *stand* on one (the fence is beside him,
not under him), that a body can be taken out again (nothing is, yet: these
last until the level is left), or how many the world has room for.

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

1. ~~Crash side: how a body gets into the world.~~ Done: "Making a body".
2. Crash side: taking a body out again (`hknpWorld`'s destroy routine), and
   how many bodies the world has room for.
3. Crash side: the cheapest shape that can carry Spyro's triangles. In order
   of preference: one `hknpExternMeshShape` (if what builds its tree is in
   the exe), many `hknpTriangleShape` or `hknpConvexPolytopeShape` bodies,
   or the level's own compressed mesh rewritten in place.
4. Prove standing: one flat piece under Crash, somewhere his own level has
   nothing, and see him stand on it. (Stopped by a body beside him: done,
   "The fence".)
5. Spyro side: read the shapes near the pawn out of PhysX, on the game's
   thread, with the origin.
6. Join them: Spyro's shapes, scaled by two (the inverse of 0.5) and then
   into metres, mirrored for handedness, into Crash's world around where he
   is; his own level's bodies taken out or moved away.
7. Steer Crash by Spyro's camera: write Spyro's view into Crash's camera
   object, which is also what Phase 2 draws from. Until then the follow's
   camera-relative translation stands in.
