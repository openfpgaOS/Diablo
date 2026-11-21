/*
 * of_mpq_prefetch.h -- asynchronous read-ahead for the streamed music track.
 *
 * On the Pocket every blocking data-slot read that misses the kernel's file
 * cache is one APF bridge command, and the APF host costs ~10-25 ms per
 * command regardless of size. The streamed music WAV (11-25 MB, read at
 * 44-88 KB/s) misses the kernel's 32 KB cache blocks every 0.37-0.74 s, so
 * the frame that happens to pull the next music sector stalls for a whole
 * frame budget. This module reads the music member's raw archive bytes ahead
 * with of_file_read_async() into a CRAM0 stage, copies them into an SDRAM
 * ring in slices from the audio pump, and serves libmpq's block reads for
 * that archive handle from the ring. Everything else (and any miss) takes
 * the normal blocking path.
 *
 * The bridge carries one transfer at a time and the kernel fails a blocking
 * read issued while an async one is in flight, and the CPU must not touch
 * CRAM0 then either. of_bridge_drain() retires an in-flight transfer; the
 * build wraps every stdio/path entry point that can reach the bridge or the
 * CRAM0 save windows (see LDFLAGS in the Makefile) so it runs before them.
 */
#ifndef OF_MPQ_PREFETCH_H
#define OF_MPQ_PREFETCH_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(OPENFPGAOS) && !defined(OF_PC)

/* Start reading [start, start + size) of the archive file `fp` (opened from
 * `archive_path`) ahead, looping back to `start` at the end. Replaces any
 * previous attachment. No-op if the platform has no usable async reads. */
void of_mpq_prefetch_attach(FILE *fp, const char *archive_path, uint64_t start, uint64_t size);
/* Detach if `fp` is the attached handle (called before it is closed). */
void of_mpq_prefetch_forget(FILE *fp);
/* Serve `len` bytes at absolute file offset `off` of `fp` from the ring.
 * Returns 1 when served, 0 when the caller must read the file itself. */
int of_mpq_prefetch_read(FILE *fp, uint64_t off, void *dst, uint32_t len);
/* Advance the read-ahead: retire a finished transfer, copy a slice of a
 * staged chunk into the ring, issue the next transfer. Called by the audio
 * pump; cheap when there is nothing to do. */
void of_mpq_prefetch_pump(void);
/* Wait for an in-flight async transfer to finish (bounded). */
void of_bridge_drain(void);

#else

static inline void of_mpq_prefetch_attach(FILE *fp, const char *archive_path, uint64_t start, uint64_t size)
{
	(void)fp;
	(void)archive_path;
	(void)start;
	(void)size;
}
static inline void of_mpq_prefetch_forget(FILE *fp) { (void)fp; }
static inline int of_mpq_prefetch_read(FILE *fp, uint64_t off, void *dst, uint32_t len)
{
	(void)fp;
	(void)off;
	(void)dst;
	(void)len;
	return 0;
}
static inline void of_mpq_prefetch_pump(void) {}
static inline void of_bridge_drain(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif /* OF_MPQ_PREFETCH_H */
