# Diablo for Analogue Pocket 1.1.0

Release candidate — 2026-10-02.

## Changes

- Fix the memory pressure behind issue #6 when reloading the final dungeon
  floor after dying near Diablo, including Diablo played through Hellfire.
  Monster animations now load individually, and reloads release old player
  sprites and active sound buffers before allocating the next level.
- Correct frame pacing in the CPU sleep mode and count audio processing time
  toward SDL sleep deadlines.
- Cache gamma conversion while preserving the original palette colors.
- Handle zero-size writes, read-only memory streams, and failed file seeks
  correctly in the SDL compatibility layer.
- Track header, compiler-option, and SDK library changes during builds.
- Exclude locally staged commercial game MPQs from Pocket release ZIPs.

## Installation

Merge the ZIP's `Cores`, `Assets`, and `Platforms` folders into the SD card
root. Keep the existing `Saves` folder and game MPQs. `DIABDAT.mpq` and the
optional Hellfire MPQs must come from your own game copy. The candidate
includes the redistributable `devilutionx.mpq` engine assets.

## Verification

The host regressions exercise the production loader and cleanup routines with
generated CL2 assets, including 20 successive loads of a complete final-floor
monster roster. They verify decoded pixels, bounded temporary allocation, and
complete cleanup between reloads. They also cover SDL streams, clock wrap,
frame pacing, and gamma results. The regular suite and ASan/UBSan checks pass
with profiling enabled; leak detection was disabled. These are synthetic
checks, not measurements of a real saved game on the Pocket.

For the generated Diablo animations, peak loading allocations dropped from
14,730 KiB before the fix to 7,514 KiB. The full generated final-floor roster
uses 19,780 KiB after loading and peaks at 21,115 KiB during conversion.

## Final release checks

The candidate passes package validation against the current SDK build.
The final-floor death/reload hardware test remains required before publishing.
To rebuild and verify a release, run from the repository root:

```sh
python3 tools/check_diablo_regressions.py
python3 tools/check_release_packaging.py
make package CORE=diablo TARGET=pocket PERF=0 GPU=0 BITMANIP=0
python3 tools/check_diablo_package.py --require-current-build
```

The package check writes payload hashes and build status alongside the ZIP.
Test repeated final-floor death/reloads with that ZIP, including Diablo
played through Hellfire, before publishing.
