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

/* This code is heavily based on smacker.c from the FFmpeg project which can be obtained from http://www.ffmpeg.org/
 * below is the license from smacker.c
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Smacker decoder
 * Copyright (c) 2006 Konstantin Shishkov
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "SmackerDecoder.h"
#include "LogError.h"
#include <algorithm>
#include <assert.h>
#include <climits>
#include <cstring>

std::vector<class SmackerDecoder*> classInstances;

SmackerHandle Smacker_Open(SDL_RWops *rwops)
{
	SmackerHandle newHandle;
	newHandle.isValid = false;
	newHandle.instanceIndex = -1;

	SmackerDecoder *newDecoder = new SmackerDecoder();
	if (!newDecoder->Open(rwops))
	{
		delete newDecoder;
		return newHandle;
	}

	// add instance to global instance vector
	classInstances.push_back(newDecoder);

	// get a handle ID
	newHandle.instanceIndex = classInstances.size() - 1;

	// loaded ok, make handle valid
	newHandle.isValid = true;

	return newHandle;
}

void Smacker_Close(SmackerHandle &handle)
{
	if (!classInstances.at(handle.instanceIndex))
	{
		// invalid handle
		return;
	}

	// close bink decoder
	delete classInstances[handle.instanceIndex];
	classInstances[handle.instanceIndex] = 0;

	handle.instanceIndex = -1;
	handle.isValid = false;
}

uint32_t Smacker_GetNumAudioTracks(SmackerHandle &handle)
{
	// TODO: fixme
	return 1;
}

SmackerAudioInfo Smacker_GetAudioTrackDetails(SmackerHandle &handle, uint32_t trackIndex)
{
	return classInstances[handle.instanceIndex]->GetAudioTrackDetails(trackIndex);
}

/* Get a frame's worth of audio data. 
 * 
 * 'data' needs to be a pointer to allocated memory that this function will fill.
 * You can find the size (in bytes) to make this buffer by calling Bink_GetAudioTrackDetails()
 * and checking the 'idealBufferSize' member in the returned AudioInfo struct
 */
uint32_t Smacker_GetAudioData(SmackerHandle &handle, uint32_t trackIndex, int16_t *data)
{
	return classInstances[handle.instanceIndex]->GetAudioData(trackIndex, data);
}

uint32_t Smacker_GetNumFrames(SmackerHandle &handle)
{
	return classInstances[handle.instanceIndex]->GetNumFrames();
}

void Smacker_GetFrameSize(SmackerHandle &handle, uint32_t &width, uint32_t &height)
{
	width  = classInstances[handle.instanceIndex]->frameWidth;
	height = classInstances[handle.instanceIndex]->frameHeight;
}

uint32_t Smacker_GetCurrentFrameNum(SmackerHandle &handle)
{
	return classInstances[handle.instanceIndex]->GetCurrentFrameNum();
}

void Smacker_SetVideoKeyframesOnly(SmackerHandle &handle, int enable)
{
	SmackerDecoder *decoder = classInstances[handle.instanceIndex];
	decoder->videoKeyframesOnly = enable != 0;
}

void Smacker_ForceNextVideoDecode(SmackerHandle &handle)
{
	SmackerDecoder *decoder = classInstances[handle.instanceIndex];
	decoder->forceNextVideoDecode = true;
}

int Smacker_DidDecodeVideo(SmackerHandle &handle)
{
	SmackerDecoder *decoder = classInstances[handle.instanceIndex];
	return decoder->lastFrameHadVideo ? 1 : 0;
}

uint32_t Smacker_GetNextFrame(SmackerHandle &handle)
{
	SmackerDecoder *decoder = classInstances[handle.instanceIndex];

	uint32_t frameIndex = decoder->GetCurrentFrameNum();

	decoder->GetNextFrame();

	return frameIndex;
}

float Smacker_GetFrameRate(SmackerHandle &handle)
{
	return classInstances[handle.instanceIndex]->GetFrameRate();
}

bool Smacker_DidPaletteChange(SmackerHandle &handle)
{
	return classInstances[handle.instanceIndex]->DidPaletteChange();
}

void Smacker_GetPalette(SmackerHandle &handle, uint8_t *palette)
{
	classInstances[handle.instanceIndex]->GetPalette(palette);
}

void Smacker_GetFrame(SmackerHandle &handle, uint8_t *frame)
{
	classInstances[handle.instanceIndex]->GetFrame(frame);
}

void Smacker_Rewind(SmackerHandle &handle)
{
	classInstances[handle.instanceIndex]->Rewind();
}

SmackerDecoder::SmackerDecoder()
{
	isVer4 = false;
	currentFrame = 0;
	picture = 0;
	nextPos = 0;

	for (int i = 0; i < kMaxAudioTracks; i++)
	{
		audioTracks[i].buffer = 0;
	}
}

SmackerDecoder::~SmackerDecoder()
{
	for (int i = 0; i < kMaxAudioTracks; i++)
	{
		delete[] audioTracks[i].buffer;
	}

	delete[] picture;
}

template <typename T>
constexpr uint32_t LoadBE32(const T *b)
{
	static_assert(sizeof(T) == 1, "invalid argument");
	return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

// from bswap.h
static /*av_always_inline av_const*/ uint16_t av_bswap16(uint16_t x)
{
    x= (x>>8) | (x<<8);
    return x;
}

/* possible runs of blocks */
static const int block_runs[64] = {
      1,    2,    3,    4,    5,    6,    7,    8,
      9,   10,   11,   12,   13,   14,   15,   16,
     17,   18,   19,   20,   21,   22,   23,   24,
     25,   26,   27,   28,   29,   30,   31,   32,
     33,   34,   35,   36,   37,   38,   39,   40,
     41,   42,   43,   44,   45,   46,   47,   48,
     49,   50,   51,   52,   53,   54,   55,   56,
     57,   58,   59,  128,  256,  512, 1024, 2048 };

enum SmkBlockTypes {
    SMK_BLK_MONO = 0,
    SMK_BLK_FULL = 1,
    SMK_BLK_SKIP = 2,
    SMK_BLK_FILL = 3 };

/* palette used in Smacker */
static const uint8_t smk_pal[64] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
    0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
    0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
    0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
    0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
    0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
    0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
    0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
};

enum SAudFlags {
    SMK_AUD_PACKED  = 0x80000000, 
    SMK_AUD_16BITS  = 0x20000000, 
    SMK_AUD_STEREO  = 0x10000000, 
    SMK_AUD_BINKAUD = 0x08000000, 
    SMK_AUD_USEDCT  = 0x04000000 
};

const int kSMKpal = 0x01;
const int kFlagRingFrame = 0x01;

const int kTreeBits = 9;
const int kSMKnode = 0x80000000;

constexpr uint32_t kSMK2iD = LoadBE32("SMK2");
constexpr uint32_t kSMK4iD = LoadBE32("SMK4");


/**
 * Context used for code reconstructing
 */
typedef struct HuffContext {
    int leaves;
    int length;
    std::vector<int> values;
} HuffContext;

/* common parameters used for decode_bigtree */
typedef struct DBCtx {
	std::vector<int> recode1, recode2;
    int escapes[3];
    int *last;
    int lcur;
} DBCtx;


static void last_reset(std::vector<int> &recode, int *last) {
    recode[last[0]] = recode[last[1]] = recode[last[2]] = 0;
}

/* get code and update history */
int SmackerDecoder::GetCode(SmackerCommon::BitReader &bits, std::vector<int> &recode, int *last)
{
	int *table = &recode[0];

    int v;
    while (*table & kSMKnode)
	{
        if (bits.GetBit())
            table += (*table) & (~kSMKnode);
        table++;
    }
    v = *table;

    if (last != nullptr && v != recode[last[0]]) {
        recode[last[2]] = recode[last[1]];
        recode[last[1]] = recode[last[0]];
        recode[last[0]] = v;
    }

    return v;
}

bool SmackerDecoder::Open(SDL_RWops *rwops)
{
	// open the file (read only)
	file.Open(rwops);
	if (!file.Is_Open())
	{
		SmackerCommon::LogError("Can't open Smacker video");
		return false;
	}

	// check the file signature
	signature = file.ReadUint32BE();
	if ((signature != kSMK2iD)
		&& (signature != kSMK4iD))
	{
		SmackerCommon::LogError("Unknown Smacker signature");
		return false;
	}

	if (kSMK4iD == signature)
		isVer4 = true;

	frameWidth  = file.ReadUint32LE();
	frameHeight = file.ReadUint32LE();
	nFrames = file.ReadUint32LE();

	/* Corrupt-header guard: this build runs with -fno-exceptions, so an
	 * absurd allocation would throw from prebuilt libstdc++ and abort the
	 * whole device. Bound everything before allocating. */
	if (frameWidth == 0 || frameHeight == 0
	    || frameWidth > 2048 || frameHeight > 2048
	    || nFrames == 0 || nFrames > 0xFFFFF)
	{
		SmackerCommon::LogError("Implausible Smacker header");
		return false;
	}

	picture = new uint8_t[frameWidth * frameHeight];
	memset(picture, 0, frameWidth * frameHeight);

	int32_t frameRate = file.ReadUint32LE();

	if (frameRate > 0)
		fps = 1000 / frameRate;
	else if (frameRate < 0)
		fps = 100000 / (-frameRate);
	else
		fps = 10;

	uint32_t flags = file.ReadUint32LE();

	if (flags & kFlagRingFrame)
	{
		nFrames++;
	}

	for (int i = 0; i < kMaxAudioTracks; i++)
	{
		audioTracks[i].sizeInBytes = file.ReadUint32LE();
	}

	treeSize = file.ReadUint32LE();
	mMapSize = file.ReadUint32LE();
	MClrSize = file.ReadUint32LE();
	fullSize = file.ReadUint32LE();
	typeSize = file.ReadUint32LE();

	for (int i = 0; i < kMaxAudioTracks; i++)
	{
		audioTracks[i].flags = file.ReadUint32LE();
	}

	// skip pad
	file.Skip(4);

	if (nFrames > 0xFFFFFF)
	{
		SmackerCommon::LogError("Too many frames!");
		return false;
	}

	frameSizes.resize(nFrames);
	frameFlags.resize(nFrames);

	// read frame info
	for (uint32_t i = 0; i < nFrames; i++) {
		frameSizes[i] = file.ReadUint32LE();
	}
	for (uint32_t i = 0; i < nFrames; i++) {
		frameFlags[i] = file.ReadByte();
	}

	// handle possible audio streams
	for (int i = 0; i < kMaxAudioTracks; i++)
	{
		audioTracks[i].buffer = 0;
		audioTracks[i].bufferSize = 0;
		audioTracks[i].bytesReadThisFrame = 0;

		if (audioTracks[i].flags & 0xFFFFFF)
		{
/*
            if (audioTracks[i].flags & SMK_AUD_BINKAUD) {
                audioTracks[i].compressionType = SMK_AUD_BINKAUD;
            } else if (audioTracks[i].flags & SMK_AUD_USEDCT) {
                audioTracks[i].compressionType = SMK_AUD_USEDCT;
            } else if (audioTracks[i].flags & SMK_AUD_PACKED){
                ast[i]->codec->codec_id = CODEC_ID_SMACKAUDIO;
                ast[i]->codec->codec_tag = MKTAG('S', 'M', 'K', 'A');
            } else {
                ast[i]->codec->codec_id = CODEC_ID_PCM_U8;
            }
*/
			audioTracks[i].nChannels = (audioTracks[i].flags & SMK_AUD_STEREO) ? 2 : 1;
			audioTracks[i].sampleRate = audioTracks[i].flags & 0xFFFFFF;
			audioTracks[i].bitsPerSample = (audioTracks[i].flags & SMK_AUD_16BITS) ? 16 : 8;
		}
	}

	memset(palette, 0, 768);
	
	DecodeHeaderTrees();

	// set nextPos to where we are now, as next data is frame 1
	nextPos = file.GetPosition();
	firstPos = nextPos;

	// determine max buffer sizes for audio tracks
	file.Seek(nextPos, SmackerCommon::FileStream::kSeekStart);

	uint32_t frameSize = frameSizes[0] & (~3);
	uint8_t frameFlag  = frameFlags[0];

	// skip over palette
	if (frameFlag & kSMKpal)
	{
		uint32_t size = file.ReadByte();
		size = size * 4 - 1;
		file.Skip(size);
	}

	frameFlag >>= 1;

	for (int i = 0; i < kMaxAudioTracks; i++) 
	{
		if (frameFlag & 1) 
		{
			// skip size
			file.Skip(4);
			
			uint32_t unpackedSize = file.ReadUint32LE();
			if (unpackedSize == 0 || unpackedSize > (1u << 22)) {
				SmackerCommon::LogError("Implausible Smacker audio buffer");
				return false;
			}

			audioTracks[i].bufferSize = unpackedSize;
			audioTracks[i].buffer = new uint8_t[unpackedSize];
		}	
		frameFlag >>= 1;
	}

	return true;
}

int SmackerDecoder::DecodeTree(SmackerCommon::BitReader &bits, HuffContext *hc)
{
    if (!bits.GetBit()) // Leaf
	{
        if (hc->leaves >= 256){
			SmackerCommon::LogError("Tree size exceeded!");
            return -1;
        }

        hc->values.push_back(bits.GetBits(8));
        hc->leaves++;
        return 0;
    } else { //Node
        int t = hc->values.size();
        hc->values.push_back(0);

        int r = DecodeTree(bits, hc);
        if (r)
            return r;

        hc->values[t] = kSMKnode | (hc->values.size() - t - 1);
        return DecodeTree(bits, hc);
    }
}

/**
 * Decode header tree
 */
int SmackerDecoder::DecodeBigTree(SmackerCommon::BitReader &bits, HuffContext *hc, DBCtx *ctx)
{
	if (!bits.GetBit()) // Leaf
	{
		int i1 = 0;
		int i2 = 0;

		if (hc->leaves >= hc->length) {
			SmackerCommon::LogError("Tree size exceeded!");
            return -1;
        }

		if (ctx->recode1.size()) {
			i1 = GetCode(bits, ctx->recode1, nullptr);
		}

		if (ctx->recode2.size()) {
			i2 = GetCode(bits, ctx->recode2, nullptr);
		}

        if (i1 < 0 || i2 < 0)
            return -1;
        
        int val = i1 | (i2 << 8);
        if (val == ctx->escapes[0]) {
            ctx->last[0] = hc->values.size();
            val = 0;
        } else if (val == ctx->escapes[1]) {
            ctx->last[1] = hc->values.size();
            val = 0;
        } else if (val == ctx->escapes[2]) {
            ctx->last[2] = hc->values.size();
            val = 0;
        }

        hc->values.push_back(val);
        hc->leaves++;
        return 1;
    } else { //Node
        int t = hc->values.size();
        hc->values.push_back(0);

        int r = DecodeBigTree(bits, hc, ctx);
        if (r < 0)
            return r;

        hc->values[t] = kSMKnode | r;
        r++;
        r += DecodeBigTree(bits, hc, ctx);
        return r;
    }
}

/**
 * Store large tree as Libav's vlc codes
 */
int SmackerDecoder::DecodeHeaderTree(SmackerCommon::BitReader &bits, std::vector<int> &recodes, int *last, int size)
{
	HuffContext huff;
	HuffContext tmp1, tmp2;
	int escapes[3];
	DBCtx ctx;

	if (size >= UINT_MAX>>4)
	{
		SmackerCommon::LogError("Size too large");
		return -1;
	}

	tmp1.leaves = 0;
	tmp1.values.reserve(511);

	tmp2.leaves = 0;
	tmp2.values.reserve(511);

	// low byte tree
	if (bits.GetBit()) // 1: Read Tag
	{
		DecodeTree(bits, &tmp1);
		bits.SkipBits(1);
	}
	else
	{
		// Skipping low bytes tree
	}

	// high byte tree
	if (bits.GetBit())
	{
		DecodeTree(bits, &tmp2);
		bits.SkipBits(1);
	}
	else
	{
		// Skipping high bytes tree
	}

	escapes[0]  = bits.GetBits(8);
	escapes[0] |= bits.GetBits(8) << 8;
	escapes[1]  = bits.GetBits(8);
	escapes[1] |= bits.GetBits(8) << 8;
	escapes[2]  = bits.GetBits(8);
	escapes[2] |= bits.GetBits(8) << 8;

	last[0] = last[1] = last[2] = -1;

	ctx.escapes[0] = escapes[0];
    ctx.escapes[1] = escapes[1];
    ctx.escapes[2] = escapes[2];

    ctx.recode1 = tmp1.values;
    ctx.recode2 = tmp2.values;
    ctx.last = last;

	huff.leaves = 0;
	huff.length = ((size + 3) >> 2) + 3;
	if (huff.length > (1u << 22)) {
		/* stream-supplied size; cap so reserve() cannot throw (see the
		 * -fno-exceptions note in Open). */
		SmackerCommon::LogError("Implausible Smacker tree size");
		return false;
	}
	huff.values.reserve(2 * huff.length - 1);
	DecodeBigTree(bits, &huff, &ctx);

    bits.SkipBits(1);

	int current = huff.values.size();
	if (ctx.last[0] == -1)
		ctx.last[0] = current++;
	if (ctx.last[1] == -1)
		ctx.last[1] = current++;
	if (ctx.last[2] == -1)
		ctx.last[2] = current++;

	huff.values.resize(current);
	recodes = huff.values;
	return true;
}

// static int decode_header_trees(SmackVContext *smk) {
bool SmackerDecoder::DecodeHeaderTrees()
{
	SmackerCommon::BitReader bits(file, treeSize);

	if (!bits.GetBit())
	{
		// Skipping MMAP tree
		mmap_tbl.resize(2);
		mmap_tbl[0] = 0;
		mmap_last[0] = mmap_last[1] = mmap_last[2] = 1;
	}
	else
	{
		DecodeHeaderTree(bits, mmap_tbl, mmap_last, mMapSize);
	}

	if (!bits.GetBit())
	{
		// Skipping MCLR tree
		mclr_tbl.resize(2);
		mclr_tbl[0] = 0;
		mclr_last[0] = mclr_last[1] = mclr_last[2] = 1;
	}
	else
	{
		DecodeHeaderTree(bits, mclr_tbl, mclr_last, MClrSize);
	}

	if (!bits.GetBit())
	{
		// Skipping FULL tree
		full_tbl.resize(2);
		full_tbl[0] = 0;
		full_last[0] = full_last[1] = full_last[2] = 1;
	}
	else
	{
		DecodeHeaderTree(bits, full_tbl, full_last, fullSize);
	}

	if (!bits.GetBit())
	{
		// Skipping TYPE tree
		type_tbl.resize(2);
		type_tbl[0] = 0;
		type_last[0] = type_last[1] = type_last[2] = 1;
	}
	else
	{
		DecodeHeaderTree(bits, type_tbl, type_last, typeSize);
	}

	/* FIXME - we don't seems to read/use EVERY bit we 'load' into the bit reader
	 * and as my bitreader reads from the file rather than a buffer read from file
	 * of size 'treeSize', I need to make sure I consume the remaining bits (and thus increment
	 * the file read position to where the code expects it to be when this function returns (ie 
	 * 'treeSize' number of bytes must be read
	 */
	uint32_t left = bits.GetSize() - bits.GetPosition();
	bits.SkipBits(left);

	return true;
}

void SmackerDecoder::GetNextFrame()
{
	ReadPacket();
}

int SmackerDecoder::ReadPacket()
{
	// test-remove
	if (currentFrame >= nFrames)
		return 1;

	// seek to next frame position
	file.Seek(nextPos, SmackerCommon::FileStream::kSeekStart);

	uint32_t frameSize = frameSizes[currentFrame] & (~3);
	uint8_t frameFlag  = frameFlags[currentFrame];
	// Keyframe flag is bit 0 of the (dword-aligned) frame size entry; the
	// absolute end of this frame is fixed regardless of how much the
	// palette/audio parsing below consumes.
	const bool frameIsKeyframe = (frameSizes[currentFrame] & 1) != 0;
	const uint32_t frameEndPos = nextPos + frameSize;

	// handle palette change
	if (frameFlag & kSMKpal)
	{
		int size, sz, t, off, j, pos;
        uint8_t *pal = palette;
        uint8_t oldpal[768];

        memcpy(oldpal, pal, 768);
        size = file.ReadByte();
        size = size * 4 - 1;
        frameSize -= size;
        frameSize--;
        sz = 0;
        pos = file.GetPosition() + size;

        while (sz < 256)
		{
            t = file.ReadByte();
            if (t & 0x80){ /* skip palette entries */
                sz  += (t & 0x7F)  + 1;
                pal += ((t & 0x7F) + 1) * 3;
            } else if (t & 0x40){ /* copy with offset */
                off = file.ReadByte() * 3;
                j = (t & 0x3F) + 1;
                while (j-- && sz < 256) {
                    *pal++ = oldpal[off + 0];
                    *pal++ = oldpal[off + 1];
                    *pal++ = oldpal[off + 2];
                    sz++;
                    off += 3;
                }
            } else { /* new entries */
                *pal++ = smk_pal[t];
                *pal++ = smk_pal[file.ReadByte() & 0x3F];
                *pal++ = smk_pal[file.ReadByte() & 0x3F];
                sz++;
            }
        }
        
		paletteChanged = true;
		file.Seek(pos, SmackerCommon::FileStream::kSeekStart);
	}

	frameFlag >>= 1;

	// check for and handle audio
	for (int i = 0; i < kMaxAudioTracks; i++) 
	{
		audioTracks[i].bytesReadThisFrame = 0;

		if (frameFlag & 1) 
		{
			uint32_t size = file.ReadUint32LE() - 4;
			frameSize -= size;
			frameSize -= 4;

			DecodeAudio(size, audioTracks[i]);
		}
		frameFlag >>= 1;
	}

	if (frameSize < 0)
		return -1;

	if (!videoKeyframesOnly || frameIsKeyframe || forceNextVideoDecode) {
		forceNextVideoDecode = false;
		DecodeFrame(frameSize);
		lastFrameHadVideo = true;
		nextPos = file.GetPosition();
	} else {
		// Audio-priority mode: skip the video chunk bytewise. The held
		// picture stays the last decoded keyframe, so it is always
		// self-consistent (deltas never resume from a skipped frame).
		file.Seek(frameEndPos, SmackerCommon::FileStream::kSeekStart);
		lastFrameHadVideo = false;
		nextPos = frameEndPos;
	}

	currentFrame++;

	return 0;
}

int SmackerDecoder::DecodeFrame(uint32_t frameSize)
{
	last_reset(mmap_tbl, mmap_last);
    last_reset(mclr_tbl, mclr_last);
    last_reset(full_tbl, full_last);
    last_reset(type_tbl, type_last);

	int blocks, blk, bw, bh;
    int i;
    int stride;

	uint8_t *out = picture; // set to output image

	blk = 0;
    bw = frameWidth  >> 2;
    bh = frameHeight >> 2;
    blocks = bw * bh;

	stride = frameWidth;

	uint32_t fileStart = file.GetPosition();

	SmackerCommon::BitReader bits(file, frameSize);

	while (blk < blocks)
	{
		int type, run, mode;
        uint16_t pix;

        type = GetCode(bits, type_tbl, type_last);
        run = block_runs[(type >> 2) & 0x3F];
        switch (type & 3)
		{
        case SMK_BLK_MONO:
            while (run-- && blk < blocks)
			{
                int clr, map;
                int hi, lo;
                clr = GetCode(bits, mclr_tbl, mclr_last);
                map = GetCode(bits, mmap_tbl, mmap_last);

                out = picture + (blk / bw) * (stride * 4) + (blk % bw) * 4;

                hi = clr >> 8;
                lo = clr & 0xFF;
                for (i = 0; i < 4; i++) 
				{
                    if (map & 1) out[0] = hi; else out[0] = lo;
                    if (map & 2) out[1] = hi; else out[1] = lo;
                    if (map & 4) out[2] = hi; else out[2] = lo;
                    if (map & 8) out[3] = hi; else out[3] = lo;
                    map >>= 4;
                    out += stride;
                }
                blk++;
            }
            break;
        case SMK_BLK_FULL:
            mode = 0;
			if (kSMK4iD == signature) // In case of Smacker v4 we have three modes
			{
				if (bits.GetBit()) mode = 1;
				else if (bits.GetBit()) mode = 2;
			}

            while (run-- && blk < blocks)
			{
                out = picture + (blk / bw) * (stride * 4) + (blk % bw) * 4;
                switch (mode)
				{
                case 0:
                    for (i = 0; i < 4; i++)
					{
                        pix = GetCode(bits, full_tbl, full_last);
// FIX                        AV_WL16(out+2, pix);
						out[2] = pix & 0xff;
						out[3] = pix >> 8;

                        pix = GetCode(bits, full_tbl, full_last);
// FIX                        AV_WL16(out, pix);
						out[0] = pix & 0xff;
						out[1] = pix >> 8;
                        out += stride;
                    }
                    break;
                case 1:
                    pix = GetCode(bits, full_tbl, full_last);
                    out[0] = out[1] = pix & 0xFF;
                    out[2] = out[3] = pix >> 8;
                    out += stride;
                    out[0] = out[1] = pix & 0xFF;
                    out[2] = out[3] = pix >> 8;
                    out += stride;
                    pix = GetCode(bits, full_tbl, full_last);
                    out[0] = out[1] = pix & 0xFF;
                    out[2] = out[3] = pix >> 8;
                    out += stride;
                    out[0] = out[1] = pix & 0xFF;
                    out[2] = out[3] = pix >> 8;
                    out += stride;
                    break;
                case 2:
                    for (i = 0; i < 2; i++)
					{
                        uint16_t pix1, pix2;
                        pix2 = GetCode(bits, full_tbl, full_last);
                        pix1 = GetCode(bits, full_tbl, full_last);

// FIX                        AV_WL16(out, pix1);
// FIX                        AV_WL16(out+2, pix2);
						out[0] = pix1 & 0xff;
						out[1] = pix1 >> 8;
						out[2] = pix2 & 0xff;
						out[3] = pix2 >> 8;

                        out += stride;

// FIX                        AV_WL16(out, pix1);
// FIX                        AV_WL16(out+2, pix2);
						out[0] = pix1 & 0xff;
						out[1] = pix1 >> 8;
						out[2] = pix2 & 0xff;
						out[3] = pix2 >> 8;

                        out += stride;
                    }
                    break;
                }
                blk++;
            }
            break;
        case SMK_BLK_SKIP:
            while (run-- && blk < blocks)
                blk++;
            break;
        case SMK_BLK_FILL:
            mode = type >> 8;
            while (run-- && blk < blocks)
			{
                uint32_t col;
                out = picture + (blk / bw) * (stride * 4) + (blk % bw) * 4;
                col = mode * 0x01010101;
                for (i = 0; i < 4; i++) {
                    *((uint32_t*)out) = col;
                    out += stride;
                }
                blk++;
            }
            break;
        }
	}

	/* FIXME - we don't seems to read/use EVERY bit we 'load' into the bit reader
	 * and as my bitreader reads from the file rather than a buffer read from file
	 * of size 'frameSize', I need to make sure I consume the remaining bits (and thus increment
	 * the file read position to where the code expects it to be when this function returns (ie 
	 * 'frameSize' number of bytes must be read
	 */
	uint32_t left = bits.GetSize() - bits.GetPosition();
	bits.SkipBits(left);

	return 0;
}

/**
 * Decode Smacker audio data
 */
int SmackerDecoder::DecodeAudio(uint32_t size, SmackerAudioTrack &track)
{
    HuffContext h[4];
    int16_t *samples = reinterpret_cast<int16_t*>(track.buffer);
    int8_t *samples8 = reinterpret_cast<int8_t*>(track.buffer);
    int val;
    int i, res;
    int unpackedSize;
    int sampleBits, stereo;
    int pred[2] = {0, 0};

	int buf_size = track.bufferSize;

    if (buf_size <= 4) {
		SmackerCommon::LogError("packet is too small");
		return -1;
    }

	SmackerCommon::BitReader bits(file, size);

    unpackedSize = bits.GetBits(32);

    if (!bits.GetBit()) {
		// no sound data
        return 1;
    }

    /* track.buffer was sized from frame 0's unpackedSize at Open(); a frame
     * claiming more would overflow it (the sample-write loops below are
     * bounded only by this stream-supplied value). Corrupt-input guard. */
    if (unpackedSize < 0 || (uint32_t)unpackedSize > track.bufferSize)
        return -1;

    stereo     = bits.GetBit();
    sampleBits = bits.GetBit();

    if (stereo ^ (track.nChannels != 1)) {
		SmackerCommon::LogError("channels mismatch");
		return -1;
    }

    // Initialize
    for (i = 0; i < (1 << (sampleBits + stereo)); i++) {
        h[i].leaves = 0;
        h[i].values.reserve(511);

        bits.SkipBits(1);
		DecodeTree(bits, &h[i]);
        bits.SkipBits(1);
    }
    if (sampleBits) { //decode 16-bit data
        for (i = stereo; i >= 0; i--)
            pred[i] = av_bswap16(bits.GetBits(16));
        for (i = 0; i <= stereo; i++)
            *samples++ = pred[i];
        for (; i < unpackedSize / 2; i++) {
            if (i & stereo) {
				if (h[2].values.size())
					res = GetCode(bits, h[2].values, nullptr);
                else
                    res = 0;
                val  = res;
				if (h[3].values.size())
					res = GetCode(bits, h[3].values, nullptr);
                else
                    res = 0;
                val |= res << 8;
                pred[1] += (int16_t)val;
                *samples++ = pred[1];
            } else {
				if (h[0].values.size())
					res = GetCode(bits, h[0].values, nullptr);
                else
                    res = 0;
                val  = res;
				if (h[1].values.size())
					res = GetCode(bits, h[1].values, nullptr);
                else
                    res = 0;
                val |= res << 8;
                pred[0] += val;
                *samples++ = pred[0];
            }
        }
    } 
	else { //8-bit data
        for (i = stereo; i >= 0; i--)
            pred[i] = bits.GetBits(8);
        for (i = 0; i <= stereo; i++)
            *samples8++ = pred[i];
        for (; i < unpackedSize; i++) {
            if (i & stereo){
				if (h[1].values.size())
					res = GetCode(bits, h[1].values, nullptr);
                else
                    res = 0;
                pred[1] += (int8_t)res;
                *samples8++ = pred[1];
            } else {
				if (h[0].values.size())
				res = GetCode(bits, h[0].values, nullptr);
                else
                    res = 0;
                pred[0] += (int8_t)res;
                *samples8++ = pred[0];
            }
        }
    }

	track.bytesReadThisFrame = unpackedSize;

	uint32_t left = bits.GetSize() - bits.GetPosition();
	bits.SkipBits(left);

	return 0;
}

void SmackerDecoder::Rewind()
{
	currentFrame = 0;
	nextPos = firstPos;
}

void SmackerDecoder::GetPalette(uint8_t *palette)
{
	memcpy(palette, this->palette, 768);
}

bool SmackerDecoder::DidPaletteChange()
{
	bool didPaletteChange = paletteChanged;
	paletteChanged = false;
	return didPaletteChange;
}

void SmackerDecoder::GetFrame(uint8_t *frame)
{
	memcpy(frame, this->picture, frameWidth * frameHeight);
}

void SmackerDecoder::GetFrameSize(uint32_t &width, uint32_t &height)
{
	width  = this->frameWidth;
	height = this->frameHeight;
}

uint32_t SmackerDecoder::GetNumFrames()
{
	return nFrames;
}

uint32_t SmackerDecoder::GetCurrentFrameNum()
{
	return currentFrame;
}

float SmackerDecoder::GetFrameRate()
{
	return (float)fps;
}

SmackerAudioInfo SmackerDecoder::GetAudioTrackDetails(uint32_t trackIndex)
{
	SmackerAudioInfo info;
	SmackerAudioTrack *track = &audioTracks[trackIndex];

	info.sampleRate = track->sampleRate;
	info.nChannels  = track->nChannels;
	info.bitsPerSample = track->bitsPerSample;

	// audio buffer size in bytes
	info.idealBufferSize = track->bufferSize;

	return info;
}

uint32_t SmackerDecoder::GetAudioData(uint32_t trackIndex, int16_t *audioBuffer)
{
	if (!audioBuffer)
		return 0;

	SmackerAudioTrack *track = &audioTracks[trackIndex];

	if (track->bytesReadThisFrame)
		memcpy(audioBuffer, track->buffer, std::min(track->bufferSize, track->bytesReadThisFrame));

	return track->bytesReadThisFrame;
}
