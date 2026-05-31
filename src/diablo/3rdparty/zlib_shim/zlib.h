#ifndef OF_ZLIB_SHIM_H
#define OF_ZLIB_SHIM_H
/*
 * Minimal zlib shim. DevilutionX's bundled libmpq (extract.c) references
 * zlib for ZLIB-compressed MPQ sectors. Diablo's DIABDAT.MPQ uses PKWARE
 * implode / WAVE / Huffman sectors, never ZLIB, so a stub that satisfies
 * the compiler and linker is sufficient (see platform/zlib_bzip2_stub.c).
 */
#include <stddef.h>

typedef unsigned char  Bytef;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef uLong          uLongf;
typedef void          *voidpf;

#define Z_NULL        0
#define Z_OK          0
#define Z_STREAM_END  1
#define Z_NO_FLUSH    0
#define Z_FINISH      4
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define ZLIB_VERSION  "1.2.13"

typedef struct z_stream_s {
	Bytef  *next_in;  uInt avail_in;  uLong total_in;
	Bytef  *next_out; uInt avail_out; uLong total_out;
	char   *msg; void *state;
	voidpf (*zalloc)(voidpf, uInt, uInt);
	void   (*zfree)(voidpf, voidpf);
	voidpf  opaque;
	int     data_type; uLong adler, reserved;
} z_stream;
typedef z_stream *z_streamp;

#ifdef __cplusplus
extern "C" {
#endif
int inflateInit_(z_stream *strm, const char *version, int stream_size);
int inflate(z_stream *strm, int flush);
int inflateEnd(z_stream *strm);
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
#ifdef __cplusplus
}
#endif
#define inflateInit(strm) inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))

#endif /* OF_ZLIB_SHIM_H */
