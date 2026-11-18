# Diablo for Analogue Pocket 1.1.1

Release candidate — 2026-11-17.

## Changes

- Fix the very low sound volume reported in issue #1. The core's hardware
  mixer divided the final mix by 16; it now divides by 4 (+12 dB). Music,
  speech and movie audio were additionally scaled to half level in the port
  and now play at the same level as sound effects (+6 dB).
- Update the bundled runtime to openfpgaOS v0.9.1 (os20 image and kernel).
  The os20 image tracks the open SDRAM row per bank and reads each video
  line in shorter bursts, so CPU memory accesses wait less behind scanout.
  The kernel's time queries and memory-map scans are cheaper.

Expect everything to be noticeably louder at the same Pocket volume setting.
Several loud sounds at once can now reach full scale and are limited there.

## Installation

Merge the ZIP's `Cores`, `Assets`, and `Platforms` folders into the SD card
root. Keep the existing `Saves` folder and game MPQs. `DIABDAT.mpq` and the
optional Hellfire MPQs must come from your own game copy. The candidate
includes the redistributable `devilutionx.mpq` engine assets.

Install the whole ZIP: the new `os20.rbf_r`, `os.bin` and `diablo.elf` were
tested together and should not be mixed with files from 1.1.0.

## Verification

The mixer change passes the hardware-mixer and software-mixer benches, which
compare output levels against an independent reference. The memory changes
pass the SDRAM, arbiter, scanout and GPU write-path simulations, and the
kernel changes pass the host memory-map test and a comparison of the new
time divide against the 64-bit one on 100 million random inputs.

The os20 image is configured at 100 MHz and does not close main-clock setup
timing: seed 25 estimates 94.66 MHz at the worst corner, with setup -0.564 ns
and worst hold +0.074 ns. Version 1.1.0 shipped an image with -0.498 ns.

The packaged files were booted on a Pocket and played with the new audio
level. No long-session soak, frame-rate measurement or clipping measurement
was made.

## Final release checks

To rebuild and verify a release, run from the repository root:

```sh
python3 tools/check_diablo_regressions.py
python3 tools/check_release_packaging.py
make package CORE=diablo TARGET=pocket PERF=0 GPU=0 BITMANIP=0
python3 tools/check_diablo_package.py --require-current-build
```

The package check writes payload hashes and build status alongside the ZIP.
