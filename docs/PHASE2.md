# Phase 2: seeing him

Goal (from `docs/DESIGN.md`): Crash drawn by Crash's game from Spyro's
camera and mixed into Spyro's picture with depth. Spyro hidden. Crash's
window hidden; his sounds heard. Done when Crash runs behind a pillar in a
Spyro level and is hidden by it, and nobody would guess a second game is
running.

**Where it stands (2026-10-10):** Crash is in Spyro's picture. With
following on, Spyro's window shows Crash, drawn by Crash's game through
Spyro's camera, standing and running on Spyro's ground with Spyro's own
character not drawn, and with Spyro's grass in front of his feet where it
is nearer than they are.

Not yet: seen behind something as big as a pillar (the camera keeps him in
view; it wants playing); his shadow; Crash's window hidden; a person
playing it. And one thing to decide, below: **he is big.**

## Crash's camera

Crash's game works its camera out afresh every frame and then draws
through it, so a view written at any other time is overwritten before it
is used. Where it is worked out was found by asking the processor
(`common/watch.cpp`, below) what writes to the camera object:

| What | Where | Notes |
|---|---|---|
| The camera system's update | virtual slot `+0xB0` of the object at `[exe+0x1A890E0]`, called from the frame routine at `exe+0x16A27C` | skipped while paused |
| One camera's update | `exe+0x3775E0`, starts `48 8b c4 55 41 56 48 8d 68 a1 48 81 ec e8 00 00 00` | `(camera, seconds)`; called for each camera in turn |
| in it, the view | `exe+0xDC2CD0(camera + 0x180, ...)` | a look-at |
| in it, the projection | `exe+0xDC3290(camera + 0x1C0, ...)` | from the field of view at camera `+0xD0` |
| in it, the position | camera `+0x80`, written three times | |

The bridge hooks the one-camera update and, straight after the game's own
has run for the camera in use, writes its view, position and field of view
over the game's. The picture follows: with `[debug] camera_test = 1` the
camera swings round Crash, and the window shows it.

**The view matrix**, as read from the game: sixteen floats, a row for each
of the world's axes and one for the eye. Each of the first three rows is
(right, up, back) along that axis; the last is minus the eye measured along
right, up and back. The world is right-handed, z up; the camera looks along
minus its own z; right is "ahead cross world-up". The projection is the
usual one for that: `[0]` and `[5]` are 1 / tan(half the field of view)
across and down, and the game's own was 80 degrees across at 16:9, which
happens to be Spyro's too.

**Spyro's camera in Crash's world** (`[view] by_spyro`, on unless set to
0, and only while Spyro's ground is in): the eye is Spyro's camera position
taken through the ground's join (`GroundToCrash`); the line of sight is
Unreal's (cos pitch cos yaw, cos pitch sin yaw, sin pitch) with y the other
way; the field of view is Spyro's.

Seen, following on in Sunrise Spring, from the shared block
(each game's own character through its own camera, as fractions of the
screen from its middle):

| | across | up | depth |
|---|---|---|---|
| Crash on Crash's screen | +0.000 | -0.281 | 297.0 of his units = 148.5 of Unreal's |
| Spyro on Spyro's screen | +0.000 | -0.282 | 148.5 |

and by eye: the two windows side by side show the two characters the same
size at the same place in the frame.

Not looked at: whatever else the game derives from the camera in that
routine (what it leaves out as off-screen, for one). Nothing has gone
missing from the picture so far, but there is only Crash and a platform
in it.

## Crash's picture, out of Crash

Both games draw with Direct3D 11. `common/gfx.cpp` gets the bridge called
in each just before a frame is shown (`IDXGISwapChain::Present`) and each
time a depth buffer is cleared (`ID3D11DeviceContext::ClearDepthStencilView`),
the two routines being found in the tables of a throwaway device of the
bridge's own.

In Crash (`crash/src/picture.cpp`), at each Present:

- The back buffer is copied. It is 1024x768 for the window, and the game
  draws its 16:9 scene in the middle of it with bars above and below: the
  part that is the scene is worked out from the camera's projection.
- The scene's depth buffer is copied. The game clears five depth buffers a
  frame: two the scene's size (1024x576, `R24G8`), two for shadows
  (2048x4096 and 4096x16384) and one at half size. The first of the
  scene-sized ones is the one that holds the scene when the frame ends
  (`[picture] depth_pick` chooses another).
- One pass writes both into a texture made to be shared
  (`R16G16B16A16_FLOAT`): colour as it is, and in the fourth channel the
  distance from the camera, from the depth by the game's own projection
  (depth = -p22 + p32 / distance) and into Unreal's units by the join's
  scale. The far end of the depth buffer, which is the sky, comes out as
  about 17,800.
- The texture's handle, its size and that far distance go into the shared
  block (`GuestPicture`; the protocol is version 2 for it).

## Into Spyro's picture

In Spyro (`spyro/src/picture.cpp`), at each Present while Crash is on
Spyro's ground: Crash's texture is opened by its handle and drawn over the
back buffer. A point of it is kept only if it is something (its distance
is short of the far one) and it is nearer than what Spyro drew there.

Spyro's depth buffer is the first one cleared in the frame that is the back
buffer's size (1176x664, `R24G8`; the engine clears it to 0 and its three
2048x2048 shadow buffers to 1), and the engine made it readable, so it is
read where it is. Unreal keeps depth the other way up: depth = near plane /
distance. The near plane is taken as 10 (`[picture] near`): with 9 the
soles of Crash's shoes are shaved off by the ground he stands on, with 10
they are whole, so that is right to within a unit in ten. `[picture] bias`
(2) is how much nearer Spyro's scene has to be to hide him.

Spyro's own character is hidden with the engine's own
`SetActorHiddenInGame` while following with ground is on, and shown again
when it ends (`[picture] hide_spyro`).

The empty level's platform is now marked hidden in the level itself: it
holds Crash up before the join and is never drawn. So what Crash's game
draws is Crash and a sky, and the sky is cut out by its distance.

`[picture] corner = 1` in Spyro's ini draws the whole of what arrives,
small, in a corner of Spyro's window.

Seen: Crash standing on the path by the pool, whole, with nothing else of
his world; then run across the lawn by the bridge's controller, Spyro's
camera following, and stopped in long grass with the blades in front of his
shoes.

**He is big.** The scale between the worlds (0.5 of Unreal's units to one
of Crash's) was chosen in Phase 0 so that Crash runs as fast as Spyro and
jumps as high. At that scale his body comes out two to three times Spyro's
height: about 110 units against Spyro's 40. Halving the scale would make
him Spyro's size and halve his run and his jump with it, unless his own
physics were retuned. Left as it is until that is decided.

Known rough edges: Crash's frame is one behind Spyro's camera when the
camera moves fast; he is drawn over Spyro's own on-screen counters where
they overlap; he casts no shadow on Spyro's ground.

## The write-watch

`common/watch.cpp`: sets a data breakpoint on an address on every thread of
the game for a few seconds and logs each place a write came from, with its
callers. `[debug] watch_camera = <offset> <bytes> #n` in Crash's ini points
it at the camera object. This is how the table above was found, in three
runs of three seconds; it is the tool for "what code sets this?" from now
on.

## To do, in order

1. ~~Crash's camera is Spyro's.~~ Done: "Crash's camera".
2. ~~Crash's level drawn as nothing.~~ Done: the platform is hidden in the
   level, and the sky goes by its distance.
3. ~~Crash's picture out of Crash.~~ Done: "Crash's picture, out of Crash".
4. ~~Into Spyro's picture, with depth.~~ Done: "Into Spyro's picture".
5. Spyro's own character hidden: done. Crash's window hidden and his sound
   left on: not yet.
6. Decide the scale ("He is big").
7. Behind a pillar, by hand: the phase's own test.
8. A shadow under him; his frame and Spyro's camera in step; drawn under
   Spyro's counters rather than over them.
