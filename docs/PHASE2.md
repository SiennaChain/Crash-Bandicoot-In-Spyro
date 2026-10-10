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

It has been played, and the player's verdict on the first go was: too
small (he had been drawn at half size), very jittery, and the stick did
not go where the camera was looking once the camera had been turned. All
three are answered below: "He is big", "Moving evenly" and "Steering by
Spyro's camera".

Not yet: seen behind something as big as a pillar (the camera keeps him in
view; it wants playing).

Work stopped here on 2026-10-10 (see the README): the answers to the
player's three complaints were measured with the bridge's own controller
and have not been judged by the player's eye.

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
distance. **The near plane is 5** (`[picture] near`), not the engine's
usual 10. `[picture] bias` (2) is how much nearer Spyro's scene has to be to
hide him.

(Corrected: this first said 10, on the strength of the soles of Crash's
shoes being shaved by the ground at 9 and whole at 10. That test changed
the bias from 2 to 0 at the same time, which is what shaved them. The
shadow, below, is what showed it: worked out with 10, the ground under his
feet came out twice as far away as his feet are, and with 5 the shadow is
under them. So for a while Crash was being drawn in front of anything less
than twice as far away as he was.)

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

## Where the camera is now, not where it was

A picture of Crash's is a frame or two old by the time Spyro shows it, and
it was drawn through the camera Spyro had then. If Spyro's camera has moved
since, a picture drawn where it was drawn has Crash sliding against the
world. So each picture carries the camera it was drawn through (as Spyro
gave it) and where in Spyro's world Crash's feet were (`GuestPicture`, the
protocol is version 3 for it), and Spyro's bridge, as it shows a frame,
reads the engine's camera as it is at that moment, works out where Crash's
middle would be on the frame through each of the two, and draws the picture
moved and sized by the difference (`[picture] steady = 0` draws it where
it was drawn). Built; with the camera still the two agree and nothing
moves.

That was not enough: the engine's camera read as a frame is shown is not
the camera that frame was drawn through. The game's thread is a frame
ahead of what is on the screen. So Spyro's bridge now notes, on the game's
thread at the top of each tick of the pawn's, the camera the engine has at
that moment (it has not been moved for the new frame yet, so it is the one
the frame being drawn was drawn through) and where the pawn's feet are,
and the thread that shows frames uses that pair. And he is drawn not where
his own picture had him but where the pawn's feet are in that frame: the
pawn is what Spyro's camera follows, so he sits as still on the screen as
Spyro would.

## His shadow

Spyro's ground knows nothing of Crash, so the bridge darkens it itself:
before Crash is drawn, every point of Spyro's frame has its place in the
world worked out from Spyro's depth and camera, and is darkened if it is
within a short way of the point under Crash's feet (and not much above
them, and not far below). It falls on whatever is under him: slopes, steps,
and the ground far below when he jumps, fading with the drop.
`[picture] shadow` is how dark (0.45; 0 for none).

**He is big.** The scale between the worlds (0.5 of Unreal's units to one
of Crash's) was chosen in Phase 0 so that Crash runs as fast as Spyro and
jumps as high. At that scale his body comes out two to three times Spyro's
height: about 110 units against Spyro's 40. Halving the scale would make
him Spyro's size and halve his run and his jump with it, unless his own
physics were retuned.

There is a second way, and it is only looks: `[picture] size` in Spyro's
ini draws him that much smaller about his feet (0.5 is half), shadow and
all, without touching the scale. He then runs and jumps as before and
stands where he stood, but what stops him is still the full-sized Crash:
he halts a little short of walls and hangs further over edges than the
drawn Crash seems to warrant.

The player's verdict at 0.5: too small. Crash is taller than Spyro and
should come up to just below Hunter's shoulder. Measured off a picture of
the two side by side, at 0.5 he was 0.41 of Hunter's height and the
shoulder is about 0.74 of it, so `size` is 0.9 unless the ini says
otherwise (not yet seen beside Hunter at that). At that the drawn Crash and
the Crash that is stopped by walls are nearly the same thing again. His height not being Spyro's breaks nothing: what he can
reach and fit through is decided by his own body in his own game, at the
scale between the worlds, and neither game is told how tall the other's
character is.

## Moving evenly

The first go juddered. Three things were wrong, and the third was most of
it.

1. He was drawn where his own picture had him, a frame or two late and
   through a camera that had since moved. Now he is drawn at the pawn's
   feet (above).
2. The pawn was put where Crash's newest frame had him. The two games both
   run at sixty frames a second and are not in step: a tick of Spyro's
   might see the same frame of Crash's as the last tick, or the next but
   one. The pawn stood still for a frame and then went twice as far.
3. **Neither game's frames come evenly, and Crash does not move by the
   clock on the wall.**

Crash's frames, as the shared clock stamps them, with how far he ran in
each:

| gap, ms | his game's step, ms | he moved |
|---|---|---|
| 15.6 | 15.5 | 8.06 |
| 15.4 | 15.5 | 8.06 |
| 26.8 | 15.5 | 8.06 |
| 17.2 | 31.6 | 16.44 |
| 15.6 | 15.5 | 7.94 |

His game takes a step as long as the frame before took. About once in
fourteen frames one comes ten thousandths of a second late (his game runs a
little faster than the screen and has to wait for it), and he is moved a
normal step in it and a double step in the next. Against the wall clock he
stalls and lurches; against the sum of his game's own steps he moves at
exactly 0.52 of his units a millisecond. That sum is his game's clock. The
camera's update is handed the length of each step, the bridge adds them up,
and it goes out with each frame as `GuestState::gameUs` (the protocol is
version 4 for it).

Spyro's ticks come 18, 18 and 14 thousandths of a second apart, over and
over, though its pictures are shown evenly.

So in Spyro's bridge (`Place`, `spyro/src/main.cpp`):

- Spyro's ticks are made even: a clock that goes on by the usual gap
  between ticks, and is pulled a twentieth of the way towards when each
  tick really came.
- How far Crash's clock is behind the shared one is followed slowly (it
  wanders a few milliseconds and is ten out for a frame at a time).
- His last six frames are kept, each with the time on his clock.
- At each tick the pawn is put where he was `[follow] behind_ms` (20)
  before now, on his clock, on the straight line between the two frames of
  his either side of that moment. Twenty is far enough back that there
  nearly always are two. When his newest frame is late there are not, and
  he is carried on as he was going for up to 30 ms.

Measured, the bridge's controller running him across the lawn in Sunrise
Spring, the pawn's step from one of Spyro's frames to the next:

| | step, Unreal units | spread | frames with no step |
|---|---|---|---|
| put where his newest frame had him | 4.3 | 47% of a step | some |
| a place and a speed, corrected by each frame, by the wall clock | 4.34 | 20% | none |
| his own clock, as above | 4.34 | 0.4% (4.28 to 4.35) | none |

The cost is those 20 ms: he is that much later answering the stick than he
was. Not yet judged by the player.

**The late frames themselves** were the last of it, and they were the
bridge's to stop. A jump showed them up: where a frame of his was late the
pawn rose two short steps and one long. Two things were going on.

His game does not take the double step all at once. Running, he goes twice
as far in the long frame. In the air he rises or falls a single step in it
and the double in the frame after:

| gap, ms | his game's step, ms | he fell |
|---|---|---|
| 15.3 | 15.4 | 6.13 |
| 30.0 | 15.5 | 6.92 |
| 16.5 | 31.1 | 7.88 |
| 15.6 | 15.5 | 18.67 |
| 15.4 | 15.5 | 10.75 |

So `Place` keeps two moments for each frame of his, one for where he is
over the ground and one for how high, and after a long step puts each
either at the frame's own time or one usual step after the frame before,
whichever leaves him moving more as he was.

And the frames were late for no good reason. His game makes a frame every
15.5 ms, a little faster than the screen shows them, and asks Direct3D to
wait for the screen (`Present` with interval 1): so they pile up and about
every fourteenth has to wait a whole refresh. Nobody is looking at his
window while he is in Spyro's. So while he is on Spyro's ground the bridge
has his frames shown without waiting (`[picture] no_wait` in Crash's ini,
on unless set to 0; `gfx::NoWait`). His frames then:

| | gap between frames | spread | longest |
|---|---|---|---|
| waiting for the screen | 16.6 ms | 20% | 30.4 ms |
| not waiting | 15.55 ms | 2% | 16.4 ms |

No late frames, so no double steps, and nothing to carry him on through.
Seen after it, frame by frame: a run of 67 frames at 4.31 to 4.34 units
with no short step in it, and two jumps whose rise and fall went up and
came down by steps each a little different from the last, take-off to
landing.

## Steering by Spyro's camera

Crash goes where the stick points as his game's camera has it, and
writing Spyro's view into that camera did not change which way that was.
Phase 1's answer was to turn the stick by the angle between the two
cameras before he heard it. The player found it wrong, and it was, twice
over.

First, the angle was taken from the wrong camera: from the view the bridge
itself had just written (Spyro's), so the stick was turned by nothing.
That was put right by reading the game's own view before writing over it,
and then up was away from the player with the cameras a quarter turn apart
or none.

Second, at any other angle he went wide. **Crash's game does not take the
stick as it comes.** The bridge's controller, the stick pushed right over
at every five degrees from up to right, following off, which way he went
and how fast by his own camera:

| stick, degrees from up | he went, degrees | speed, units/ms |
|---|---|---|
| 0 to 15 | 0 | 0.520 |
| 20 | 4.7 | 0.522 |
| 25 to 35 | 30 | 0.60 |
| 40 to 50 | 45 | 0.65 |
| 55 to 65 | 60 | 0.60 |
| 70 | 85.3 | 0.522 |
| 75 to 90 | 90 | 0.520 |

Sixteen directions and no others, and a quarter faster along the
diagonals. And along one axis: nothing below 0.65 of a full push, a walk
(0.15) from 0.70 to 0.80, the run from 0.82. So a stick turned to
somewhere in between is taken to the nearest of the sixteen by his camera,
which is up to twenty degrees from where the player pointed, and his speed
changes as Spyro's camera turns. No turning of the stick can mend that.

**What he is steered by** is in the camera object, beside what was already
known of it:

| Camera | What |
|---|---|
| `+0x80` | where it is |
| `+0x8C` | how it is turned: three angles in radians, the third about the world's up, from x towards y |
| `+0x98` | 1.0 |
| `+0x9C` | a second copy of where it is |
| `+0xA8` | a second copy of the three angles. **The game steers by the third of these (`+0xB0`).** |
| `+0xB4` | 1.0 |

Writing the angle at `+0x94` does nothing to the steering; writing the one
at `+0xB0` turns it at once. So, straight after the game has worked its
camera out, with the view, the bridge writes there the way Spyro's camera
looks (minus Spyro's yaw: Crash's y runs the other way), and the stick is
left alone. The game puts its own angle back by itself within a few
seconds of the bridge stopping, and the bridge puts back what it found as
well.

Everything the game does with the stick it now does by the screen, as in
Crash's own game: up is away from the player, the sixteen directions are
sixteen directions on the screen, and the diagonals are fast on the
screen's diagonals.

Seen, the bridge's controller, Spyro's camera turned to eleven angles
between -107 and +88 degrees (most of them in between the sixteen), the
stick pushed up, right, down and left at each: the pawn went 85 to 93 units
that way and 0.0 to either side, every time. With the stick turned instead,
at the same sort of angles: up to 28 units to the side in 89, and 89 to 122
units gone.

`[ground] steer_by_spyro` in Crash's ini: 1 is this, 2 the turned stick
(kept to compare), 0 neither.

## Crash's window

`[window] hide = 1` in Crash's ini: while he is drawn in Spyro's picture
his own window is hidden (not minimised, which stops the game), and shown
again when he is not. Seen: with it hidden his pictures went on arriving
at sixty a second and Spyro's was the only window on the screen. Off unless
asked for, while both windows are still useful to look at.

Known rough edges: he is drawn over Spyro's own on-screen counters where
they overlap.

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
5. ~~Spyro's own character hidden; Crash's window hidden and his sound
   left on.~~ Done: "Crash's window" (off unless asked for).
6. ~~Decide his size.~~ Drawn at 0.9 ("He is big"), on the player's say.
7. Behind a pillar, by hand: the phase's own test.
8. ~~A shadow under him; his frame and Spyro's camera in step.~~ Done:
   "His shadow", "Where the camera is now".
9. Drawn under Spyro's counters rather than over them.
10. ~~He judders; the stick does not follow the camera.~~ Done: "Moving
    evenly", "Steering by Spyro's camera". Both want the player's eye.
