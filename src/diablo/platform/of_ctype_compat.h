#ifndef OF_CTYPE_COMPAT_H
#define OF_CTYPE_COMPAT_H
/*
 * of_ctype_compat.h -- force-included before everything.
 *
 * The riscv64-elf libstdc++ was built --with-newlib, so its
 * <bits/ctype_base.h> defines the ctype facet masks in terms of newlib's
 * ctype bit macros (_U/_L/_N/_S/_P/_C/_X/_B). We link against musl, whose
 * <ctype.h> does not define those names, so the libstdc++ locale/ctype
 * facet headers (pulled in transitively by <memory>, <string>, etc.) fail
 * to compile. Define the newlib values here so those headers compile.
 *
 * This is purely a compile-time fix for the mask *constants*. We do not
 * use std::locale / std::ctype / iostream-formatted I/O at runtime
 * (DevilutionX formats via fmt and logs via its own Log()), so newlib's
 * runtime _ctype_ classification table is never referenced.
 */
#ifndef _U
#define _U 01
#define _L 02
#define _N 04
#define _S 010
#define _P 020
#define _C 040
#define _X 0100
#define _B 0200
#endif
#endif /* OF_CTYPE_COMPAT_H */
