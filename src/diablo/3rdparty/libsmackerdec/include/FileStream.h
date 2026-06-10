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

#ifndef _SmackerFileStream_h_
#define _SmackerFileStream_h_

#include <string>
#include <SDL.h>
#include <stdint.h>

namespace SmackerCommon {

class FileStream
{
	public:
		FileStream()
		    : rwops(nullptr)
		{
		}

		~FileStream()
		{
			Close();
		}

		bool Open(SDL_RWops *rwops);
		bool Is_Open();
		void Close();

		int32_t ReadBytes(uint8_t *data, uint32_t nBytes);

		uint32_t ReadUint32LE();
		uint32_t ReadUint32BE();

		uint16_t ReadUint16LE();
		uint16_t ReadUint16BE();

		uint8_t ReadByte();

		enum SeekDirection{
			kSeekCurrent = 0,
			kSeekStart   = 1,
			kSeekEnd     = 2
		};

		bool Seek(int32_t offset, SeekDirection = kSeekStart);
		bool Skip(int32_t offset);

		int32_t GetPosition();
		bool Is_Eos();

	private:
		bool RefillBuffer();

		SDL_RWops *rwops;
		bool is_eos;

		/* Read buffering: the Smacker bit reader pulls 1-4 bytes per call,
		 * which cost a full MPQ-rwops round trip each (~3000/frame). Serve
		 * small reads from a local buffer instead. Seek/GetPosition account
		 * for buffered-but-unconsumed bytes so chunk math stays exact. */
		uint8_t buffer[4096];
		uint32_t bufPos = 0;
		uint32_t bufLen = 0;
};

} // close namespace SmackerCommon

#endif
