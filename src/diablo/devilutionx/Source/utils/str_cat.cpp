#include "utils/str_cat.hpp"

#include <algorithm>
#include <limits>

#include <fmt/format.h>

namespace devilution {
namespace {

char HexDigit(uint8_t v) { return "0123456789abcdef"[v]; }

// [of] fmt::format_int::size() is a pointer difference across its internal
// 22-byte buffer. GCC 15's value-range propagation cannot prove the
// difference non-negative, assumes it may wrap to SIZE_MAX, and flags the
// memcpy with -Wstringop-overflow. The value is in fact bounded by the
// widest possible rendering -- every digit plus a sign -- so clamping to
// that constant is a runtime no-op that gives the optimizer the bound it
// cannot infer.
constexpr size_t MaxFormatIntSize = std::numeric_limits<unsigned long long>::digits10 + 2;

} // namespace

char *BufCopy(char *out, long long value)
{
	const fmt::format_int formatted { value };
	const size_t size = std::min<size_t>(formatted.size(), MaxFormatIntSize);
	std::memcpy(out, formatted.data(), size);
	return out + size;
}
char *BufCopy(char *out, unsigned long long value)
{
	const fmt::format_int formatted { value };
	const size_t size = std::min<size_t>(formatted.size(), MaxFormatIntSize);
	std::memcpy(out, formatted.data(), size);
	return out + size;
}
char *BufCopy(char *out, AsHexU8Pad2 value)
{
	*out++ = HexDigit(value.value >> 4);
	*out++ = HexDigit(value.value & 0xf);
	return out;
}
char *BufCopy(char *out, AsHexU16Pad2 value)
{
	if (value.value > 0xff) {
		if (value.value > 0xfff) {
			out = BufCopy(out, AsHexU8Pad2 { static_cast<uint8_t>(value.value >> 8) });
		} else {
			*out++ = HexDigit(value.value >> 8);
		}
	}
	return BufCopy(out, AsHexU8Pad2 { static_cast<uint8_t>(value.value & 0xff) });
}

void StrAppend(std::string &out, long long value)
{
	const fmt::format_int formatted { value };
	out.append(formatted.data(), formatted.size());
}
void StrAppend(std::string &out, unsigned long long value)
{
	const fmt::format_int formatted { value };
	out.append(formatted.data(), formatted.size());
}
void StrAppend(std::string &out, AsHexU8Pad2 value)
{
	char hex[2];
	BufCopy(hex, value);
	out.append(hex, 2);
}

void StrAppend(std::string &out, AsHexU16Pad2 value)
{
	char hex[4];
	const auto len = static_cast<size_t>(BufCopy(hex, value) - hex);
	out.append(hex, len);
}

} // namespace devilution
