# Diablo for Analogue Pocket 1.1.2

Release candidate — 2026-11-18.

## Changes

- Faster CPU in the bundled os20 image: shift instructions complete in one
  pipeline stage instead of two. In the core's CPU test programs this saves
  10.6% of cycles overall and 9.5% to 16% in the shift-heavy ones. Diablo
  draws on the CPU, so it should benefit, but no frame rate was measured.
- The same image computes store-buffer occupancy and operand-bypass flags
  earlier in the pipeline. These do not change cycle counts.

`diablo.elf` and `os.bin` are the same files as in 1.1.1. Only `os20.rbf_r`
and the version in `core.json` differ.

## Installation

Merge the ZIP's `Cores`, `Assets`, and `Platforms` folders into the SD card
root. Keep the existing `Saves` folder and game MPQs. `DIABDAT.mpq` and the
optional Hellfire MPQs must come from your own game copy. The candidate
includes the redistributable `devilutionx.mpq` engine assets.

## Verification

The new CPU passes the core's CPU stress program in simulation, and its
benchmark results match the previous CPU's bit for bit. The interrupt
simulation was not run; interrupts were exercised only on hardware.

The os20 image is configured at 100 MHz and does not close main-clock setup
timing: seed 18 has setup -0.734 ns and worst hold +0.071 ns. Version 1.1.1
shipped an image with -0.564 ns and +0.074 ns, so this release trades 0.17 ns
of setup margin for the faster shifts. It was the best of 41 seeds with a
positive hold margin of this size.

The packaged image was booted on a Pocket and played with the 1.1.1 kernel
and executable. No long-session soak or frame-rate measurement was made. If
this version is less stable than 1.1.1 on your Pocket, reinstall 1.1.1 and
report it.

## Final release checks

To rebuild and verify a release, run from the repository root:

```sh
python3 tools/check_diablo_regressions.py
python3 tools/check_release_packaging.py
make package CORE=diablo TARGET=pocket PERF=0 GPU=0 BITMANIP=0
python3 tools/check_diablo_package.py --require-current-build
```

The package check writes payload hashes and build status alongside the ZIP.
