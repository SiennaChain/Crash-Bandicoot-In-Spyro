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

**Status: nothing playable yet.** The two games start with their bridges in
and talk to each other; nothing of Crash is in Spyro yet. See
`docs/PHASE0.md`.

- `docs/DESIGN.md`: the plan, who owns what, and the phases.
- `docs/PHASE0.md`: the link between the two games. Done: what exists, what
  was checked and how.
- `docs/PHASE1.md`: Spyro's ground under Crash's feet. Under way.

## Needs

Both games on Steam, on Windows, in the builds named in `docs/PHASE0.md`.
To build: Visual Studio with the desktop C++ workload.

```powershell
tools\build.ps1
tools\install.ps1
```

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
