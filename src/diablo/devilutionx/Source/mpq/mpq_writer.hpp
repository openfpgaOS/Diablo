/**
 * @file mpq/mpq_writer.hpp
 *
 * Interface of functions for creating and editing MPQ files.
 */
#pragma once

#include <cstdint>

#include "mpq/mpq_common.hpp"
#include "utils/logged_fstream.hpp"
#include "utils/stdcompat/cstddef.hpp"

namespace devilution {
class MpqWriter {
public:
	// [of] `recreate` starts a brand-new archive at `path` using THIS build's
	// table geometry, ignoring any header already there. Needed by the save
	// migration: the normal path deliberately adopts an existing archive's
	// layout, which would keep re-creating the legacy tables it is trying to
	// shed. Truncating the file first is not an option -- on the openfpgaOS
	// slot filesystem truncate() is a no-op, so the stale header would be read
	// straight back. Callers MUST have every member staged in memory first;
	// this discards the on-disk archive.
	explicit MpqWriter(const char *path, bool recreate = false);
	explicit MpqWriter(const std::string &path, bool recreate = false)
	    : MpqWriter(path.c_str(), recreate)
	{
	}
	MpqWriter(MpqWriter &&other) = default;
	MpqWriter &operator=(MpqWriter &&other) = default;
	~MpqWriter();

	// [of] Table geometry for archives this build CREATES. Older builds wrote
	// 2048/2048, which costs 64 KB of mostly-empty tables -- a quarter of the
	// 256 KB nonvolatile save slot -- for an archive that never holds more
	// than a few dozen members. Existing archives keep whatever geometry
	// their own header declares; see the comment on hashEntriesCount_.
	//
	// A full Hellfire save holds at most ~62 members (25 perm + 25 temp level
	// blobs, the set levels, game/hero/heroitems/hotkeys/additionalMissiles),
	// so 256 hash slots keep the open-addressed probe under a quarter full.
	// The BLOCK table gets more room than the hash table because it also holds
	// one entry per free-list fragment, and running it dry is not a soft
	// failure -- NewBlock() app_fatals. The extra 4 KB is cheap insurance
	// against fragmentation on a slot that stays near capacity.
	static constexpr uint32_t NewHashEntriesCount = 256;
	static constexpr uint32_t NewBlockEntriesCount = 512;
	static constexpr uint32_t LegacyTableEntriesCount = 2048;

	// [of] 64 KB sectors (512 << 7) for new archives, up from the 4 KB every
	// build wrote before. Sectors are compressed independently, so the old size
	// reset PkWare's 4 KB implode dictionary on every single sector boundary.
	static constexpr uint16_t NewBlockSizeFactor = 7;
	static constexpr uint16_t LegacyBlockSizeFactor = 3;

	// True if this archive uses the legacy 2048-entry tables and would gain
	// meaningful space by being rewritten. Drives the one-shot save migration.
	bool IsLegacyLayout() const
	{
		return valid_
		    && (hashEntriesCount_ > NewHashEntriesCount
		        || blockEntriesCount_ > NewBlockEntriesCount
		        || blockSizeFactor_ < NewBlockSizeFactor);
	}

	bool HasFile(const char *name) const;

	void RemoveHashEntry(const char *filename);
	void RemoveHashEntries(bool (*fnGetName)(uint8_t, char *));
	bool WriteFile(const char *filename, const byte *data, size_t size);
	void RenameFile(const char *name, const char *newName);

	// [of] True if the archive could not be opened or any WriteFile since
	// construction failed. Callers use this to surface "save failed" to the
	// player and to avoid clearing dirty flags -- historically every failure
	// here was swallowed into a LogError nobody sees on device.
	bool HadWriteFailure() const
	{
		return !valid_ || write_failed_;
	}

private:
	bool IsValidMpqHeader(MpqFileHeader *hdr) const;
	uint32_t GetHashIndex(uint32_t index, uint32_t hashA, uint32_t hashB) const;
	uint32_t FetchHandle(const char *filename) const;

	// [of] Geometry-derived quantities. These used to be file-scope constants
	// baked to 2048; they are per-instance now so an archive keeps the layout
	// its header declares. The hash probe mask in particular was a hard-coded
	// 0x7FF, which silently wraps to the wrong slot on any other table size.
	uint32_t HashIndexMask() const
	{
		return hashEntriesCount_ - 1;
	}
	uint32_t BlockTableBytes() const
	{
		return blockEntriesCount_ * static_cast<uint32_t>(sizeof(MpqBlockEntry));
	}
	uint32_t HashTableBytes() const
	{
		return hashEntriesCount_ * static_cast<uint32_t>(sizeof(MpqHashEntry));
	}
	uint32_t HashEntriesOffset() const
	{
		return static_cast<uint32_t>(sizeof(MpqFileHeader)) + BlockTableBytes();
	}
	uint32_t SectorSize() const
	{
		return 512u << blockSizeFactor_;
	}

	bool ReadMPQHeader(MpqFileHeader *hdr);
	MpqBlockEntry *AddFile(const char *filename, MpqBlockEntry *block, uint32_t blockIndex);
	bool WriteFileContents(const char *filename, const byte *fileData, size_t fileSize, MpqBlockEntry *block);

	// Returns an unused entry in the block entry table.
	MpqBlockEntry *NewBlock(uint32_t *blockIndex = nullptr);

	// Marks space at `blockOffset` of size `blockSize` as free (unused) space.
	void AllocBlock(uint32_t blockOffset, uint32_t blockSize);

	// Returns the file offset that is followed by empty space of at least the given size.
	uint32_t FindFreeBlock(uint32_t size);

	bool WriteHeaderAndTables();
	bool WriteHeader();
	bool WriteBlockTable();
	bool WriteHashTable();
	void InitDefaultMpqHeader(MpqFileHeader *hdr);

	LoggedFStream stream_;
	std::string name_;
	std::uintmax_t size_ {};
	std::unique_ptr<MpqHashEntry[]> hashTable_;
	std::unique_ptr<MpqBlockEntry[]> blockTable_;
	// [of] True only if the ctor successfully opened the file AND built the
	// in-memory tables. When the underlying save slot is unwritable (on
	// openfpgaOS until the nonvolatile bridge-writeback is fixed), the ctor
	// leaves this false and the public methods short-circuit -- DevilutionX
	// keeps running in-memory with no save persistence, instead of app_fatal-
	// exiting back to the launcher.
	bool valid_ = false;
	// [of] Set when any WriteFile fails (capacity abort, stream error).
	bool write_failed_ = false;

	// [of] Table geometry of THIS archive. Adopted from the on-disk header in
	// ReadMPQHeader when it declares a supported layout, so a save written by
	// an older build keeps its 2048-entry tables and stays readable/writable.
	// IsValidMpqHeader used to demand an exact match against compile-time
	// constants and fall through to InitDefaultMpqHeader on mismatch -- i.e.
	// changing the table size in a new build would have silently discarded
	// every existing archive, erasing the hero. Same class of bug as the
	// `fileSize <= size_` scar documented in IsValidMpqHeader.
	uint32_t hashEntriesCount_ = NewHashEntriesCount;
	uint32_t blockEntriesCount_ = NewBlockEntriesCount;
	uint16_t blockSizeFactor_ = NewBlockSizeFactor;

// Amiga cannot Seekp beyond EOF.
// See https://github.com/bebbo/libnix/issues/30
#ifndef __AMIGA__
#define CAN_SEEKP_BEYOND_EOF
#endif

#ifndef CAN_SEEKP_BEYOND_EOF
	long streamBegin_;
#endif
};

} // namespace devilution
