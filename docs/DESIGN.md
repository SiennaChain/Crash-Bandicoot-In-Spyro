# Crash in Spyro: design

Play **Spyro: Year of the Dragon** (the third game in Spyro Reignited
Trilogy) as **Crash Bandicoot** from **Crash Bandicoot: Warped** (the third
game in the N. Sane Trilogy): the real Crash, simulated by the real Crash
game, moving through Spyro's real world.

The method is the one [SkyCraft](https://github.com/chasmlol/SkyCraft) used
for Minecraft inside Skyrim, and the one this repo's sibling used for
[Battlefront II characters in World at War zombies](https://github.com/SiennaChain/WaW-Zombies-with-SWBF2):
both games run at once, a small DLL in each lets them talk, and each game
does the half it is the authority on.

This file is the plan. `docs/PHASE0.md` is what exists so far and how it was
checked. Nothing here is playable yet.

## Scope

- The mod runs **only in Spyro 3**. In Spyro 1 and 2, and in the trilogy's
  menus, the bridge does nothing and Spyro is Spyro.
- The guest is **Crash 3's** Crash: his moves, his power-ups (double jump,
  death tornado, body slam, bazooka, Crash Dash), his vehicles. Where a
  mapping asks for something Crash 3 does not have (the jet-board is Crash
  2's), it comes from elsewhere in the same owned N. Sane Trilogy install.
- One PC, both games owned on Steam, single player, offline play. Nothing of
  either game is redistributed; nothing is patched on disk.

## 1. The two halves

| | Game | Role | The bridge |
|---|---|---|---|
| **Host** | Spyro Reignited Trilogy (Unreal Engine 4.19, `Spyro-Win64-Shipping.exe`) | The game on screen. Owns the world. | `xinput1_3.dll` beside the exe |
| **Guest** | Crash Bandicoot N. Sane Trilogy (Vicarious Visions' Alchemy engine with Havok physics, `CrashBandicootNSaneTrilogy.exe`) | Runs unseen. Owns the player. | `xinput9_1_0.dll` beside the exe |

Each bridge stands in for the XInput DLL its game loads from its own folder
and hands every call on to the real one. That gets the bridge loaded without
touching the exe, needs no other mod loader on a player's PC, and happens to
be the one place both games read the controller through (section 4).

A bridge does nothing unless it finds itself in the exact build it was made
for and the mod's launcher started the game (`common/session.h`). Played by
themselves, both games behave as if the DLL were not there.

## 2. Authority

Who owns what. This table is the user's, kept as written.

| System | Authority |
|---|---|
| Player position, velocity, ordinary movement, actions, animation and equipment | **Crash.** Spyro's player becomes a puppet at the guest's published position; its movement solver must not fight the guest. |
| Terrain, colliders, water/ice/supercharge volumes, enemy AI and positions | **Spyro.** Export the environment into Crash's world queries; Crash resolves movement against it. |
| Attacks and player damage response | Crash evaluates native spins, slams, bazooka projectiles and damage reactions. Spyro provides target proxies and incoming-hit events. |
| Enemy/object health, chest eligibility, task completion, pickups and progression | **Spyro.** Apply guest hit results through host gameplay functions exactly once; return the accepted outcome. Preserve level-specific permissions and unlocks. |
| Player health/death and ability state | Crash owns damage/life state while linked. Translate death to Spyro's checkpoint flow; host respawn/load supplies an explicit guest reset command. |
| Camera and rendering | Spyro owns world framing, menus and cutscenes; send its view/projection to Crash for aligned rendering. Crash owns its character/vehicle pose. |
| Saves, portals and level transitions | Spyro owns progression and world context; a paired guest snapshot restores Crash state with the corresponding host save. |

Requested translations: body slam opens eligible headbash chests; bazooka gives
ranged attacks; water enables Crash's scuba gear; supercharge zones select his
Crash Dash boost-pad run animation; ice supplies surface properties; speedways
and flight powerups select the biplane. Death-tornado gliding must match Spyro's
glide trajectory: this is an explicit guest movement adaptation using a measured
host glide profile, not unmodified Crash physics. Crash remains the single
writer of linked player position in that mode. Each mapping needs a gameplay
test; none is implemented by the bootstrap.

Additional requested mappings:

| Feature | Decision and gameplay ownership | Evidence still needed |
|---|---|---|
| Sparx -> Aku Aku | Show Aku Aku as the companion. Keep host pickup collection/gem-finding services; show the guest's protection state. Route incoming damage once through Crash, with no second Sparx damage deduction. Preserve host health for safe handback. | Asset/attachment and damage-state hooks; explicit treatment of Sparx-only playable levels. A companion replacement does not implement those levels. |
| Gems -> Wumpa fruit | Replace the pickup presentation while Spyro retains each gem's ID, value, collection flag, level total and Moneybags spending. A five-value gem still credits five host units. Do not also grant Crash's native fruit-to-life reward from these same pickups. | Pickup mesh/FX and UI references, collection and save/reload tests. |
| Lives portrait -> Crash icon | Replace the lives image; while linked the displayed number mirrors Crash's authoritative life state. Host checkpoint/death transitions acknowledge one death event and must not deduct a second life. | Exact widget/atlas and counter bindings, life-loss/gain and reload tests. |
| Skateboarding -> jet-board poses | Use Crash's jet-board stance/lean as animation candidates; jumps, tricks, wipeouts and landing blends require adaptation. Spyro supplies course, timer, scoring and task rules; Crash's guest skating mode remains the position writer. Do not drive the host skateboard and Crash controller simultaneously. | Locate/load the NST jet-board assets (the Crash 2 asset set), retime poses, establish guest board movement and host trick-event integration. Water-board motion alone is not skateboard physics. |

Two rules follow from the table and are worth saying once:

- **One writer for everything.** Position has one writer (Crash), a gem's
  collected flag has one (Spyro), a life has one (Crash). Wherever both games
  have their own version of a thing, one is switched off or made a display
  of the other. Every double-counting bug this mod can have comes from
  breaking that.
- **Events cross once.** A hit, a pickup, a death carries a number; the side
  that owns the outcome applies it once and answers with what happened.

## 3. What crosses, and how

**State** goes through one block of shared memory
(`protocol/csp_protocol.h`, the only definition, included by both bridges).
Each side writes its own slots and only reads the other's; a slot is written
whole and a reader can tell if it caught one half-written. Each side counts
its game's frames from the game's own thread, so the other can tell a running
game from a process that is merely still there.

**Pictures** (Phase 2) go as Direct3D 11 shared textures: both games are
D3D11, so Crash's colour and depth can be handed to Spyro on the graphics
card without a copy through memory.

| Direction | What | When |
|---|---|---|
| Crash -> Spyro | Player: position, the matrix his model is drawn with, grounded/action/animation state, health and lives | every Crash frame |
| Spyro -> Crash | Camera: view, projection, viewport, for Crash to draw from | every Spyro frame |
| Spyro -> Crash | The world near the player: collision shapes, with surface kind (ice, water, supercharge) | as the player moves; all of it again on a level load |
| Spyro -> Crash | Things that can be hit or can hurt: enemies, chests, breakables, as proxies with an id, a box and a velocity | about 20 times a second |
| Crash -> Spyro | Hit: which proxy, with what (spin, slam, slide, bazooka) | when it happens |
| Spyro -> Crash | Hit result; player hurt; respawn/teleport; level changing; paused | when it happens |
| Crash -> Spyro | Player died; mode changed (plane, scuba, board) | when it happens |
| Crash -> Spyro | Colour + depth of Crash and whatever he holds or rides | every Crash frame |

Lengths and axes are translated in exactly one place, on the host. The scale
between the games is a decision, not a fact: it is whatever makes Crash's
run and jump suit distances Spyro's levels were built for, and it is measured
against both games before it is chosen (Phase 1).

**If one game stops.** Crash gone or hung: Spyro stops using its state,
gives the player back to Spyro's own movement at a place Spyro's own
collision accepts, and carries on as plain Spyro. Spyro gone or hung: Crash
pauses and waits. Neither waits on the other without a time limit.

## 4. One controller, two games

Both games read the same controller and, through XInput, both hear it
whichever window is in front. So each bridge edits what its own game hears:

- Crash hears the left stick and the action buttons.
- Spyro hears the right stick (its camera), Start/Back and the menus, and
  while linked not the buttons Crash is using.
- Crash is made to steer relative to **Spyro's** camera, by giving Crash's
  camera Spyro's view. It needs that view to draw from anyway.

Keyboard and mouse reach a game only while its window is in front, so they
need their own routing (as Battlefront II did in the sibling project). The
controller comes first.

## 5. The hard parts

In the order they can sink the project.

1. **Spyro's world under Crash's feet.** Crash's movement asks Crash's own
   physics world (Havok; the level's collision is one mesh in a `.hkx` file)
   where the ground and walls are. Spyro's collision has to answer instead.
   Spyro's side is the easy end: Unreal keeps its collision in PhysX, whose
   DLLs sit beside the game with their functions exported by name, so every
   shape near the player can be read out. Crash's side is not: either Spyro's
   shapes are built into Crash's Havok world at run time, or the handful of
   queries Crash's movement makes are intercepted and answered from a copy of
   Spyro's shapes kept in the bridge. Which, is decided by reading Crash's
   code in the running game, and it is the first real work after Phase 0.
   **If neither can be made to work**, the fallback is the arrangement the
   World at War mod uses: Spyro's own movement carries the player through
   Spyro's world and Crash follows it for animation and attacks. That gives
   up Crash's real physics, so it is a fallback and not the plan.
2. **Drawing only Crash.** Crash has to be drawn by his own game from
   Spyro's camera, with nothing else of Crash's level in the picture, and
   then be hidden correctly behind Spyro's walls and trees. That needs
   Crash's depth as well as his colour, and Spyro's depth buffer at the
   moment of mixing.
3. **Which Crash level he is "in".** Crash needs a level loaded to exist.
   Plane, scuba and board are different levels in Crash's own game, with
   different controllers. Whether those can be had without sitting through a
   Crash level load in the middle of a Spyro level is unknown.
4. **Running unseen.** Crash must keep simulating, and keep its sound, with
   its window hidden and never in front.
5. **Loads, cutscenes, deaths, the other playable characters.** Every place
   Spyro takes control away, the link has to let go cleanly and pick up
   again. Sheila, Sgt. Byrd, Bentley, Agent 9 and the Sparx levels are not
   Crash; the mod hands back to Spyro's own game for those.

## 6. Phases

Each ends with something that can be seen in the real games.

| Phase | What gets built | Done when |
|---|---|---|
| **0. Link** | Both bridges load, know their build, and exchange player and camera every frame. Spyro's pawn is made to follow Crash's position on Spyro's game thread. Crash keeps running when not in front. | Walk Crash around Toad Village with the controller and watch Spyro's character trace the same path in a Spyro 3 level, smoothly, and get control back on a key. |
| **1. Ground** | Spyro's collision reaches Crash's movement; Crash's own level stops mattering; input routed (section 4); scale chosen. | From Spyro's window: run on Spyro's ground, be stopped by Spyro's wall, jump onto Spyro's ledge and fall off it, all with Crash's physics. The first moment it is really a passthrough. |
| **2. Seeing him** | Crash drawn by Crash's game from Spyro's camera and mixed into Spyro's picture with depth. Spyro hidden. Crash's window hidden; his sounds heard. | Crash runs behind a pillar in a Spyro level and is hidden by it. Nobody would guess a second game is running. |
| **3. Fighting** | Enemies, chests and breakables as targets in Crash's world; spin, slide and slam hit them; they hurt him. Death and respawn through Spyro's checkpoints. Body slam opens headbash chests; bazooka. | Spin an enemy away, get hit once, die once, come back at Spyro's checkpoint, open a headbash chest, shoot something across a gap. |
| **4. Company** | Aku Aku for Sparx, Wumpa fruit for gems, Crash's lives icon and count. | Collect fruit and see Spyro's gem total go up by the right amounts; lose and gain a life once each; save, reload, nothing counted twice. |
| **5. Getting around** | Ice, supercharge as Crash Dash, tornado glide matched to Spyro's glide, water as scuba, flight and speedways as the biplane, skateboarding as the jet-board. One at a time. | One real task of each kind completed as Crash. |
| **6. The whole game** | Portals, loads, cutscenes, saves paired between the two games, hand-back for the other characters, a launcher, a release. | Start a new game from the launcher and play through the first home world without touching anything but the controller. |

## 7. Rules for working on it

- Only the builds named in `common/session.h`. An address is a fact about
  one build.
- A game is changed only from its own thread, and only at a point where it
  is known to be between things.
- Crash has online leaderboards for its time trials. The mod never runs
  them, and the Crash bridge will refuse score uploads while it is active.
- Saves of both games are backed up before anything that could write to
  them is tried.
- Nothing taken from either game is committed: no assets, no dumps. Places
  in the exes (offsets) are, as in the sibling project.
- What is claimed to work was seen to work in the real games, and
  `docs/PHASEn.md` says how.
