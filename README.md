# openfpgaOS SDK

Build games for the [Analogue Pocket](https://www.analogue.co/pocket) in C or C++.

**Hardware:** VexiiRiscv rv32imafc @ 100 MHz, 8 KB I-cache + 32 KB D-cache, 64 MB SDRAM, 320x240 video, 48 kHz stereo audio, 32-voice hardware PCM mixer, and sample-based MIDI playback.

> **New here?** See [GETTING_STARTED.md](GETTING_STARTED.md) — clone to running code in 5 minutes.

## Quick Start

```bash
git clone https://github.com/openfpgaOS/openfgpaSDK.git
cd openfgpaSDK
make setup                    # install RISC-V toolchain
make core                     # create your app (follow the prompts)
cd src/mygame
make                          # build mygame.elf
make copy                   # copy to Pocket SD card
```

### Toolchain

`make setup` detects your OS and offers to install automatically:

- **Arch:** `pacman -S riscv64-elf-gcc`
- **Ubuntu/Debian:** `apt install gcc-riscv64-unknown-elf`
- **Fedora:** `dnf install gcc-riscv64-linux-gnu`
- **macOS:** `brew install riscv64-elf-gcc`
- **NixOS:** `pkgsCross.riscv64.buildPackages.gcc`
- **MSYS2:** `pacman -S mingw-w64-ucrt-x86_64-riscv64-unknown-elf-gcc`

`make core` prompts for your app name and author, then creates `src/<app>/` with a self-contained Makefile, stub code, and instance JSON. Your app gets its own core identity from the start.

---

## Writing Your App

Edit `src/mygame/main.c`:

```c
#include "of.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    of_video_init();

    /* Set up a palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | ((255 - i) << 8) | 128);

    /* Draw to the framebuffer */
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            fb[y * 320 + x] = x ^ y;

    of_video_flip();
    printf("Hello from openfpgaOS!\n");

    /* Main loop */
    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) {
            /* handle button press */
        }
        usleep(16000);  /* ~60 fps */
    }
}
```

### Standard C library

Apps statically link a full C standard library (upstream musl). Include the standard headers:

```c
#include <stdio.h>    // printf, snprintf, sscanf,
                      // fopen, fclose, fread, fwrite, fseek, ftell
#include <stdlib.h>   // malloc, free, calloc, realloc, atoi, atof,
                      // strtol, strtod, qsort, bsearch, rand, abs
#include <string.h>   // memcpy, memset, strlen, strcmp, strdup,
                      // strcat, strtok, memchr, strspn, strcspn, ...
#include <math.h>     // sinf, cosf, sqrtf, powf, logf, atan2f, fabsf, ...
#include <ctype.h>    // toupper, tolower, isalpha, isdigit, isspace, ...
#include <unistd.h>   // usleep, sleep, open, close, read, write, lseek
#include <time.h>     // clock_ms, clock_us, clock_gettime
```

### C++ support

The SDK supports C++ (freestanding, no exceptions, no RTTI). Place `.cpp` files alongside `.c` files and they are compiled automatically.

What works:
- Classes, inheritance, virtual methods
- `operator new` / `delete` (backed by the OS `malloc`/`free`)
- Templates
- Static constructors and destructors (`.init_array` / `.fini_array`)
- All SDK headers are `extern "C"` compatible
- `<iostream>` — `std::cout`, `std::cerr`, `std::cin` (lightweight)

What is **not** available (freestanding environment):
- Exceptions (`-fno-exceptions`)
- RTTI / `dynamic_cast` (`-fno-rtti`)
- The rest of the C++ Standard Library (`<vector>`, `<string>`, `<algorithm>`, etc.)

#### `<iostream>` — cout / cerr / cin

```cpp
#include <iostream>

int main(void) {
    std::cout << "Hello from cout!\n";
    std::cout << "int=" << 42 << " float=" << 3.14f << std::endl;
    std::cerr << "error message\n";

    int n;
    std::cin >> n;                         // reads from fd 0 (stdin / serial)
    std::cout << "you entered: " << n << "\n";
}
```

`std::cout` and `std::cerr` write through `write(1, …)` / `write(2, …)` syscalls — identical to calling `printf`. `std::cin` reads from fd 0 character-by-character; on the Analogue Pocket there is no keyboard, so `cin` is mainly useful when stdin is connected to a serial port or redirected by the host OS.

Supported `operator<<` types: `bool`, `char`, `unsigned char`, `const char*`, `short`, `unsigned short`, `int`, `unsigned int`, `long`, `unsigned long`, `long long`, `unsigned long long`, `float`, `double`, `void*`.

Supported `operator>>` types: `char`, `char*` (one word), `int`, `unsigned int`, `long`, `unsigned long`, `float`, `double`, `bool`.

Example (`main.cpp`):

```cpp
#include "of.h"
#include <stdio.h>
#include <unistd.h>

class Game {
    int score;
public:
    Game() : score(0) {}
    void tick() {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) score++;
    }
    void draw() {
        of_video_clear(0);
        printf("Score: %d\n", score);
        of_video_flip();
    }
};

int main(void) {
    of_video_init();
    Game game;
    while (1) {
        game.tick();
        game.draw();
        usleep(16000);
    }
}
```

You can add custom `CXXFLAGS` in your Makefile before including `sdk.mk`:

```makefile
CXXFLAGS = -std=c++17
```

### PC build

Test on your computer with SDL2:

```bash
make test
./app_pc
```

---

## Porting SDL2 Games

The SDK ships an **SDL2 compatibility layer** so existing 2D SDL games
(DevilutionX, ECWolf, Doom, ScummVM, …) port with little or no source
change. Just `#include <SDL2/SDL.h>` (and `<SDL2/SDL_mixer.h>` for audio) —
there is no extra Makefile wiring: `sdk.mk` auto-links the implementation
(`src/sdk/of_sdl2.c`) into every app, and `--gc-sections` removes it from
apps that don't call any `SDL_*` function, so non-SDL apps pay nothing.

See **`src/apps/sdldemo/`** for a complete, minimal template (8-bit surface
+ palette, colorkey blit, callback audio, input) that builds for both the
Pocket (`make`) and the desktop (`make test`).

**What's implemented** (over the `of_*` HAL, CPU/surface based — no GPU dep):

- **Video** — 8-bit indexed window surface, palette (`SDL_SetPaletteColors`),
  full `SDL_Surface` create/convert/blit (colorkey + clip + scale), software
  `SDL_Renderer`/`SDL_Texture`, and SDL 1.2 `SDL_SetVideoMode`/`SDL_Flip`.
  When the window size matches the active video mode the surface aliases the
  OS triple-buffer (zero-copy present); otherwise present nearest-neighbor
  scales into the framebuffer.
- **Input** — the Analogue Pocket gamepad is exposed three ways at once:
  `SDL_PollEvent` emits **both** `SDL_CONTROLLERBUTTON*`/axis events **and**
  keyboard `SDL_KEYDOWN`/`SDL_KEYUP` events, and `SDL_GetKeyboardState()`
  returns a live keystate. Game-controller and joystick query APIs work too.
- **Audio** — `SDL_AudioCallback` is **auto-pumped** from `SDL_PollEvent` /
  `SDL_Delay` / `SDL_RenderPresent` / `SDL_Flip` (no audio thread needed);
  `SDL_QueueAudio`, `SDL_LoadWAV`, and `SDL_mixer` (SFX via `of_mixer`, MIDI
  music via `of_midi`) are supported.
- **Misc** — `SDL_RWops` (file + memory), timers, threads (run cooperatively),
  mutex/cond/sem (no-ops on the single core), hints, message boxes.

**Tuning knobs** (compile-time `-D…`):

| Define | Effect |
|---|---|
| `OF_SDL_NO_KEYBOARD_EVENTS` | Suppress the keyboard event stream (controller-only games) |
| `OF_SDL_NO_CONTROLLER_EVENTS` | Suppress the controller event stream (keyboard-only games) |

A game that uses **SDL_mixer music** (`Mix_PlayMusic`) also appends
`$(OF_MIDI_SRC)` to `SRCS` (that path pulls in the MIDI engine). The button →
SDL scancode map lives in `of_to_scancode()` in `src/sdk/of_sdl2.c`; tweak it
there for a specific game's controls. Truecolor (RGB) rendering is approximated
to the 8-bit screen palette — the layer is indexed-color first, like the games
it targets.

---

## API Reference

Include `"of.h"` for the entire API, or include individual headers.

### Video — `of_video.h`

320x240 framebuffer with double buffering. Default mode is 8-bit indexed (256-color palette).

```c
of_video_init();                              // Initialize video
uint8_t *fb = of_video_surface();             // Get back buffer (write here)
of_video_flip();                              // Swap front/back buffers
of_video_sync();                              // Wait until flip completes
of_video_vsync();                             // Wait for next vblank (no flip)
of_video_clear(0);                            // Fill back buffer with palette index
of_video_pixel(x, y, color);                  // Set one pixel (bounds-checked)
of_video_flush();                             // Flush D-cache (advanced)
```

**Palette:**

```c
of_video_palette(index, 0x00RRGGBB);          // Set one palette entry (0-255)
of_video_palette_bulk(rgb_array, count);       // Set multiple entries at once
of_video_palette_vga6(vga_triplets, count);    // Convert 6-bit VGA palette (0-63 per channel)
```

**Color modes:**

Six video modes, switched at runtime. Indexed modes use the palette; direct modes encode color per pixel.

```c
of_video_set_color_mode(OF_VIDEO_MODE_8BIT);      // 256 colors, 1 byte/pixel (default)
of_video_set_color_mode(OF_VIDEO_MODE_4BIT);      // 16 colors,  2 pixels/byte
of_video_set_color_mode(OF_VIDEO_MODE_2BIT);      // 4 colors,   4 pixels/byte
of_video_set_color_mode(OF_VIDEO_MODE_RGB565);    // 16-bit direct, 2 bytes/pixel
of_video_set_color_mode(OF_VIDEO_MODE_RGB555);    // 15-bit direct, 2 bytes/pixel
of_video_set_color_mode(OF_VIDEO_MODE_RGBA5551);  // 15-bit + alpha, 2 bytes/pixel

uint16_t *fb16 = of_video_surface16();   // Use for 16-bit modes
```

| Mode | Framebuffer size | Pixels per byte |
|------|-----------------|-----------------|
| 8-bit indexed | 76,800 B | 1 |
| 4-bit indexed | 38,400 B | 2 (low nibble first) |
| 2-bit indexed | 19,200 B | 4 (LSB first) |
| RGB565 | 153,600 B | 0.5 (16-bit per pixel) |
| RGB555 | 153,600 B | 0.5 |
| RGBA5551 | 153,600 B | 0.5 (bit 0 = alpha) |

**Display mode:**

```c
of_video_set_display_mode(0);    // Terminal only (text console)
of_video_set_display_mode(1);    // Framebuffer only (default after of_video_init)
of_video_set_display_mode(2);    // Overlay: white terminal text over framebuffer
```

**Blitting helpers:**

```c
of_blit(dx, dy, w, h, src, src_stride);               // Blit with transparency (pixel 0 = skip)
of_blit_pal(dx, dy, w, h, src, src_stride, offset);   // Blit with palette offset
of_fill_rect(x, y, w, h, color);                       // Solid filled rectangle
of_video_blit_letterbox(src, src_w, src_h);             // Center vertically, black bars
```

### Audio — `of_audio.h`

48 kHz stereo PCM output. `of_audio_write` accepts interleaved signed
16-bit stereo pairs and streams them through a reserved hardware mixer
voice.

```c
of_audio_init();                                      // Initialize audio system
of_audio_write(samples, count);                       // Queue stereo int16_t pairs
int free = of_audio_free();                           // Stereo pairs free in ring buffer
of_audio_stream_open(sample_rate);                    // Gapless mono stream, resampled
of_audio_stream_write(samples, count);                // Write mono int16_t samples
of_audio_stream_close();                              // Stop stream playback
```

### MIDI Playback — `of_midi.h`

Plays Standard MIDI Files (Format 0 and 1) through the sample-based
MIDI engine. Ship a `.ofsf` SoundFont bank in a data slot; the kernel
auto-loads the first bank it finds and exposes it to the MIDI runtime.

```c
of_midi_init();                              // Init sample voice engine
of_midi_play(midi_data, midi_len, 1);        // Play (1 = loop)
of_midi_stop();                              // Stop and silence all
of_midi_pause();                             // Pause
of_midi_resume();                            // Resume
of_midi_set_volume(200);                     // Master volume 0-255
int playing = of_midi_playing();             // Query state
```

`of_midi_play()` installs the MIDI pump on the timer ISR. Do not call
`of_midi_pump()` from the main loop while playback is active.

**Features:** Format 0 + Format 1 (multi-track), velocity-scaled volume, channel volume (CC7), pan (CC10), pitch bend, tempo changes, looping, and `.ofsf` instrument banks. Error codes: `OF_MIDI_OK`, `OF_MIDI_ERR_BAD_HDR`, `OF_MIDI_ERR_FORMAT`, `OF_MIDI_ERR_NO_TRACKS`, `OF_MIDI_ERR_PLAYING`, `OF_MIDI_ERR_NO_BANK`.

**Example (mididemo):**

```c
#include "of.h"
#include <unistd.h>

static uint8_t midi_buf[256 * 1024] __attribute__((aligned(512)));

int main(void) {
    of_file_slot_register(3, "music.mid");
    FILE *f = fopen("music.mid", "rb");
    uint32_t n = fread(midi_buf, 1, sizeof(midi_buf), f);
    fclose(f);

    of_midi_init();
    of_midi_play(midi_buf, n, 1);     // loop

    while (1) {
        usleep(1000);
    }
}
```

### Audio Mixer — `of_mixer.h`

32-voice hardware PCM mixer with automatic resampling to 48 kHz. Native
input is signed 16-bit mono PCM stored in the SDRAM mixer sample pool.
The 8-bit API accepts signed 8-bit mono PCM and expands it to 16-bit.

```c
of_mixer_init(32, OF_MIXER_OUTPUT_RATE);             // 32 voices, 48 kHz output
int16_t *pcm = of_mixer_alloc_samples(count * 2);    // SDRAM sample pool
int voice = of_mixer_play((const uint8_t *)pcm, count, rate, pri, vol);
of_mixer_set_volume(voice, 200);                     // Volume: 0-255
of_mixer_set_pan(voice, 128);                        // 0=left, 128=center, 255=right
of_mixer_set_loop(voice, loop_start, loop_end);      // Forward loop
of_mixer_stop(voice);                                // Stop one voice
of_mixer_stop_all();                                 // Stop all voices
int active = of_mixer_voice_active(voice);           // 1 if playing, 0 if done
```

`of_mixer_pump()` is a compatibility no-op on current firmware. The
`of_mixer_set_bidi()` and `of_mixer_set_filter()` entry points remain
for older source compatibility, but current hardware ignores them.

### Input — `of_input.h`

Two controllers with d-pad, face buttons, shoulders, triggers, and analog sticks.

```c
of_input_poll();                          // Read hardware (call once per frame)

/* Player 1 */
if (of_btn(OF_BTN_A))          { ... }   // Held this frame
if (of_btn_pressed(OF_BTN_A))  { ... }   // Just pressed (edge)
if (of_btn_released(OF_BTN_A)) { ... }   // Just released (edge)

/* Player 2 */
if (of_btn_p2(OF_BTN_START))          { ... }
if (of_btn_pressed_p2(OF_BTN_START))  { ... }
```

**Button constants:** `OF_BTN_UP`, `DOWN`, `LEFT`, `RIGHT`, `A`, `B`, `X`, `Y`, `L1`, `R1`, `L2`, `R2`, `L3`, `R3`, `SELECT`, `START`

**Full state (sticks, triggers):**

```c
of_input_state_t state;
of_input_state(0, &state);            // Player 0
int16_t lx = state.joy_lx;           // Left stick X: -32768..32767
int16_t ly = state.joy_ly;           // Left stick Y
uint16_t lt = state.trigger_l;       // Left trigger: 0..65535

of_input_set_deadzone(4000);          // Stick deadzone (default: 0)
```

### Timer — `of_timer.h` / `<time.h>` / `<unistd.h>`

100 MHz hardware timer. Time queries via `<time.h>`, delays via `<unistd.h>`.

```c
#include <time.h>
uint32_t ms = clock_ms();             // Milliseconds since boot
uint32_t us = clock_us();             // Microseconds since boot

#include <unistd.h>
usleep(100);                          // Sleep 100 microseconds
usleep(16000);                        // Sleep 16 ms (~60 fps frame time)
sleep(1);                             // Sleep 1 second
```

**Periodic timer interrupt** (advanced — runs in interrupt context):

```c
of_timer_set_callback(my_func, 60);   // Call my_func at 60 Hz
of_timer_stop();                      // Disable callback
```

### File I/O — `of_file.h`

Apps register filenames at startup, then use standard C file I/O:

```c
/* Register data files (call once at startup) */
of_file_slot_register(3, "game.dat");

/* Then use standard fopen */
FILE *f = fopen("game.dat", "rb");    // Resolves to slot 3
fread(buf, 1, size, f);
fclose(f);

/* Or access slots directly without registration */
FILE *f = fopen("slot:3", "rb");
```

**Low-level (bypasses stdio):**

```c
int n = of_file_read(slot_id, offset, dest, length);   // DMA read from data slot
long sz = of_file_size(slot_id);                         // File size in bytes
```

**Idle hook** — called during DMA waits for background work (e.g., audio pump):

```c
of_set_idle_hook(my_audio_pump);      // Called by OS during bridge waits
of_set_idle_hook(NULL);               // Disable
```

### Save Files

10 persistent save slots (256 KB each), mapped to APF file IDs 10-19.
Slot 8 is reserved for SDK/shared config and is not an app save slot.
Dirty save files are committed through the bridge when closed.

**Preferred: standard C file I/O with the save filename from the instance JSON:**

```c
FILE *f = fopen("MyGame_0.sav", "wb");
fwrite(data, sizeof(data), 1, f);
fclose(f);

FILE *f = fopen("MyGame_0.sav", "rb");
fread(data, sizeof(data), 1, f);
fclose(f);
```

The aliases `"save_0"` through `"save_9"` and `"save:0"` through `"save:9"` remain available for compatibility. The SDK intentionally exposes saves through POSIX file I/O rather than a separate save API.

### Terminal — `of_terminal.h`

40x30 text console with CP437 character set. Useful for debug output.

```c
of_print("Hello\n");                  // Print string
of_print_char('X');                   // Print one character
of_print_clear();                     // Clear screen
of_print_at(col, row);               // Move cursor (0-indexed)
printf("Score: %d\n", score);         // Standard printf works too
```

**Box drawing (CP437, ncurses-compatible names):**

```c
of_print_char(ACS_ULCORNER);  of_print_char(ACS_HLINE);  of_print_char(ACS_URCORNER);
of_print_char(ACS_VLINE);     of_print(" text ");         of_print_char(ACS_VLINE);
of_print_char(ACS_LLCORNER);  of_print_char(ACS_HLINE);  of_print_char(ACS_LRCORNER);
```

Available: single-line (`ACS_VLINE`, `ACS_HLINE`, corners, tees, `ACS_PLUS`), double-line (`ACS_D_*`), block elements (`ACS_BLOCK`, `ACS_CKBOARD`), arrows (`ACS_UARROW`, etc.), symbols (`ACS_BULLET`, `ACS_DEGREE`).

### Tile Engine — `of_tile.h`

Hardware tile layer (64x32 tilemap of 8x8 tiles, 4bpp) plus 64 hardware sprites (8x8, 4bpp).

```c
/* Tile layer */
of_tile_enable(1);                                    // Enable tile layer
of_tile_scroll(scroll_x, scroll_y);                   // Pixel-level scrolling
of_tile_set(col, row, tile_index);                    // Set one tile
of_tile_load_map(map_data, count);                    // Load tilemap
of_tile_load_chr(chr_data, size);                     // Load tile graphics

/* Sprites */
of_sprite_enable(1);                                  // Enable sprite layer
of_sprite_set(id, tile, palette, flip_h, flip_v);     // Configure sprite
of_sprite_move(id, x, y);                             // Position sprite
of_sprite_load_chr(chr_data, size);                   // Load sprite graphics
of_sprite_hide(id);                                   // Hide one sprite
of_sprite_hide_all();                                 // Hide all sprites
```

### Link Cable — `of_link.h`

Inter-device communication for multiplayer:

```c
int ok = of_link_send(data_32bit);         // Send 32-bit word (0=success)
int ok = of_link_recv(&data_32bit);        // Receive 32-bit word (0=success)
uint32_t status = of_link_status();        // Connection status
```

### Interact — `of_interact.h`

Read Pocket menu options (defined in `interact.json`). Up to 64 variables.

```c
uint32_t val = of_interact_get(0);    // Read variable at index 0
```

Variable indices match `interact.json` order. The first 4 are reserved by the SDK (Analogizer, SNAC, video offsets). App-specific options start at index 4.

### Analogizer — `of_analogizer.h`

```c
int enabled = of_analogizer_enabled();    // 1 if Analogizer hardware present
uint32_t state = of_analogizer_state();   // SNAC type, video mode, offsets
```

### Audio Codec — `of_codec.h`

Parse VOC and WAV audio files into raw PCM:

```c
of_codec_result_t result;
of_codec_parse_wav(wav_data, wav_size, &result);
// result.pcm, result.pcm_len, result.sample_rate, result.bits_per_sample, result.channels
```

### LZW Compression — `of_lzw.h`

Build Engine compatible LZW compression:

```c
int32_t compressed_size = of_lzw_compress(in, in_len, out);
int32_t decompressed_size = of_lzw_uncompress(in, comp_len, out);
```

### Cache — `of_cache.h`

For advanced users. Most apps never need this.

```c
of_cache_flush_video();           // Flush D-cache for framebuffer
of_cache_invalidate_icache();     // Invalidate I-cache (after code loading)
```

### BRAM Hot Path — `of_bram.h`

Place performance-critical functions in on-chip BRAM for zero-wait-state execution (~55 KB available). Normal code runs from SDRAM with cache; BRAM code has guaranteed zero-cycle latency.

```c
#include "of.h"

OF_FASTTEXT void inner_loop(void) {
    /* Runs from BRAM — no cache misses */
}

OF_FASTDATA int lookup_table[256];       // Initialized data in BRAM
OF_FASTRODATA const int constants[16];   // Read-only data in BRAM

int main(void) {
    inner_loop();    // Direct call to BRAM address
}
```

The linker places `OF_FASTTEXT` code in BRAM (VMA 0x2000-0xFE00) with load data in SDRAM. The OS copies it to BRAM at app startup. No runtime API needed — just annotate functions.

### Version — `of_version.h`

```c
uint32_t v = of_get_version();     // Runtime API version from kernel
// OF_API_VERSION_MAJOR, OF_API_VERSION_MINOR, OF_API_VERSION_PATCH
```

---

## Instance JSON

Each app has an `instance.json` that maps filenames to data slots. This is the only config file you maintain — all core JSON configs (data.json, audio.json, video.json, etc.) are SDK-owned and deployed automatically.

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": { "id": 666, "select": false },
        "data_slots": [
            { "id": 1, "filename": "os.bin" },
            { "id": 2, "filename": "mygame.elf" },
            { "id": 3, "filename": "music.mod" },
            { "id": 10, "filename": "mygame.sav" }
        ]
    }
}
```

When there's only one instance JSON for your app, the Pocket auto-selects it — no file picker is shown.

### Data Slot Layout

| Slot ID | Name | Purpose |
|---------|------|---------|
| 0 | Game | Instance selector (SDK-owned in data.json) |
| 1 | OS Binary | `os.bin` — loaded by bootloader via DMA |
| 2 | Application | Your app ELF — loaded by OS kernel |
| 3-6 | Data 1-4 | App data files (WAD, GRP, images, audio, etc.) |
| 7 | Sound Bank | Optional `.ofsf` SoundFont bank for MIDI |
| 8 | Shared Config | SDK/system-owned nonvolatile config, 256 KB |
| 10-19 | Save 0-9 | Nonvolatile CRAM0 save slots (256 KB each) |

**Rules:**
- Slot 0 (Game selector) is defined in the SDK's `data.json` — don't add it to your instance
- Slot 8 is reserved by the SDK/system — do not use it for app data or saves
- Save slots use bridge address `0x20100000` (CRAM0 save window) with 256 KB stride
- Place data files in your app directory — copy copies them to the SD card

---

## UART Development (PHDP)

The **Pocket-Host Debug Protocol** streams binaries over UART at 2 Mbaud, bypassing the SD card for rapid iteration. Requires a DevKey cartridge connected via USB-UART adapter.

### Architecture

Two host-side tools in `src/tools/phdp/`:

- **`phdpd`** — background daemon that owns the UART connection and manages protocol state
- **`phdp`** — CLI client that talks to the daemon via Unix socket

### Workflow

```bash
# Start the daemon (once)
phdpd                               # auto-detects /dev/ttyUSB0
phdpd -d /dev/ttyACM0               # or specify device

# Queue files for the next boot
phdp push --slot 1 build/Assets/openfpgaos/common/os.bin
phdp push --slot 2 build/Assets/openfpgaos/common/myapp.elf

# Reboot the core and stream
phdp reset
phdp wait                           # blocks until OS is running
phdp logs                           # tail console output
```

### Protocol phases

1. **Discovery** (250ms) — Pocket broadcasts `EVT_BOOT_ALIVE` over UART. If no host responds, boots from SD.
2. **Override** (200ms per slot) — before loading each data slot, Pocket asks the host. Host responds with `RES_STREAM` (send over UART) or `RES_USE_SD` (load from SD).
3. **Streaming** — host sends `DATA_CHUNK` packets (up to 512B), Pocket ACKs with `REPORT_PROGRESS`. CRC-16/CCITT on every packet.
4. **Monitoring** — after `EVT_EXEC_START`, terminal output is mirrored to UART as raw ASCII.

### CLI commands

| Command | Description |
|---------|-------------|
| `phdp status` | Connection state, queued slots, transfer progress |
| `phdp push --slot N file` | Queue binary for slot N |
| `phdp clear [--slot N]` | Clear queued overrides |
| `phdp reset` | Reboot the RISC-V core |
| `phdp wait` | Block until OS is running |
| `phdp logs [--last N]` | Tail or show last N lines of console output |

### Building PHDP tools

```bash
make tools                          # build phdpd + phdp
cd src/tools/phdp
sudo make install                   # install to /usr/local/bin
```

### Typical dev loop

```bash
make                                # rebuild your app
./scripts/debug.sh src/mygame/mygame.elf
```

`debug.sh` starts the daemon if needed, clears pending slots, pushes the file (auto-detects slot 1 for `os.bin`, slot 2 for app ELFs), resets the core, and streams console output until Ctrl+C.

---

## Memory Map

```
0x00000000 ┌──────────────────────┐
           │ BRAM (32 KB)         │
           │ 0x0000-0x1FFF: OS    │  Boot, trap handler
           │ 0x2000-0x7DFF: App   │  OF_FASTTEXT (~24 KB)
           │ 0x7E00-0x7FFF: Stack │  Trap frame
0x00008000 ├──────────────────────┤
           │                      │
0x10300000 ├──────────────────────┤
           │ OS Kernel (SDRAM)    │  ~128 KB
0x10400000 ├──────────────────────┤
           │ App Code + Data      │  Up to 48 MB
           │ (loaded from ELF)    │
0x13400000 ├──────────────────────┤
           │ App heap / mmap      │
0x13700000 ├──────────────────────┤
           │ Mixer Sample Pool    │  8 MB SDRAM
0x13F00000 ├──────────────────────┤
           │ Runtime reserve      │  Stack / cache-evict area
0x13FFFFFF └──────────────────────┘

0x20100000   CRAM0 bridge save window: Save slots (10 x 256 KB)
0x20380000   CRAM0 bridge shared config slot: 256 KB
```

---

## Multiplatform

The SDK is designed for multiple hardware targets. Platform-specific logic — JSON templates, copy scripts, directory layout — lives in `src/sdk/platforms/<target>/`.

```
src/sdk/platforms/
├── pocket/                  ← Analogue Pocket (current)
│   ├── templates/*.json     ← APF JSON config templates
│   └── copy.sh            ← SD card copy script
└── mister/                  ← MiSTer FPGA (planned)
    ├── templates/            ← MiSTer-specific configs
    └── copy.sh             ← MiSTer copy script
```

Your C code is the same across all platforms. When creating an app:

```bash
make core                               # default: pocket
make core --target mister              # future: MiSTer
```

---

## Makefile Targets

### From your app directory (`src/<app>/`)

| Command | What it does |
|---------|-------------|
| `make` | Build your app |
| `make debug` | Build, push via UART, stream console |
| `make copy` | Copy to Pocket SD card |
| `make package` | Package core into a ZIP |
| `make test` | Test on desktop (SDL2) |
| `make clean` | Remove build artifacts |

### From the demos directory (`src/apps/`)

| Command | What it does |
|---------|-------------|
| `make` | Build all demos |
| `make new APP=demo` | Create a new demo app |
| `make copy` | Copy SDK + demos to SD card |
| `make package` | Package SDK core into a ZIP |
| `make clean` | Remove build artifacts |

### From the repo root

| Command | What it does |
|---------|-------------|
| `make setup` | Install RISC-V toolchain |
| `make core` | Create your app (interactive) |
| `make build` | Build everything |
| `make build APP=<app>` | Build sdk or a specific app |
| `make debug APP=<app>` | Build, push via UART, stream console |
| `make copy` | Copy everything to SD card |
| `make copy APP=<app>` | Copy sdk or a specific app |
| `make tools` | Build PHDP host tools |
| `make package` | Package all cores into ZIPs |
| `make clean` | Remove all build artifacts |

---

## Scripts

| Script | What it does |
|--------|-------------|
| `scripts/setup.sh` | Detects OS, installs RISC-V toolchain |
| `scripts/new.sh` | Creates a new app (Makefile, main.c) |
| `scripts/customize.sh` | Creates a standalone core for distribution (interactive) |
| `scripts/copy.sh` | Copies build/ to Pocket SD card |
| `scripts/debug.sh` | Push binary via UART, reset core, stream output |
| `scripts/package.sh` | ZIPs a core for distribution |

---

## Packaging for Distribution

When your app is ready to ship as its own Pocket menu entry:

### Packaging and distribution

```bash
make                                   # build
./scripts/package.sh MyGame            # creates releases/MyGame-v1.0.0.zip
```

Users extract the ZIP to their SD card root.

---

## Porting Existing Apps

For larger ports (Duke Nukem, Doom, etc.) that carry their own build system:

1. Copy `src/sdk/include/`, `src/sdk/musl/`, and `src/sdk/sdk.mk` into your repo as `sdk/`
2. Copy `src/sdk/app.ld`
3. Write a `posix_shim.c` with app-specific stubs
4. Use `sdk/of_posix.c` for POSIX I/O (`open`/`read`/`write`/`lseek`)
5. Register data files: `of_file_slot_register(3, "game.grp")`

**Important for POSIX I/O:** The kernel uses riscv32 `_llseek` (5-argument convention). Your `lseek()` wrapper must pass `(fd, off_hi, off_lo, &result, whence)` via syscall 62, not the traditional 3-argument form. See `src/sdk/include/of_posix.c` for the reference implementation.

---

## Project Structure

```
openfgpaSDK/
├── Makefile              <- Top-level: build, copy, debug, package
├── GETTING_STARTED.md    <- Quick start guide for developers
├── src/
│   ├── <mygame>/         <- YOUR app (created by make core)
│   │   └── main.c        <- Your code + Makefile
│   ├── apps/             <- Bundled demo apps (SDK-owned)
│   │   ├── bramdemo/     <- BRAM hot-path benchmarking
│   │   ├── celeste/      <- Full game example
│   │   ├── colordemo/    <- Video color mode demo (all 6 modes)
│   │   ├── cray/         <- Real-time C raytracer
│   │   ├── cxxdemo/      <- C++ classes, templates, iostream
│   │   ├── fbdemo/       <- PNG framebuffer display
│   │   ├── interactdemo/ <- Pocket menu variables
│   │   ├── memdemo/      <- memset/memcpy throughput benchmark
│   │   ├── mididemo/     <- Sample-based MIDI playback
│   │   ├── moddemo/      <- MOD/tracker music playback
│   │   ├── savea/        <- Save slot integrity test
│   │   ├── saveb/        <- Save cross-pollution test
│   │   ├── slotdemo/     <- File slot registry display
│   │   ├── testdemo/     <- Kernel test suite (182 assertions)
│   │   ├── triplebuf/    <- Triple-buffer framebuffer demo
│   │   └── wavdemo/      <- WAV audio playback
│   └── sdk/              <- Headers, musl libc, build rules (SDK-owned)
│       ├── include/      <- openfpgaOS API headers
│       ├── musl/         <- bundled musl C library + linker script
│       ├── platforms/    <- Platform templates & copy scripts
│       │   └── pocket/   <- Analogue Pocket target
│       └── pc/           <- SDL2 shim for desktop builds
├── dist/                 <- Static core configs (SD card layout)
│   ├── sdk/              <- SDK core (Cores/, Assets/, Platforms/)
│   └── <mygame>/         <- Your app's core (created by make core)
├── scripts/              <- Build/copy/packaging scripts (SDK-owned)
└── runtime/              <- FPGA bitstream, OS binary, loader (SDK-owned)
```

### What you change vs. what the SDK owns

| Yours (edit freely) | SDK-owned (updated via git pull) |
|---------------------|----------------------------------|
| `src/<mygame>/` (your code) | `src/sdk/` (headers, build rules) |
| `dist/<mygame>/` (your core configs) | `dist/sdk/` (SDK core configs) |
| | `src/apps/` (demo apps) |
| | `runtime/` (bitstream, os.bin) |
| | `scripts/` |

Core JSON configs in `dist/sdk/` are SDK-owned. Your app's configs live in `dist/<mygame>/` (created by `make core`). Both get assembled into `build/` at build time.

---

## Updating the SDK

```bash
git pull                              # or: git fetch sdk-upstream && git merge sdk-upstream/main
make clean && make                    # rebuild
```

SDK-owned files (headers, core configs, runtime, templates) update automatically. Your app source and instance.json are never touched.

---

## Reference

This SDK builds apps for [openfpgaOS](https://github.com/openfpgaOS/openfpgaOS) — a RISC-V operating system running on the Analogue Pocket's Cyclone V FPGA. The openfpgaOS repo is the source of truth for API headers and the OS kernel. See that repo for architecture details, FPGA design, and OS internals.
