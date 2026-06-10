/*
 * libsmackerdec - Smacker video decoder
 * Copyright (C) 2011 Barry Duncan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "FileStream.h"
#include <string.h>
#include <stdlib.h>

namespace SmackerCommon {

bool FileStream::Open(SDL_RWops *rwops)
{
	this->rwops = rwops;
	is_eos = false;
	bufPos = bufLen = 0;
	return rwops != nullptr;
}

bool FileStream::RefillBuffer()
{
	bufPos = 0;
	size_t got = SDL_RWread(rwops, buffer, 1, sizeof(buffer));
	bufLen = static_cast<uint32_t>(got);
	return bufLen != 0;
}

bool FileStream::Is_Open()
{
	return rwops != nullptr;
}

void FileStream::Close()
{
	if (Is_Open())
		SDL_RWclose(rwops);
	rwops = nullptr;
}

int32_t FileStream::ReadBytes(uint8_t *data, uint32_t nBytes)
{
	uint32_t total = 0;
	/* Serve from the buffer first. */
	if (bufPos < bufLen) {
		uint32_t have = bufLen - bufPos;
		uint32_t take = (nBytes < have) ? nBytes : have;
		memcpy(data, buffer + bufPos, take);
		bufPos += take;
		total += take;
	}
	uint32_t left = nBytes - total;
	if (left >= sizeof(buffer)) {
		/* Large remainder: read straight through, no double copy.
		 * INVALIDATE the buffer: it mirrors the bytes immediately
		 * before the underlying position, and after this read that
		 * window is the directly-read region, not the stale buffer --
		 * Seek()'s in-buffer fast path would otherwise serve stale
		 * bytes for seeks into the just-read chunk. */
		bufPos = bufLen = 0;
		size_t got = SDL_RWread(rwops, data + total, 1, left);
		total += static_cast<uint32_t>(got);
	} else if (left > 0) {
		if (RefillBuffer()) {
			uint32_t take = (left < bufLen) ? left : bufLen;
			memcpy(data + total, buffer, take);
			bufPos = take;
			total += take;
		}
	}
	is_eos = total == 0;
	return static_cast<int32_t>(total);
}

uint32_t FileStream::ReadUint32LE()
{
	Uint32 value;
	int32_t bytesRead = ReadBytes(reinterpret_cast<uint8_t *>(&value), 4);
	if (bytesRead < 4)
		return 0;
	return static_cast<uint32_t>(SDL_SwapLE32(value));
}

uint32_t FileStream::ReadUint32BE()
{
	Uint32 value;
	int32_t bytesRead = ReadBytes(reinterpret_cast<uint8_t *>(&value), 4);
	if (bytesRead < 4)
		return 0;
	return static_cast<uint32_t>(SDL_SwapBE32(value));
}

uint16_t FileStream::ReadUint16LE()
{
	Uint16 value;
	int32_t bytesRead = ReadBytes(reinterpret_cast<uint8_t *>(&value), 2);
	if (bytesRead < 2)
		return 0;
	return static_cast<uint16_t>(SDL_SwapLE16(value));
}

uint16_t FileStream::ReadUint16BE()
{
	Uint16 value;
	int32_t bytesRead = ReadBytes(reinterpret_cast<uint8_t *>(&value), 2);
	if (bytesRead < 2)
		return 0;
	return static_cast<uint16_t>(SDL_SwapBE16(value));
}

uint8_t FileStream::ReadByte()
{
	uint8_t value;
	int32_t bytesRead = ReadBytes(reinterpret_cast<uint8_t *>(&value), 1);
	if (bytesRead < 1)
		return 0;
	return value;
}

bool FileStream::Seek(int32_t offset, SeekDirection direction)
{
	Sint64 result = -1;
	if (kSeekStart == direction) {
		/* Cheap in-buffer seek when the target lies inside the buffered
		 * window (the decoder hops between nearby sub-chunks a lot). */
		Sint64 underlying = SDL_RWtell(rwops);
		if (underlying >= 0 && bufLen > 0) {
			Sint64 bufStart = underlying - static_cast<Sint64>(bufLen);
			if (offset >= bufStart && offset <= underlying) {
				bufPos = static_cast<uint32_t>(offset - bufStart);
				return true;
			}
		}
		bufPos = bufLen = 0;
		result = SDL_RWseek(rwops, static_cast<Sint64>(offset), RW_SEEK_SET);
	} else if (kSeekCurrent == direction) {
		/* Logical position trails the underlying one by the unread tail. */
		Sint64 newPos = static_cast<Sint64>(bufPos) + offset;
		if (newPos >= 0 && newPos <= static_cast<Sint64>(bufLen)) {
			bufPos = static_cast<uint32_t>(newPos);
			return true;
		}
		Sint64 adj = static_cast<Sint64>(offset) - static_cast<Sint64>(bufLen - bufPos);
		bufPos = bufLen = 0;
		result = SDL_RWseek(rwops, adj, RW_SEEK_CUR);
	} else if (kSeekEnd == direction) {
		bufPos = bufLen = 0;
		result = SDL_RWseek(rwops, static_cast<Sint64>(offset), RW_SEEK_END);
	}
	return result >= 0;
}

bool FileStream::Skip(int32_t offset)
{
	return Seek(offset, kSeekCurrent);
}

bool FileStream::Is_Eos()
{
	return is_eos;
}

int32_t FileStream::GetPosition()
{
	return static_cast<int32_t>(SDL_RWtell(rwops)) - static_cast<int32_t>(bufLen - bufPos);
}

} // close namespace SmackerCommon
