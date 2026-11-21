/**
 * @file dun_render.cpp
 *
 * Implementation of functionality for rendering the level tiles.
 */

// Debugging variables
// #define DEBUG_STR
// #define DEBUG_RENDER_COLOR
// #define DEBUG_RENDER_OFFSET_X 5
// #define DEBUG_RENDER_OFFSET_Y 5

#include "engine/render/dun_render.hpp"

#include <SDL_endian.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#ifdef OPENFPGAOS
#include <cstdlib>
#include <cstring>
#endif

#include "engine/render/blit_impl.hpp"
#include "lighting.h"
#include "options.h"
#include "utils/attributes.h"
#include "utils/stdcompat/algorithm.hpp"
#ifdef DEBUG_STR
#include "engine/render/text_render.hpp"
#endif
#if defined(DEBUG_STR) || defined(DUN_RENDER_STATS)
#include "utils/str_cat.hpp"
#endif

namespace devilution {

namespace {

/** Width of a tile rendering primitive. */
constexpr int_fast16_t Width = TILE_WIDTH / 2;

/** Height of a tile rendering primitive (except triangles). */
constexpr int_fast16_t Height = TILE_HEIGHT;

/** Height of the lower triangle of a triangular or a trapezoid tile. */
constexpr int_fast16_t LowerHeight = TILE_HEIGHT / 2;

/** Height of the upper triangle of a triangular tile. */
constexpr int_fast16_t TriangleUpperHeight = TILE_HEIGHT / 2 - 1;

/** Height of the upper rectangle of a trapezoid tile. */
constexpr int_fast16_t TrapezoidUpperHeight = TILE_HEIGHT / 2;

constexpr int_fast16_t TriangleHeight = LowerHeight + TriangleUpperHeight;

/** For triangles, for each pixel drawn vertically, this many pixels are drawn horizontally. */
constexpr int_fast16_t XStep = 2;

int_fast16_t GetTileHeight(TileType tile)
{
	if (tile == TileType::LeftTriangle || tile == TileType::RightTriangle)
		return TriangleHeight;
	return Height;
}

#ifdef DEBUG_STR
std::pair<string_view, UiFlags> GetTileDebugStr(TileType tile)
{
	// clang-format off
	switch (tile) {
		case TileType::Square: return {"S", UiFlags::AlignCenter | UiFlags::VerticalCenter};
		case TileType::TransparentSquare: return {"T", UiFlags::AlignCenter | UiFlags::VerticalCenter};
		case TileType::LeftTriangle: return {"<", UiFlags::AlignRight | UiFlags::VerticalCenter};
		case TileType::RightTriangle: return {">", UiFlags::VerticalCenter};
		case TileType::LeftTrapezoid: return {"\\", UiFlags::AlignCenter};
		case TileType::RightTrapezoid: return {"/", UiFlags::AlignCenter};
		default: return {"", {}};
	}
	// clang-format on
}
#endif

#ifdef DEBUG_RENDER_COLOR
int DBGCOLOR = 0;

int GetTileDebugColor(TileType tile)
{
	// clang-format off
	switch (tile) {
		case TileType::Square: return PAL16_YELLOW + 5;
		case TileType::TransparentSquare: return PAL16_ORANGE + 5;
		case TileType::LeftTriangle: return PAL16_GRAY + 5;
		case TileType::RightTriangle: return PAL16_BEIGE;
		case TileType::LeftTrapezoid: return PAL16_RED + 5;
		case TileType::RightTrapezoid: return PAL16_BLUE + 5;
		default: return 0;
	}
}
#endif // DEBUG_RENDER_COLOR

// Masks are defined by 2 template variables:
//
// 1. `OpaquePrefix`: Whether the line starts with opaque pixels
//    followed by blended pixels or the other way around.
// 2. `PrefixIncrement`: The change to the prefix when going
//    up 1 line.
//
// The Left mask can only be applied to LeftTrapezoid and TransparentSquare.
// The Right mask can only be applied to RightTrapezoid and TransparentSquare.
// The Left/RightFoliage masks can only be applied to TransparentSquare.

// True if the given OpaquePrefix and PrefixIncrement represent foliage.
// For foliage, we skip transparent pixels instead of blending them.
template <bool OpaquePrefix, int8_t PrefixIncrement>
constexpr bool IsFoliage = PrefixIncrement != 0 && (OpaquePrefix == (PrefixIncrement > 0));

// True for foliage:
template <bool OpaquePrefix, int8_t PrefixIncrement>
constexpr bool SkipTransparentPixels = IsFoliage<OpaquePrefix, PrefixIncrement>;

// True if the entire lower half of the mask is transparent.
// True for Transparent, LeftFoliage, and RightFoliage.
template <bool OpaquePrefix, int8_t PrefixIncrement>
constexpr bool LowerHalfTransparent = (OpaquePrefix == (PrefixIncrement >= 0));

// The initial value for the prefix:
template <int8_t PrefixIncrement>
DVL_ALWAYS_INLINE int8_t InitPrefix()
{
	return PrefixIncrement >= 0 ? -32 : 64;
}

// The initial value for the prefix at y-th line (counting from the bottom).
template <int8_t PrefixIncrement>
DVL_ALWAYS_INLINE int8_t InitPrefix(int8_t y)
{
	return InitPrefix<PrefixIncrement>() + PrefixIncrement * y;
}

#ifdef DEBUG_STR
template <bool OpaquePrefix, int8_t PrefixIncrement>
std::string prefixDebugString(int8_t prefix) {
	std::string out(32, OpaquePrefix ? '0' : '1');
	const uint8_t clamped = clamp<int8_t>(prefix, 0, 32);
	out.replace(0, clamped, clamped, OpaquePrefix ? '1' : '0');
	StrAppend(out, " prefix=", prefix, " OpaquePrefix=", OpaquePrefix, " PrefixIncrement=", PrefixIncrement);
	return out;
}
#endif

enum class LightType : uint8_t {
	FullyDark,
	PartiallyLit,
	FullyLit,
};

template <LightType Light>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineOpaque(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl);

template <>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineOpaque<LightType::FullyDark>(uint8_t *DVL_RESTRICT dst, [[maybe_unused]] const uint8_t *DVL_RESTRICT src, uint_fast8_t n, [[maybe_unused]] const uint8_t *DVL_RESTRICT tbl)
{
	BlitFillDirect(dst, n, 0);
}

template <>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineOpaque<LightType::FullyLit>(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, [[maybe_unused]] const uint8_t *DVL_RESTRICT tbl)
{
#ifndef DEBUG_RENDER_COLOR
	BlitPixelsDirect(dst, src, n);
#else
	BlitFillDirect(dst, n, DBGCOLOR);
#endif
}

template <>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineOpaque<LightType::PartiallyLit>(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl)
{
#ifndef DEBUG_RENDER_COLOR
	BlitPixelsWithMap(dst, src, n, tbl);
#else
	BlitFillDirect(dst, n, tbl[DBGCOLOR]);
#endif
}

#ifndef DEBUG_RENDER_COLOR
template <LightType Light>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineTransparent(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl);

template <>
void RenderLineTransparent<LightType::FullyDark>(uint8_t *DVL_RESTRICT dst, [[maybe_unused]] const uint8_t *DVL_RESTRICT src, uint_fast8_t n, [[maybe_unused]] const uint8_t *DVL_RESTRICT tbl)
{
	BlitFillBlended(dst, n, 0);
}

template <>
void RenderLineTransparent<LightType::FullyLit>(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, [[maybe_unused]] const uint8_t *DVL_RESTRICT tbl)
{
	BlitPixelsBlended(dst, src, n);
}

template <>
void RenderLineTransparent<LightType::PartiallyLit>(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl)
{
	BlitPixelsBlendedWithMap(dst, src, n, tbl);
}
#else // DEBUG_RENDER_COLOR
template <LightType Light>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineTransparent(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl)
{
	for (size_t i = 0; i < n; i++) {
		dst[i] = paletteTransparencyLookup[dst[i]][tbl[DBGCOLOR + 4]];
	}
}
#endif

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineTransparentOrOpaque(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t width, const uint8_t *DVL_RESTRICT tbl)
{
	if (Transparent) {
		RenderLineTransparent<Light>(dst, src, width, tbl);
	} else {
		RenderLineOpaque<Light>(dst, src, width, tbl);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLineTransparentAndOpaque(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t prefixWidth, uint_fast8_t width, const uint8_t *DVL_RESTRICT tbl)
{
	if (OpaquePrefix) {
		RenderLineOpaque<Light>(dst, src, prefixWidth, tbl);
		if (!SkipTransparentPixels<OpaquePrefix, PrefixIncrement>)
			RenderLineTransparent<Light>(dst + prefixWidth, src + prefixWidth, width - prefixWidth, tbl);
	} else {
		if (!SkipTransparentPixels<OpaquePrefix, PrefixIncrement>)
			RenderLineTransparent<Light>(dst, src, prefixWidth, tbl);
		RenderLineOpaque<Light>(dst + prefixWidth, src + prefixWidth, width - prefixWidth, tbl);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLine(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, uint_fast8_t n, const uint8_t *DVL_RESTRICT tbl, int8_t prefix)
{
	if (PrefixIncrement == 0) {
		RenderLineTransparentOrOpaque<Light, OpaquePrefix>(dst, src, n, tbl);
	} else if (prefix >= static_cast<int8_t>(n)) {
		// We clamp the prefix to (0, n] and avoid calling `RenderLineTransparent/Opaque` with width=0.
		if (OpaquePrefix) {
			RenderLineOpaque<Light>(dst, src, n, tbl);
		} else {
			if (!SkipTransparentPixels<OpaquePrefix, PrefixIncrement>)
				RenderLineTransparent<Light>(dst, src, n, tbl);
		}
	} else if (prefix <= 0) {
		if (!OpaquePrefix) {
			RenderLineOpaque<Light>(dst, src, n, tbl);
		} else {
			if (!SkipTransparentPixels<OpaquePrefix, PrefixIncrement>)
				RenderLineTransparent<Light>(dst, src, n, tbl);
		}
	} else {
		RenderLineTransparentAndOpaque<Light, OpaquePrefix, PrefixIncrement>(dst, src, prefix, n, tbl);
	}
}

struct Clip {
	int_fast16_t top;
	int_fast16_t bottom;
	int_fast16_t left;
	int_fast16_t right;
	int_fast16_t width;
	int_fast16_t height;
};

DVL_ALWAYS_INLINE Clip CalculateClip(int_fast16_t x, int_fast16_t y, int_fast16_t w, int_fast16_t h, const Surface &out)
{
	Clip clip;
	clip.top = y + 1 < h ? h - (y + 1) : 0;
	clip.bottom = y + 1 > out.h() ? (y + 1) - out.h() : 0;
	clip.left = x < 0 ? -x : 0;
	clip.right = x + w > out.w() ? x + w - out.w() : 0;
	clip.width = w - clip.left - clip.right;
	clip.height = h - clip.top - clip.bottom;
	return clip;
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderSquareFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	for (auto i = 0; i < Height; ++i, dst -= dstPitch) {
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, Width, tbl);
		src += Width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderSquareClipped(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	src += clip.bottom * Height + clip.left;
	for (auto i = 0; i < clip.height; ++i, dst -= dstPitch) {
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, clip.width, tbl);
		src += Width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderSquare(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width && clip.height == Height) {
		RenderSquareFull<Light, Transparent>(dst, dstPitch, src, tbl);
	} else {
		RenderSquareClipped<Light, Transparent>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTransparentSquareFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	int8_t prefix = InitPrefix<PrefixIncrement>();
	for (auto i = 0; i < Height; ++i, dst -= dstPitch + Width) {
		uint_fast8_t drawWidth = Width;
		while (drawWidth > 0) {
			auto v = static_cast<int8_t>(*src++);
			if (v > 0) {
				RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, v, tbl, prefix - (Width - drawWidth));
				src += v;
			} else {
				v = -v;
			}
			dst += v;
			drawWidth -= v;
		}
		prefix += PrefixIncrement;
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): Actually complex and has to be fast.
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTransparentSquareClipped(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const auto skipRestOfTheLine = [&src](int_fast16_t remainingWidth) {
		while (remainingWidth > 0) {
			const auto v = static_cast<int8_t>(*src++);
			if (v > 0) {
				src += v;
				remainingWidth -= v;
			} else {
				remainingWidth -= -v;
			}
		}
		assert(remainingWidth == 0);
	};

	// Skip the bottom clipped lines.
	for (auto i = 0; i < clip.bottom; ++i) {
		skipRestOfTheLine(Width);
	}

	int8_t prefix = InitPrefix<PrefixIncrement>(clip.bottom);
	for (auto i = 0; i < clip.height; ++i, dst -= dstPitch + clip.width) {
		auto drawWidth = clip.width;

		// Skip initial src if clipping on the left.
		// Handles overshoot, i.e. when the RLE segment goes into the unclipped area.
		auto remainingLeftClip = clip.left;
		while (remainingLeftClip > 0) {
			auto v = static_cast<int8_t>(*src++);
			if (v > 0) {
				if (v > remainingLeftClip) {
					const auto overshoot = v - remainingLeftClip;
					RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src + remainingLeftClip, overshoot, tbl, prefix - (Width - remainingLeftClip));
					dst += overshoot;
					drawWidth -= overshoot;
				}
				src += v;
			} else {
				v = -v;
				if (v > remainingLeftClip) {
					const auto overshoot = v - remainingLeftClip;
					dst += overshoot;
					drawWidth -= overshoot;
				}
			}
			remainingLeftClip -= v;
		}

		// Draw the non-clipped segment
		while (drawWidth > 0) {
			auto v = static_cast<int8_t>(*src++);
			if (v > 0) {
				if (v > drawWidth) {
					RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, drawWidth, tbl, prefix - (Width - drawWidth));
					src += v;
					dst += drawWidth;
					drawWidth -= v;
					break;
				}
				RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, v, tbl, prefix - (Width - drawWidth));
				src += v;
			} else {
				v = -v;
				if (v > drawWidth) {
					dst += drawWidth;
					drawWidth -= v;
					break;
				}
			}
			dst += v;
			drawWidth -= v;
		}

		// Skip the rest of src line if clipping on the right
		assert(drawWidth <= 0);
		skipRestOfTheLine(clip.right + drawWidth);
		prefix += PrefixIncrement;
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement = 0>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTransparentSquare(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width && clip.height == Height) {
		RenderTransparentSquareFull<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl);
	} else {
		RenderTransparentSquareClipped<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	}
}

/** Vertical clip for the lower and upper triangles of a diamond tile (L/RTRIANGLE).*/
struct DiamondClipY {
	int_fast16_t lowerBottom;
	int_fast16_t lowerTop;
	int_fast16_t upperBottom;
	int_fast16_t upperTop;
};

template <int_fast16_t UpperHeight = TriangleUpperHeight>
DVL_ALWAYS_INLINE DiamondClipY CalculateDiamondClipY(const Clip &clip)
{
	DiamondClipY result;
	if (clip.bottom > LowerHeight) {
		result.lowerBottom = LowerHeight;
		result.upperBottom = clip.bottom - LowerHeight;
		result.lowerTop = result.upperTop = 0;
	} else if (clip.top > UpperHeight) {
		result.upperTop = UpperHeight;
		result.lowerTop = clip.top - UpperHeight;
		result.upperBottom = result.lowerBottom = 0;
	} else {
		result.upperTop = clip.top;
		result.lowerBottom = clip.bottom;
		result.lowerTop = result.upperBottom = 0;
	}
	return result;
}

DVL_ALWAYS_INLINE std::size_t CalculateTriangleSourceSkipLowerBottom(int_fast16_t numLines)
{
	return XStep * numLines * (numLines + 1) / 2 + 2 * ((numLines + 1) / 2);
}

DVL_ALWAYS_INLINE std::size_t CalculateTriangleSourceSkipUpperBottom(int_fast16_t numLines)
{
	return 2 * TriangleUpperHeight * numLines - numLines * (numLines - 1) + 2 * ((numLines + 1) / 2);
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleLower(uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	dst += XStep * (LowerHeight - 1);
	for (auto i = 1; i <= LowerHeight; ++i, dst -= dstPitch + XStep) {
		src += 2 * (i % 2);
		const auto width = XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleLowerClipVertical(const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1);
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		src += 2 * (i % 2);
		const auto width = XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleLowerClipLeftAndVertical(int_fast16_t clipLeft, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1) - clipLeft;
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		src += 2 * (i % 2);
		const auto width = XStep * i;
		const auto startX = Width - XStep * i;
		const auto skip = startX < clipLeft ? clipLeft - startX : 0;
		if (width > skip)
			RenderLineTransparentOrOpaque<Light, Transparent>(dst + skip, src + skip, width - skip, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleLowerClipRightAndVertical(int_fast16_t clipRight, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1);
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		src += 2 * (i % 2);
		const auto width = XStep * i;
		if (width > clipRight)
			RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width - clipRight, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	RenderLeftTriangleLower<Light, Transparent>(dst, dstPitch, src, tbl);
	dst += 2 * XStep;
	for (auto i = 1; i <= TriangleUpperHeight; ++i, dst -= dstPitch - XStep) {
		src += 2 * (i % 2);
		const auto width = Width - XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleClipVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	RenderLeftTriangleLowerClipVertical<Light, Transparent>(clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	dst += 2 * XStep + XStep * clipY.upperBottom;
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch - XStep) {
		src += 2 * (i % 2);
		const auto width = Width - XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleClipLeftAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	const int_fast16_t clipLeft = clip.left;
	RenderLeftTriangleLowerClipLeftAndVertical<Light, Transparent>(clipLeft, clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	dst += 2 * XStep + XStep * clipY.upperBottom;
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch - XStep) {
		src += 2 * (i % 2);
		const auto width = Width - XStep * i;
		const auto startX = XStep * i;
		const auto skip = startX < clipLeft ? clipLeft - startX : 0;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst + skip, src + skip, width > skip ? width - skip : 0, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangleClipRightAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	const int_fast16_t clipRight = clip.right;
	RenderLeftTriangleLowerClipRightAndVertical<Light, Transparent>(clipRight, clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	dst += 2 * XStep + XStep * clipY.upperBottom;
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch - XStep) {
		src += 2 * (i % 2);
		const auto width = Width - XStep * i;
		if (width <= clipRight)
			break;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width - clipRight, tbl);
		src += width;
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTriangle(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width) {
		if (clip.height == TriangleHeight) {
			RenderLeftTriangleFull<Light, Transparent>(dst, dstPitch, src, tbl);
		} else {
			RenderLeftTriangleClipVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		}
	} else if (clip.right == 0) {
		RenderLeftTriangleClipLeftAndVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
	} else {
		RenderLeftTriangleClipRightAndVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleLower(uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	for (auto i = 1; i <= LowerHeight; ++i, dst -= dstPitch) {
		const auto width = XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleLowerClipVertical(const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch) {
		const auto width = XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleLowerClipLeftAndVertical(int_fast16_t clipLeft, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch) {
		const auto width = XStep * i;
		if (width > clipLeft)
			RenderLineTransparentOrOpaque<Light, Transparent>(dst, src + clipLeft, width - clipLeft, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleLowerClipRightAndVertical(int_fast16_t clipRight, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT &dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT &src, const uint8_t *DVL_RESTRICT tbl)
{
	src += CalculateTriangleSourceSkipLowerBottom(clipY.lowerBottom);
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch) {
		const auto width = XStep * i;
		const auto skip = Width - width < clipRight ? clipRight - (Width - width) : 0;
		if (width > skip)
			RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width - skip, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	RenderRightTriangleLower<Light, Transparent>(dst, dstPitch, src, tbl);
	for (auto i = 1; i <= TriangleUpperHeight; ++i, dst -= dstPitch) {
		const auto width = Width - XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleClipVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	RenderRightTriangleLowerClipVertical<Light, Transparent>(clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		const auto width = Width - XStep * i;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleClipLeftAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	const int_fast16_t clipLeft = clip.left;
	RenderRightTriangleLowerClipLeftAndVertical<Light, Transparent>(clipLeft, clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		const auto width = Width - XStep * i;
		if (width <= clipLeft)
			break;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src + clipLeft, width - clipLeft, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangleClipRightAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY(clip);
	const int_fast16_t clipRight = clip.right;
	RenderRightTriangleLowerClipRightAndVertical<Light, Transparent>(clipRight, clipY, dst, dstPitch, src, tbl);
	src += CalculateTriangleSourceSkipUpperBottom(clipY.upperBottom);
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		const auto width = Width - XStep * i;
		const auto skip = Width - width < clipRight ? clipRight - (Width - width) : 0;
		RenderLineTransparentOrOpaque<Light, Transparent>(dst, src, width > skip ? width - skip : 0, tbl);
		src += width + 2 * (i % 2);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTriangle(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width) {
		if (clip.height == TriangleHeight) {
			RenderRightTriangleFull<Light, Transparent>(dst, dstPitch, src, tbl);
		} else {
			RenderRightTriangleClipVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		}
	} else if (clip.right == 0) {
		RenderRightTriangleClipLeftAndVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
	} else {
		RenderRightTriangleClipRightAndVertical<Light, Transparent>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTrapezoidUpperHalf(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl) {
	if (PrefixIncrement != 0) {
		// The first and the last line are always fully transparent/opaque (or vice-versa).
		// We handle them specially to avoid calling the blitter with width=0.
		const uint8_t *srcEnd = src + Width * TrapezoidUpperHeight;
		constexpr bool FirstLineIsTransparent = OpaquePrefix ^ (PrefixIncrement < 0);
		if (FirstLineIsTransparent) {
			if (!SkipTransparentPixels<OpaquePrefix, PrefixIncrement>)
				RenderLineTransparent<Light>(dst, src, Width, tbl);
		} else {
			RenderLineOpaque<Light>(dst, src, Width, tbl);
		}
		src += Width;
		dst -= dstPitch;
		uint8_t prefixWidth = (PrefixIncrement < 0 ? 32 : 0) + PrefixIncrement;
		do {
			RenderLineTransparentAndOpaque<Light, OpaquePrefix, PrefixIncrement>(dst, src, prefixWidth, Width, tbl);
			prefixWidth += PrefixIncrement;
			src += Width;
			dst -= dstPitch;
		} while (src != srcEnd);
	} else { // PrefixIncrement == 0;
		const uint8_t *srcEnd = src + Width * TrapezoidUpperHeight;
		do {
			RenderLineTransparentOrOpaque<Light, OpaquePrefix>(dst, src, Width, tbl);
			src += Width;
			dst -= dstPitch;
		} while (src != srcEnd);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTrapezoidUpperHalfClipVertical(const Clip &clip, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl) {
	const auto upperMax = TrapezoidUpperHeight - clipY.upperTop;
	int8_t prefix = InitPrefix<PrefixIncrement>(clip.bottom);
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, Width, tbl, prefix);
		src += Width;
		prefix += PrefixIncrement;
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTrapezoidUpperHalfClipLeftAndVertical(const Clip &clip, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl) {
	const auto upperMax = TrapezoidUpperHeight - clipY.upperTop;
	int8_t prefix = InitPrefix<PrefixIncrement>(clip.bottom);
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, clip.width, tbl, prefix - clip.left);
		src += Width;
		prefix += PrefixIncrement;
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTrapezoidUpperHalfClipRightAndVertical(const Clip &clip, const DiamondClipY &clipY, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl) {
	const auto upperMax = TrapezoidUpperHeight - clipY.upperTop;
	int8_t prefix = InitPrefix<PrefixIncrement>(clip.bottom);
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch) {
		RenderLine<Light, OpaquePrefix, PrefixIncrement>(dst, src, clip.width, tbl, prefix);
		src += Width;
		prefix += PrefixIncrement;
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	RenderLeftTriangleLower<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(dst, dstPitch, src, tbl);
	dst += XStep;
	RenderTrapezoidUpperHalf<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidClipVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderLeftTriangleLowerClipVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width;
	dst += XStep;
	RenderTrapezoidUpperHalfClipVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidClipLeftAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderLeftTriangleLowerClipLeftAndVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clip.left, clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width + clip.left;
	dst += XStep + clip.left;
	RenderTrapezoidUpperHalfClipLeftAndVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidClipRightAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderLeftTriangleLowerClipRightAndVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clip.right, clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width;
	dst += XStep;
	RenderTrapezoidUpperHalfClipRightAndVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement = 0>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoid(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width) {
		if (clip.height == Height) {
			RenderLeftTrapezoidFull<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl);
		} else {
			RenderLeftTrapezoidClipVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		}
	} else if (clip.right == 0) {
		RenderLeftTrapezoidClipLeftAndVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	} else {
		RenderLeftTrapezoidClipRightAndVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl)
{
	RenderRightTriangleLower<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(dst, dstPitch, src, tbl);
	RenderTrapezoidUpperHalf<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidClipVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderRightTriangleLowerClipVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width;
	RenderTrapezoidUpperHalfClipVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidClipLeftAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderRightTriangleLowerClipLeftAndVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clip.left, clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width + clip.left;
	RenderTrapezoidUpperHalfClipLeftAndVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidClipRightAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	const DiamondClipY clipY = CalculateDiamondClipY<TrapezoidUpperHeight>(clip);
	RenderRightTriangleLowerClipRightAndVertical<Light, LowerHalfTransparent<OpaquePrefix, PrefixIncrement>>(clip.right, clipY, dst, dstPitch, src, tbl);
	src += clipY.upperBottom * Width;
	RenderTrapezoidUpperHalfClipRightAndVertical<Light, OpaquePrefix, PrefixIncrement>(clip, clipY, dst, dstPitch, src, tbl);
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement = 0>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoid(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (clip.width == Width) {
		if (clip.height == Height) {
			RenderRightTrapezoidFull<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl);
		} else {
			RenderRightTrapezoidClipVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		}
	} else if (clip.right == 0) {
		RenderRightTrapezoidClipLeftAndVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	} else {
		RenderRightTrapezoidClipRightAndVertical<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTileType(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	switch (tile) {
	case TileType::Square:
		RenderSquare<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::TransparentSquare:
		RenderTransparentSquare<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::LeftTriangle:
		RenderLeftTriangle<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::RightTriangle:
		RenderRightTriangle<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::LeftTrapezoid:
		RenderLeftTrapezoid<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::RightTrapezoid:
		RenderRightTrapezoid<Light, Transparent>(dst, dstPitch, src, tbl, clip);
		break;
	}
}

template <bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTransparentSquareDispatch(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (tbl == LightTables[LightsMax].data()) {
		RenderTransparentSquare<LightType::FullyDark, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	} else if (tbl == LightTables[0].data()) {
		RenderTransparentSquare<LightType::FullyLit, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	} else {
		RenderTransparentSquare<LightType::PartiallyLit, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidOrTransparentSquare(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	switch (tile) {
	case TileType::TransparentSquare:
		RenderTransparentSquare<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::LeftTrapezoid:
		RenderLeftTrapezoid<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		break;
	default:
		app_fatal("Given mask can only be applied to TransparentSquare or LeftTrapezoid tiles");
	}
}

template <LightType Light, bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidOrTransparentSquare(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	switch (tile) {
	case TileType::TransparentSquare:
		RenderTransparentSquare<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		break;
	case TileType::RightTrapezoid:
		RenderRightTrapezoid<Light, OpaquePrefix, PrefixIncrement>(dst, dstPitch, src, tbl, clip);
		break;
	default:
		app_fatal("Given mask can only be applied to TransparentSquare or LeftTrapezoid tiles");
	}
}

template <bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderLeftTrapezoidOrTransparentSquareDispatch(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (tbl == LightTables[LightsMax].data()) {
		RenderLeftTrapezoidOrTransparentSquare<LightType::FullyDark, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	} else if (tbl == LightTables[0].data()) {
		RenderLeftTrapezoidOrTransparentSquare<LightType::FullyLit, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	} else {
		RenderLeftTrapezoidOrTransparentSquare<LightType::PartiallyLit, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	}
}

template <bool OpaquePrefix, int8_t PrefixIncrement>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderRightTrapezoidOrTransparentSquareDispatch(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (tbl == LightTables[LightsMax].data()) {
		RenderRightTrapezoidOrTransparentSquare<LightType::FullyDark, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	} else if (tbl == LightTables[0].data()) {
		RenderRightTrapezoidOrTransparentSquare<LightType::FullyLit, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	} else {
		RenderRightTrapezoidOrTransparentSquare<LightType::PartiallyLit, OpaquePrefix, PrefixIncrement>(tile, dst, dstPitch, src, tbl, clip);
	}
}

template <bool Transparent>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void RenderTileDispatch(TileType tile, uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, const uint8_t *DVL_RESTRICT src, const uint8_t *DVL_RESTRICT tbl, Clip clip)
{
	if (tbl == LightTables[LightsMax].data()) {
		RenderTileType<LightType::FullyDark, Transparent>(tile, dst, dstPitch, src, tbl, clip);
	} else if (tbl == LightTables[0].data()) {
		RenderTileType<LightType::FullyLit, Transparent>(tile, dst, dstPitch, src, tbl, clip);
	} else {
		RenderTileType<LightType::PartiallyLit, Transparent>(tile, dst, dstPitch, src, tbl, clip);
	}
}

// Blit with left and vertical clipping.
void RenderBlackTileClipLeftAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, int sx, DiamondClipY clipY)
{
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1);
	// Lower triangle (drawn bottom to top):
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = clipY.lowerBottom + 1; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		const auto w = 2 * XStep * i;
		const auto curX = sx + TILE_WIDTH / 2 - XStep * i;
		if (curX >= 0) {
			memset(dst, 0, w);
		} else if (-curX <= w) {
			memset(dst - curX, 0, w + curX);
		}
	}
	dst += 2 * XStep + XStep * clipY.upperBottom;
	// Upper triangle (drawn bottom to top):
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = clipY.upperBottom; i < upperMax; ++i, dst -= dstPitch - XStep) {
		const auto w = 2 * XStep * (TriangleUpperHeight - i);
		const auto curX = sx + TILE_WIDTH / 2 - XStep * (TriangleUpperHeight - i);
		if (curX >= 0) {
			memset(dst, 0, w);
		} else if (-curX <= w) {
			memset(dst - curX, 0, w + curX);
		} else {
			break;
		}
	}
}

// Blit with right and vertical clipping.
void RenderBlackTileClipRightAndVertical(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, int_fast16_t maxWidth, DiamondClipY clipY)
{
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1);
	// Lower triangle (drawn bottom to top):
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = clipY.lowerBottom + 1; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		const auto width = 2 * XStep * i;
		const auto endX = TILE_WIDTH / 2 + XStep * i;
		const auto skip = endX > maxWidth ? endX - maxWidth : 0;
		if (width > skip)
			memset(dst, 0, width - skip);
	}
	dst += 2 * XStep + XStep * clipY.upperBottom;
	// Upper triangle (drawn bottom to top):
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch - XStep) {
		const auto width = TILE_WIDTH - 2 * XStep * i;
		const auto endX = TILE_WIDTH / 2 + XStep * (TriangleUpperHeight - i + 1);
		const auto skip = endX > maxWidth ? endX - maxWidth : 0;
		if (width <= skip)
			break;
		memset(dst, 0, width - skip);
	}
}

// Blit with vertical clipping only.
void RenderBlackTileClipY(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch, DiamondClipY clipY)
{
	dst += XStep * (LowerHeight - clipY.lowerBottom - 1);
	// Lower triangle (drawn bottom to top):
	const auto lowerMax = LowerHeight - clipY.lowerTop;
	for (auto i = 1 + clipY.lowerBottom; i <= lowerMax; ++i, dst -= dstPitch + XStep) {
		memset(dst, 0, 2 * XStep * i);
	}
	dst += 2 * XStep + XStep * clipY.upperBottom;
	// Upper triangle (drawn bottom to top):
	const auto upperMax = TriangleUpperHeight - clipY.upperTop;
	for (auto i = 1 + clipY.upperBottom; i <= upperMax; ++i, dst -= dstPitch - XStep) {
		memset(dst, 0, TILE_WIDTH - 2 * XStep * i);
	}
}

// Blit a black tile without clipping (must be fully in bounds).
void RenderBlackTileFull(uint8_t *DVL_RESTRICT dst, uint16_t dstPitch)
{
	dst += XStep * (LowerHeight - 1);
	// Tile is fully in bounds, can use constant loop boundaries.
	// Lower triangle (drawn bottom to top):
	for (unsigned i = 1; i <= LowerHeight; ++i, dst -= dstPitch + XStep) {
		memset(dst, 0, 2 * XStep * i);
	}
	dst += 2 * XStep;
	// Upper triangle (drawn bottom to to top):
	for (unsigned i = 1; i <= TriangleUpperHeight; ++i, dst -= dstPitch - XStep) {
		memset(dst, 0, TILE_WIDTH - 2 * XStep * i);
	}
}

#ifdef OPENFPGAOS
/* [of] Lit micro-tile cache.
 *
 * Partially lit tiles (light levels 1..14) normally pay one colour-map lookup
 * per pixel on every frame (BlitPixelsWithMap / BlitPixelsBlendedWithMap),
 * and that is most of the frame on the 100 MHz rv32 core.  The lit result of a
 * MIN block depends only on (block frame, light table), so build it once into
 * an arena and render it through the existing FullyLit paths (plain copy,
 * or BlitPixelsBlended for MaskType::Transparent).  Output is byte-identical:
 * the opaque path writes tbl[src] either way, and the blended path computes
 * paletteTransparencyLookup[dst][tbl[src]] before and [tbl[src]][dst] after,
 * which is the same entry because GenerateBlendedLookupTable() builds the
 * table symmetric and CycleColors() permutes both indices alike.
 *
 * Invalidation: MakeLightTable() (level load) and FreeGameMem() reset the
 * cache, and a changed pDungeonCels pointer flushes it.  Hell's
 * lighting_color_cycling() rotates light-table entries 1..31 every tick, so a
 * block containing any source byte in 1..31 is marked cycle-sensitive and is
 * rendered uncached while cycling is active.  If the arena cannot be
 * allocated, rendering falls back to the uncached path. */
constexpr uint32_t LitFrames = 4096; // LevelCelBlock::frame() is 12 bits
constexpr size_t LitArenaBytes = 1024 * 1024;
uint32_t *LitIndex;     // [frame][light]: 0 empty, 2 uncacheable, else (offset + 1) | (sensitive << 1)
uint8_t *LitArena;
size_t LitArenaUsed;
const void *LitCels;
bool LitAllocFailed;
bool LitCycling;

void LitRelease()
{
	LitTileCacheReset(/*releaseMemory=*/true);
}

bool LitAlloc()
{
	OfReleaseLevelRenderCaches = LitRelease;
	LitIndex = static_cast<uint32_t *>(std::calloc(LitFrames * NumLightingLevels, sizeof(uint32_t)));
	LitArena = static_cast<uint8_t *>(std::malloc(LitArenaBytes));
	if (LitIndex == nullptr || LitArena == nullptr) {
		std::free(LitIndex);
		std::free(LitArena);
		LitIndex = nullptr;
		LitArena = nullptr;
		LitAllocFailed = true;
		return false;
	}
	LitArenaUsed = 0;
	return true;
}

void LitFlush()
{
	if (LitIndex != nullptr)
		std::memset(LitIndex, 0, LitFrames * NumLightingLevels * sizeof(uint32_t));
	LitArenaUsed = 0;
}

size_t LitRawSize(TileType tile)
{
	switch (tile) {
	case TileType::Square: return Width * Height;                       // 32x32 pixels
	case TileType::LeftTriangle:
	case TileType::RightTriangle: return 544;                           // 512 pixels + 2 padding bytes on 16 rows
	case TileType::LeftTrapezoid:
	case TileType::RightTrapezoid: return 800;                          // 288-byte lower triangle + 32x16 upper rows
	default: return 0;
	}
}

/* [of] Floor triangles get one cached copy per destination phase (see
 * CopyTriangleCo): the entry of a Left/RightTriangle points at a 16-byte
 * record of four variant offsets, variant p placed at an arena address
 * congruent to p mod 4. Light 0 (fully lit) triangles are cached too -- as a
 * plain copy of the source, exactly what the FullyLit path draws -- only so
 * that they get phase-matched copies. Other tile types keep one phase-0 copy
 * and are not cached at light 0. */
bool LitIsTriangle(TileType tile)
{
	return tile == TileType::LeftTriangle || tile == TileType::RightTriangle;
}

const uint8_t *LitTileLookup(LevelCelBlock levelCelBlock, TileType tile, const uint8_t *src, const uint8_t *tbl, unsigned phase)
{
	const ptrdiff_t d = tbl - LightTables[0].data();
	if (d < 0 || d >= static_cast<ptrdiff_t>(LightsMax) * 256 || (d & 255) != 0)
		return nullptr;
	const unsigned light = static_cast<unsigned>(d) >> 8;
	const bool triangle = LitIsTriangle(tile);
	if (light == 0 && !triangle)
		return nullptr;
	if (!triangle)
		phase = 0;
	const unsigned frame = levelCelBlock.frame();
	if (frame + 1 >= LitFrames)
		return nullptr;
	if (LitCels != pDungeonCels.get()) {
		LitFlush();
		LitCels = pDungeonCels.get();
	}
	if (LitIndex == nullptr && (LitAllocFailed || !LitAlloc()))
		return nullptr;
	uint32_t *entry = &LitIndex[frame * NumLightingLevels + light];
	if ((*entry & 1) != 0) {
		if ((*entry & 2) != 0 && LitCycling)
			return nullptr;
		if (!triangle)
			return LitArena + (*entry & ~3u);
		const uint32_t *rec = reinterpret_cast<const uint32_t *>(LitArena + (*entry & ~3u));
		if (rec[phase] != 0)
			return LitArena + rec[phase] - 1;
	} else if (*entry == 2) {
		return nullptr;
	}

	const auto *pFrameTable = reinterpret_cast<const uint32_t *>(pDungeonCels.get());
	const size_t available = SDL_SwapLE32(pFrameTable[frame + 1]) - SDL_SwapLE32(pFrameTable[frame]);
	size_t size = LitRawSize(tile);
	if (tile == TileType::TransparentSquare) {
		// RLE: per row, int8 runs; positive = that many pixels follow, negative = skip.
		size_t pos = 0;
		for (int row = 0; row < Height && size == 0; ++row) {
			int remaining = Width;
			while (remaining > 0) {
				if (pos >= available) {
					*entry = 2;
					return nullptr;
				}
				const auto v = static_cast<int8_t>(src[pos++]);
				const int run = v > 0 ? v : -v;
				if (run == 0 || run > remaining) {
					*entry = 2;
					return nullptr;
				}
				if (v > 0)
					pos += v;
				remaining -= run;
			}
		}
		size = pos;
	}
	if (size == 0 || size > available || size + 4 + 16 > LitArenaBytes) {
		*entry = 2;
		return nullptr;
	}
	const bool haveRecord = triangle && (*entry & 1) != 0;
	const size_t need = (triangle && !haveRecord ? 16 : 0) + ((size + phase + 3) & ~static_cast<size_t>(3));
	if (LitArenaUsed + need + 16 > LitArenaBytes) {
		LitFlush(); // every entry, including *entry and its record, is now gone
		return LitTileLookup(levelCelBlock, tile, src, tbl, phase);
	}
	uint32_t *rec = nullptr;
	if (triangle) {
		if (haveRecord) {
			rec = reinterpret_cast<uint32_t *>(LitArena + (*entry & ~3u));
		} else {
			rec = reinterpret_cast<uint32_t *>(LitArena + LitArenaUsed);
			rec[0] = rec[1] = rec[2] = rec[3] = 0;
			LitArenaUsed += 16;
		}
	}
	uint8_t *out = LitArena + LitArenaUsed + phase;
	bool sensitive = false;
	if (light == 0) {
		std::memcpy(out, src, size);
	} else if (tile == TileType::TransparentSquare) {
		size_t pos = 0;
		while (pos < size) {
			const auto v = static_cast<int8_t>(src[pos]);
			out[pos] = src[pos];
			++pos;
			for (int i = 0; i < v; ++i, ++pos) {
				const uint8_t c = src[pos];
				sensitive |= static_cast<uint8_t>(c - 1) < 31;
				out[pos] = tbl[c];
			}
		}
	} else {
		for (size_t i = 0; i < size; ++i) {
			const uint8_t c = src[i];
			sensitive |= static_cast<uint8_t>(c - 1) < 31;
			out[i] = tbl[c];
		}
	}
	if (sensitive && LitCycling) {
		if (rec != nullptr && !haveRecord)
			LitArenaUsed -= 16;
		*entry = 2;
		return nullptr;
	}
	if (triangle) {
		rec[phase] = static_cast<uint32_t>(out - LitArena) + 1;
		if (!haveRecord)
			*entry = static_cast<uint32_t>(reinterpret_cast<uint8_t *>(rec) - LitArena) + 1 + (sensitive ? 2 : 0);
	} else {
		*entry = static_cast<uint32_t>(LitArenaUsed) + 1 + (sensitive ? 2 : 0);
	}
	LitArenaUsed += (size + phase + 3) & ~static_cast<size_t>(3);
	return out;
}

/* [of] Inline hit path of LitTileLookup (same result when it returns non-null;
 * nullptr means "ask LitTileLookup", which also builds missing entries). */
DVL_ALWAYS_INLINE const uint8_t *LitTileLookupHit(LevelCelBlock levelCelBlock, TileType tile, const uint8_t *tbl, unsigned phase)
{
	if (LitIndex == nullptr || LitCels != pDungeonCels.get())
		return nullptr;
	const uintptr_t d = static_cast<uintptr_t>(tbl - LightTables[0].data());
	if (d >= static_cast<uintptr_t>(LightsMax) * 256 || (d & 255) != 0)
		return nullptr;
	const uint32_t entry = LitIndex[levelCelBlock.frame() * NumLightingLevels + (d >> 8)];
	if ((entry & 1) == 0 || ((entry & 2) != 0 && LitCycling))
		return nullptr;
	if (!LitIsTriangle(tile))
		return LitArena + (entry & ~3u);
	const uint32_t variant = reinterpret_cast<const uint32_t *>(LitArena + (entry & ~3u))[phase];
	return variant != 0 ? LitArena + variant - 1 : nullptr;
}

/* [of] Unclipped Solid floor triangle drawn from a source at the same address
 * offset mod 4 as the destination (cached variants are placed so). The rows
 * are those of RenderLeftTriangleFull / RenderRightTriangleFull (Diablo's
 * triangle layout pads odd rows by two bytes so that this co-alignment holds
 * for every row), fully unrolled per destination phase: each row is a few
 * aligned word moves plus at most a halfword/byte at each end, instead of a
 * BlitPixelsDirect call with byte head/tail loops. Same bytes written. */
typedef uint16_t __attribute__((may_alias)) OfHalf;

template <unsigned A, unsigned W>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void CopyRowCo(uint8_t *DVL_RESTRICT d, const uint8_t *DVL_RESTRICT s)
{
	constexpr unsigned B = ((A & 1) != 0 && W >= 1) ? 1 : 0; // byte to reach an even address
	constexpr unsigned A1 = (A + B) & 3;
	constexpr unsigned H = (A1 == 2 && W - B >= 2) ? 2 : 0;  // halfword to reach a word boundary
	constexpr unsigned R = W - B - H;
	constexpr unsigned NW = ((A1 + H) & 3) == 0 ? R / 4 : 0;
	constexpr unsigned T = R - 4 * NW;
	if constexpr (B != 0)
		d[0] = s[0];
	if constexpr (H != 0)
		*reinterpret_cast<OfHalf *>(d + B) = *reinterpret_cast<const OfHalf *>(s + B);
	constexpr unsigned O = B + H;
	if constexpr (NW >= 2) {
		for (unsigned k = 0; k + 1 < NW; k += 2) {
			const uint32_t w0 = *reinterpret_cast<const OfWord *>(s + O + 4 * k);
			const uint32_t w1 = *reinterpret_cast<const OfWord *>(s + O + 4 * k + 4);
			*reinterpret_cast<OfWord *>(d + O + 4 * k) = w0;
			*reinterpret_cast<OfWord *>(d + O + 4 * k + 4) = w1;
		}
	}
	if constexpr ((NW & 1) != 0)
		*reinterpret_cast<OfWord *>(d + O + 4 * (NW - 1)) = *reinterpret_cast<const OfWord *>(s + O + 4 * (NW - 1));
	constexpr unsigned E = O + 4 * NW;
	if constexpr (T >= 2)
		*reinterpret_cast<OfHalf *>(d + E) = *reinterpret_cast<const OfHalf *>(s + E);
	if constexpr ((T & 1) != 0)
		d[E + (T & 2)] = s[E + (T & 2)];
}

template <bool Left>
constexpr unsigned TriLowerSrc(unsigned i)
{
	unsigned off = 0;
	for (unsigned j = 1; j < i; ++j)
		off += XStep * j + 2 * (j % 2);
	return Left ? off + 2 * (i % 2) : off;
}

template <bool Left>
constexpr unsigned TriUpperSrc(unsigned i)
{
	unsigned off = TriLowerSrc<false>(LowerHeight + 1);
	for (unsigned j = 1; j < i; ++j)
		off += Width - XStep * j + 2 * (j % 2);
	return Left ? off + 2 * (i % 2) : off;
}
static_assert(TriLowerSrc<false>(LowerHeight + 1) == 288 && TriUpperSrc<false>(TriangleUpperHeight + 1) == 544);

template <unsigned P, bool Left, unsigned I>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void CopyTriLowerFrom(uint8_t *DVL_RESTRICT row, uint16_t pitch, const uint8_t *DVL_RESTRICT src)
{
	if constexpr (I <= LowerHeight) {
		constexpr unsigned W = XStep * I;
		constexpr unsigned Dx = Left ? Width - W : 0;
		CopyRowCo<(P + Dx) & 3, W>(row + Dx, src + TriLowerSrc<Left>(I));
		CopyTriLowerFrom<P, Left, I + 1>(row - pitch, pitch, src);
	}
}

template <unsigned P, bool Left, unsigned I>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void CopyTriUpperFrom(uint8_t *DVL_RESTRICT row, uint16_t pitch, const uint8_t *DVL_RESTRICT src)
{
	if constexpr (I <= TriangleUpperHeight) {
		constexpr unsigned W = Width - XStep * I;
		constexpr unsigned Dx = Left ? XStep * I : 0;
		CopyRowCo<(P + Dx) & 3, W>(row + Dx, src + TriUpperSrc<Left>(I));
		CopyTriUpperFrom<P, Left, I + 1>(row - pitch, pitch, src);
	}
}

template <unsigned P, bool Left>
DVL_ATTRIBUTE_HOT __attribute__((noinline)) void CopyTriangleCo(uint8_t *DVL_RESTRICT dst, uint16_t pitch, const uint8_t *DVL_RESTRICT src)
{
	CopyTriLowerFrom<P, Left, 1>(dst, pitch, src);
	CopyTriUpperFrom<P, Left, 1>(dst - LowerHeight * pitch, pitch, src);
}

void RenderFloorTriangleCo(bool left, uint8_t *dst, uint16_t pitch, const uint8_t *src)
{
	switch (reinterpret_cast<uintptr_t>(dst) & 3) {
	case 0: left ? CopyTriangleCo<0, true>(dst, pitch, src) : CopyTriangleCo<0, false>(dst, pitch, src); break;
	case 1: left ? CopyTriangleCo<1, true>(dst, pitch, src) : CopyTriangleCo<1, false>(dst, pitch, src); break;
	case 2: left ? CopyTriangleCo<2, true>(dst, pitch, src) : CopyTriangleCo<2, false>(dst, pitch, src); break;
	default: left ? CopyTriangleCo<3, true>(dst, pitch, src) : CopyTriangleCo<3, false>(dst, pitch, src); break;
	}
}
#endif // OPENFPGAOS

} // namespace

#ifdef OPENFPGAOS
void LitTileCacheReset(bool releaseMemory)
{
	if (releaseMemory) {
		std::free(LitIndex);
		std::free(LitArena);
		LitIndex = nullptr;
		LitArena = nullptr;
		LitArenaUsed = 0;
		LitAllocFailed = false;
	} else {
		LitFlush();
	}
	LitCels = nullptr;
	LitCycling = false;
}

void LitTileCacheNoteColorCycling()
{
	LitCycling = true;
}
#endif

#ifdef DUN_RENDER_STATS
std::unordered_map<DunRenderType, size_t, DunRenderTypeHash> DunRenderStats;

string_view TileTypeToString(TileType tileType)
{
	// clang-format off
	switch (tileType) {
	case TileType::Square: return "Square";
	case TileType::TransparentSquare: return "TransparentSquare";
	case TileType::LeftTriangle: return "LeftTriangle";
	case TileType::RightTriangle: return "RightTriangle";
	case TileType::LeftTrapezoid: return "LeftTrapezoid";
	case TileType::RightTrapezoid: return "RightTrapezoid";
	default: return "???";
	}
	// clang-format on
}

string_view MaskTypeToString(MaskType maskType)
{
	// clang-format off
	switch (maskType) {
	case MaskType::Solid: return "Solid";
	case MaskType::Transparent: return "Transparent";
	case MaskType::Right: return "Right";
	case MaskType::Left: return "Left";
	case MaskType::RightFoliage: return "RightFoliage";
	case MaskType::LeftFoliage: return "LeftFoliage";
	default: return "???";
	}
	// clang-format on
}
#endif

void RenderTile(const Surface &out, Point position,
    LevelCelBlock levelCelBlock, MaskType maskType, const uint8_t *tbl)
{
	const TileType tile = levelCelBlock.type();

#ifdef DEBUG_RENDER_OFFSET_X
	position.x += DEBUG_RENDER_OFFSET_X;
#endif
#ifdef DEBUG_RENDER_OFFSET_Y
	position.y += DEBUG_RENDER_OFFSET_Y;
#endif
#ifdef DEBUG_RENDER_COLOR
	DBGCOLOR = GetTileDebugColor(tile);
#endif

	const Clip clip = CalculateClip(position.x, position.y, Width, GetTileHeight(tile), out);
	if (clip.width <= 0 || clip.height <= 0)
		return;

	const auto *pFrameTable = reinterpret_cast<const uint32_t *>(pDungeonCels.get());
	const auto *src = reinterpret_cast<const uint8_t *>(&pDungeonCels[SDL_SwapLE32(pFrameTable[levelCelBlock.frame()])]);
	uint8_t *dst = out.at(static_cast<int>(position.x + clip.left), static_cast<int>(position.y - clip.bottom));
	const uint16_t dstPitch = out.pitch();
#ifdef OPENFPGAOS
	const bool floorTriangle = maskType == MaskType::Solid && LitIsTriangle(tile)
	    && clip.width == Width && clip.height == TriangleHeight;
	if (tbl != LightTables[LightsMax].data() && (tbl != LightTables[0].data() || floorTriangle)) {
		const unsigned phase = floorTriangle ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(dst) & 3) : 0;
		const uint8_t *lit = LitTileLookupHit(levelCelBlock, tile, tbl, phase);
		if (lit == nullptr)
			lit = LitTileLookup(levelCelBlock, tile, src, tbl, phase);
		if (lit != nullptr) {
			src = lit;
			tbl = LightTables[0].data();
		}
	}
	if (floorTriangle && tbl == LightTables[0].data()
	    && ((reinterpret_cast<uintptr_t>(dst) ^ reinterpret_cast<uintptr_t>(src)) & 3) == 0) {
		RenderFloorTriangleCo(tile == TileType::LeftTriangle, dst, dstPitch, src);
		return;
	}
#endif

#ifdef DUN_RENDER_STATS
	++DunRenderStats[DunRenderType { tile, maskType }];
#endif

	switch (maskType) {
	case MaskType::Solid:
		RenderTileDispatch</*Transparent=*/false>(tile, dst, dstPitch, src, tbl, clip);
		break;
	case MaskType::Transparent:
		RenderTileDispatch</*Transparent=*/true>(tile, dst, dstPitch, src, tbl, clip);
		break;
	case MaskType::Left:
		RenderLeftTrapezoidOrTransparentSquareDispatch</*OpaquePrefix=*/false, /*PrefixIncrement=*/2>(tile, dst, dstPitch, src, tbl, clip);
		break;
	case MaskType::Right:
		RenderRightTrapezoidOrTransparentSquareDispatch</*OpaquePrefix=*/true, /*PrefixIncrement=*/-2>(tile, dst, dstPitch, src, tbl, clip);
		break;
	case MaskType::LeftFoliage:
		RenderTransparentSquareDispatch</*OpaquePrefix=*/true, /*PrefixIncrement=*/2>(dst, dstPitch, src, tbl, clip);
		break;
	case MaskType::RightFoliage:
		RenderTransparentSquareDispatch</*OpaquePrefix=*/false, /*PrefixIncrement=*/-2>(dst, dstPitch, src, tbl, clip);
		break;
	}

#ifdef DEBUG_STR
	const std::pair<string_view, UiFlags> debugStr = GetTileDebugStr(tile);
	DrawString(out, debugStr.first, Rectangle { Point { position.x + 2, position.y - 29 }, Size { 28, 28 } }, { debugStr.second });
#endif
}

void world_draw_black_tile(const Surface &out, int sx, int sy)
{
#ifdef DEBUG_RENDER_OFFSET_X
	sx += DEBUG_RENDER_OFFSET_X;
#endif
#ifdef DEBUG_RENDER_OFFSET_Y
	sy += DEBUG_RENDER_OFFSET_Y;
#endif
	auto clip = CalculateClip(sx, sy, TILE_WIDTH, TriangleHeight, out);
	if (clip.width <= 0 || clip.height <= 0)
		return;

	auto clipY = CalculateDiamondClipY(clip);
	uint8_t *dst = out.at(sx, static_cast<int>(sy - clip.bottom));
	if (clip.width == TILE_WIDTH) {
		if (clip.height == TriangleHeight) {
			RenderBlackTileFull(dst, out.pitch());
		} else {
			RenderBlackTileClipY(dst, out.pitch(), clipY);
		}
	} else {
		if (clip.right == 0) {
			RenderBlackTileClipLeftAndVertical(dst, out.pitch(), sx, clipY);
		} else {
			RenderBlackTileClipRightAndVertical(dst, out.pitch(), clip.width, clipY);
		}
	}
}

} // namespace devilution
