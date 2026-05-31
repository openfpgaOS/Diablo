/*
 * Compression glue for libmpq's optional decompressors.
 *
 *  - bzip2: REAL decompressor is now vendored in 3rdparty/bzip2/ (devilutionx.mpq
 *    compresses nearly every CLX asset -- fonts, backgrounds -- with bzip2, sector
 *    mask 0x10). The library leaves one symbol to the caller, bz_internal_error(),
 *    provided here.
 *
 *  - zlib: NOT used by Diablo's MPQs (DIABDAT.MPQ = PKWARE implode; devilutionx.mpq
 *    = bzip2 + PKWARE; zero zlib sectors). Stubbed to FAIL CLEANLY -- libmpq treats
 *    a non-Z_STREAM_END inflate() as a decompression error -- so a future zlib file
 *    surfaces as a loud read error instead of silently corrupt data. Replace with
 *    real zlib (e.g. miniz) if one ever appears.
 */
#include <zlib.h>
#include <stdio.h>

int inflateInit_(z_stream *s, const char *v, int n) { (void)v; (void)n; if (s) s->total_out = 0; return Z_OK; }
int inflate(z_stream *s, int flush) { (void)s; (void)flush; return Z_DATA_ERROR; }
int inflateEnd(z_stream *s) { (void)s; return Z_OK; }
int uncompress(Bytef *d, uLongf *dl, const Bytef *s, uLong sl) { (void)d; (void)s; (void)sl; if (dl) *dl = 0; return Z_DATA_ERROR; }

/* bzip2 calls this on an internal consistency failure (corrupt input or a bug).
 * Normally defined by the bzip2 command-line program; we link only the library. */
void bz_internal_error(int errcode) { printf("[of] bz_internal_error %d\n", errcode); }
