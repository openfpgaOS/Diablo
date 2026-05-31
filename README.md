# Diablo — Analogue Pocket port

[DevilutionX](https://github.com/diasurgical/devilutionX) (a from-scratch reimplementation of Diablo I) running on the Analogue Pocket via [openfpgaOS](https://github.com/openfpgaOS/openfpgaOS).

**Target:** VexiiRiscv `rv32imafc` @ 100 MHz · 64 MB SDRAM · 640×480 8-bit indexed framebuffer · 48 kHz stereo · 32-voice HW mixer (currently using one streaming voice).

---

## Status

| Working | Caveats |
|---|---|
| Boot · intro · character creation · gameplay loop | — |
| Town + dungeon level loading, combat, NPCs, inventory, character sheet | — |
| Palette across cutscenes, fades and gameplay | Blit-source tracked, 256-entry HW push |
| Mono PCM WAV SFX → HW mixer voices (OS 1 kHz ISR-serviced) | Survives main-thread stalls |
| Music + non-conforming SFX (stereo/mp3) → Aulib SW mixer → HW voice 31 | Fed only between frames; see below |
| Save slots 0–9 (openfpgaOS nonvolatile slots 10–19, 256 KB each) | Launcher commits save on game exit |

Known limitations:
- Frame rate caps below 60 fps in heavy renders — the CPU has no D extension, so soft-FP doubles are expensive. The SDL shim has been audited to use single-precision floats throughout.
- The SW-mixed **music** ring is fed only from the main thread (on present/poll), so a single frame longer than the ring depth can briefly starve music. Faster frames are the main lever — try `make HOT_OPT='-O2 -ffp-contract=fast'` (see Build) to compile the hot audio/render/decompress TUs at `-O2` and measure on hardware. Long level loads deliberately suspend music (not replay) via `OpenFpgaProgressAudioGuard`.
- Mono PCM 8/16-bit WAV SFX already play on the HW mixer voices (see `ParseWavForHwMixer`/`SetChunk` in `soundsample.cpp`) and are serviced by the OS 1 kHz ISR, so they don't depend on the main-thread pump; only stereo, mp3, or otherwise non-conforming SFX still run on the Aulib SW path.

---

## Controller layout

The 12 physical inputs (D-pad×4, A/B/X/Y, L1, R1, Select, Start) use **Select** for panel/map/menu shortcuts and **Start** for quick spell hotkeys. L1 and R1 are direct potion buttons.

| Action | Button / Combo |
|---|---|
| Move character | D-Pad |
| Attack / Talk / Pickup / Confirm | A |
| Select spell / Back | B |
| Pickup items / Open chests and doors / Use item | X |
| Cast spell | Y |
| Use health potion | L1 |
| Use mana potion | R1 |
| Character Sheet | Select + L1 or Select + Left |
| Inventory | Select + R1 or Select + Right |
| Toggle Automap / Map | Select + Down |
| Game Menu | Select + Up or Start + Select |
| Quest Log | Select + X (square-equivalent face button) |
| Spell Book | Select + A (cross-equivalent face button) |
| Quick spell hotkeys | Start + A/B/X/Y |

In panels and menus, D-Pad navigates, A confirms, B backs out, and X performs the contextual item action.

### Where this lives in code

- Modifier flags: `src/diablo/devilutionx/Source/controls/game_controls.cpp` — `PadHotspellMenuActive` (Start) and `PadMenuNavigatorActive` (Select) suppress normal menu/movement handling while combos are active.
- Default Padmapper bindings: `src/diablo/devilutionx/Source/diablo.cpp` `InitPadmapActions()`.
- The Padmapper INI is rebindable; the table above describes the defaults.

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
