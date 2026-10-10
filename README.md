# Crash Bandicoot in Spyro

Play Spyro: Year of the Dragon as Crash Bandicoot: the real Crash from
*Crash Bandicoot N. Sane Trilogy*, simulated by his own game, inside the
world of *Spyro Reignited Trilogy*.

Both games run at once. A small DLL in each lets them talk: Crash's game
owns Crash (how he moves, spins, jumps and dies), Spyro's game owns
everything else (the level, the enemies, the gems, the story), and Crash is
drawn into Spyro's picture. The idea is
[SkyCraft](https://github.com/chasmlol/SkyCraft)'s, and this is a sibling of
[Battlefront II in World at War zombies](https://github.com/SiennaChain/WaW-Zombies-with-SWBF2).

**Status: he can be walked about, and that is all.** With both games
running, Crash stands, runs and jumps on Spyro's ground in Sunrise Spring,
drawn into Spyro's window by his own game, hidden by Spyro's scenery where
it is nearer, with a shadow, steered by Spyro's camera. Nothing in Spyro's
world knows he is there yet: no fighting, no gems, no water, no changing
level (phases 3 to 6 of `docs/DESIGN.md`).

Work stopped here on 2026-10-10, after phase 2, while the other way of
doing it is tried somewhere else: Crash as a character inside Spyro's own
game, without Crash's game running. This repository is left as it stands.

- `docs/DESIGN.md`: the plan, who owns what, and the phases.
- `docs/PHASE0.md`: the link between the two games. Done: what exists, what
  was checked and how.
- `docs/PHASE1.md`: Spyro's ground under Crash's feet. Done.
- `docs/PHASE2.md`: Crash in Spyro's picture. He is in it, moves evenly
  and goes where the stick points on the screen. Not done: seen behind
  something as big as a pillar, and drawn under Spyro's counters.

## Needs

Both games on Steam, on Windows, in the builds named in `docs/PHASE0.md`.
To build: Visual Studio with the desktop C++ workload, and PhysX 3.4's
headers, which are not in this repository. They come with Unreal Engine
4.19's source (the engine Spyro was built on); `tools\build.ps1` finds an
engine installed in the usual place, and `-DCSP_PHYSX_ROOT=...` names any
other (`CMakeLists.txt`).

```powershell
tools\build.ps1
tools\run.ps1 -Install
```

`run.ps1` closes both games if they are running, puts the bridges in, and
starts both, Crash straight into the level he stands in. Then F2 in Spyro
puts Crash there. (`tools\install.ps1` only puts the bridges in, or with
`-Uninstall` takes them out.)

## Layout

| | |
|---|---|
| `protocol/` | What the two games say to each other. One header, used by both. |
| `common/` | Shared by both bridges: the shared block, safe memory reads, hooks, logging. |
| `crash/` | The bridge inside Crash (`xinput9_1_0.dll`). |
| `spyro/` | The bridge inside Spyro (`xinput1_3.dll`). |
| `tools/` | Build, install, and a monitor that shows the link from outside. |
| `third_party/minhook/` | [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4, BSD 2-clause. |

Nothing from either game is in this repository, and neither game's files
are changed. Not affiliated with Activision, Toys for Bob or Vicarious
Visions.
