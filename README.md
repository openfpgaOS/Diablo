# Diablo for Analogue Pocket

Play the original **Diablo** (and **Hellfire**) natively on the Analogue Pocket.
This is a port of [DevilutionX](https://github.com/diasurgical/devilutionx) running
on an FPGA core with a 100 MHz RISC-V soft CPU — not emulation of an old computer,
but the real game engine built for this hardware. It renders at the game's native
640×480, plays the full-motion cutscenes, and uses the Pocket's hardware audio
mixer for sound.

**You need a copy of Diablo.** The game data is not included.

---

## What you need

- An Analogue Pocket (updated firmware) and a microSD card.
- `DIABDAT.MPQ` from your own Diablo copy — the original CD, or the
  [GOG release](https://www.gog.com/game/diablo).
- `devilutionx.mpq` — free engine assets from the
  [DevilutionX 1.5.5 release](https://github.com/diasurgical/devilutionx/releases/tag/1.5.5)
  (it is inside any platform download).
- *(Optional, for Hellfire)* `hellfire.mpq`, `hfmonk.mpq`, `hfmusic.mpq`,
  `hfvoice.mpq` from the Hellfire expansion.

## Installing

1. Copy the core onto the SD card: merge this release's `Cores/` and `Assets/`
   folders into the matching folders at the root of the card.
2. Put your game data into **`Assets/diablo/common/`** on the card:

   ```
   Assets/diablo/common/
   ├── DIABDAT.mpq        <- from your Diablo CD or GOG install
   ├── devilutionx.mpq    <- from the DevilutionX release
   ├── fonts.mpq          <- optional (extra language fonts)
   └── hellfire / hf*.mpq <- optional (Hellfire expansion)
   ```

3. In the Pocket menu open **openFPGA → Diablo**. Hellfire owners get a
   separate **Hellfire** entry in the same core.

First boot shows the Blizzard logo and the intro movie — press **A** or
**Start** to skip them.

## Controls

Designed to feel like the other openfpgaOS cores (Duke3D, Quake): **A acts,
B interacts, shoulders tap for potions and hold for chords.**

### In the game

| Input | Action |
|---|---|
| **D-pad** | Move |
| **A** | Attack, talk, place the held item |
| **B** | Open chests and doors, pick up items |
| **X** | Cast the active spell |
| **Y** | Close open panels; with nothing open: open the spell list |
| **L1** (tap) | Drink a health potion |
| **R1** (tap) | Drink a mana potion |
| **L1 + X / A / B / Y** | Cast quick spell 1 / 2 / 3 / 4 |
| **R1 + X** | Inventory |
| **R1 + Y** | Character sheet |
| **R1 + B** | Spellbook |
| **R1 + A** | Quest log |
| **Select** | Toggle the automap |
| **Start** | Game menu (save, options, quit) |

**Setting up quick spells:** press **Y** to open the spell list, highlight a
spell with the d-pad, then hold **L1** and press a face button to bind it to
that slot. From then on the same `L1 + face` chord casts it instantly.

**Virtual mouse** (rarely needed, e.g. some shop interactions): hold
**Select** and use the **d-pad** to move the cursor; **Select + L1** is a
left click, **Select + R1** a right click.

### In the menus

| Input | Action |
|---|---|
| **D-pad** | Navigate |
| **A** | Confirm |
| **B** | Back |
| **A / Start** | Skip a cutscene |

## Saves and settings

Saved games and options live on the SD card. The Pocket writes them back
**when you leave the core through the Analogue menu** (or when the device
sleeps) — exit that way after playing. Powering off abruptly can lose
progress made since the last save-and-exit.

- Save in game via **Start → Save Game**. Ten single-player slots are available.
- Options changed in **Start → Options** persist the same way.

## Tips for new players

- **Potions are on the shoulders for a reason.** Keep your belt stocked;
  L1 (health) will save your life more than any spell.
- Town portals home: buy scrolls early; cast them with X like any spell.
- The **automap (Select)** overlays the dungeon; it is the fastest way to
  find stairs.
- If music or speech ever go quiet during a long load, that is normal —
  audio resumes automatically.

## Troubleshooting

| Message / symptom | Meaning |
|---|---|
| `Failed to open DIABDAT.MPQ` (error at boot) | `DIABDAT.mpq` is missing from `Assets/diablo/common/` |
| Missing menu art / crash at title | `devilutionx.mpq` is missing — it is required |
| `Missing: fonts.mpq` in the log | Harmless; only needed for some languages |
| Hellfire entry quits to menu | Hellfire needs all four of `hellfire.mpq`, `hfmonk.mpq`, `hfmusic.mpq`, `hfvoice.mpq` |
| Settings did not stick | Exit the core through the Pocket menu so the card is written |

## Building from source

This repository contains the full source: the DevilutionX engine with the
Pocket port layer (`src/diablo/`), the SDK (`src/sdk/`), and the prebuilt
core bitstream/OS (`runtime/`).

```bash
make setup            # one-time: RISC-V toolchain
cd src/diablo && make # builds app.elf and stages build/diablo/
make copy             # copy to an SD card at $POCKETDEV
```

Developer documentation for the underlying SDK lives in
[GETTING_STARTED.md](GETTING_STARTED.md) and [docs/SDK_README.md](docs/SDK_README.md).

## Credits

- [DevilutionX](https://github.com/diasurgical/devilutionx) — the engine this
  port is built on.
- Blizzard North — the original game. Diablo game data remains the property
  of Blizzard Entertainment; bring your own copy.
