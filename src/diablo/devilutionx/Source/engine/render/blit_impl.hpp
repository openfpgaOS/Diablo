#pragma once

#include <cstdint>
#include <cstring>

#ifdef __has_include
#if __has_include(<version>)
#include <version>
#endif

#if __cpp_lib_execution >= 201902L
#include <execution>
#endif
#endif

#include "engine/palette.h"
#include "utils/attributes.h"

namespace devilution {

#if __cpp_lib_execution >= 201902L
#define DEVILUTIONX_BLIT_EXECUTION_POLICY std::execution::unseq,
#else
#define DEVILUTIONX_BLIT_EXECUTION_POLICY
#endif

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillDirect(uint8_t *dst, unsigned length, uint8_t color)
{
	DVL_ASSUME(length != 0);
	std::memset(dst, color, length);
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsDirect(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length)
{
	DVL_ASSUME(length != 0);
	std::memcpy(dst, src, length);
}

struct BlitDirect {
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsDirect(dst, src, length);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillDirect(dst, length, color);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillWithMap(uint8_t *dst, unsigned length, uint8_t color, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::memset(dst, colorMap[color], length);
}

#ifdef OPENFPGAOS
/* [of] Word-at-a-time variants of the per-pixel lookup loops below.
 *
 * These loops are the frame-time hot spot on the 100 MHz in-order dual-issue
 * rv32 core: the generic std::transform form compiles to one serial
 * lbu->add->lbu->sb chain per pixel (7 instructions, no ILP). Gathering four
 * mapped bytes into a register and issuing a single word store instead of
 * four byte stores drops that to ~5.5 instructions/pixel, and the four table
 * loads are independent so the dual-issue pipeline can overlap them. Byte
 * head/tail loops keep the word stores aligned (unaligned sw traps on
 * VexiiRiscv). Little-endian byte order is assumed (true for rv32 and for
 * every host that builds this port). */
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	while ((reinterpret_cast<uintptr_t>(dst) & 3u) != 0) {
		*dst++ = colorMap[*src++];
		if (--length == 0) return;
	}
	uint32_t *DVL_RESTRICT dst32 = reinterpret_cast<uint32_t *>(dst);
	for (; length >= 4; length -= 4, src += 4, ++dst32) {
		uint32_t w = colorMap[src[0]];
		w |= static_cast<uint32_t>(colorMap[src[1]]) << 8;
		w |= static_cast<uint32_t>(colorMap[src[2]]) << 16;
		w |= static_cast<uint32_t>(colorMap[src[3]]) << 24;
		*dst32 = w;
	}
	dst = reinterpret_cast<uint8_t *>(dst32);
	for (; length != 0; --length)
		*dst++ = colorMap[*src++];
}
#else
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, [colorMap](uint8_t srcColor) { return colorMap[srcColor]; });
}
#endif

struct BlitWithMap {
	const uint8_t *DVL_RESTRICT colorMap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsWithMap(dst, src, length, colorMap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillWithMap(dst, length, color, colorMap);
	}
};

#ifdef OPENFPGAOS
/* [of] Same word-store treatment for the in-place blend fill: one lw + four
 * independent table loads + one sw per 4 pixels. */
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillBlended(uint8_t *dst, unsigned length, uint8_t color)
{
	DVL_ASSUME(length != 0);
	const uint8_t *DVL_RESTRICT tbl = paletteTransparencyLookup[color];
	while ((reinterpret_cast<uintptr_t>(dst) & 3u) != 0) {
		*dst = tbl[*dst];
		++dst;
		if (--length == 0) return;
	}
	uint32_t *DVL_RESTRICT dst32 = reinterpret_cast<uint32_t *>(dst);
	for (; length >= 4; length -= 4, ++dst32) {
		const uint32_t d = *dst32;
		uint32_t w = tbl[d & 0xFFu];
		w |= static_cast<uint32_t>(tbl[(d >> 8) & 0xFFu]) << 8;
		w |= static_cast<uint32_t>(tbl[(d >> 16) & 0xFFu]) << 16;
		w |= static_cast<uint32_t>(tbl[d >> 24]) << 24;
		*dst32 = w;
	}
	dst = reinterpret_cast<uint8_t *>(dst32);
	for (; length != 0; --length) {
		*dst = tbl[*dst];
		++dst;
	}
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlended(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length)
{
	DVL_ASSUME(length != 0);
	while ((reinterpret_cast<uintptr_t>(dst) & 3u) != 0) {
		*dst = paletteTransparencyLookup[*src++][*dst];
		++dst;
		if (--length == 0) return;
	}
	uint32_t *DVL_RESTRICT dst32 = reinterpret_cast<uint32_t *>(dst);
	for (; length >= 4; length -= 4, src += 4, ++dst32) {
		const uint32_t d = *dst32;
		uint32_t w = paletteTransparencyLookup[src[0]][d & 0xFFu];
		w |= static_cast<uint32_t>(paletteTransparencyLookup[src[1]][(d >> 8) & 0xFFu]) << 8;
		w |= static_cast<uint32_t>(paletteTransparencyLookup[src[2]][(d >> 16) & 0xFFu]) << 16;
		w |= static_cast<uint32_t>(paletteTransparencyLookup[src[3]][d >> 24]) << 24;
		*dst32 = w;
	}
	dst = reinterpret_cast<uint8_t *>(dst32);
	for (; length != 0; --length) {
		*dst = paletteTransparencyLookup[*src++][*dst];
		++dst;
	}
}
#else
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillBlended(uint8_t *dst, unsigned length, uint8_t color)
{
	DVL_ASSUME(length != 0);
	std::for_each(DEVILUTIONX_BLIT_EXECUTION_POLICY dst, dst + length, [tbl = paletteTransparencyLookup[color]](uint8_t &dstColor) {
		dstColor = tbl[dstColor];
	});
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlended(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, dst, [pal = paletteTransparencyLookup](uint8_t srcColor, uint8_t dstColor) {
		return pal[srcColor][dstColor];
	});
}
#endif

struct BlitBlended {
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsBlended(dst, src, length);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillBlended(dst, length, color);
	}
};

#ifdef OPENFPGAOS
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlendedWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	while ((reinterpret_cast<uintptr_t>(dst) & 3u) != 0) {
		*dst = paletteTransparencyLookup[*dst][colorMap[*src++]];
		++dst;
		if (--length == 0) return;
	}
	uint32_t *DVL_RESTRICT dst32 = reinterpret_cast<uint32_t *>(dst);
	for (; length >= 4; length -= 4, src += 4, ++dst32) {
		const uint32_t d = *dst32;
		uint32_t w = paletteTransparencyLookup[d & 0xFFu][colorMap[src[0]]];
		w |= static_cast<uint32_t>(paletteTransparencyLookup[(d >> 8) & 0xFFu][colorMap[src[1]]]) << 8;
		w |= static_cast<uint32_t>(paletteTransparencyLookup[(d >> 16) & 0xFFu][colorMap[src[2]]]) << 16;
		w |= static_cast<uint32_t>(paletteTransparencyLookup[d >> 24][colorMap[src[3]]]) << 24;
		*dst32 = w;
	}
	dst = reinterpret_cast<uint8_t *>(dst32);
	for (; length != 0; --length) {
		*dst = paletteTransparencyLookup[*dst][colorMap[*src++]];
		++dst;
	}
}
#else
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlendedWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, dst, [colorMap, pal = paletteTransparencyLookup](uint8_t srcColor, uint8_t dstColor) {
		return pal[dstColor][colorMap[srcColor]];
	});
}
#endif

struct BlitBlendedWithMap {
	const uint8_t *DVL_RESTRICT colorMap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsBlendedWithMap(dst, src, length, colorMap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillBlended(dst, length, colorMap[color]);
	}
};

} // namespace devilution
