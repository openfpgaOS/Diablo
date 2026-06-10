//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * sdldemo -- reference/template app for the openfpgaOS SDL2 compatibility
 * layer (<SDL2/SDL.h>, implemented in src/sdk/of_sdl2.c).
 *
 * It is a normal SDL2 program: it compiles unmodified on a desktop against
 * the real SDL2 (`make test`) and on the Pocket against the shim (`make`).
 * No SDL-specific Makefile wiring is needed -- sdk.mk auto-links the shim.
 *
 * Exercises the parts a typical 2D port relies on:
 *   - 8-bit indexed window surface + palette (SDL_SetPaletteColors)
 *   - offscreen SDL_Surface + colorkey blit (SDL_BlitSurface)
 *   - present (SDL_UpdateWindowSurface) and frame pacing (SDL_Delay)
 *   - input via BOTH the keyboard and game-controller event streams
 *   - a callback-driven audio tone (auto-pumped by the shim)
 */
#include <SDL2/SDL.h>

#include <stdint.h>
#include <string.h>

#define DEMO_W 320
#define DEMO_H 240

/* Audio: a simple square-wave tone, toggled by the A button. The shim
 * pumps this callback automatically from SDL_PollEvent/SDL_Delay/present,
 * so no audio thread is required. */
static volatile int g_tone;
static unsigned     g_phase;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	int16_t *out = (int16_t *)stream;
	int frames = len / 4;                 /* stereo signed-16 */
	for (int i = 0; i < frames; i++) {
		int16_t s = 0;
		if (g_tone) { s = (g_phase % 80 < 40) ? 5000 : -5000; g_phase++; }
		out[i * 2 + 0] = s;
		out[i * 2 + 1] = s;
	}
}

int main(void)
{
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);

	SDL_Window  *win    = SDL_CreateWindow("sdldemo", 0, 0,
	                                       DEMO_W, DEMO_H, SDL_WINDOW_SHOWN);
	SDL_Surface *screen = SDL_GetWindowSurface(win);   /* 8-bit indexed */

	/* Palette: 0..63 grayscale ramp (we cycle it), 200 = red marker. */
	SDL_Color pal[256];
	memset(pal, 0, sizeof pal);
	for (int i = 0; i < 64; i++) pal[i].r = pal[i].g = pal[i].b = (Uint8)(i * 4);
	pal[200].r = 255; pal[200].g = 0; pal[200].b = 0;
	SDL_SetPaletteColors(screen->format->palette, pal, 0, 256);

	/* Offscreen sprite: red block on a transparent (colorkey 0) field. */
	SDL_Surface *spr = SDL_CreateRGBSurface(0, 32, 32, 8, 0, 0, 0, 0);
	SDL_SetPaletteColors(spr->format->palette, pal, 0, 256);
	SDL_FillRect(spr, NULL, 0);
	SDL_Rect inner = { 4, 4, 24, 24 };
	SDL_FillRect(spr, &inner, 200);
	SDL_SetColorKey(spr, 1, 0);

	/* Callback audio, started immediately. */
	SDL_AudioSpec want;
	memset(&want, 0, sizeof want);
	want.freq = 22050; want.format = AUDIO_S16SYS; want.channels = 2;
	want.samples = 1024; want.callback = audio_cb;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	SDL_PauseAudioDevice(dev, 0);

	SDL_GameController *pad = SDL_GameControllerOpen(0);
	(void)pad;

	int x = 0, dx = 2, frame = 0, running = 1;
	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				running = 0; break;
			case SDL_KEYDOWN:
				if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = 0;
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) running = 0;
				if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A)     g_tone = !g_tone;
				break;
			}
		}

		/* Vertical grayscale gradient background. */
		for (int y = 0; y < screen->h; y++)
			memset((Uint8 *)screen->pixels + (size_t)y * screen->pitch,
			       (y * 64 / screen->h) & 63, screen->w);

		/* Bouncing sprite. */
		SDL_Rect dst = { x, screen->h / 2 - 16, 0, 0 };
		SDL_BlitSurface(spr, NULL, screen, &dst);
		x += dx;
		if (x < 0 || x > screen->w - 32) dx = -dx;

		/* Cycle the ramp so the gradient shimmers. */
		if ((frame++ & 7) == 0) {
			SDL_Color c0 = pal[1];
			for (int i = 1; i < 63; i++) pal[i] = pal[i + 1];
			pal[63] = c0;
			SDL_SetPaletteColors(screen->format->palette, pal, 0, 64);
		}

		SDL_UpdateWindowSurface(win);   /* present */
		SDL_Delay(16);                  /* ~60 Hz */
	}

	SDL_CloseAudioDevice(dev);
	SDL_FreeSurface(spr);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
