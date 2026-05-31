#ifndef _LIBMPQ_ENDIAN_H
#define _LIBMPQ_ENDIAN_H

#define LIBMPQ_LIL_ENDIAN 1234
#define LIBMPQ_BIG_ENDIAN 4321

#ifndef LIBMPQ_BYTEORDER
#ifdef __linux__
#include <endian.h>
#define LIBMPQ_BYTEORDER __BYTE_ORDER
#elif defined(__OpenBSD__)
#include <endian.h>
#define LIBMPQ_BYTEORDER BYTE_ORDER
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/endian.h>
#define LIBMPQ_BYTEORDER BYTE_ORDER
#else
#if defined(__BIG_ENDIAN__) || defined(__hppa__) || \
    defined(__m68k__) || defined(mc68000) || defined(_M_M68K) || \
    (defined(__MIPS__) && defined(__MIPSEB__)) || \
    defined(__ppc__) || defined(__POWERPC__) || defined(_M_PPC) || \
    defined(__sparc__)
#define LIBMPQ_BYTEORDER LIBMPQ_BIG_ENDIAN
#else
#define LIBMPQ_BYTEORDER LIBMPQ_LIL_ENDIAN
#endif
#endif
#endif

#ifdef __has_builtin
#define _LIBMPQ_HAS_BUILTIN(x) __has_builtin(x)
#else
#define _LIBMPQ_HAS_BUILTIN(x) 0
#endif

#define HAS_BUILTIN_BSWAP16 (_LIBMPQ_HAS_BUILTIN(__builtin_bswap16))
#define HAS_BUILTIN_BSWAP32 (_LIBMPQ_HAS_BUILTIN(__builtin_bswap32))
#define HAS_BUILTIN_BSWAP64 (_LIBMPQ_HAS_BUILTIN(__builtin_bswap64))

#ifdef HAS_BUILTIN_BSWAP16
#define libmpq__bswap16 __builtin_bswap16
#endif
#ifdef HAS_BUILTIN_BSWAP32
#define libmpq__bswap32 __builtin_bswap32
#endif
#ifdef HAS_BUILTIN_BSWAP64
#define libmpq__bswap64 __builtin_bswap64
#endif

#if LIBMPQ_BYTEORDER == LIBMPQ_LIL_ENDIAN
#define libmpq__bswap_LE16(X) (X)
#define libmpq__bswap_LE32(X) (X)
#define libmpq__bswap_LE64(X) (X)
#define libmpq__bswap_BE16(X) libmpq__bswap16(X)
#define libmpq__bswap_BE32(X) libmpq__bswap32(X)
#define libmpq__bswap_BE64(X) libmpq__bswap64(X)
#else
#define libmpq__bswap_LE16(X) libmpq__bswap16(X)
#define libmpq__bswap_LE32(X) libmpq__bswap32(X)
#define libmpq__bswap_LE64(X) libmpq__bswap64(X)
#define libmpq__bswap_BE16(X) (X)
#define libmpq__bswap_BE32(X) (X)
#define libmpq__bswap_BE64(X) (X)
#endif

#endif /* _LIBMPQ_ENDIAN_H */
