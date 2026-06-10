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

#include "BitReader.h"
#include <assert.h>

namespace SmackerCommon {

BitReader::BitReader(SmackerCommon::FileStream &file, uint32_t size)
{
	this->file = &file;
	this->totalSize = size;
	this->nCachedBits = 0;
	this->currentOffset = 0;
	this->bytesRead = 0;

	FillCache();
}

BitReader::~BitReader()
{
//	file->Skip(totalSize - (currentOffset/8));
}

void BitReader::FillCache()
{
	if (bytesRead < totalSize - 4)
	{
		this->cache = this->file->ReadUint32LE();
		nCachedBits = 32;
		bytesRead += 4;
	}
	else if (bytesRead < totalSize)
	{
		this->cache = this->file->ReadByte();
		nCachedBits = 8;
		bytesRead++;
	}
	else
	{
		/* Bitstream overread (corrupt/truncated video). Upstream asserts;
		 * with NDEBUG that left nCachedBits == 0 and the next GetBit
		 * underflowed it to ~4 billion. Serve sticky zero bits instead so
		 * the decoder finishes the frame and fails gracefully. */
		this->cache = 0;
		nCachedBits = 32;
	}
}

uint32_t BitReader::GetSize()
{
	return totalSize * 8;
}

uint32_t BitReader::GetPosition()
{
	return currentOffset;
}

uint32_t BitReader::GetBit()
{
	if (nCachedBits == 0)
		FillCache();

	uint32_t ret = cache & 1;

	cache >>= 1;
	nCachedBits--;
	currentOffset++;

	return ret;
}

uint32_t BitReader::GetBits(uint32_t n)
{
	uint32_t ret = 0;

	int bitsTodo = n;

	uint32_t theShift = 0;

	while (bitsTodo)
	{
		uint32_t bit = GetBit();
		bit <<= theShift;

		theShift++;

		ret |= bit;

		bitsTodo--;
	}

	return ret;
}

void BitReader::SkipBits(uint32_t n)
{
	GetBits(n);
}

} // close namespace SmackerCommon

