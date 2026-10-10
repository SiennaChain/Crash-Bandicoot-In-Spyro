# Phase 2: seeing him

Goal (from `docs/DESIGN.md`): Crash drawn by Crash's game from Spyro's
camera and mixed into Spyro's picture with depth. Spyro hidden. Crash's
window hidden; his sounds heard. Done when Crash runs behind a pillar in a
Spyro level and is hidden by it, and nobody would guess a second game is
running.

**Where it stands (2026-10-10):** the first step only. Crash's game now
draws Crash through Spyro's camera: where each game puts its own character
on its own screen agrees to a thousandth of the screen. Nothing of Crash is
in Spyro's picture yet.

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

## The write-watch

`common/watch.cpp`: sets a data breakpoint on an address on every thread of
the game for a few seconds and logs each place a write came from, with its
callers. `[debug] watch_camera = <offset> <bytes> #n` in Crash's ini points
it at the camera object. This is how the table above was found, in three
runs of three seconds; it is the tool for "what code sets this?" from now
on.

## To do, in order

1. ~~Crash's camera is Spyro's.~~ Done: "Crash's camera".
2. Crash's level drawn as nothing: the platform hidden (the editor has a
   switch for it) and the sky one flat colour, so that what is not Crash
   can be told from what is.
3. Crash's picture out of Crash: his colour and depth, each frame, somewhere
   Spyro's process can read them (a shared texture). Which Direct3D each
   game uses, and where its frame ends, first.
4. Into Spyro's picture: drawn over Spyro's scene where Crash is nearer
   than what Spyro drew there. The two depths are in different units and
   different conventions and have to be brought to one.
5. Spyro's own character hidden; Crash's window hidden; his sound left on.
6. Crash facing the way he is going, as Spyro's picture shows it (he does
   already, in his own world; check it survives the mirror).
