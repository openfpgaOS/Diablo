/*
 * of_libc_compat.c -- newlib -> musl glue for the toolchain's libstdc++.
 *
 * The riscv64-elf GCC ships libstdc++.a / libsupc++.a built --with-newlib.
 * Linked against musl (the SDK's libc), almost everything resolves; the
 * handful of newlib-only symbols those archives reference are provided
 * here. We never run newlib's stdio/locale machinery (DevilutionX formats
 * via fmt and logs via its own Log()), so these are link-satisfying shims.
 *
 * NOTE: <iostream>/<locale> from this libstdc++ are not used; only the
 * ctype mask *constants* are needed at compile time (see of_ctype_compat.h),
 * and the runtime _ctype_ table below covers the few libstdc++ objects that
 * reference it.
 */
#include <ctype.h>
#include <sys/stat.h>

extern int *__errno_location(void);

/*
 * mkdir(): the openfpgaOS slot FS has NO directories -- it's a flat
 * basename->slot namespace, so "creating" a directory is conceptually
 * free. DevilutionX's defensive RecursivelyCreateDir() callers (config
 * dir, save dir, demo dir, ...) expect mkdir to succeed; under musl
 * the kernel returns EINVAL for these paths, which then either logs
 * an error or, via libstdc++'s std::filesystem path under -fno-exceptions,
 * faults (the original `RecursivelyCreateDir(".")` reboot). Stub it to
 * always succeed so the higher layers stop trying to handle a failure
 * mode that doesn't really apply here. Our local definition wins over
 * musl's via link order (our .o comes before -lc).
 */
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }

/*
 * truncate() / ftruncate(): the slot FS has fixed-capacity backings, so
 * truncating a file to an arbitrary size is meaningless. DevilutionX's
 * `ResizeFile` calls this after the MpqWriter destructor writes the
 * tables; if the kernel doesn't implement these for save fds it would
 * either return -ENOSYS (harmless) or fault. Treat as a successful no-op.
 */
int truncate(const char *path, off_t length)  { (void)path; (void)length; return 0; }
int ftruncate(int fd, off_t length)           { (void)fd;   (void)length; return 0; }

/* newlib per-thread reentrancy pointer (address-taken by a few objects). */
static char of_fake_reent[1024] __attribute__((aligned(16)));
void *_impure_ptr = of_fake_reent;

/* C++ ABI / newlib runtime symbols. */
void *__dso_handle = 0;                                  /* __cxa_atexit DSO tag */
int  *__errno(void) { return __errno_location(); }       /* newlib errno accessor */
int   __locale_mb_cur_max(void) { return 1; }            /* C locale: 1 byte/char */

/*
 * newlib ctype classification table. Some libstdc++ objects reference the
 * `_ctype_` array directly. We fill it from musl's classifiers at startup,
 * before libstdc++'s own static initializers run (constructor priority 101).
 * _ctype_[c + 1] holds the mask for byte c; index 0 is the EOF slot.
 */
char _ctype_[1 + 256];

__attribute__((constructor(101)))
static void of_fill_ctype(void) {
	for (int c = 0; c < 256; c++) {
		unsigned m = 0;
		if (isupper(c))  m |= _U;
		if (islower(c))  m |= _L;
		if (isdigit(c))  m |= _N;
		if (isspace(c))  m |= _S;
		if (ispunct(c))  m |= _P;
		if (iscntrl(c))  m |= _C;
		if (isxdigit(c)) m |= _X;
		if (c == ' ')    m |= _B;
		_ctype_[c + 1] = (char)m;
	}
	_ctype_[0] = 0;
}
