# Phase 1: ground

Goal (from `docs/DESIGN.md`): Spyro's collision reaches Crash's movement;
Crash's own level stops mattering; Crash steers by Spyro's camera. Done when,
from Spyro's window, Crash runs on Spyro's ground, is stopped by Spyro's
wall, and jumps onto and falls off Spyro's ledge, all with Crash's physics.

**Where it stands (2026-10-10):** the two are joined. With following on,
Spyro's collision around its pawn is read out of the engine's physics and
sent to Crash, where it becomes one body he stands on; the hub's own ground
is taken out from under him meanwhile; and his stick is turned to Spyro's
camera. Walked about by the bridge's own controller in Sunrise Spring, he
stayed on Spyro's ground everywhere he went ("The join", below).

Not yet: a person playing it (walls, ledges and jumps by hand); the things
in Crash's own level that are not ground (its triggers and doors are still
there, unseen); ground that moves; height fields.

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

## Shapes of the bridge's own

The fence borrowed Crash's own capsule. Ground needs shapes the bridge
makes itself, and there are two.

### A box

An `hknpConvexPolytopeShape` is one block of memory with its arrays inside
it, each array given as a count and how far on from that field it starts.
The bridge fills in a block of its own (`MakeBoxShape`); nothing is copied
from the game. As read from a box of the game's in the hub (0x190 bytes):

| Offset | What |
|---|---|
| `+0x00` | vtable (`exe+0x1853BF8`) |
| `+0x08` | references held (16 bits), then the block's size (16 bits). **Size 0 means Havok neither counts references nor frees it**, which is what the bridge wants for memory that is its own |
| `+0x10` | flags `0x01C3`; `+0x12` bits of shape key (0); `+0x13` kind (1, convex) |
| `+0x14` | convex radius, 0.01 m |
| `+0x20` | properties (mass and so on); none, for a body that does not move |
| `+0x30` | corners: count 8, at `+0x50` |
| `+0x40` | planes: count 8 (six, padded), at `+0xD0` |
| `+0x44` | sides: count 6, at `+0x150` |
| `+0x48` | which corners each side has: count 24, at `+0x170` |
| `+0x50` | corners, (x, y, z, w), w being 0.5 with the corner's number in its lowest bits |
| `+0xD0` | planes, (normal, distance) with normal . point + distance = 0 on the side |
| `+0x150` | sides: first index (16 bits), how many (8 bits), `0x80` |
| `+0x170` | corner numbers, a byte each, anticlockwise seen from outside |

### A mesh

**Havok's `hknpExternMeshShape` is in the exe whole, tree builder and all.**
It is the shape for triangles kept somewhere else: the shape holds a search
tree and asks a "mesh" object for a triangle's corners when it needs them.
So one body can carry any number of Spyro's triangles, with the bridge
answering.

| What | Where | Notes |
|---|---|---|
| Constructor that builds the tree | `exe+0x1119C10`, starts `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 30 0f 29 74 24 20 41 0f b6 d8` | `(shape, mesh, also build the fast tree, tolerance)`. The shape is 0x70 bytes. Nothing in the game calls it; Havok's code is simply all linked in |
| Constructor from a ready-made tree | `exe+0x1119CB0` | `(shape, mesh, tree)`; one caller, in Havok |
| The tree builder | `exe+0x1280940` | `(mesh, fast tree, tolerance, list for the triangles left out)`; asks the mesh for every triangle once |
| Destructor | `exe+0x1119D40`, or vtable slot 0 with flags 0 | lets go of the tree |
| In the shape | `+0x60` the mesh, `+0x68` the tree | |

The mesh is a reference-counted object (vtable, then references and size as
above) whose vtable the shape calls at:

| Slot | Question |
|---|---|
| `+0x20` | how many triangles |
| `+0x28` | `(index, out)`: the three corners of one, as three (x, y, z, w). **Three**, not four: one caller packs them 0x30 bytes apart |
| `+0x30` | `(index)`: the triangle's shape tag, 16 bits. `0xFFFF` is "none" |
| `+0x38` | whether to build a second structure (looked at while building; the bridge says no) |

The first four slots are every Havok object's (destructor, a blank, class,
delete-this) and are never called on an object of size 0.

Things that made this safe to try:

- The world's shape tag codec is Havok's `hknpNullShapeTagCodec` (world
  `+0x640`), so tags are ignored and every triangle goes by its body's
  filter and material.
- The shape asks for corners **from Havok's worker threads** while the world
  steps. A mesh is never changed once a shape is made from it; new ground
  is a new mesh, shape and body.
- With "fast tree" off, queries use the plain tree (`+0x10` in the tree
  object); on, a second tree (`+0x40`) is built and used instead. Only off
  has been tried.
- Triangles thinner than the tolerance (1e-7) are left out of the tree but
  keep their numbers.

### Taking a body out

`hknpWorld::destroyBodies` is at `exe+0x10E6800` (starts `48 89 54 24 10 53
57 41 55 41 56 41 57 48 83 ec 50 49 63 d8 48 8b fa 44`): `(world, numbers,
how many, 1)`, the last as all the game's own calls have it. It first calls
`removeBodies` (`exe+0x10E64C0`, same arguments), which takes them out of
the search structure, then frees the numbers for reuse.

The bridge keeps a list of what it has made, and gives a shape's memory
back 120 frames after its body is destroyed rather than at once, in case
anything from the step before still points at it.

**Room:** the world's body array holds 4,096 (`world +0x28`); the hub uses
159 of them. A whole mesh is one body.

## Standing: the box and the ramp

All in the Crash 3 hub on 2026-10-10, the bridge holding the stick, nobody
at the controller. Heights are in Crash's units (inches) above where he
started.

**The box** (`[debug] slab`): 20 m square, its top 0.5 m (19.7 units) above
the floor, starting 1 m ahead of him.

| | Ahead | Up |
|---|---|---|
| Stick forward 0.6 s | 226 | 20.1 |
| Jump forward | 468 | 20.1 |
| Two seconds later | 468 | 20.1 |

He stands on it, 20.1 up: the 0.5 m and the box's 0.01 m of rounding. He
did not need the jump: a step of half a metre he simply walks up.

**The ramp** (`[debug] mesh`): 96 triangles in one body, rising 1.2 m over
4 m from 1 m ahead of him, then flat for 4 m.

| | Ahead | Up | The ramp's height there |
|---|---|---|---|
| Stick forward 0.25 s | 62 | 7.9 | 6.8 |
| and 0.15 s more | 92 | 16.9 | 15.8 |
| Two seconds later | 92 | 16.9 | 15.8 |
| and 0.25 s more (on the flat top) | 170 | 40.3 | 39.1 |
| Two seconds later | 170 | 40.3 | 39.1 |
| Stick back 0.85 s | -154 | 0.0 | 0 |

He walks up it, stands still on the slope without sliding, stands on top,
and walks back down, a unit above the triangles throughout (his capsule's
rounding).

**Taken away** (`[debug] clear`): standing on the ramp at 30.6 up, the ramp
destroyed, he dropped to the hub's floor (5.6 up there). Done twice running,
the second ramp getting the first one's body number; the game carried on.

## Spyro's side: reading the ground

Spyro's collision is PhysX 3.4, in DLLs beside the game
(`Engine\Binaries\ThirdParty\PhysX3\Win64\VS2015\`). The game's
`PhysX3_x64.dll` is **byte for byte** the one in an installed Unreal Engine
4.19 (same SHA-1), so that engine's own PhysX headers describe it exactly.
The bridge is built against those headers and links nothing: it asks the
DLL for `PxGetPhysics` by name and from there everything is a call through
PhysX's own interfaces (`spyro/src/ground.cpp`).

Each time, on the game's thread, at the pawn's tick, under the scene's read
lock:

1. Every scene is asked which shapes reach into a box around the pawn
   (`PxScene::overlap`, no filter).
2. Each shape's query filter data says what Unreal made it for
   (`Engine/Public/Physics/PhysicsFiltering.h`): `word1` has a bit for each
   channel it blocks, `word3` has its own channel and whether it is simple
   or complex collision. Kept: shapes that **block the Pawn channel** and
   are **simple** collision, and are not themselves pawns. That is what the
   engine moves its own characters against.
3. Each kept shape becomes triangles in the world: a triangle mesh's own,
   a convex's sides as fans, a box as twelve, a ball or capsule coarsely.
   Only triangles reaching into the box are kept.

In Sunrise Spring, 60 m (6,000 units) each way from the pawn: 557 shapes
touched, 92 kept, **18,772 triangles, in 0.5 to 1 ms**. Nearly all of them
come from 4 meshes: the level has collision meshes made for the purpose,
marked simple and complex both, and its 193 detailed meshes are complex
only and are left out. Drawn from above, what comes out is plainly the
home world: the round middle, the towers, the pools.

`[debug] ground = <anything>` in Spyro's ini writes what would be sent to
`crashspyro_ground.bin` beside the exe (a count, the centre, nine floats a
triangle) and logs how far the ground is under the pawn's middle (24.2
units, standing).

Not handled: height fields (none seen), and anything that moves is sent as
where it was when read.

The engine moves the world's origin about: the same spot in Sunrise Spring
has read (-299705, -297964, 266) on one load and (295, -297964, 266) on
another. The ground is sent as distances from the point it was gathered
around, with that point, so each page stands by itself.

## The ground channel

Too big for the shared block and written seldom, the ground has a mapping
of its own, `Local\CrashSpyro_ground_v1` (`protocol/csp_protocol.h`): a
header and two pages of up to 300,000 triangles. Spyro fills the page that
is not the latest, then names it the latest; Crash copies the latest and
checks by the page's own sequence number that it was not rewritten
meanwhile.

A page carries **the join**: one point of Spyro's world and the point of
Crash's that it is, and the scale.

    crash = crashAnchor + (dx, -dy, dz) / scale,   d = spyro - spyroAnchor

Crash's y runs the other way (Unreal is left-handed, Crash's world is
not). The anchors are taken when following begins: Crash's position (his
feet) and the ground straight under Spyro's pawn. So the ground he was
standing on becomes the ground Spyro was standing on, and from then on the
pawn is put exactly where he is by the same rule. Scale is the measured 0.5
Unreal units per Crash unit (`docs/PHASE0.md`).

Spyro gathers again when the pawn has gone 0.4 of the reach from where it
last gathered, and Crash swaps the new ground in.

## Crash's side: taking it in

Once a frame on the game's thread (`TendGround`), while Spyro's state says
ground is being sent (`kHostGround`):

- A page newer than the one in the world is copied out, turned into
  Havok's terms (the join, then metres: Crash's units are inches; the
  corners of each triangle in the other order, since mirroring y turns it
  over), and made into one `hknpExternMeshShape` body at the join. 18,772
  triangles took **22.8 ms**, tree and all.
- The ground before it is destroyed four frames after the new one goes in.
- The level's own still bodies with mesh or compound shapes (three in the
  hub) are taken out of the world with `hknpWorld::removeBodies`
  (`exe+0x10E64C0`) and put back with `addBodies` (`exe+0x10E5EA0`,
  `(world, numbers, how many, 2, 0)`) when Spyro stops sending. Without
  this he would stand on whichever ground was higher. `[ground]
  hide_own_level = 0` leaves them in.

**Steering.** Crash goes where the stick points as *his* camera sees it,
and nobody is looking through his camera. Rather than move his camera, the
bridge turns the stick before he hears it, by the angle between his
camera's line of sight and Spyro's (Spyro's taken into Crash's world by the
same mirror). Pushing the stick away from the player then sends him away
from the player in the picture the player has. `[ground] steer_by_spyro =
0` turns it off. (Writing Spyro's view into Crash's camera is still wanted
for Phase 2, which draws from it.)

## The join, seen

2026-10-10, Spyro in Sunrise Spring, Crash in the Crash 3 hub, nobody at
the controller: following switched on from the ini, then Crash's stick held
by the bridge 0.4 s at a time, and after each move Spyro's bridge asked how
far the ground was under its pawn.

| | Crash moved (his units) | Pawn moved (Unreal's) | Ground under the pawn |
|---|---|---|---|
| Following on | | | 24.2 |
| Stick forward | (1, 49, -68) | (0, -77, -34) | 24.2 |
| Stick forward | (0, 193, -57) | (0, -149, -29) | 24.2 |
| Stick right | (145, 193, -132) | (72, -149, -66) | 24.2 |
| Stick back | (187, 129, -132) | (93, -117, -66) | 24.2 |
| Stick left | (74, 191, -132) | (37, -148, -66) | 24.1 |

He went down off a step and into a pool's basin, 66 units below where he
began, and the ground stayed 24.2 under the pawn's middle throughout: he
was standing on Spyro's ground, not on anything of his own (the hub is
flat). Spyro's camera was looking along -y: forward took the pawn along -y
and right along +x, as the player would see it. Following off: the hub's
three meshes went back, Spyro's ground came out, both games carried on.

The first step forward shows the pawn moving 77 for Crash's 49: Crash was
still walking in from the hub's entrance when following began, and the
table's first row is from just before.

## To do, in order

1. ~~Crash side: how a body gets into the world.~~ Done: "Making a body".
2. ~~Crash side: taking a body out again, and how many bodies the world has
   room for.~~ Done: "Taking a body out".
3. ~~Crash side: the cheapest shape that can carry Spyro's triangles.~~ Done:
   one `hknpExternMeshShape`, "A mesh".
4. ~~Prove standing.~~ Done: "Standing: the box and the ramp".
5. ~~Spyro side: read the shapes near the pawn out of PhysX.~~ Done:
   "Spyro's side: reading the ground".
6. ~~Join them.~~ Done: "The ground channel", "Crash's side: taking it in".
7. ~~Steer Crash by Spyro's camera.~~ Done by turning the stick: "Steering".
8. Played by hand: a wall, a ledge up and a ledge down, a jump across a gap,
   in more than one place. This is what "done" above asks for.
9. Crash's own level out of the way altogether, not just its ground: an
   empty level of his own to stand in (the N. Sane Trilogy level editor
   makes one), or its triggers taken out as its meshes are.
10. Ground that moves (Spyro's platforms and doors), and Spyro's water,
    which is not ground.
