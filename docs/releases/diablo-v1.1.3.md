# Diablo for Analogue Pocket 1.1.3

Release candidate — 2026-11-20.

## Changes

This release is about holding 30 frames per second in normal play.

- Frames are paced on the display's vertical blank: one frame every two
  refreshes. Before, frames were shown as soon as they were ready and stayed
  on screen for one, two or three refreshes irregularly. Pacing adds up to
  one refresh of input latency.
- Streamed music no longer stalls a frame. Music is read ahead in the
  background into a 256 KB buffer; before, every 32 KB of music cost a
  blocking SD read inside the frame loop.
- World drawing is cheaper: each dungeon tile is lit once per light level
  and reused, short rows are copied a word at a time, and floor triangles
  use copies matched to their screen alignment.
- Music mixing takes about 40% less CPU time. The mixed output is
  bit-identical.
- Side-panel backgrounds (inventory, character, spell book, quest log) are
  drawn from a decoded copy. The first time a panel opens, building that
  copy can make one frame long.
- Pathfinding looks up visited positions by index and remembers each
  tile's walkability within one search; gamepad targeting no longer
  allocates a list node per tile; monster target search and lighting skip
  work that cannot change the result.
- Settings → Graphics → Show FPS now shows displayed frames per second and
  the longest time any frame stayed on screen in the last second. Steady
  play reads `30.0 FPS 33ms`.

The picture is unchanged: replayed demos are pixel-identical to the previous
renderer (1,281 frames over three demos for the last set of changes).

`os20.rbf_r` and `os.bin` are the same files as in 1.1.2. Only `diablo.elf`
and the version in `core.json` differ.

## What was measured

Frame costs were measured in a simulated os20 system with a stand-in
kernel and an assumed 15 ms per SD read, not on a Pocket. In that model,
displayed frame rate over 36 frames per scene:

| Scene | Displayed frames per second |
| --- | --- |
| Hell, walking | 30.0 |
| Town | 30.0 |
| Hell, idle next to lava | 30.0 |
| Inventory, character panel and map open | 30.0 |
| Cathedral | 30.0 |
| Catacombs, cursor held on an unreachable spot | 29.2 |
| Caves | 30.0 |

Hell next to lava has almost no margin left, and holding the cursor on an
unreachable spot still misses frames. Heavy combat was not among the final
measured scenes.

## Installation

Merge the ZIP's `Cores`, `Assets`, and `Platforms` folders into the SD card
root. Keep the existing `Saves` folder and game MPQs. `DIABDAT.mpq` and the
optional Hellfire MPQs must come from your own game copy. The candidate
includes the redistributable `devilutionx.mpq` engine assets.

## Verification

- The five host regression suites pass, including a new one that compares
  the audio fast path with the previous mixer bit for bit.
- A test build of this source with the frame counter switched on at boot
  was booted and played on a Pocket. The released executable is the same
  source built with the counter off by default; that exact file was not run
  on a Pocket. No long-session soak was made.
- The background music reads were exercised in simulation and in that play
  session only. Not checked: opening the Pocket menu while music is being
  read, and memory headroom over long sessions. If music stutters or the
  game hangs where 1.1.2 did not, reinstall 1.1.2 and report it.
- MiSTer is not part of this release; the music read-ahead is switched off
  on that platform.

The os20 image is configured at 100 MHz and does not close main-clock setup
timing: seed 18 has setup -0.734 ns and worst hold +0.071 ns.

## Final release checks

To rebuild and verify a release, run from the repository root:

```sh
python3 tools/check_diablo_regressions.py
python3 tools/check_release_packaging.py
make package CORE=diablo TARGET=pocket PERF=0 GPU=0 BITMANIP=0
python3 tools/check_diablo_package.py --require-current-build
```

The package check writes payload hashes and build status alongside the ZIP.
