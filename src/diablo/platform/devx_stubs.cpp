/*
 * devx_stubs.cpp -- stubs for DevilutionX subsystems not wired on the
 * Pocket: SDL_image PNG loading (the UI art DevilutionX needs is CLX, not
 * PNG, so failing PNG loads is non-fatal). Smacker cutscenes are REAL now:
 * movie.cpp / storm_svid.cpp compile against the vendored libsmackerdec.
 */
#include <SDL.h>

namespace devilution {

// restrict.cpp's ReadOnlyTest() probes writability by creating an arbitrary
// throwaway file in PrefPath. That can't work on the slot-based filesystem
// (only registered names resolve), and its failure path shows an early error
// dialog before the UI is ready -> crash. Config/saves still write to their
// real slots, so a no-op probe is correct here.
void ReadOnlyTest() {}

} // namespace devilution

extern "C" {
int IMG_Init(int) { return 0; }
void IMG_Quit(void) {}
int IMG_isPNG(SDL_RWops *) { return 0; }
SDL_Surface *IMG_LoadPNG_RW(SDL_RWops *) { return nullptr; }
int IMG_SavePNG(SDL_Surface *, const char *) { return -1; }
int IMG_SavePNG_RW(SDL_Surface *, SDL_RWops *, int) { return -1; }
}
