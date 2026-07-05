# openfpgaOS SDK — MiSTer platform

Packaging for the MiSTer (DE10-Nano / SuperStation One) target. Unlike
the Pocket's APF JSON manifests, MiSTer needs no metadata: discovery is
by filename and folder convention.

The game-agnostic **core** (`OpenfpgaOS.rbf` + `boot.rom`) is released once
from the openfpgaOS repo; each **game** ships a self-contained per-game
bundle — a read-only `boot.vhd` shell (S0), a writable `<Game>.saves.vhd`
image (S1), a loose F-loaded engine ELF, and one `.mgl` launcher per
instance. The full build → package → publish flow (`make package
TARGET=mister`) is documented in **[PACKAGING.md](PACKAGING.md)**; this
file covers the artifacts and the firmware slot contract they satisfy.

## Artifacts

| File | Where on the MiSTer | What |
|---|---|---|
| `OpenfpgaOS.rbf` | `/media/fat/_Computer/` | the core bitstream (`make build TARGET=mister` in openfpgaOS); game-agnostic, installed once |
| `boot.rom` | `/media/fat/games/OpenfpgaOS/` | the OS kernel (`os.bin`, `TARGET=mister`), auto-loaded by the framework at core start |
| `<Game>/boot.vhd` | `/media/fat/games/OpenfpgaOS/<Game>/` | read-only **S0** shell: `bank.ofsf` + the user's injected wads (no engine ELF) |
| `<Game>.saves.vhd` | `/media/fat/saves/OpenfpgaOS/` | writable **S1** image: per-instance `/config/*` + `/saves/slot_*.sav` (256 KB `f_expand` slots) |
| `<Game>/<GameElf>` | `/media/fat/games/OpenfpgaOS/<Game>/` | loose engine ELF, F-loaded by the `.mgl` — an update swaps just this one file |
| `<Instance>.mgl` | `/media/fat/games/OpenfpgaOS/` | one launcher per instance (mounts S0 + S1, F-loads engine + ini) |

## Disk image layout (the slot→path contract)

The OS resolves APF-style slot ids to fixed paths inside the mounted
images (`openfpgaOS/src/firmware/os/targets/mister/file.c`). Names resolve
writable-instance-first (**S1 → S0**), so the saves image shadows the
read-only shell:

```
/os.ini                  slot 2   instance config ([os] GAME/INSTANCE/ELF/ARGS)
<engine>.elf             slot 3   the app engine (a loose F-loaded file in the per-game model)
/bank.ofsf               slot 7   MIDI soundfont (optional)
/config/shared.cfg       slot 8   SDK shared config   (256 KB, preallocated)
/config/<game>.cfg       slot 9   per-game settings   (256 KB, preallocated)
/saves/slot_0..9.sav     10–19    save slots          (256 KB, preallocated)
/assets/*                20+      app data + wads, registered by directory scan —
                                  apps open these by filename as on Pocket
```

**Do not re-create the save/config files with ordinary tools.** They are
preallocated contiguously (FatFs `f_expand`) so the firmware can persist
saves by overwriting data clusters in place, never touching FAT metadata
— that is the power-cut safety guarantee. `mkgame.sh` (via the host-native
`mkimage` tool) does this correctly; a plain file copy may fragment them.

Filenames are limited to 23 characters (the OS registry's
`FILE_SLOT_NAME_MAX`), same as on Pocket.

## Usage

```sh
# --- Per-game engine update (primary; update-safe) ---
# scp a freshly-built engine ELF to the loose, F-loaded file
# games/OpenfpgaOS/<Game>/<GameElf>, written atomically. boot.vhd (the
# user's wads) and the saves volume are left untouched (no Main-stop /
# loop-mount). `make copy TARGET=mister` (== `make copy-app`) maps here.
./copy.sh game Doom doom.elf path/to/doom.elf 192.168.1.42

# --- Core bring-up (dev) ---
# push the game-agnostic runtime artifacts synced by `make sdk`
# (os.bin -> boot.rom, and the rbf if present). The core normally installs
# from its own release zip; this is the network shortcut for development.
./copy.sh core 192.168.1.42
```

The per-game images are built by `mkgame.sh` (+ `mkmgl.sh` for launchers),
which compile `mkimage.c` on first use — a host-native FAT32 image builder
linked against the same vendored FatFs the firmware mounts with (`fatfs/`,
`FF_USE_MKFS` + `FF_USE_EXPAND` for the host build only). No
mtools/loopback/root required, and the images are bare FAT32 superfloppies
(no MBR, `FM_SFD`) that MiSTer's mount path accepts.

See **[PACKAGING.md](PACKAGING.md)** for the complete build → package →
publish flow.
