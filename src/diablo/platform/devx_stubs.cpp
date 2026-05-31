/*
 * devx_stubs.cpp -- stubs for DevilutionX subsystems not yet wired on the
 * Pocket: Smacker cutscenes (movie.cpp / storm_svid.cpp are excluded until
 * libsmackerdec is vendored) and SDL_image PNG loading. These let the game
 * link and boot; cutscenes are skipped and PNG assets fail to load (the UI
 * art DevilutionX needs is CLX, not PNG, so this is non-fatal).
 */
#include <SDL.h>

namespace devilution {

bool movie_playing = false;
bool loop_movie = false;
void play_movie(const char * /*pszMovie*/, bool /*user_can_close*/) {}
void PlayInGameMovie(const char * /*pszMovie*/) {}
void SVidMute() {}
void SVidUnmute() {}

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
