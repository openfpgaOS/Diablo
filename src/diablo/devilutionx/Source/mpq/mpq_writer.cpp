#include "mpq/mpq_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

#include "appfat.h"
#include "encrypt.h"
#include "engine.h"
#include "utils/endian_write.hpp"
#include "utils/file_util.h"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

#ifdef OPENFPGAOS
#ifndef OF_NV_SLOT_CAPACITY
// [of] Capacity of one openfpgaOS nonvolatile save slot. The Pocket maps each
// save to a fixed 256 KB CRAM0 window laid out contiguously (0x40000 stride
// from 0x20100000; see the core's data.json and the firmware nvslot map). This
// value is a firmware + bitstream contract (OF_TARGET_SAVE_MAX_SLOTS /
// NV_SLOT_BYTES) -- changing it here alone does nothing; the slot size can only
// be grown by changing data.json, the OS nvslot map, and the CRAM window in
// lockstep. Used only as an overflow backstop below.
#define OF_NV_SLOT_CAPACITY 0x40000u
#endif
#endif

namespace devilution {

namespace {

// Validates that a Type is of a particular size and that its alignment is <= the size of the type.
// Done with templates so that error messages include actual size.
template <size_t A, size_t B>
struct AssertEq : std::true_type {
	static_assert(A == B, "A == B not satisfied");
};
template <size_t A, size_t B>
struct AssertLte : std::true_type {
	static_assert(A <= B, "A <= B not satisfied");
};
template <typename T, size_t S>
struct CheckSize : AssertEq<sizeof(T), S>, AssertLte<alignof(T), sizeof(T)> {
};

// Check sizes and alignments of the structs that we decrypt and encrypt.
// The decryption algorithm treats them as a stream of 32-bit uints, so the
// sizes must be exact as there cannot be any padding.
static_assert(CheckSize<MpqHashEntry, static_cast<size_t>(4 * 4)>::value, "sizeof(MpqHashEntry) == 4 * 4 && alignof(MpqHashEntry) <= 4 * 4 not satisfied");
static_assert(CheckSize<MpqBlockEntry, static_cast<size_t>(4 * 4)>::value, "sizeof(MpqBlockEntry) == 4 * 4 && alignof(MpqBlockEntry) <= 4 * 4 not satisfied");

// [of] Table sizes are per-archive now (MpqWriter::hashEntriesCount_ /
// blockEntriesCount_), adopted from the header of an existing archive and
// defaulted to MpqWriter::New*EntriesCount for one we create. Only the bounds
// are fixed here, so a corrupt or hostile header cannot drive the allocation.
//
// Note the old file-scope byte-count constants had their entry counts crossed
// over -- the block table's byte size was derived from the hash entry count
// and vice versa. Harmless only because both were 2048; it would have
// corrupted both tables the moment they differed, which is exactly what this
// change makes possible. The accessors below derive each from its own count.
constexpr uint32_t MinTableEntriesCount = 64;
constexpr uint32_t MaxTableEntriesCount = 2048;

// A power of two within bounds: the hash probe wraps with a mask, so anything
// else would index outside the table.
bool IsSupportedTableCount(uint32_t count)
{
	return count >= MinTableEntriesCount && count <= MaxTableEntriesCount
	    && (count & (count - 1)) == 0;
}

// We store the block and the hash entry tables immediately after the header.
// This is unlike most other MPQ archives, that store these at the end of the file.
constexpr long MpqBlockEntryOffset = sizeof(MpqFileHeader);

// Special return value for `GetHashIndex` and `GetHandle`.
constexpr uint32_t HashEntryNotFound = -1;

// [of] Sector size is per-archive too (MpqWriter::blockSizeFactor_), adopted
// from an existing header and defaulted to MpqWriter::NewBlockSizeFactor for a
// new archive. Bounds only here: factor 3 is the 4 KB every shipped MPQ uses
// (DIABDAT and devilutionx.mpq both), factor 7 is the 64 KB new saves get.
//
// Why it matters: every sector is compressed independently, so PkWare's 4 KB
// implode dictionary was being reset every 4096 bytes. Letting it run across
// what used to be sector boundaries costs nothing and shrinks a finished
// 16-level Hellfire save by ~11%, measured. libmpq derives its own block size
// from the header (mpq.c: `block_size = 512 << header.block_size`), so the read
// path needs no change.
constexpr uint16_t MinBlockSizeFactor = 3;
constexpr uint16_t MaxBlockSizeFactor = 7;

bool IsSupportedBlockSizeFactor(uint16_t factor)
{
	return factor >= MinBlockSizeFactor && factor <= MaxBlockSizeFactor;
}

// Sometimes we can end up with smaller blocks.
constexpr uint32_t MinBlockSize = 1024;

void ByteSwapHdr(MpqFileHeader *hdr)
{
	hdr->signature = SDL_SwapLE32(hdr->signature);
	hdr->headerSize = SDL_SwapLE32(hdr->headerSize);
	hdr->fileSize = SDL_SwapLE32(hdr->fileSize);
	hdr->version = SDL_SwapLE16(hdr->version);
	hdr->blockSizeFactor = SDL_SwapLE16(hdr->blockSizeFactor);
	hdr->hashEntriesOffset = SDL_SwapLE32(hdr->hashEntriesOffset);
	hdr->blockEntriesOffset = SDL_SwapLE32(hdr->blockEntriesOffset);
	hdr->hashEntriesCount = SDL_SwapLE32(hdr->hashEntriesCount);
	hdr->blockEntriesCount = SDL_SwapLE32(hdr->blockEntriesCount);
}

bool IsAllocatedUnusedBlock(const MpqBlockEntry *block)
{
	return block->offset != 0 && block->flags == 0 && block->unpackedSize == 0;
}

bool IsUnallocatedBlock(const MpqBlockEntry *block)
{
	return block->offset == 0 && block->packedSize == 0 && block->unpackedSize == 0 && block->flags == 0;
}

} // namespace

MpqWriter::MpqWriter(const char *path, bool recreate)
{
	const std::string dir = std::string(Dirname(path));
	RecursivelyCreateDir(dir.c_str());
	LogVerbose("Opening {}", path);
	bool isNewFile = false;
	std::string error;
	if (!FileExists(path)) {
		// FileExists() may return false in the case of an error
		// so we use "ab" instead of "wb" to avoid accidentally
		// truncating an existing file
		stream_.Open(path, "ab");

		// However, we cannot actually use a file handle that was
		// opened in "ab" mode because we need to be able to seek
		// and write to the middle of the file
		stream_.Close();
	}

	if (!GetFileSize(path, &size_)) {
		error = R"(GetFileSize failed: "{}")";
		LogError(error, path, std::strerror(errno));
		goto on_error;
	}
	isNewFile = size_ == 0;
	LogVerbose("GetFileSize(\"{}\") = {}", path, size_);

	if (!stream_.Open(path, "r+b")) {
		stream_.Close();
		error = "Failed to open file";
		goto on_error;
	}

	name_ = path;

	if (blockTable_ == nullptr || hashTable_ == nullptr) {
		MpqFileHeader fhdr;
		// [of] Treat a header-read failure the same as a new file. On the
		// slot FS, a fresh nonvolatile slot reports its full CRAM capacity
		// as the file size (open_nv_fd in the kernel) but the bytes are
		// zero, so MPQ-magic-check on the read header fails. Initializing
		// a default header lets subsequent EncodeHero/SaveHeroItems writes
		// populate the MPQ in place instead of falling into on_error.
		// [of] `recreate` skips the adopt-existing-layout path entirely, so the
		// archive is rebuilt with this build's geometry (see the constructor
		// comment). fhdr is zeroed, so the table reads below are skipped and
		// the in-memory tables start empty.
		if (recreate || isNewFile || !ReadMPQHeader(&fhdr)) {
			InitDefaultMpqHeader(&fhdr);
		}
		// [of] Sized from the geometry ReadMPQHeader adopted, not a constant.
		blockTable_ = std::make_unique<MpqBlockEntry[]>(blockEntriesCount_);
		std::memset(blockTable_.get(), 0, BlockTableBytes());
		if (fhdr.blockEntriesCount > 0) {
			if (!stream_.Read(reinterpret_cast<char *>(blockTable_.get()), static_cast<size_t>(fhdr.blockEntriesCount * sizeof(MpqBlockEntry)))) {
				error = "Failed to read block table";
				goto on_error;
			}
			uint32_t key = Hash("(block table)", 3);
			Decrypt(reinterpret_cast<uint32_t *>(blockTable_.get()), fhdr.blockEntriesCount * sizeof(MpqBlockEntry), key);
		}
		hashTable_ = std::make_unique<MpqHashEntry[]>(hashEntriesCount_);

		// We fill with 0xFF so that the `block` field defaults to -1 (a null block pointer).
		std::memset(hashTable_.get(), 0xFF, HashTableBytes());

		if (fhdr.hashEntriesCount > 0) {
			if (!stream_.Read(reinterpret_cast<char *>(hashTable_.get()), static_cast<size_t>(fhdr.hashEntriesCount * sizeof(MpqHashEntry)))) {
				error = "Failed to read hash entries";
				goto on_error;
			}
			uint32_t key = Hash("(hash table)", 3);
			Decrypt(reinterpret_cast<uint32_t *>(hashTable_.get()), fhdr.hashEntriesCount * sizeof(MpqHashEntry), key);
		}

#ifndef CAN_SEEKP_BEYOND_EOF
		if (!stream_.Seekp(0, SEEK_SET))
			goto on_error;

		// Memorize stream begin, we'll need it for calculations later.
		if (!stream_.Tellp(&streamBegin_))
			goto on_error;

		// Write garbage header and tables because some platforms cannot `Seekp` beyond EOF.
		// The data is incorrect at this point, it will be overwritten on Close.
		if (isNewFile)
			WriteHeaderAndTables();
#endif
	}
	valid_ = true; // [of] ctor reached the end without on_error -- methods may operate
	return;
on_error:
	// [of] Instead of app_fatal-exiting back to the launcher when the save
	// slot is unwritable, log and leave valid_=false so the public methods
	// short-circuit. DevilutionX runs in-memory with no save persistence.
	LogError("MpqWriter: cannot open '{}' for writing ({}). Save subsystem disabled for this session.", path, error);
}

MpqWriter::~MpqWriter()
{
	// [of] Belt-and-suspenders: if the ctor never reached its `valid_=true`
	// success path, blockTable_/hashTable_ are null and WriteHeaderAndTables
	// would dereference them (the encrypt+write loop hangs/faults). Close
	// the stream if we got that far and bail.
	if (!valid_) {
		if (stream_.IsOpen())
			stream_.Close();
		return;
	}
	if (!stream_.IsOpen())
		return;
	LogVerbose("Closing {}", name_);

	bool result = true;
	if (!(stream_.Seekp(0, SEEK_SET) && WriteHeaderAndTables()))
		result = false;
	stream_.Close();
	if (result && size_ != 0) {
		LogVerbose("ResizeFile(\"{}\", {})", name_, size_);
		result = ResizeFile(name_.c_str(), size_);
	}
	if (!result)
		LogVerbose("Closing failed {}", name_);
}

uint32_t MpqWriter::FetchHandle(const char *filename) const
{
	return GetHashIndex(Hash(filename, 0), Hash(filename, 1), Hash(filename, 2));
}

void MpqWriter::InitDefaultMpqHeader(MpqFileHeader *hdr)
{
	std::memset(hdr, 0, sizeof(*hdr));
	hdr->signature = MpqFileHeader::DiabloSignature;
	hdr->headerSize = MpqFileHeader::DiabloSize;
	hdr->blockSizeFactor = NewBlockSizeFactor;
	blockSizeFactor_ = NewBlockSizeFactor;
	hdr->version = 0;
	// [of] A brand-new archive gets this build's geometry. Reset explicitly:
	// this is also the fallback path for an unreadable header on a writer that
	// may already have adopted a previous archive's larger tables.
	hashEntriesCount_ = NewHashEntriesCount;
	blockEntriesCount_ = NewBlockEntriesCount;
	size_ = HashEntriesOffset() + HashTableBytes();
}

bool MpqWriter::IsValidMpqHeader(MpqFileHeader *hdr) const
{
	return hdr->signature == MpqFileHeader::DiabloSignature
	    && hdr->headerSize == MpqFileHeader::DiabloSize
	    && hdr->version <= 0
	    // [of] Sector size stays pinned. Every archive this game has ever
	    // written uses factor 3 (4 KB), PkWare's implode dictionary maxes out
	    // at 4096 anyway, and keeping it constant keeps the per-sector scratch
	    // buffer a fixed-size stack array. Only the TABLE geometry varies.
	    && IsSupportedBlockSizeFactor(hdr->blockSizeFactor)
	    // [of] `<=`, not `==`: on the slot FS truncate() is a no-op, so the
	    // physical size reported by stat() can be LARGER than the archive's
	    // logical size whenever a save shrinks (and a fresh-but-committed
	    // slot may report its full 256 KB capacity). Requiring equality made
	    // every such reopen discard the existing archive via
	    // InitDefaultMpqHeader -- i.e. one shrinking save silently erased
	    // the hero. The stale tail past `fileSize` is harmless: both this
	    // writer and libmpq address blocks by header offsets, never by
	    // physical file size. ReadMPQHeader() adopts the logical size.
	    && hdr->fileSize <= size_
	    && hdr->blockEntriesOffset == sizeof(MpqFileHeader)
	    // [of] Accept ANY supported table geometry rather than only this
	    // build's. Requiring equality here is what would turn a table-size
	    // change into "every existing save is silently reinitialized".
	    // The tables sit immediately after the header, so the hash table's
	    // offset is implied by the block table's length -- verifying that
	    // relation is what makes an adopted geometry safe to index with.
	    && IsSupportedTableCount(hdr->hashEntriesCount)
	    && IsSupportedTableCount(hdr->blockEntriesCount)
	    && hdr->hashEntriesOffset == sizeof(MpqFileHeader) + hdr->blockEntriesCount * sizeof(MpqBlockEntry);
}

bool MpqWriter::ReadMPQHeader(MpqFileHeader *hdr)
{
	const bool hasHdr = size_ >= sizeof(*hdr);
	if (hasHdr) {
		if (!stream_.Read(reinterpret_cast<char *>(hdr), sizeof(*hdr)))
			return false;
		ByteSwapHdr(hdr);
	}
	if (!hasHdr || !IsValidMpqHeader(hdr)) {
		InitDefaultMpqHeader(hdr);
	} else {
		// [of] Adopt this archive's declared table geometry before the tables
		// are allocated, so a save written by an older build (2048/2048) keeps
		// working byte-for-byte instead of being discarded and recreated.
		hashEntriesCount_ = hdr->hashEntriesCount;
		blockEntriesCount_ = hdr->blockEntriesCount;
		// Sector size is part of the adopted layout: existing sectors were
		// compressed at this size and their offset tables assume it.
		blockSizeFactor_ = hdr->blockSizeFactor;
		if (hdr->fileSize != size_) {
			// [of] Adopt the archive's logical size; the physical file is
			// allowed to be larger (see IsValidMpqHeader). New blocks append
			// from the logical end, overwriting the stale tail.
			size_ = hdr->fileSize;
		}
	}
	return true;
}

MpqBlockEntry *MpqWriter::NewBlock(uint32_t *blockIndex)
{
	MpqBlockEntry *blockEntry = blockTable_.get();

	for (unsigned i = 0; i < blockEntriesCount_; ++i, ++blockEntry) {
		if (!IsUnallocatedBlock(blockEntry))
			continue;

		if (blockIndex != nullptr)
			*blockIndex = i;

		return blockEntry;
	}

	app_fatal("Out of free block entries");
}

void MpqWriter::AllocBlock(uint32_t blockOffset, uint32_t blockSize)
{
	MpqBlockEntry *block;
	bool expand;
	do {
		block = blockTable_.get();
		expand = false;
		for (unsigned i = blockEntriesCount_; i-- != 0; ++block) {
			// Expand to adjacent blocks.
			if (!IsAllocatedUnusedBlock(block))
				continue;
			if (block->offset + block->packedSize == blockOffset) {
				blockOffset = block->offset;
				blockSize += block->packedSize;
				memset(block, 0, sizeof(MpqBlockEntry));
				expand = true;
				break;
			}
			if (blockOffset + blockSize == block->offset) {
				blockSize += block->packedSize;
				memset(block, 0, sizeof(MpqBlockEntry));
				expand = true;
				break;
			}
		}
	} while (expand);
	if (blockOffset + blockSize > size_) {
		// Expanded beyond EOF, this should never happen.
		app_fatal("MPQ free list error");
	}
	if (blockOffset + blockSize == size_) {
		size_ = blockOffset;
	} else {
		block = NewBlock();
		block->offset = blockOffset;
		block->packedSize = blockSize;
		block->unpackedSize = 0;
		block->flags = 0;
	}
}

uint32_t MpqWriter::FindFreeBlock(uint32_t size)
{
	uint32_t result;

	MpqBlockEntry *block = blockTable_.get();
	for (unsigned i = 0; i < blockEntriesCount_; ++i, ++block) {
		// Find a block entry to use space from.
		if (!IsAllocatedUnusedBlock(block) || block->packedSize < size)
			continue;

		result = block->offset;
		block->offset += size;
		block->packedSize -= size;

		// Clear the block entry if we used its entire capacity.
		if (block->packedSize == 0)
			memset(block, 0, sizeof(*block));

		return result;
	}

	result = size_;
	size_ += size;
	return result;
}

uint32_t MpqWriter::GetHashIndex(uint32_t index, uint32_t hashA, uint32_t hashB) const // NOLINT(bugprone-easily-swappable-parameters)
{
	uint32_t i = hashEntriesCount_;
	for (unsigned idx = index & HashIndexMask(); hashTable_[idx].block != MpqHashEntry::NullBlock; idx = (idx + 1) & HashIndexMask()) {
		if (i-- == 0)
			break;
		if (hashTable_[idx].hashA != hashA)
			continue;
		if (hashTable_[idx].hashB != hashB)
			continue;
		if (hashTable_[idx].block == MpqHashEntry::DeletedBlock)
			continue;

		return idx;
	}

	return HashEntryNotFound;
}

bool MpqWriter::WriteHeaderAndTables()
{
	return WriteHeader() && WriteBlockTable() && WriteHashTable();
}

MpqBlockEntry *MpqWriter::AddFile(const char *filename, MpqBlockEntry *block, uint32_t blockIndex)
{
	uint32_t h1 = Hash(filename, 0);
	uint32_t h2 = Hash(filename, 1);
	uint32_t h3 = Hash(filename, 2);
	if (GetHashIndex(h1, h2, h3) != HashEntryNotFound)
		app_fatal(StrCat("Hash collision between \"", filename, "\" and existing file\n"));
	unsigned int hIdx = h1 & HashIndexMask();

	bool hasSpace = false;
	for (unsigned i = 0; i < hashEntriesCount_; ++i) {
		if (hashTable_[hIdx].block == MpqHashEntry::NullBlock || hashTable_[hIdx].block == MpqHashEntry::DeletedBlock) {
			hasSpace = true;
			break;
		}
		hIdx = (hIdx + 1) & HashIndexMask();
	}
	if (!hasSpace)
		app_fatal("Out of hash space");

	if (block == nullptr)
		block = NewBlock(&blockIndex);

	MpqHashEntry &entry = hashTable_[hIdx];
	entry.hashA = h2;
	entry.hashB = h3;
	entry.locale = 0;
	entry.platform = 0;
	entry.block = blockIndex;

	return block;
}

bool MpqWriter::WriteFileContents(const char *filename, const byte *fileData, size_t fileSize, MpqBlockEntry *block)
{
	const char *tmp;
	while ((tmp = strchr(filename, ':')) != nullptr)
		filename = tmp + 1;
	while ((tmp = strchr(filename, '\\')) != nullptr)
		filename = tmp + 1;
	Hash(filename, 3);

	const uint32_t sectorSize = SectorSize();
	const uint32_t numSectors = (fileSize + (sectorSize - 1)) / sectorSize;
	const uint32_t offsetTableByteSize = sizeof(uint32_t) * (numSectors + 1);

#ifdef OPENFPGAOS
	// [of] Compress-first strategy for the fixed-capacity CRAM save slot.
	//
	// Upstream (below, #else) reserves `fileSize + tableSize` -- the
	// UNCOMPRESSED size -- via FindFreeBlock, streams compressed sectors
	// with a seek-past-EOF backpatch, and gives the unused tail back
	// afterwards. In a 256 KB slot that reservation is fatal: a ~200 KB
	// uncompressed "game" file (~45 KB compressed) demands 200 KB of
	// contiguous free space, so fragmented archives spuriously fail saves
	// that would fit several times over (host soak test reproduced this by
	// save #7). An earlier guard variant also set `valid_=false` on
	// overflow, which no-op'ed the caller's RemoveHashEntry unwind and made
	// ~MpqWriter skip the table flush -- in-memory/on-disk table divergence.
	//
	// Here we compress into a scratch buffer first (PkwareCompress never
	// expands -- it falls back to the raw bytes), allocate EXACTLY the
	// compressed size, bound-check it against the slot window once, and
	// write table+sectors in a single sequential pass (no seek-past-EOF,
	// no backpatch -- the pattern the slot FS is actually validated for).
	// On overflow: return false with `valid_` intact; WriteFile() hands the
	// carved space back and clears the never-published block entry -- the
	// member's previous copy, if any, stays live -- and close still writes
	// consistent tables.
	{
		std::unique_ptr<byte[]> packed { new byte[offsetTableByteSize + fileSize] };
		uint32_t *offsetTable = reinterpret_cast<uint32_t *>(packed.get());
		uint32_t destSize = offsetTableByteSize;
		// [of] Heap, not stack: a 64 KB sector buffer would blow the device stack.
		std::unique_ptr<byte[]> mpqBuf { new byte[sectorSize] };
		size_t curSector = 0;
		size_t remaining = fileSize;
		while (true) {
			uint32_t len = std::min<uint32_t>(remaining, sectorSize);
			memcpy(mpqBuf.get(), fileData, len);
			fileData += len;
			len = PkwareCompress(mpqBuf.get(), len);
			memcpy(packed.get() + destSize, mpqBuf.get(), len);
			offsetTable[curSector++] = SDL_SwapLE32(destSize);
			destSize += len;
			if (remaining <= sectorSize)
				break;
			remaining -= sectorSize;
		}
		offsetTable[numSectors] = SDL_SwapLE32(destSize);

		block->offset = FindFreeBlock(destSize);
		block->packedSize = destSize;
		block->unpackedSize = fileSize;
		block->flags = MpqBlockEntry::FlagExists | MpqBlockEntry::CompressPkZip;

		if (block->offset + destSize > OF_NV_SLOT_CAPACITY) {
			LogError("MpqWriter: '{}' in {}: {} compressed bytes at offset {} exceed the {} KB slot; save aborted",
			    filename, name_, static_cast<unsigned>(destSize), static_cast<unsigned>(block->offset),
			    OF_NV_SLOT_CAPACITY / 1024);
			return false;
		}
		if (!stream_.Seekp(block->offset, SEEK_SET))
			return false;
		if (!stream_.Write(reinterpret_cast<const char *>(packed.get()), destSize))
			return false;
		return true;
	}
#else
	block->offset = FindFreeBlock(fileSize + offsetTableByteSize);
	// `packedSize` is reduced at the end of the function if it turns out to be smaller.
	block->packedSize = fileSize + offsetTableByteSize;
	block->unpackedSize = fileSize;
	block->flags = MpqBlockEntry::FlagExists | MpqBlockEntry::CompressPkZip;

	// We populate the table of sector offsets while we write the data.
	// We can't pre-populate it because we don't know the compressed sector sizes yet.
	// First offset is the start of the first sector, last offset is the end of the last sector.
	std::unique_ptr<uint32_t[]> offsetTable { new uint32_t[numSectors + 1] };

#ifdef CAN_SEEKP_BEYOND_EOF
	if (!stream_.Seekp(block->offset + offsetTableByteSize, SEEK_SET))
		return false;
#else
	// Ensure we do not Seekp beyond EOF by filling the missing space.
	long stream_end;
	if (!stream_.Seekp(0, SEEK_END) || !stream_.Tellp(&stream_end))
		return false;
	const std::uintmax_t cur_size = stream_end - streamBegin_;
	if (cur_size < block->offset + offsetTableByteSize) {
		if (cur_size < block->offset) {
			std::unique_ptr<char[]> filler { new char[block->offset - cur_size] };
			if (!stream_.Write(filler.get(), block->offset - cur_size))
				return false;
		}
		if (!stream_.Write(reinterpret_cast<const char *>(offsetTable.get()), offsetTableByteSize))
			return false;
	} else {
		if (!stream_.Seekp(block->offset + offsetTableByteSize, SEEK_SET))
			return false;
	}
#endif

	uint32_t destSize = offsetTableByteSize;
	std::unique_ptr<byte[]> mpqBuf { new byte[sectorSize] };
	size_t curSector = 0;
	while (true) {
		uint32_t len = std::min<uint32_t>(fileSize, sectorSize);
		memcpy(mpqBuf.get(), fileData, len);
		fileData += len;
		len = PkwareCompress(mpqBuf.get(), len);
		if (!stream_.Write(reinterpret_cast<const char *>(&mpqBuf[0]), len))
			return false;
		offsetTable[curSector++] = SDL_SwapLE32(destSize);
		destSize += len; // compressed length
		if (fileSize <= sectorSize)
			break;

		fileSize -= sectorSize;
	}

	offsetTable[numSectors] = SDL_SwapLE32(destSize);
	if (!stream_.Seekp(block->offset, SEEK_SET))
		return false;
	if (!stream_.Write(reinterpret_cast<const char *>(offsetTable.get()), offsetTableByteSize))
		return false;
	if (!stream_.Seekp(destSize - offsetTableByteSize, SEEK_CUR))
		return false;

	if (destSize < block->packedSize) {
		const uint32_t remainingBlockSize = block->packedSize - destSize;
		if (remainingBlockSize >= MinBlockSize) {
			// Allocate another block if we didn't use all of this one.
			block->packedSize = destSize;
			AllocBlock(block->packedSize + block->offset, remainingBlockSize);
		}
	}
	return true;
#endif
}

bool MpqWriter::WriteHeader()
{
	MpqFileHeader fhdr;

	memset(&fhdr, 0, sizeof(fhdr));
	fhdr.signature = MpqFileHeader::DiabloSignature;
	fhdr.headerSize = MpqFileHeader::DiabloSize;
	fhdr.fileSize = static_cast<uint32_t>(size_);
	fhdr.version = 0;
	fhdr.blockSizeFactor = blockSizeFactor_;
	fhdr.hashEntriesOffset = HashEntriesOffset();
	fhdr.blockEntriesOffset = MpqBlockEntryOffset;
	fhdr.hashEntriesCount = hashEntriesCount_;
	fhdr.blockEntriesCount = blockEntriesCount_;
	ByteSwapHdr(&fhdr);

	return stream_.Write(reinterpret_cast<const char *>(&fhdr), sizeof(fhdr));
}

bool MpqWriter::WriteBlockTable()
{
	Encrypt(reinterpret_cast<uint32_t *>(blockTable_.get()), BlockTableBytes(), Hash("(block table)", 3));
	const bool success = stream_.Write(reinterpret_cast<const char *>(blockTable_.get()), BlockTableBytes());
	Decrypt(reinterpret_cast<uint32_t *>(blockTable_.get()), BlockTableBytes(), Hash("(block table)", 3));
	return success;
}

bool MpqWriter::WriteHashTable()
{
	Encrypt(reinterpret_cast<uint32_t *>(hashTable_.get()), HashTableBytes(), Hash("(hash table)", 3));
	const bool success = stream_.Write(reinterpret_cast<const char *>(hashTable_.get()), HashTableBytes());
	Decrypt(reinterpret_cast<uint32_t *>(hashTable_.get()), HashTableBytes(), Hash("(hash table)", 3));
	return success;
}

void MpqWriter::RemoveHashEntry(const char *filename)
{
	if (!valid_) return; // [of] ctor failed: in-memory tables not allocated
	uint32_t hIdx = FetchHandle(filename);
	if (hIdx == HashEntryNotFound) {
		return;
	}

	MpqHashEntry *hashEntry = &hashTable_[hIdx];
	MpqBlockEntry *block = &blockTable_[hashEntry->block];
	hashEntry->block = MpqHashEntry::DeletedBlock;
	const uint32_t blockOffset = block->offset;
	const uint32_t blockSize = block->packedSize;
	memset(block, 0, sizeof(*block));
	AllocBlock(blockOffset, blockSize);
}

void MpqWriter::RemoveHashEntries(bool (*fnGetName)(uint8_t, char *))
{
	if (!valid_) return; // [of] see RemoveHashEntry
	char pszFileName[MaxMpqPathSize];

	for (uint8_t i = 0; fnGetName(i, pszFileName); i++) {
		RemoveHashEntry(pszFileName);
	}
}

bool MpqWriter::WriteFile(const char *filename, const byte *data, size_t size)
{
	if (!valid_) {
		write_failed_ = true; // [of] ctor failed: tables/stream not ready
		return false;
	}
	// [of] Write the replacement BEFORE retiring the copy already in the
	// archive. This used to RemoveHashEntry() first and only then attempt the
	// write, so a write that failed for ANY reason -- most commonly the fixed
	// slot running out of room -- left the archive with neither copy. Because
	// SaveGameData writes the large "game" member first and the small "hero"
	// last, a full slot dropped "game" and then still fitted "hero": the save
	// reloaded as a valid character with no game state, and the player was
	// silently sent back to difficulty selection as if starting anew, losing
	// their progress. Ordering it this way turns that into a recoverable
	// "Save failed!" with the previous save still intact.
	//
	// The cost is that a replacement transiently needs room for both copies,
	// so a nearly-full archive can now fail a write that previously "worked"
	// by destroying the old copy first. Failing while the good save survives
	// is the strictly better outcome, and the smaller tables written by
	// New*EntriesCount buy back far more room than this costs.
	const uint32_t oldHashIdx = FetchHandle(filename);
	const bool replacing = oldHashIdx != HashEntryNotFound;
	const uint32_t oldBlockIdx = replacing ? hashTable_[oldHashIdx].block : 0;

	uint32_t newBlockIdx = 0;
	MpqBlockEntry *newBlock = NewBlock(&newBlockIdx);
	if (!WriteFileContents(filename, data, size, newBlock)) {
		// Hand back whatever FindFreeBlock carved out before the failure; the
		// entry itself never became reachable, so nothing else must be undone.
		if (newBlock->packedSize != 0)
			AllocBlock(newBlock->offset, newBlock->packedSize);
		std::memset(newBlock, 0, sizeof(*newBlock));
		write_failed_ = true; // [of] surfaced via HadWriteFailure()
		return false;
	}

	if (replacing) {
		// Repoint the existing hash entry, then release the superseded block.
		// AddFile() cannot be used here: it app_fatals on a name that is still
		// present, which is precisely the state we are in.
		hashTable_[oldHashIdx].block = newBlockIdx;
		MpqBlockEntry *oldBlock = &blockTable_[oldBlockIdx];
		const uint32_t oldOffset = oldBlock->offset;
		const uint32_t oldSize = oldBlock->packedSize;
		std::memset(oldBlock, 0, sizeof(*oldBlock));
		AllocBlock(oldOffset, oldSize);
	} else {
		AddFile(filename, newBlock, newBlockIdx);
	}
	return true;
}

void MpqWriter::RenameFile(const char *name, const char *newName) // NOLINT(bugprone-easily-swappable-parameters)
{
	if (!valid_) return; // [of] ctor failed
	uint32_t index = FetchHandle(name);
	if (index == HashEntryNotFound) {
		return;
	}

	MpqHashEntry *hashEntry = &hashTable_[index];
	uint32_t block = hashEntry->block;
	MpqBlockEntry *blockEntry = &blockTable_[block];
	hashEntry->block = MpqHashEntry::DeletedBlock;
	AddFile(newName, blockEntry, block);
}

bool MpqWriter::HasFile(const char *name) const
{
	if (!valid_) return false; // [of] ctor failed
	return FetchHandle(name) != HashEntryNotFound;
}

} // namespace devilution
