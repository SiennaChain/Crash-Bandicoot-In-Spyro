# Phase 0: the link

Goal (from `docs/DESIGN.md`): both bridges load, know their build, and
exchange player and camera every frame; Spyro's pawn follows Crash's
position; Crash keeps running when not in front.

**Where it stands (2026-10-10): done.** Both games start themselves and link
up; each publishes its player and camera every frame and reads the other's;
the one controller reaches the right game; and with Spyro's window in front
Spyro's character goes where Crash is steered, at the right speed and in the
right direction, by the player's word ("yeah all looks good"). What it does
not do is anything about Spyro's ground: that is Phase 1 (`docs/PHASE1.md`).

## Steps

| # | Step | State |
|---|---|---|
| 1 | Both bridges build; protocol tests pass | done |
| 2 | Each stand-in DLL forwards XInput to the real one | done outside the games (below); both games start and run with them in; no controller has been connected yet |
| 3 | Each bridge loads in its game, recognises the build, writes its log | **seen** |
| 4 | Crash publishes player, model matrix, camera, clock, level from its game thread | **seen**, 60 frames a second, in the Crash 3 hub and in Toad Village |
| 5 | Spyro publishes pawn and camera | **seen** in Sunrise Spring |
| 6 | Each sees the other: heartbeats and frame counts both ways | **seen**, both logs and the monitor |
| 7 | Spyro's bridge gets onto Spyro's game thread and can look objects up by name | **seen**: the tick runs 60 times a second in Sunrise Spring; the pawn's class read back by name |
| 8 | Follow test: Spyro's pawn placed at Crash's position every frame, released on a key (F2) | **seen** (below) |
| 9 | Crash keeps simulating, and listening to the controller, when its window is not in front | **seen** for simulating: in Toad Village with neither game in front, its gameplay clock kept counting at 60 a second. The controller has not been tried. |
| 10 | Axes and scale between the two games measured | **measured**: one Crash unit is half an Unreal unit; his world is right-handed with z up (below) |

## Build, install, run

```powershell
tools\build.ps1              # both bridges, the monitor, and the protocol tests
tools\install.ps1 -WhatIf    # says where it would put what
tools\install.ps1            # one DLL + crashspyro.ini beside each game's exe
tools\install.ps1 -Uninstall
```

Installed files, and nothing else:

| Game | Folder | Files |
|---|---|---|
| Crash | `...\Crash Bandicoot - N Sane Trilogy\` | `xinput9_1_0.dll`, `crashspyro.ini`, and `crashspyro_crash.log` once run |
| Spyro | `...\Spyro Reignited Trilogy\Falcon\Binaries\Win64\` | `xinput1_3.dll`, `crashspyro.ini`, and `crashspyro_spyro.log` once run |

`-Only crash` or `-Only spyro` updates one game's bridge while the other is
running.

Start both games from Steam as usual. Each bridge logs beside itself, and
`build\out\tools\csp_monitor.exe` prints what both are publishing, once a
second, without being inside either game.

## What was checked, and how

- **Protocol** (`tests/test_protocol.cpp`): heartbeat timeout including the
  clock wrap; a slot read while another thread writes it never returns half
  of one value and half of the next. These test the block's own logic and
  say nothing about either game.
- **Stand-in DLLs, outside the games:** loaded into a 64-bit PowerShell and
  called through their exports. `XInputGetState` in `xinput9_1_0.dll` by
  name, and in `xinput1_3.dll` by name, by number 2 (how Spyro asks for it)
  and by number 100, each returned what the real DLL returned (no controller
  was connected at the time, so that was "not connected" every time; a real
  reading passing through has not been seen). Number 3 is present. Loaded
  into a process that is not their game they create no log and no shared
  block.
- **Imports:** Crash's exe imports `XInputGetState` and `XInputSetState` from
  `XINPUT9_1_0.dll` by name; Spyro's imports numbers 2 and 3 from
  `XINPUT1_3.dll`. Both read with `dumpbin /imports`.

## The builds

| | Steam app | Build | Exe link stamp | Image size | SHA-256 of the exe |
|---|---|---|---|---|---|
| Crash | 731490 | 2964270 | `0x5B50A076` | `0x1D74000` | `170c61d3…317b6af3` |
| Spyro | 996580 | 3849171 | `0x5CE389A2` | `0x3A38000` | `996a7789…c66d4fd6` |

Both exes are wrapped by Steam (a `.bind` section): their code is stored
encrypted and unpacked as the game starts, after the bridge has been loaded.
So a disassembly of the file on disk is noise, and a bridge must not look at
code until it is there. The Crash bridge waits for its frame routine's first
bytes to appear before hooking it. **The bytes being there is not enough:**
the wrapper writes the code out first and only afterwards makes that memory
runnable, and a hook put in between is refused (MinHook says
`MH_ERROR_NOT_EXECUTABLE`; this happened on the first run). Both bridges wait
for the bytes *and* for the memory to be executable.

To read either game's code, copy it out of the running game with
`tools\probe\dumpimage.ps1` (read-only; writes under `local\`, which git
ignores) and disassemble the copy with `dumpbin /disasm /range:...`.

## Places

Offsets into the exe, or into an object. All of these were found before this
repo existed, by an earlier attempt at the same mod on the same PC and the
same two builds (external memory reads, a Frida-loaded probe DLL, and UE4SS's
own log). They are the starting point here. The last column is whether
**this** code has seen them hold in a running game.

### Crash

| Place | Value | What it is | Seen here |
|---|---|---|---|
| `frame` | `exe+0x169B90` | The game's frame routine, `bool(client)`, on the main thread. Starts `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56 48 83`. | **yes**: hooked, called 60 times a second |
| `actorList` | `exe+0x1A9E778` | Pointer to a list whose first entry is the player actor | **yes** |
| `actorVtable` | `exe+0x1592220` | The class of that actor, as Crash on foot | **yes**, in the hub and in Toad Village. At a title or loading screen the list's first entry is something else entirely. |
| actor `+0x10` | | Parent (null when not riding anything) | read as null on foot |
| actor `+0x20` | | Position, three floats (`_parentSpacePosition`) | **yes**: changes as he walks. z is up; about 62 to 84 along Toad Village's first stretch. |
| actor `+0xB0` -> `+0x28` | vtables `0x150DF30`, `0x1676CB8` | Behaviour model -> model instance | **yes** (both classes match) |
| model `+0x30` | | The 4x4 matrix the model is drawn with; written once a frame, unchanged to the frame's end | read, values not yet looked at |
| `cameraSystem` | `exe+0x1A890E0`, vtable `0x151D850` | Camera system; `+0x10` is the camera in use (vtable `0x151D390`) | **yes** (both classes match) |
| camera `+0x80`, `+0x180`, `+0x1C0` | | Position; view matrix (world to camera, looks down -Z); projection | read, values not yet looked at |
| `clock` | `exe+0x1A61888`, vtable `0x13F7FB8` | Pointer to the gameplay clock: `+0x14` real ticks, `+0x20` gameplay ticks, `+0x24` gameplay time, `+0x2C` paused | **yes**: ticks count up, paused flag seen both ways |
| client | vtable `0x13ED108` | The object the frame routine is called on. `+0xB8` level name (`crash3/l301_toadvillage/l301_toadvillage`), `+0xE8` input locks, `+0xF0` pause stack | **yes** for the class and the level name (`Crash1/C1_StartScreen/C1_StartScreen` at the title screen) |
| pause / resume | `exe+0x14F170` / `exe+0x158E70` | The game's own pause request and release | not used yet |
| actor `+0x1A8` | vtable `0x1589128` | The actor's input; skipped by the game while a lock count at `+0x58 -> +0x10` is above zero | not used yet |

Known about Crash from that earlier work, and worth not relearning: the
gameplay clock ticks once per frame at both 30 and 60 fps; a level load
reuses the same player address, so a pointer staying the same does not mean
the player is the same (hence `generation` in the protocol); the player
object is back before the loading screen has gone, and the portal-arrival
animation holds input for about 1.9 s after that. Seen here: `generation`
went 1 in the hub and 2 in Toad Village, the level name reads
`crash3/l300_hub/l300_hub` then `crash3/l301_toadvillage/l301_toadvillage`,
and the clock reads paused during the load.

Crash's levels keep their collision as Havok data
(`StaticCollision_<level>.hkx`, classes named `hknp…`, i.e. the newer Havok
physics), seen in the names inside `archives\L301_ToadVillage.pak`. That is
what Phase 1 has to get Spyro's world into, or around.

### Spyro

| Place | Value | What it is | Seen here |
|---|---|---|---|
| `world` | `exe+0x3415F30` | `GWorld` | **yes** |
| world `+0x140` | | Game instance; `+0x38` its local players (array, count at `+0x40`) | **yes** |
| local player `+0x30` | | Player controller | **yes** |
| controller `+0x388` | | The pawn it is driving | **yes**: in Sunrise Spring (Spyro 3) it is a `BP_CPS1999_Playable_C`, read back by name |
| pawn `+0x158` | | Root component; `+0x1A0` its world location, `+0x160` relative location | **yes**: (-299705, -297964, 266) at Sunrise Spring's start, the same place the earlier work recorded. **Not the same on every load:** after a restart the same spot read (295, -297964, 266), 300,000 different in x. The engine moves the world's origin about, so a position means nothing without the origin it was read under; anything that carries Spyro's world across (Phase 1) has to carry that too. |
| root `+0x16C` | | Relative rotation (pitch, yaw, roll) | reads (0, -90, 0) at the start, which fits; not yet seen to change |
| controller `+0x3A0` | | Camera manager; `+0x3AC` camera rotation | **yes**: yaw changes as the camera turns |
| manager `+0x3A0`, `+0x3B8` | | Camera location and field of view | **yes**: the camera sits about 140 units from the pawn and 40 above it; field of view 80 in the level, 90 in menus |

For step 7, derived from UE4SS's log of 2026-10-06 and the exe's load address
in the same session (`0x7FF7C0430000`), then checked in a copy of the running
game's code:

| | Value | Checked |
|---|---|---|
| `UObject::ProcessEvent` | `exe+0x7688B0`, starts `40 55 56 57 41 54 41 55 41 56 41 57 48 81 ec f0 00 00 00` | **yes**: hooked in the running game; its code reads the object's index (`+0xC`) and compares it with the count in `GUObjectArray` |
| `GUObjectArray` | `exe+0x3562330`; objects pointer at `+0x10`, capacity `+0x18`, count `+0x1C` | **yes**: 195,756 objects of 2,097,152 at the title screen |
| `FName::ToString` | `exe+0x5E7AB0` | **yes**, by its code (16384 names to a chunk) |
| the name table (`GetNames()` is `exe+0x5E51A0`) | pointer at `exe+0x35590D8`: 128 chunk pointers, count at `+0x400`. An entry is its number (doubled; low bit = wide text) at `+0`, text at `+0x10`. | **yes**: "ReceiveTick" found and read back by number in the running game |
| `UGameViewportClient::Tick` | `exe+0x13C04E0` | is only a two-instruction thunk onto the real routine; not used |
| `UGameEngine::Tick` | `exe+0x1390F30` | not looked at |
| `StaticConstructObject_Internal` | `exe+0x78BCF0` | not looked at |
| `GNatives`, `GMalloc` | `exe+0x3560E50`, `exe+0x3437258` | not looked at |

The bridge gets onto the game thread once a frame by watching `ProcessEvent`
for the pawn running its own `ReceiveTick`. That needs the pawn to be a
Blueprint class that ticks; if a level has a pawn that is not, the tick count
in the log stays at zero and another anchor is needed.

Object layout as UE4SS reported it for this game: name at `+0x18`, class at
`+0x10`, outer at `+0x20`, index at `+0xC`, flags at `+0x8`. The engine's
own source for 4.19 is the reference for everything else.

Spyro's collision is PhysX 3, in DLLs beside the game
(`Engine\Binaries\ThirdParty\PhysX3\Win64\VS2015\PhysX3_x64.dll` and
friends) that export their functions by name. That is the way into Spyro's
world for Phase 1.

## The follow test

`[follow]` in Spyro's `crashspyro.ini`; F2 switches it (two rising beeps on,
two falling off), and so does changing `on` and saving. While it is on,
Spyro's movement component (`PhasmidCharacterMovementComponent`) has its tick
switched off and the pawn is placed with `K2_SetActorLocation` (no sweep,
teleport) from inside its own tick. Both are found by name each run, not by
address; the function's parameters came out at location `+0`, sweep `+12`,
teleport `+152`.

(Corrected: these notes first called Spyro's level Sunny Villa throughout.
It is Sunrise Spring, the home world the save starts in; the positions
recorded here lie inside the collision later read out there.)

What was seen, 2026-10-10, Crash in Toad Village and Spyro in Sunrise Spring:

- The player ran Crash about with the controller and saw Spyro's character
  move with him ("both characters move when crash is controlled").
- Sampled once a second from outside both games for 150 s, Spyro's position
  was where the mapping put it to within 0.1 units whenever Crash was still
  or slow, and about a frame's travel behind when Crash was jumping (12
  units at the top of his rise). That lag is the two games' frames not being
  in step, and is Phase 2's to deal with.
- Mapping used: Unreal x = -Crash x, Unreal y = Crash y, z = z, one for one
  (`scale = 1.0`, `yaw = 180`, `flip_y = 1`). That is a mirror, as it has to
  be: Crash's world is right-handed and Unreal's is left-handed. With both
  cameras as they start in those two levels, it makes "away from Crash's
  camera" come out as "away from Spyro's". The player has not yet said
  whether the directions and the speed looked right.
- Crash runs at roughly 500 of his units a second and Toad Village is some
  30,000 long, so his units are about the size of Unreal's (centimetres).
  Not measured against anything yet.
- The first F2 switched it on and off again within 0.36 s (a second press
  or a bouncing key), which nobody could have seen: hence the beeps, and a
  second during which another press is ignored.
- Left on while Crash ran the length of his level, it dragged Spyro's
  character far outside Sunrise Spring and the game had to be restarted. It
  now lets go, and puts the pawn back where it started, if Crash gets more
  than `reach` (6000) units from where following began.

`[start] auto_continue = 1` has Spyro's bridge press the controller's A from
the title screen until there is a pawn, which takes "Continue" (the save
played last, whichever of the three games that is). See "Getting in" below
for what that took, and for a claim made here earlier that was wrong.

## Scale and axes

Measured 2026-10-10. Spyro through his bridge's own stick and A button in
Sunrise Spring; Crash by watching the player run and jump him in Toad Village,
one sample a game frame.

| | Crash, in his units | Spyro, in Unreal's | Ratio |
|---|---|---|---|
| Run, steady | median 510 a second, 90th percentile 563 | 271 a second | 0.48 to 0.53 |
| Standing jump, how high | 162.0 (four jumps within 0.1 of each other), 0.70 s in the air | 81.2 with A held, 60.3 on a tap; 0.96 and 0.79 s | 0.50 |

**Scale: 0.5 Unreal units to one of Crash's.** At that he runs at Spyro's
pace and his jump is exactly as high as Spyro's full one, so distances built
for Spyro suit him without his movement being touched. He is in the air for
less time than Spyro (0.70 s against 0.96): his gravity is stronger. The
glide is not measured yet.

**Axes.** Crash's world is right-handed, z up; Unreal's is left-handed, z
up. Crash's view matrix (as stored, world to camera) has the camera's own
right in its first column and its line of sight in minus its third. Checked
against what has to be true: over the ground, minus the third column points
from the camera's position to Crash (agreement 1.000), and that direction
turned clockwise is the first column (1.000). Spyro's camera at yaw ψ looks
along (cos ψ, sin ψ) with (-sin ψ, cos ψ) to its right; checked the same way
against where the camera is.

**The player's verdict on the first version of the follow** (one fixed turn
between the worlds, one for one): "all directions are inverted and Spyro
moves too fast (also ignores terrain and collision)". Inverted because one
fixed turn is right for one direction of Spyro's camera and backwards for
the opposite one; too fast because one for one was a guess; and it ignores
terrain because it is told to. The follow now turns each of Crash's steps
from his camera to Spyro's (`[follow] relative = 1`), at the measured scale,
and turns the pawn to face the way it is going. In a run with the bridge
pushing Crash's stick, Spyro moved 0.49 to 0.50 of what Crash did each
time; the directions could not be judged from that run because the player
was playing Crash at the same moment. The player then tried it with Spyro's
window in front, turning Spyro's camera: "yeah all looks good".

## One controller

Seen by the player: with both games running, the controller moved Spyro
whichever window was selected, and Crash at the same time.

**How the controller reaches the games: through XInput, with Steam in the
way.** Both exes also ask for Steam's own controller interface
(`SteamController006`; the string is in both, and Steam's
`logs\controller.txt` shows it adopting a binding for each app), which
looked for a while like the route. It is not: each bridge logs the first
real use of the controller with the route it came by, and both said XInput
(controller 0). Neither game has been seen to ask Steam's interface about a
stick. What had defeated the first filter was Steam's overlay rewriting the
bridge's exported XInput functions in Spyro, so that the game's calls
skipped the bridge (3 below). The filter on Steam's interface (1 below) is
still installed and has so far had nothing to do.

What it took, in the order it was found:

1. **A filter on Steam's interface** (`common/steampad`). The interface is
   one object per process; the bridge gets it the way the game does
   (`SteamInternal_CreateInterface("SteamClient017")`, then
   `SteamAPI_ISteamClient_GetISteamController(..., "SteamController006")`)
   and replaces two of its methods: `GetDigitalActionData` (entry `+0x68`)
   and `GetAnalogActionData` (`+0x80`). The entry numbers are read from the
   plain-C wrappers in each game's own `steam_api64.dll`, which are "fetch
   the table, jump to entry N"; both games' DLLs give the same numbers. A
   method returning a struct is handed, after `this`, the place to write it.
   Muting is "not pressed" and "centred" in what Steam returns, so the game
   sees buttons released and sticks let go, not a controller vanishing.
2. **The bridge's own controller** (`common/standin`), for pressing through
   menus, goes in through XInput, where the real one is absent.
3. **Steam's overlay was eating it.** In Spyro the overlay rewrites the
   start of anything exported as `XInputGetState` by a DLL called
   `xinput1_3` (the bridge's exports began `e9 ...`, a jump), so the game's
   calls went to Steam and never ran the bridge's code. `pad::TakeBack`
   points the exe's two imports at functions inside the DLL that are not
   exported, which the overlay does not know about. (Crash's stand-in,
   `xinput9_1_0`, was not rewritten.)
4. **Crash is deaf when it is not the active window.** A press through the
   stand-in did nothing until Crash's window was sent the messages that say
   "you have been activated". Its bridge now keeps those from changing back
   while Crash is wanted awake (`KeepAwake`: while linked, and while getting
   in). Spyro takes XInput whether or not it is in front.

Who hears what (`OnPad` and `OnSteam` in each `main.cpp`, `[pad]
only_in_front` in each ini):

| | Not linked | Linked (Spyro's character following Crash, level running) |
|---|---|---|
| Spyro hears | everything, but only while its window is in front | the right stick, Start and Back, whichever window is in front |
| Crash hears | everything, but only while its window is in front | everything, whichever window is in front, and is kept awake to hear it |

"Linked" is a flag Spyro publishes (`kHostLinked`). It drops when Spyro's
pawn stops ticking (paused, a menu, a load), and Spyro then has its whole
controller back so that its menus work.

What has been seen:

- With another window in front (so the real controller was shut out of
  Spyro), the bridge's stick held up for 1.2 s moved Spyro's character 307
  units along its camera's line, and back again held down.
- The player's test, recorded from outside both games once a second for 235
  s (2026-10-10, Crash in Toad Village, Spyro in Sunrise Spring):
  - **Follow on, neither game's window in front, 47 s:** Crash moved in 45
    of them and Spyro's character in 44. So Crash answers the controller
    without being in front (kept awake, and let hear it because linked), and
    Spyro's character goes with him.
  - **Follow off, Crash's window in front, 181 s:** Crash moved in 161,
    Spyro in none. The controller no longer reaches Spyro when it is not in
    front, which is what the player had asked for.
  - The follow let go by itself after about 2 min 40 s because Crash had run
    more than `reach` (6000) units from where it began. That is the test's
    own guard, not a fault, but it will look like one to whoever is playing.
- Then, by the player's word: with Spyro's window in front and the follow
  off, Spyro moves and Crash does not; with it on, Crash answers the
  controller and the right stick still turns Spyro's camera. That is the
  arrangement the mod will run in.

## Getting in

Both bridges can take their game from launch to somewhere playable with
nobody at the controller, using the stand-in controller.

**Spyro**, `[start] auto_continue = 1`: A every 1.5 s from the title screen
until there is a pawn. With Spyro's calls routed back (3 above) it reached a
level 37 s after launch.

> An earlier version of this page said this had been "seen to get from
> launch into the last save in 54 s with nobody touching it". That was
> wrong. The presses were not reaching the game at all then (3 above), and
> nobody had checked that the player's own controller was idle; the player
> pressing A is the likely explanation. Two later runs with Spyro not in
> front, when the player's controller could not reach it, sat at the title
> screen until the bridge gave up, which is what exposed it.

**Crash**, `[start] auto_load = 1`: to the Crash 3 hub with the save in use.

| Screen | How the bridge knows | What it does |
|---|---|---|
| Publisher's video, then the Vicarious Visions logo (about 45 s together) | level `Crash1/C1_StartScreen`, camera far up (z about 12,900) | nothing. None of A, Start, Y, X, B skips the logo, though the game says "press ... to skip". |
| Title, "press A to begin" | camera at about (-108, -476, 982) | A, once a second |
| The three games; Crash 1 picked out, "Continue" first in its list | camera settled at about (-108, -229, 76) for 1.2 s | right, right (to Crash Warped), then A on "Continue" |
| Loading, then the hub | level `crash3/l300_hub`, player present | stops; hands the controller back |

Choosing a game does not go through a save menu: it loads that game's hub
with the save in use, about 4 s. Nothing says which of the three games is
picked out (the camera does not move), so the two rights are done once from
a screen that has just appeared and the level that loads is checked; if it
is not Crash 3 the bridge says so and stops. Seen: 61 s from launch to the
hub, the log showing the bridge's own steps. Crash's window happened to be
in front for that run.

## Next

Phase 1: `docs/PHASE1.md`, which also holds what was learnt here about
Crash's physics.
