# Getting Started

Build games for the Analogue Pocket in C or C++. Five minutes from clone to running code.

## 1. Clone

```bash
git clone https://github.com/openfpgaOS/openfgpaSDK.git
cd openfgpaSDK
```

## 2. Setup

```bash
make setup
```

This detects your OS and installs the RISC-V toolchain. Supported platforms:

| OS | Toolchain package |
|----|-------------------|
| Arch / Manjaro / EndeavourOS | `riscv64-elf-gcc` (pacman) |
| Ubuntu / Debian / Pop!_OS | `gcc-riscv64-unknown-elf` (apt) |
| Fedora / Nobara | `gcc-riscv64-linux-gnu` (dnf) |
| openSUSE | `cross-riscv64-gcc14` (zypper) |
| macOS | `riscv64-elf-gcc` (Homebrew) |
| NixOS | `pkgsCross.riscv64.buildPackages.gcc` |
| Windows (MSYS2) | `mingw-w64-ucrt-x86_64-riscv64-unknown-elf-gcc` |

Setup offers to install automatically, or you can install manually and re-run.

## 3. Create your app

```bash
make core
```

Follow the prompts (name, author). This creates `src/mygame/` with:
- `Makefile` — self-contained build with copy, package, and PC test targets
- `main.c` — hello world stub to start from
- `instance.json` — maps your app's files to data slots (the only config you maintain)

## 4. Build

```bash
cd src/mygame
make
```

Builds `mygame.elf` — a RISC-V binary that runs on openfpgaOS.

## 5. Copy

Insert your Pocket's SD card and:

```bash
make copy
```

Auto-detects the SD card, copies the openfpgaOS runtime + your app, and creates the right directory structure. Eject, boot the Pocket, and your app appears in the menu.

## 6. Test on desktop (optional)

If you have SDL2 installed:

```bash
make test
./app_pc
```

Same code, runs in a window on your computer. Useful for quick iteration.

---

## Project layout

After `make core`, your app directory looks like:

```
openfgpaSDK/
├── src/
│   ├── mygame/              ← YOUR code
│   │   ├── Makefile
│   │   ├── main.c
│   │   └── instance.json    ← your app's data slot mapping
│   ├── sdk/                 ← SDK (don't edit)
│   │   ├── include/         ← openfpgaOS API (of.h, of_video.h, ...)
│   │   ├── musl/            ← bundled musl C library + linker script
│   │   ├── platforms/       ← platform templates & copy scripts
│   │   │   └── pocket/      ← Analogue Pocket target
│   └── apps/                ← bundled demo apps (reference code)
├── dist/sdk/                ← SDK core configs (SDK-owned, auto-deployed)
├── runtime/                 ← FPGA bitstream, OS binary, loader
└── scripts/                 ← setup, scaffolding, packaging
```

## What you maintain vs. what the SDK owns

| Yours | SDK-owned (updated via `git pull`) |
|-------|-------------------------------------|
| `src/mygame/main.c` (your code) | `src/sdk/` (headers, build rules) |
| `src/mygame/instance.json` | `dist/sdk/` (core.json, data.json, audio.json, ...) |
| | `runtime/` (bitstream, os.bin, loader) |
| | `src/sdk/platforms/` (templates, copy scripts) |

Core JSON configs (data.json, audio.json, video.json, etc.) are SDK-owned and deployed directly from `dist/sdk/`. When the SDK updates these files, you get the changes automatically with `git pull` — no regeneration step needed.

## Your Makefile targets

From `src/mygame/`:

| Command | What it does |
|---------|-------------|
| `make` | Build `mygame.elf` |
| `make debug` | Build, push via UART, stream console |
| `make copy` | Copy to Pocket SD card |
| `make package` | Create distributable ZIP |
| `make test` | Test on desktop (SDL2) |
| `make clean` | Remove build artifacts |

## instance.json

This is the only config file you edit. It maps your app's files to data slots:

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [
            { "id": 1, "filename": "os.bin" },
            { "id": 2, "filename": "mygame.elf" },
            { "id": 10, "filename": "mygame.sav" }
        ]
    }
}
```

To add data files (up to 4), add entries for slots 3-6:

```json
{ "id": 3, "filename": "music.mod" },
{ "id": 4, "filename": "sprites.dat" }
```

Place the data files in your app directory alongside `main.c`. They get copied to the SD card automatically.
Slot 7 is reserved for an optional `.ofsf` SoundFont bank. Slot 8 is SDK/system shared config; do not use it for app data or saves.

## Writing your app

Edit `src/mygame/main.c`:

```c
#include "of.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    of_video_init();

    /* 256-color palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | (i << 8) | i);

    while (1) {
        of_input_poll();
        uint8_t *fb = of_video_surface();

        /* draw your frame here */

        of_video_flip();
        usleep(16000);  /* ~60 fps */
    }
}
```

Add more `.c` or `.cpp` files — they're picked up automatically.

## What's available

- **Video:** 320x240, 6 color modes (8/4/2-bit indexed, RGB565/555/RGBA5551), double-buffered
- **Audio:** 48 kHz stereo PCM, 32-voice hardware PCM mixer, sample-based MIDI playback
- **Input:** D-pad, face buttons, shoulders, triggers, analog sticks, 2 players
- **Files:** Standard `fopen`/`fread`/`fwrite` with 4 data slots
- **Saves:** 10 persistent save slots (256 KB each)
- **libc:** `printf`, `malloc`, `memcpy`, `sinf`, `qsort`, ... — full C standard library
- **C++:** Classes, templates, `new`/`delete`, `iostream` (no exceptions/RTTI)

See [README.md](README.md) for the full API reference.

## Multiplatform

Apps target a platform. The default is `pocket` (Analogue Pocket). Platform-specific logic — copy scripts, directory layout, JSON format — lives in `src/sdk/platforms/<target>/`.

```bash
make core --target pocket     # Analogue Pocket (default)
# make core --target mister   # MiSTer (coming soon)
```

Your C code is the same across all platforms. Only the copy and packaging differ.

## Updating the SDK

When the SDK gets updated (new API features, core config changes):

```bash
git pull                      # or: git fetch sdk-upstream && git merge sdk-upstream/main
make clean && make            # rebuild
```

Core configs, runtime binaries, and SDK headers update automatically. Your app's `main.c` and `instance.json` are yours — they never conflict.

## Next steps

- Browse `src/apps/` for example code (raytracer, Celeste, MIDI player, ...)
- Run `make` from `src/apps/` to build all demo apps
- See [README.md](README.md) for the full API reference, UART development, and advanced topics
