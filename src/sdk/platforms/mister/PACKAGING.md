# openfpgaOS SDK — MiSTer packaging & deployment

How a game is built into a per-game bundle, published, installed, and booted on
MiSTer. This is the **per-game / update-safe** model — the only supported
MiSTer distribution shape. See [`README.md`](README.md) for the artifact list
and the firmware slot contract.

## The idea

- The **core is game-agnostic** and installed **once** (`_Computer/OpenfpgaOS.rbf`
  + `games/OpenfpgaOS/boot.rom`). It knows nothing about which games exist.
- Each game ships a **self-contained bundle** that drops into
  `games/OpenfpgaOS/`.
- On-card state is split across **three volumes with independent update
  lifecycles**, so an engine update can never overwrite a player's saves or wads.

### The three volumes

| Volume | `.mgl` mount | Path on card | Holds | Lifecycle |
|---|---|---|---|---|
| **S0 `boot.vhd`** | `type=s index=0` (games-relative) | `games/OpenfpgaOS/<Game>/boot.vhd` | read-only shell: `/<Game>/common` = `bank.ofsf` + injected IWADs. **No engine ELF** (`--no-elf`). | update-replaceable |
| **S1 `<Game>.vhd`** | `type=s index=1` (**absolute**) | `saves/OpenfpgaOS/<Game>.vhd` | writable: per-instance `cfg`s + `slot_0..9.sav`, each a 256 KB contiguous preallocated slot | **seeded once, never overwritten** |
| **loose `<elf>`** | `type=f index=2` (F-load) | `games/OpenfpgaOS/<Game>/<elf>` (e.g. `doom.elf`) | the engine; DMA'd to ELF staging → firmware serves it as slot 3 | **swap this one file to update the engine** |

S1 lives under the Downloader-**reserved** `saves/` tree (not `games/`), and the
`.mgl` mounts it by **absolute** path, precisely so a Downloader re-sync or a
manual reinstall never touches it.

## 1. Host packaging — `make package TARGET=mister`

`package` depends on `game-dist`, which depends on the built `app.elf`. Two
sub-phases run in order.

**game-dist** — build the artifacts into `build/mister/<game>/`:

```
mkgame.sh --no-elf --saves-out <Game>.saves.vhd …  →  boot.vhd (S0) + <Game>.saves.vhd (S1)
mkmgl.sh <Game> <inis_dir> <stage>                 →  one 4-line <Inst>.mgl per instance ini
cp app.elf         →  <Game>/<elf>                 (loose engine)
cp <inis>/*.ini    →  <Game>/                      (loose, F-loaded per instance)
setup.sh (GAME baked in)  →  <Game>/setup.sh       (device-side installer, shipped in the bundle)
cp external_files.csv     →  <Game>/               (reference copy; the real consumer is mkdb.py)
```

- `mkgame.sh` drives **`mkimage.c`** — a host build of the *same FatFs* the
  firmware mounts, so the on-disk format is exact. `--no-default-nv` (passed to
  **both** images) strips mkimage's legacy root slots, so S0 gets **zero** nv
  slots (pure read-only) and S1 gets **only** the per-instance
  `nv=/<Game>/<Inst>/…` slots. Every nv slot is `f_expand`'d to **256 KB
  contiguous** and zero-filled — the firmware's power-cut-safe save contract
  refuses any save file that isn't present at full size, and never rewrites FAT
  metadata mid-save, which is why contiguous prealloc is mandatory.
- Instances are enumerated from the `*.ini` files (each with `[os]
  GAME`/`INSTANCE`/`ELF`), validated by `validate-ini.sh`. There is no
  `INSTANCES` variable.
- Default profile ships boot.vhd with **no IWADs** (small, redistributable).
  `--with-wads` bakes them in for a self-contained dev/test image.

**package** — turn the staged tree into deliverables:

```
scripts/package.sh <game>   →  releases/mister/<game>-v<ver>.zip   (the whole staged tree)
mkdb.py …                   →  releases/mister/<game>.json.zip      (Downloader custom DB)
                            +  releases/mister/<game>.downloader.ini (the [db_id] snippet)
```

`mkdb.py` walks the staged tree computing each local file's real MD5 + size,
folds in `external_files.csv` (freeware wads with maintainer-pinned
url/size/md5), and writes a **genuine ZIP holding one `<game>.json`** (not gzip,
despite the `.json.zip` name; byte-reproducible via a fixed 1980 Zip timestamp).
`boot.vhd` (and any `--install-once` glob) is marked `overwrite:false` so a
re-sync can't clobber injected wads; everything else defaults to
`overwrite:true` so an engine/asset update lands.

### Artifacts produced

| Artifact | Where | Role |
|---|---|---|
| `<game>-v<ver>.zip` | `releases/mister/` | the per-game bundle a user unzips into `games/OpenfpgaOS/` |
| `<game>.json.zip` | `releases/mister/` | the **only** file the MiSTer Downloader consumes |
| `<game>.downloader.ini` | `releases/mister/` | the `[db_id]` + `db_url` snippet the user pastes into `downloader.ini` |

## 2. Publishing

Host the staged tree at `MISTER_DB_BASE_URL` and `<game>.json.zip` at
`MISTER_DB_URL`. `MISTER_DB_ID` (e.g. `thinkelastic/openfpgaos-doom`) is the
Downloader database key — **never change it once published**.

## 3. Installing on the device

**Once, for both channels:** install the game-agnostic core from the **openfpgaOS
core release** — `make package/release TARGET=mister` in the openfpgaOS repo emits
`openfpgaos-core-v<ver>.zip` (`OpenfpgaOS.rbf → /media/fat/_Computer/`, `boot.rom →
games/OpenfpgaOS/`) plus a Downloader DB. Game repos no longer vendor the core
(`make sdk` stops at `os.bin`).

**Manual:**

1. Unzip the bundle into `games/OpenfpgaOS/`.
2. Copy the commercial IWADs you own into `<Game>/wads/`.
3. Run `setup.sh` (Scripts menu, or `bash <Game>/setup.sh <Game>`): it **seeds
   `saves/OpenfpgaOS/<Game>.vhd` only if absent** (existence check → your saves
   are safe on re-run), then loop-mounts boot.vhd rw and copies each wad into
   `/<Game>/common` (idempotent — skips same-size files).
4. Pick a `.mgl` from the menu.

**Downloader:** paste the `[db_id] db_url=…` snippet into
`/media/fat/downloader.ini` and sync. The Downloader fetches the freeware wads
(from their own absolute URLs in `external_files.csv`) and the rest of the tree
straight to the SD card.

## 4. Runtime — the 4-line `.mgl`

```
<rbf>_Computer/OpenfpgaOS</rbf>
delay 1  s  idx 0  <Game>/boot.vhd                          → mount S0 (read tree)
delay 2  s  idx 1  /media/fat/saves/OpenfpgaOS/<Game>.vhd   → mount S1 (writable saves)
delay 3  f  idx 2  <Game>/<elf>                             → F-load engine → ELF staging (sets ELF_LOADED). NO reset.
delay 4  f  idx 1  <Game>/<inst>.ini                        → F-load ini → triggers ini_reset (SoC reboot)
```

On the reset the OS reads the staged ini's `[os] GAME/INSTANCE`, derives
`common_root=/<Game>/common` (reads on S0) and `instance_root=/<Game>/<Inst>`
(writes pinned to S1), forces the app slot to 3, and reads the engine from ELF
staging. **Order is correctness-critical:** the ELF must stage (idx 2) *before*
the ini (idx 1) fires the reset, or the slot-3 read has nothing to load. Path
rule (MiSTer `menu.cpp`): a path starting with `/` is verbatim, otherwise it is
prefixed with `HomeDir()=/media/fat/games/OpenfpgaOS` — which is why boot.vhd,
the ELF and the ini are games-relative but the S1 saves path is absolute.

## Developer fast-path — `copy.sh` (not a release step)

For iterating against a *live* MiSTer over ssh. **`make copy TARGET=mister`**
(and `make copy-app`) map to the `game` mode — the update-safe engine swap:

| Command | Pushes |
|---|---|
| `copy.sh game <Game> <GameElf> <elf> [ip]` | the loose `games/OpenfpgaOS/<Game>/<GameElf>` (the F-loaded engine), written atomically — **boot.vhd (your wads) and saves are untouched**; no Main-stop / loop-mount. Reload the core to run it. |
| `copy.sh core [ip]` | `boot.rom` (+ the rbf only if you still vendor it — see note) |

Target host: positional arg > `$MISTER_IP` > `mister.local`. Core dir is
`/media/fat/_Computer`.

> The core bitstream is **no longer vendored** into game repos — `make sdk`
> (openfpgaOS) stops at `os.bin`, and the core is installed once from the
> **openfpgaOS core release** (§ *Installing on the device*). So `copy.sh core`
> now pushes only `boot.rom`.

## Script reference

| Script | Stage | What it does |
|---|---|---|
| `mkgame.sh` | host | builds the S0 boot.vhd + S1 saves.vhd pair |
| `mkimage.c` (`.mkimage`) | host | FatFs image builder; `--no-default-nv`, `--list` (verify contiguity) |
| `mkmgl.sh` | host | emits the 4-line `.mgl` launchers (one per instance) |
| `validate-ini.sh` | host | gate: enforces `[os] GAME/INSTANCE/ELF` in each ini |
| `package.sh` | host | zips the staged tree + authors `INSTALL.txt` |
| `mkdb.py` | host | generates the Downloader `<game>.json.zip` + ini snippet |
| `setup.sh` | **device** | seeds S1 saves once + injects user wads into S0 |
| `copy.sh` | dev | ssh/scp fast-path deploy / in-place ELF hot-swap |

## Invariants & gotchas

- **`_Computer/OpenfpgaOS.rbf`** — the `.mgl` `<rbf>` line hardcodes this, so the
  core must live here or the launchers won't find it.
- **`MISTER_DB_ID` is permanent** once published (keys `downloader.ini`).
- **Never recreate S1 save/config files** with ordinary tools — they must stay
  256 KB and contiguous. Use `mkimage.c` / `--list` to verify.
- **`.json.zip` is a real ZIP** with one JSON member (not gzip).
- **`external_files.csv` is freeware-only and host-side** — commercial IWADs are
  never listed; the user supplies those and `setup.sh` injects them. Rows with
  any `TODO_`/invalid field are skipped with a warning — hashes are never
  invented.
- **Engine update = replace the loose `<elf>`**; boot.vhd and saves are untouched.
