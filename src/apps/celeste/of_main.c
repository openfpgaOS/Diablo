//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * celeste — Celeste Classic port (SDL2-native shim into openfpgaOS HAL)
 *
 * Canonical example of:
 *   - Porting a third-party C game to Pocket via the SDK's SDL2 shim
 *     using SDL2-native APIs (SDL_Init + SDL_CreateWindow +
 *     SDL_GetWindowSurface + SDL_UpdateWindowSurface — *not* the
 *     deprecated SDL_SetVideoMode/SDL_Flip pair).
 *   - SDL_SetPaletteColors against the surface's palette object,
 *     and SDL_GetKeyboardState returning a SCANCODE-indexed array
 *     (`kbstate[SDL_SCANCODE_LEFT]`, not `kbstate[SDLK_LEFT]`).
 *   - SDL_Mixer for SFX init, mapping onto the hardware mixer.
 *   - Wiring the on-board controller into SDL keyboard events under
 *     the hood — the game's main loop sees standard SDL2 input and
 *     doesn't know it's running on a Pocket.
 *   - Centred 2x scale into the 320x240 HW framebuffer (OFS_X/OFS_Y
 *     below): doubles a 128x128 PICO-8 surface into 256x256 with
 *     letterboxing.
 *
 * The pure game logic lives in celeste.c / celeste.h (MIT, lemon32767)
 * and is unmodified upstream.  This file is the only Pocket-aware
 * code: the SDL shim, the PICO-8 palette mapping, and the audio
 * + tilemap loaders.
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "of_codec.h"
#include "of_mixer.h"

#include "celeste.h"
#include "tilemap.h"

/* ======================================================================
 * SFX bank — bundled `celeste-sfx.bin` at slot:5
 *
 * The 23 PICO-8 SFX Celeste actually uses are concatenated into a
 * single binary at slot:5.  Format (little-endian):
 *
 *   uint32  magic = 'CSFX' (0x58465343)
 *   uint32  version = 1
 *   uint32  num_slots = 64
 *   slots[64]:
 *     uint32  offset    (file byte offset, 0 = empty / unused index)
 *     uint32  length    (raw .wav bytes)
 *   ... wav data, 4-byte aligned ...
 *
 * tools/build_celeste_sfx.sh builds this bundle from upstream's
 * data/snd*.wav files.  At startup we mmap the file (just keep the
 * blob in BSS-allocated RAM), parse each populated slot through
 * of_codec_parse_wav, and stash a Mix_Chunk for it.  CELESTE_P8_SFX
 * then resolves to a 1-line Mix_PlayChannel.  Music is not bundled —
 * see the CELESTE_P8_MUSIC stub below.
 * ====================================================================== */

#define CELESTE_SFX_MAGIC  0x58465343u
#define CELESTE_SFX_NUM    64

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_slots;
    struct {
        uint32_t offset;
        uint32_t length;
    } slot[CELESTE_SFX_NUM];
} celeste_sfx_header_t;

static Mix_Chunk *sfx_bank[CELESTE_SFX_NUM];
static uint8_t   *sfx_blob;       /* whole file kept around while WAV chunks parse */
static int        sfx_loaded;     /* count of clips successfully decoded */

static int16_t read_s16le(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ======================================================================
 * Globals — SDL2 owns the window, we just keep the surface handle for
 * the per-frame draw helpers that need pixels[].
 * ====================================================================== */

static SDL_Window  *window = NULL;
static SDL_Surface *screen = NULL;

#define PICO8_W 128
#define PICO8_H 128

/* Scale=2: render 128x128 at 2x into 320x240, centered with clipping */
static const int scale = 2;
#define OFS_X ((320 - PICO8_W * 2) / 2)   /* 32 */
#define OFS_Y ((240 - PICO8_H * 2) / 2)   /* -8 */

/* ======================================================================
 * Palette — PICO-8 16 colors, stored as SDL_Color for getcolor()
 * ====================================================================== */

static const SDL_Color base_palette[16] = {
    {0x00,0x00,0x00,0xFF}, {0x1D,0x2B,0x53,0xFF},
    {0x7E,0x25,0x53,0xFF}, {0x00,0x87,0x51,0xFF},
    {0xAB,0x52,0x36,0xFF}, {0x5F,0x57,0x4F,0xFF},
    {0xC2,0xC3,0xC7,0xFF}, {0xFF,0xF1,0xE8,0xFF},
    {0xFF,0x00,0x4D,0xFF}, {0xFF,0xA3,0x00,0xFF},
    {0xFF,0xEC,0x27,0xFF}, {0x00,0xE4,0x36,0xFF},
    {0x29,0xAD,0xFF,0xFF}, {0x83,0x76,0x9C,0xFF},
    {0xFF,0x77,0xA8,0xFF}, {0xFF,0xCC,0xAA,0xFF},
};
static SDL_Color palette[16];

static inline Uint8 getcolor(int idx) {
    SDL_Color c = palette[idx & 15];
    for (int i = 0; i < 16; i++)
        if (base_palette[i].r == c.r && base_palette[i].g == c.g && base_palette[i].b == c.b)
            return (Uint8)i;
    return (Uint8)(idx & 15);
}

static void ResetPalette(void) {
    memcpy(palette, base_palette, sizeof palette);
}

/* ======================================================================
 * Sprite / font data (embedded BMP)
 * ====================================================================== */

static uint8_t gfx_data[128 * 64];
static uint8_t font_data[128 * 85];

static int load_bmp_data(const uint8_t *bmp, int bmp_len, uint8_t *out,
                         int w, int h, int bpp) {
    if (bmp_len < 54) return -1;
    int data_offset = bmp[10]|(bmp[11]<<8)|(bmp[12]<<16)|(bmp[13]<<24);
    int bmp_w = bmp[18]|(bmp[19]<<8)|(bmp[20]<<16)|(bmp[21]<<24);
    int bmp_h = bmp[22]|(bmp[23]<<8)|(bmp[24]<<16)|(bmp[25]<<24);
    int top_down = 0;
    if (bmp_h < 0) { bmp_h = -bmp_h; top_down = 1; }
    if (bmp_w != w || bmp_h != h) return -2;
    int row_bytes = (bpp == 4) ? (w+1)/2 : (w+7)/8;
    int row_stride = (row_bytes + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h-1-y);
        const uint8_t *row = bmp + data_offset + src_y * row_stride;
        for (int x = 0; x < w; x++) {
            uint8_t pixel;
            if (bpp == 4) { uint8_t b = row[x/2]; pixel = (x&1) ? (b&0x0F) : (b>>4); }
            else { pixel = (row[x/8] >> (7-(x&7))) & 1; }
            out[y*w+x] = pixel;
        }
    }
    return 0;
}

#include "gfx_data.h"

static void LoadData(void) {
    printf("loading gfx...");
    load_bmp_data(gfx_bmp, gfx_bmp_len, gfx_data, 128, 64, 4);
    printf("done\nloading font...");
    load_bmp_data(font_bmp, font_bmp_len, font_data, 128, 85, 1);
    printf("done\n");
}

/* Decode one populated SFX slot from the bank into a Mix_Chunk.  Mirrors
 * the shim's Mix_LoadWAV but reads from a memory blob instead of a file
 * — every clip in the bundle is itself a complete WAV, so we hand each
 * one to of_codec_parse_wav unchanged. */
static Mix_Chunk *make_chunk_from_wav(const uint8_t *wav, uint32_t size) {
    of_codec_result_t result;
    if (of_codec_parse_wav(wav, size, &result) < 0) return NULL;

    uint32_t num_samples = result.pcm_len;
    if (result.bits_per_sample == 16) num_samples /= 2;
    if (result.channels == 2)         num_samples /= 2;

#ifdef OF_PC
    int16_t *pcm_s16 = (int16_t *)malloc(num_samples * sizeof(int16_t));
    if (!pcm_s16) return NULL;

    /* PC uses the real SDL_mixer path opened as signed 16-bit mono. */
    if (result.bits_per_sample == 16) {
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = read_s16le(result.pcm + (i * step * 2));
    } else {
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = (int16_t)(((int)result.pcm[i * step] - 128) << 8);
    }

    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!chunk) {
        free(pcm_s16);
        return NULL;
    }
    chunk->allocated = 1;
    chunk->abuf = (Uint8 *)pcm_s16;
    chunk->alen = num_samples * sizeof(int16_t);
    chunk->volume = MIX_MAX_VOLUME;
#else
    int16_t *pcm_s16 = (int16_t *)of_mixer_alloc_samples(num_samples * sizeof(int16_t));
    if (!pcm_s16) return NULL;

    /* Pocket SFX live in the SDRAM sample pool used by the hardware mixer. */
    if (result.bits_per_sample == 16) {
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = read_s16le(result.pcm + (i * step * 2));
    } else {
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_s16[i] = (int16_t)(((int)result.pcm[i * step] - 128) << 8);
    }

    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!chunk) return NULL;
    chunk->pcm_s16 = pcm_s16;
    chunk->sample_count = num_samples;
    chunk->sample_rate = result.sample_rate;
    chunk->volume = MIX_MAX_VOLUME;
#endif
    return chunk;
}

static void LoadSFX(void) {
    FILE *f = fopen("slot:5", "rb");
    if (!f) {
        printf("[celeste] no slot:5 (sfx bank); audio silent\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < (long)sizeof(celeste_sfx_header_t)) {
        fclose(f); printf("[celeste] sfx bank: file too small\n"); return;
    }

    sfx_blob = (uint8_t *)malloc((size_t)sz);
    if (!sfx_blob) { fclose(f); printf("[celeste] sfx bank: OOM\n"); return; }
    fread(sfx_blob, 1, (size_t)sz, f);
    fclose(f);

    celeste_sfx_header_t *hdr = (celeste_sfx_header_t *)sfx_blob;
    if (hdr->magic != CELESTE_SFX_MAGIC ||
        hdr->version != 1 ||
        hdr->num_slots != CELESTE_SFX_NUM) {
        printf("[celeste] sfx bank: bad header (magic=%08X v=%u n=%u)\n",
               (unsigned)hdr->magic, (unsigned)hdr->version,
               (unsigned)hdr->num_slots);
        free(sfx_blob); sfx_blob = NULL;
        return;
    }

    for (int i = 0; i < CELESTE_SFX_NUM; i++) {
        uint32_t off = hdr->slot[i].offset;
        uint32_t len = hdr->slot[i].length;
        if (off == 0 || len == 0) continue;
        if (off + len > (uint32_t)sz) continue;
        sfx_bank[i] = make_chunk_from_wav(sfx_blob + off, len);
        if (sfx_bank[i]) sfx_loaded++;
    }
    printf("[celeste] sfx bank: %d/%d clips loaded\n",
           sfx_loaded, CELESTE_SFX_NUM);
}

/* ======================================================================
 * Drawing — render at 2x scale with centering, directly to HW FB
 *
 * All functions take PICO-8 coordinates (0..127) and apply SCALE + OFS.
 * screen->pixels points directly at the 320x240 hardware framebuffer.
 * ====================================================================== */

static inline void Xblit(int sprite, int x, int y, int flipx, int flipy) {
    int sx = (sprite % 16) * 8;
    int sy = (sprite / 16) * 8;
    uint8_t *fb = (uint8_t *)screen->pixels;

    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            int gx = sx + (flipx ? (7-px) : px);
            int gy = sy + (flipy ? (7-py) : py);
            if (gy >= 64) continue;
            uint8_t pixel = gfx_data[gy * 128 + gx];
            if (pixel == 0) continue;
            uint8_t c = getcolor(pixel);
            /* Write scale×scale block */
            for (int ssy = 0; ssy < scale; ssy++) {
                int fy = OFS_Y + (y+py) * scale + ssy;
                if ((unsigned)fy >= 240) continue;
                for (int ssx = 0; ssx < scale; ssx++) {
                    int fx = OFS_X + (x+px) * scale + ssx;
                    if ((unsigned)fx >= 320) continue;
                    fb[fy * 320 + fx] = c;
                }
            }
        }
    }
}

static void p8_rectfill(int x0, int y0, int x1, int y1, int col) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    Uint8 c = getcolor(col);
    SDL_Rect rc = { OFS_X + x0*scale, OFS_Y + y0*scale,
                    (x1-x0+1)*scale, (y1-y0+1)*scale };
    SDL_FillRect(screen, &rc, c);
}

static void p8_line(int x0, int y0, int x1, int y1, int col) {
    Uint8 c = getcolor(col);
    uint8_t *fb = (uint8_t *)screen->pixels;
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx-dy;
    for (;;) {
        for (int ssy = 0; ssy < scale; ssy++) {
            int fy = OFS_Y + y0*scale + ssy;
            if ((unsigned)fy >= 240) continue;
            for (int ssx = 0; ssx < scale; ssx++) {
                int fx = OFS_X + x0*scale + ssx;
                if ((unsigned)fx >= 320) continue;
                fb[fy*320+fx] = c;
            }
        }
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void p8_print(const char *str, int x, int y, int col) {
    Uint8 c = getcolor(col);
    uint8_t *fb = (uint8_t *)screen->pixels;
    for (const char *s = str; *s; s++) {
        int ch = (*s) & 0x7F;
        int fx = (ch%16)*8, fy = (ch/16)*8;
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 4; px++)
                if (fy+py < 85 && font_data[(fy+py)*128+fx+px])
                    for (int ssy = 0; ssy < scale; ssy++) {
                        int dy = OFS_Y + (y+py)*scale + ssy;
                        if ((unsigned)dy >= 240) continue;
                        for (int ssx = 0; ssx < scale; ssx++) {
                            int dx = OFS_X + (x+px)*scale + ssx;
                            if ((unsigned)dx >= 320) continue;
                            fb[dy*320+dx] = c;
                        }
                    }
        x += 4;
    }
}

static void p8_circfill(int cx, int cy, int r, int col) {
    if (r <= 0) { p8_rectfill(cx,cy,cx,cy,col); return; }
    /* Horizontal-line fill for each scanline */
    Uint8 c = getcolor(col);
    int xx = r, yy = 0, err = 1-r;
    while (xx >= yy) {
        p8_rectfill(cx-xx, cy+yy, cx+xx, cy+yy, col);
        p8_rectfill(cx-xx, cy-yy, cx+xx, cy-yy, col);
        p8_rectfill(cx-yy, cy+xx, cx+yy, cy+xx, col);
        p8_rectfill(cx-yy, cy-xx, cx+yy, cy-xx, col);
        (void)c;
        yy++;
        if (err < 0) err += 2*yy+1;
        else { xx--; err += 2*(yy-xx)+1; }
    }
}

static void p8_map(int mx, int my, int tx, int ty, int mw, int mh, int mask,
                   int cam_x, int cam_y) {
    for (int y = 0; y < mh; y++)
        for (int x = 0; x < mw; x++) {
            int tile = tilemap_data[(x+mx)+(y+my)*128];
            int draw = 0;
            if (mask == 0) draw = 1;
            else if (mask == 4 && tile_flags[tile] == 4) draw = 1;
            else if (mask != 4 && tile < (int)(sizeof(tile_flags)/sizeof(*tile_flags))
                     && (tile_flags[tile] & (1<<(mask-1)))) draw = 1;
            if (draw)
                Xblit(tile, tx+x*8-cam_x, ty+y*8-cam_y, 0, 0);
        }
}

/* ======================================================================
 * OSD
 * ====================================================================== */

static char osd_text[200] = "";
static int osd_timer = 0;
static void OSDset(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    osd_timer = 30; va_end(ap);
}
static void OSDdraw(void) {
    if (osd_timer > 0) {
        --osd_timer;
        int x = 4, y = 120 + (osd_timer<10 ? 10-osd_timer : 0);
        int len = (int)strlen(osd_text);
        p8_rectfill(x-2,y-2, x+4*len,y+6, 6);
        p8_rectfill(x-1,y-1, x+4*len-1,y+5, 0);
        p8_print(osd_text, x, y, 7);
    }
}

/* ======================================================================
 * PICO-8 callback
 * ====================================================================== */

static int camera_x, camera_y;
static int enable_screenshake = 1;
static Uint16 buttons_state;

static int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) {
    va_list args; int ret = 0;
    va_start(args, call);
    #define INT_ARG()  va_arg(args, int)
    #define BOOL_ARG() (Celeste_P8_bool_t)va_arg(args, int)

    switch (call) {
    case CELESTE_P8_MUSIC: {
        /* Music is not bundled — would need an OGG decoder added to
         * the SDL_mixer shim, OR transcoded WAV streams ~5 MB.  See
         * the upstream `data/mus*.ogg` if you want to wire it up. */
        INT_ARG(); INT_ARG(); INT_ARG();
    } break;
    case CELESTE_P8_SPR: {
        int spr=INT_ARG(), x=INT_ARG(), y=INT_ARG();
        int cols=INT_ARG(), rows=INT_ARG();
        int fx=BOOL_ARG(), fy=BOOL_ARG();
        (void)cols;(void)rows;
        if (spr>=0) Xblit(spr, x-camera_x, y-camera_y, fx, fy);
    } break;
    case CELESTE_P8_BTN: { int b=INT_ARG(); ret=(buttons_state&(1<<b))!=0; } break;
    case CELESTE_P8_SFX: {
        int idx = INT_ARG();
        if (idx >= 0 && idx < CELESTE_SFX_NUM && sfx_bank[idx])
            Mix_PlayChannel(-1, sfx_bank[idx], 0);
    } break;
    case CELESTE_P8_PAL: {
        int a=INT_ARG(), b=INT_ARG();
        if (a>=0&&a<16&&b>=0&&b<16) palette[a]=base_palette[b];
    } break;
    case CELESTE_P8_PAL_RESET: { ResetPalette(); } break;
    case CELESTE_P8_CIRCFILL: {
        int cx=INT_ARG()-camera_x, cy=INT_ARG()-camera_y;
        int r=INT_ARG(), col=INT_ARG();
        p8_circfill(cx,cy,r,col);
    } break;
    case CELESTE_P8_PRINT: {
        const char *str=va_arg(args,const char*);
        int x=INT_ARG()-camera_x, y=INT_ARG()-camera_y, col=INT_ARG()%16;
        p8_print(str,x,y,col);
    } break;
    case CELESTE_P8_RECTFILL: {
        int x0=INT_ARG()-camera_x, y0=INT_ARG()-camera_y;
        int x1=INT_ARG()-camera_x, y1=INT_ARG()-camera_y;
        int col=INT_ARG();
        p8_rectfill(x0,y0,x1,y1,col);
    } break;
    case CELESTE_P8_LINE: {
        int x0=INT_ARG()-camera_x, y0=INT_ARG()-camera_y;
        int x1=INT_ARG()-camera_x, y1=INT_ARG()-camera_y;
        int col=INT_ARG();
        p8_line(x0,y0,x1,y1,col);
    } break;
    case CELESTE_P8_MGET: {
        int tx=INT_ARG(), ty=INT_ARG();
        ret=tilemap_data[tx+ty*128];
    } break;
    case CELESTE_P8_CAMERA: {
        if (enable_screenshake) { camera_x=INT_ARG(); camera_y=INT_ARG(); }
    } break;
    case CELESTE_P8_FGET: {
        int tile=INT_ARG(), flag=INT_ARG();
        ret=tile<(int)(sizeof(tile_flags)/sizeof(*tile_flags))
            &&(tile_flags[tile]&(1<<flag))!=0;
    } break;
    case CELESTE_P8_MAP: {
        int mx=INT_ARG(),my=INT_ARG(),tx=INT_ARG(),ty=INT_ARG();
        int mw=INT_ARG(),mh=INT_ARG(),mask=INT_ARG();
        p8_map(mx,my,tx,ty,mw,mh,mask,camera_x,camera_y);
    } break;
    }
    va_end(args);
    return ret;
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    /* SDL2-native init: subsystems explicit, then create the window
     * and pull its surface.  On the Pocket the window is the 320x240
     * HW framebuffer regardless of the requested size, but writing it
     * SDL2-style means the same code runs unchanged on a desktop
     * SDL2 build (useful for the SDL2-on-PC test path). */
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    window = SDL_CreateWindow("Celeste",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              320, 240,
                              SDL_WINDOW_SHOWN);
    screen = SDL_GetWindowSurface(window);

    /* In SDL2, palette colours go through SDL_SetPaletteColors against
     * the surface's palette object — there is no SDL_PHYSPAL/LOGPAL
     * any more; the indexed surface is itself the LUT. */
    SDL_SetPaletteColors(screen->format->palette,
                         base_palette, 0, 16);
    ResetPalette();

    Mix_OpenAudio(22050, AUDIO_S16SYS, 1, 1024);

    printf("now loading...\n");
    LoadData();
    LoadSFX();

    Celeste_P8_set_call_func(pico8emu);

    void *initial_state = SDL_malloc(Celeste_P8_get_state_size());
    if (initial_state) Celeste_P8_save_state(initial_state);
    Celeste_P8_set_rndseed(SDL_GetTicks());
    Celeste_P8_init();
    printf("ready\n");

    int running = 1;
    while (running) {
        /* Drain SDL events first so the keyboard-state array sees
         * any KEYDOWN/KEYUP that happened since the last poll.  On
         * Pocket the shim folds controller input into key state too. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
        }

        /* SDL2: keyboard state is a SCANCODE-indexed array — use
         * SDL_SCANCODE_* (physical key), NOT SDLK_* (virtual key /
         * keycode), or you'll index into garbage on real SDL2. */
        const Uint8 *kbstate = SDL_GetKeyboardState(NULL);

        /* Hold Y (F9) for ~1 second to reset the run. */
        static int reset_timer = 0;
        if (initial_state && kbstate[SDL_SCANCODE_F9]) {
            if (++reset_timer >= 30) {
                reset_timer = 0;
                OSDset("reset");
                Celeste_P8_load_state(initial_state);
                Celeste_P8_set_rndseed(SDL_GetTicks());
                Celeste_P8_init();
            }
        } else {
            reset_timer = 0;
        }

        buttons_state = 0;
        if (kbstate[SDL_SCANCODE_LEFT])  buttons_state |= (1 << 0);
        if (kbstate[SDL_SCANCODE_RIGHT]) buttons_state |= (1 << 1);
        if (kbstate[SDL_SCANCODE_UP])    buttons_state |= (1 << 2);
        if (kbstate[SDL_SCANCODE_DOWN])  buttons_state |= (1 << 3);
        if (kbstate[SDL_SCANCODE_Z] || kbstate[SDL_SCANCODE_C])
            buttons_state |= (1 << 4);
        if (kbstate[SDL_SCANCODE_X] || kbstate[SDL_SCANCODE_V])
            buttons_state |= (1 << 5);

        Celeste_P8_update();
        Celeste_P8_draw();
        OSDdraw();

        /* SDL2-native present: SDL_UpdateWindowSurface flips the
         * back buffer to the display.  The shim handles the
         * "clear next buffer + refresh surface->pixels" dance that
         * SDL_Flip used to do explicitly in the SDL 1.2 path. */
        SDL_UpdateWindowSurface(window);
        screen = SDL_GetWindowSurface(window);

        /* 30 fps cap.  static `frame_start` keeps the previous-frame
         * timestamp across iterations without leaking through globals. */
        static unsigned frame_start = 0;
        unsigned frame_end  = SDL_GetTicks();
        unsigned frame_time = frame_end - frame_start;
        if (frame_time < 33) SDL_Delay(33 - frame_time);
        frame_start = SDL_GetTicks();
    }

    if (initial_state) SDL_free(initial_state);
    Mix_CloseAudio();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
