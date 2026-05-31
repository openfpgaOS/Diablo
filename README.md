# Diablo — Analogue Pocket port

[DevilutionX](https://github.com/diasurgical/devilutionX) (a from-scratch reimplementation of Diablo I) running on the Analogue Pocket via [openfpgaOS](https://github.com/openfpgaOS/openfpgaOS).

**Target:** VexiiRiscv `rv32imafc` @ 100 MHz · 64 MB SDRAM · native 640×480 @ ~60 Hz, 8-bit indexed framebuffer · 48 kHz stereo · 32-voice HW mixer (currently using one streaming voice).

---

## Status

| Working | Caveats |
|---|---|
| Boot · intro · character creation · gameplay loop | — |
| Town + dungeon level loading, combat, NPCs, inventory, character sheet | — |
| Palette across cutscenes, fades and gameplay | Blit-source tracked, 256-entry HW push |
| Mono PCM WAV SFX → HW mixer voices (OS 1 kHz ISR-serviced) | Survives main-thread stalls |
| Music + non-conforming SFX (stereo/mp3) → Aulib SW mixer → HW voice 31 | ~341 ms ring; fed between frames **and** during SD reads (idle-hook) |
| Save slots 0–9 (openfpgaOS nonvolatile slots 10–19, 256 KB each) | Launcher commits save on game exit |

Known limitations:
- Frame rate caps below 60 fps in heavy renders — the CPU has no D extension, so soft-FP doubles are expensive. The SDL shim has been audited to use single-precision floats throughout.
- The SW-mixed **music** ring (~341 ms) is filled from the main thread on present/poll, **and** topped up during blocking SD reads via the OS file-read idle-hook (`of_aulib_idle_pump`), so music no longer stalls while streaming assets or loading. A purely CPU-bound burst longer than the ring with no SD wait in between (e.g. heavy `bzip2` level-gen decompress) can still briefly affect it — `make HOT_OPT='-O2 -ffp-contract=fast'` (see Build) shrinks those. Long level-load *screens* deliberately suspend music (not replay) via `OpenFpgaProgressAudioGuard`. The Aulib resampler keeps its source position bounded, so long looping tracks don't drift in pitch.
- Mono PCM 8/16-bit WAV SFX already play on the HW mixer voices (see `ParseWavForHwMixer`/`SetChunk` in `soundsample.cpp`) and are serviced by the OS 1 kHz ISR, so they don't depend on the main-thread pump; only stereo, mp3, or otherwise non-conforming SFX still run on the Aulib SW path.

---

## Controller layout

The Pocket's inputs — D-Pad, A/B/X/Y, L, R, Select, Start — drive a Diablo-friendly scheme: the four face buttons handle combat directly, **hold Start** opens panels, and **hold Select** is the quick-spell / mouse-cursor layer.

> The Pocket has no L2/R2 triggers and no clickable sticks, so DevilutionX's stock bindings for Inventory/Character (triggers) and Automap (stick-click) are physically unreachable here. They're rebound onto **hold-Start** combos below, gated by `#if defined(OPENFPGAOS)` in `InitPadmapActions()`.

**In the game**

| Action | Button |
|---|---|
| Move | D-Pad |
| Attack / talk to NPCs / lift & place items | **B** |
| Cast the active spell | **X** |
| Open chests & doors / pick up items | **Y** |
| Open the Speedbook (choose active spell) | **A** |
| Use health potion | **L** |
| Use mana potion | **R** |

**Open a panel — hold Start, then:**

| Panel | + Button |
|---|---|
| Character | **Y** |
| Inventory | **X** |
| Spellbook | **B** |
| Quest log | **A** |
| Automap | **L** |

**Modifiers & menus**

| Action | Combo |
|---|---|
| Game menu (save / options / quit) | **Select + Start** |
| Quick-spell ring (assign & cast hotkeyed spells) | hold **Select** |
| Mouse cursor (move / left- / right-click) | hold **Select** + D-Pad / **L** / **R** |

In panels and menus the D-Pad navigates, **A** confirms, and **B** backs out.

### Where this lives in code

- Modifier flags: `src/diablo/devilutionx/Source/controls/game_controls.cpp` — `PadMenuNavigatorActive` (**Start**) and `PadHotspellMenuActive` (**Select**) suppress normal handling while a combo is held.
- Default Padmapper bindings: `src/diablo/devilutionx/Source/diablo.cpp` `InitPadmapActions()`; the Pocket panel rebinds (→ hold-Start) are gated `#if defined(OPENFPGAOS)`.
- Bindings are rebindable via the Padmapper INI; the tables above are the built-in defaults.

---

## Build

From the repository root:

```bash
make APP=diablo                   # build src/diablo/app.elf
make copy APP=diablo              # copy core + app to Pocket SD card
make debug APP=diablo             # build + PHDP UART deploy (DevKey required)
```

Or from `src/diablo/`:

```bash
make
make copy
make debug
```

The whole tree builds at `-Os` by default. To A/B-test a faster build of the hot
audio / render / asset-decompress translation units (everything that bounds frame
time, which in turn bounds the music-underrun window):

```bash
make HOT_OPT='-O2 -ffp-contract=fast'   # -O2 on the hot TUs only; rest stays -Os
```

`HOT_OPT` is empty by default, so the normal build is byte-for-byte unchanged.
Measure on hardware — the in-order core has a small I-cache, so the win isn't
guaranteed.

Required game assets on the SD card under `Assets/diablo/common/`:

| File | Source |
|---|---|
| `diabdat.mpq` | Original Diablo I install (you provide) |
| `devilutionx.mpq` | DevilutionX asset patch (bundled in `dist/`) |

The Pocket loader uses save slots **10–19** (10 × 256 KB) for character saves and slot **9** for config; both are nonvolatile CRAM-backed slots that survive power-off.

---

## Reference

- Upstream DevilutionX — https://github.com/diasurgical/devilutionX
- openfpgaOS — https://github.com/openfpgaOS/openfpgaOS
- Engine docs — [`src/diablo/devilutionx/docs/`](src/diablo/devilutionx/docs/)
