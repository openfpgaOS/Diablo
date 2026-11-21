/*
 * of_mpq_prefetch.c -- asynchronous read-ahead for the streamed music track
 * (see of_mpq_prefetch.h for why), plus the bridge-drain wrappers.
 *
 * Data path: of_file_read_async() DMAs one chunk (64 KB) of the music
 * member's raw archive bytes into a CRAM0 stage (zero-copy kernel path; the
 * completion IRQ only latches a flag here). The audio pump then copies the
 * staged chunk into one slot of a small SDRAM ring in 16 KB slices, so the
 * uncached CRAM0 reads are spread over several pumps (many of them in the
 * frame-pacing wait), and issues the next chunk when a slot is free.
 * libmpq's block reads for the attached archive handle are served from the
 * ring (decryption etc. stays in libmpq). A read that is not in the ring
 * waits for the in-flight chunk if it holds the data, otherwise it falls
 * back to the normal blocking read and the read-ahead resynchronizes.
 *
 * Only one transfer is ever in flight, and the CPU never reads the stage
 * while one is: a new transfer is issued only after the previous chunk has
 * been copied out.
 */
#include "platform/of_mpq_prefetch.h"

#if defined(OPENFPGAOS) && !defined(OF_PC)

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "of.h"
#include "of_error.h"
#include "of_file.h"

#define PF_CHUNK      65536u /* one async transfer (the kernel's CRAM0 DMA chunk) */
#define PF_SLOTS      4      /* SDRAM ring: 256 KB = 5.9 s of mono / 3 s of stereo music */
#define PF_COPY_SLICE 16384u /* stage -> ring bytes per pump */
#define PF_DRAIN_MS   500u   /* longest wait for an in-flight transfer */

enum { PF_IDLE, PF_INFLIGHT, PF_STAGED };

typedef struct {
	uint64_t off; /* archive file offset of the slot's first byte */
	uint32_t len; /* bytes held, 0 = free */
	uint32_t seq; /* fetch order */
} pf_slot_t;

static struct {
	FILE *fp; /* attached archive handle, NULL = detached */
	uint32_t slot_id;
	uint64_t start, end, next;
	uint8_t *ring;
	pf_slot_t slot[PF_SLOTS];
	uint32_t seq;
	int fill; /* slot receiving the in-flight / staged chunk, -1 none */
	uint64_t fill_off;
	uint32_t fill_len, copied;
	int state;
	int broken; /* async failed for this attachment: blocking reads only */
} pf;

static uint8_t *pf_stage;       /* CRAM0, allocated once */
static int pf_checked;          /* platform probed */
static int pf_unusable;         /* no usable async reads on this platform/OS */
static volatile int pf_inflight; /* a transfer we issued has not been retired */
static volatile int pf_done, pf_result;

/* Completion callback: data-slot IRQ (or of_file_async_poll) context. */
static void pf_callback(int token, int result)
{
	(void)token;
	pf_result = result;
	pf_done = 1;
}

static void pf_reset_slots(void)
{
	for (int i = 0; i < PF_SLOTS; ++i)
		pf.slot[i].len = 0;
}

/* Retire a finished transfer (IRQ normally latched pf_done; the poll is the
 * kernel's lost-IRQ fallback and also drains its completion counter). */
static void pf_check(void)
{
	if (!pf_inflight)
		return;
	if (!pf_done)
		(void)of_file_async_poll();
	if (!pf_done)
		return;
	pf_done = 0;
	pf_inflight = 0;
	if (pf.fp != NULL && pf.state == PF_INFLIGHT) {
		if (pf_result == 0) {
			pf.state = PF_STAGED;
			pf.copied = 0;
		} else {
			pf.state = PF_IDLE;
			pf.fill = -1;
			pf.broken = 1;
		}
	}
}

static void pf_copy(uint32_t max)
{
	if (pf.state != PF_STAGED)
		return;
	uint32_t n = pf.fill_len - pf.copied;
	if (max != 0 && n > max)
		n = max;
	memcpy(pf.ring + (size_t)pf.fill * PF_CHUNK + pf.copied, pf_stage + pf.copied, n);
	pf.copied += n;
	if (pf.copied == pf.fill_len) {
		pf.slot[pf.fill].off = pf.fill_off;
		pf.slot[pf.fill].len = pf.fill_len;
		pf.slot[pf.fill].seq = ++pf.seq;
		pf.next = pf.fill_off + pf.fill_len;
		if (pf.next >= pf.end)
			pf.next = pf.start;
		pf.state = PF_IDLE;
		pf.fill = -1;
	}
}

static void pf_issue(void)
{
	if (pf.fp == NULL || pf.broken || pf.state != PF_IDLE || pf_inflight)
		return;
	int k = -1;
	for (int i = 0; i < PF_SLOTS; ++i) {
		if (pf.slot[i].len == 0) {
			k = i;
			break;
		}
	}
	if (k < 0)
		return;
	uint64_t left = pf.end - pf.next;
	uint32_t len = left < PF_CHUNK ? (uint32_t)left : PF_CHUNK;
	pf_done = 0;
	int tok = of_file_read_async((int)pf.slot_id, (uint32_t)pf.next, pf_stage, len, pf_callback);
	if (tok < 0) {
		/* Bridge momentarily busy / unconfirmed: retry on a later pump. */
		if (tok != OF_ERR_BUSY && tok != OF_ERR_TIMEOUT)
			pf.broken = 1;
		return;
	}
	pf_inflight = 1;
	pf.state = PF_INFLIGHT;
	pf.fill = k;
	pf.fill_off = pf.next;
	pf.fill_len = len;
}

void of_bridge_drain(void)
{
	if (!pf_inflight)
		return;
	const uint32_t t0 = of_time_ms();
	for (;;) {
		pf_check();
		if (!pf_inflight)
			return;
		if (of_time_ms() - t0 > PF_DRAIN_MS) {
			/* Wedged transfer: stop waiting for it and never issue another. */
			pf_inflight = 0;
			pf_unusable = 1;
			pf.broken = 1;
			pf.state = PF_IDLE;
			pf.fill = -1;
			return;
		}
	}
}

void of_mpq_prefetch_pump(void)
{
	if (pf.fp == NULL)
		return;
	pf_check();
	if (pf.state == PF_STAGED)
		pf_copy(PF_COPY_SLICE);
	if (pf.state == PF_IDLE)
		pf_issue();
}

static int pf_find(uint64_t off)
{
	for (int i = 0; i < PF_SLOTS; ++i) {
		if (pf.slot[i].len != 0 && off >= pf.slot[i].off && off < pf.slot[i].off + pf.slot[i].len)
			return i;
	}
	return -1;
}

static int pf_platform_ok(void)
{
	if (pf_checked)
		return !pf_unusable;
	pf_checked = 1;
	/* APF bridge latency is a Pocket property; elsewhere keep the plain
	 * blocking reads (MiSTer's async read is itself synchronous). */
	const struct of_capabilities *caps = of_get_caps();
	if (caps == NULL || caps->platform_id != OF_PLATFORM_POCKET || of_file_async_max_read() < PF_CHUNK) {
		pf_unusable = 1;
		return 0;
	}
	pf_stage = (uint8_t *)of_file_dma_stage_alloc(PF_CHUNK, 64);
	if (pf_stage == NULL)
		pf_unusable = 1;
	return !pf_unusable;
}

void of_mpq_prefetch_forget(FILE *fp)
{
	if (fp == NULL || fp != pf.fp)
		return;
	of_bridge_drain();
	free(pf.ring);
	memset(&pf, 0, sizeof(pf));
	pf.fill = -1;
}

void of_mpq_prefetch_attach(FILE *fp, const char *archive_path, uint64_t start, uint64_t size)
{
	of_mpq_prefetch_forget(pf.fp);
	if (fp == NULL || archive_path == NULL || size <= (uint64_t)PF_SLOTS * PF_CHUNK
	    || start + size > 0xFFFFFFFFull || !pf_platform_ok())
		return;
	const char *base = archive_path;
	for (const char *p = archive_path; *p != '\0'; ++p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	uint32_t slot = 0;
	if (of_file_slot_find(base, &slot) != 0)
		return;
	uint8_t *ring = (uint8_t *)malloc((size_t)PF_SLOTS * PF_CHUNK);
	if (ring == NULL)
		return;
	memset(&pf, 0, sizeof(pf));
	pf.fp = fp;
	pf.slot_id = slot;
	pf.start = start;
	pf.end = start + size;
	pf.next = start;
	pf.ring = ring;
	pf.fill = -1;
	pf.state = PF_IDLE;
	of_bridge_drain();
	pf_issue();
}

int of_mpq_prefetch_read(FILE *fp, uint64_t off, void *dst, uint32_t len)
{
	if (fp == NULL || fp != pf.fp || len == 0 || off < pf.start || off + len > pf.end)
		return 0;
	pf_check();
	uint8_t *out = (uint8_t *)dst;
	uint64_t cur = off;
	uint32_t left = len;
	while (left != 0) {
		int k = pf_find(cur);
		if (k < 0 && pf.state != PF_IDLE && cur >= pf.fill_off && cur < pf.fill_off + pf.fill_len) {
			/* The chunk being fetched holds it: wait for it, copy it out now. */
			if (pf.state == PF_INFLIGHT)
				of_bridge_drain();
			if (pf.state == PF_STAGED)
				pf_copy(0);
			k = pf_find(cur);
		}
		if (k < 0) {
			/* Not ahead of us (track start, a seek): read it the blocking way
			 * and restart the read-ahead at the chunk that holds it. */
			if (pf.state == PF_IDLE && !pf.broken) {
				pf_reset_slots();
				pf.next = pf.start + ((cur - pf.start) / PF_CHUNK) * PF_CHUNK;
			}
			return 0;
		}
		const uint32_t o = (uint32_t)(cur - pf.slot[k].off);
		uint32_t n = pf.slot[k].len - o;
		if (n > left)
			n = left;
		memcpy(out, pf.ring + (size_t)k * PF_CHUNK + o, n);
		out += n;
		cur += n;
		left -= n;
		/* Everything fetched before this slot is behind the reader. */
		const uint32_t seq = pf.slot[k].seq;
		for (int i = 0; i < PF_SLOTS; ++i) {
			if (pf.slot[i].len != 0 && pf.slot[i].seq < seq)
				pf.slot[i].len = 0;
		}
	}
	return 1;
}

/* ---- bridge-drain wrappers (Makefile: -Wl,--wrap=...) --------------------
 * Every musl FILE operation goes through these four vtable functions, and
 * these path calls are the ones the game makes directly; any of them can
 * reach a data-slot command or the CRAM0 save windows in the kernel, which
 * must not happen while our transfer is in flight. stdout/stderr writes
 * (the UART console) never touch the bridge and skip the drain. */
size_t __real___stdio_read(FILE *f, unsigned char *buf, size_t len);
size_t __real___stdio_write(FILE *f, const unsigned char *buf, size_t len);
off_t __real___stdio_seek(FILE *f, off_t off, int whence);
int __real___stdio_close(FILE *f);
FILE *__real_fopen(const char *path, const char *mode);
int __real_access(const char *path, int mode);
int __real_stat(const char *path, struct stat *st);
int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
int __real_truncate(const char *path, off_t len);
int __real_mkdir(const char *path, mode_t mode);

size_t __wrap___stdio_read(FILE *f, unsigned char *buf, size_t len)
{
	of_bridge_drain();
	return __real___stdio_read(f, buf, len);
}
size_t __wrap___stdio_write(FILE *f, const unsigned char *buf, size_t len)
{
	const int fd = fileno(f);
	if (fd != 1 && fd != 2)
		of_bridge_drain();
	return __real___stdio_write(f, buf, len);
}
off_t __wrap___stdio_seek(FILE *f, off_t off, int whence)
{
	of_bridge_drain();
	return __real___stdio_seek(f, off, whence);
}
int __wrap___stdio_close(FILE *f)
{
	of_bridge_drain();
	return __real___stdio_close(f);
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
	of_bridge_drain();
	return __real_fopen(path, mode);
}
int __wrap_access(const char *path, int mode)
{
	of_bridge_drain();
	return __real_access(path, mode);
}
int __wrap_stat(const char *path, struct stat *st)
{
	of_bridge_drain();
	return __real_stat(path, st);
}
int __wrap_remove(const char *path)
{
	of_bridge_drain();
	return __real_remove(path);
}
int __wrap_rename(const char *from, const char *to)
{
	of_bridge_drain();
	return __real_rename(from, to);
}
int __wrap_truncate(const char *path, off_t len)
{
	of_bridge_drain();
	return __real_truncate(path, len);
}
int __wrap_mkdir(const char *path, mode_t mode)
{
	of_bridge_drain();
	return __real_mkdir(path, mode);
}

#endif /* OPENFPGAOS && !OF_PC */
