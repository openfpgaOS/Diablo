#pragma once

#include <cstdint>

#include <SDL.h>

#include "mpq/mpq_reader.hpp"

namespace devilution {

SDL_RWops *SDL_RWops_FromMpqFile(MpqArchive &mpqArchive, uint32_t fileNumber, const char *filename, bool threadsafe);

#ifdef OPENFPGAOS
// [of] Start the music read-ahead (platform/of_mpq_prefetch.h) on an MPQ
// member stream opened with threadsafe=true (it owns its archive handle).
// No-op for any other SDL_RWops.
void SDL_RWops_MpqStartReadAhead(SDL_RWops *rwops);
#endif

} // namespace devilution
