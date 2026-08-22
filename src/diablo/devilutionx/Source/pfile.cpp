/**
 * @file pfile.cpp
 *
 * Implementation of the save game encoding functionality.
 */
#include "pfile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>

#include "codec.h"
#include "engine.h"
#include "engine/load_file.hpp"
#include "init.h"
#include "loadsave.h"
#include "menu.h"
#include "mpq/mpq_common.hpp"
#include "pack.h"
#include "playerdat.hpp"
#include "plrmsg.h"
#include "qol/stash.h"
#include "utils/endian_read.hpp"
#include "utils/file_util.h"
#include "utils/language.h"
#include "utils/paths.h"
#include "utils/stdcompat/abs.hpp"
#include "utils/stdcompat/string_view.hpp"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"
#include "utils/utf8.hpp"

#ifdef UNPACKED_SAVES
#include "utils/file_util.h"
#else
#include "mpq/mpq_reader.hpp"
#endif

namespace devilution {

#define PASSWORD_SPAWN_SINGLE "adslhfb1"
#define PASSWORD_SPAWN_MULTI "lshbkfg1"
#define PASSWORD_SINGLE "xrgyrkj1"
#define PASSWORD_MULTI "szqnlsk1"

bool gbValidSaveFile;

namespace {

/** List of character names for the character selection screen. */
char hero_names[MAX_CHARACTERS][PlayerNameLength];

std::string GetSavePath(uint32_t saveNum, string_view savePrefix = {})
{
	return StrCat(paths::PrefPath(), savePrefix,
	    gbIsSpawn
	        ? (gbIsMultiplayer ? "share_" : "spawn_")
	        : (gbIsMultiplayer ? "multi_" : "single_"),
	    saveNum,
#ifdef UNPACKED_SAVES
	    gbIsHellfire ? "_hsv" DIRECTORY_SEPARATOR_STR : "_sv" DIRECTORY_SEPARATOR_STR
#else
	    gbIsHellfire ? ".hsv" : ".sv"
#endif
	);
}

std::string GetStashSavePath()
{
	return StrCat(paths::PrefPath(),
	    gbIsSpawn ? "stash_spawn" : "stash",
#ifdef UNPACKED_SAVES
	    gbIsHellfire ? "_hsv" DIRECTORY_SEPARATOR_STR : "_sv" DIRECTORY_SEPARATOR_STR
#else
	    gbIsHellfire ? ".hsv" : ".sv"
#endif
	);
}

bool GetSaveNames(uint8_t index, string_view prefix, char *out)
{
	char suf;
	if (index < giNumberOfLevels)
		suf = 'l';
	else if (index < giNumberOfLevels * 2) {
		index -= giNumberOfLevels;
		suf = 's';
	} else {
		return false;
	}

	*fmt::format_to(out, "{}{}{:02d}", prefix, suf, index) = '\0';
	return true;
}

bool GetPermSaveNames(uint8_t dwIndex, char *szPerm)
{
	return GetSaveNames(dwIndex, "perm", szPerm);
}

bool GetTempSaveNames(uint8_t dwIndex, char *szTemp)
{
	return GetSaveNames(dwIndex, "temp", szTemp);
}

void RenameTempToPerm(SaveWriter &saveWriter)
{
	char szTemp[MaxMpqPathSize];
	char szPerm[MaxMpqPathSize];

	uint32_t dwIndex = 0;
	while (GetTempSaveNames(dwIndex, szTemp)) {
		[[maybe_unused]] bool result = GetPermSaveNames(dwIndex, szPerm); // DO NOT PUT DIRECTLY INTO ASSERT!
		assert(result);
		dwIndex++;
		if (saveWriter.HasFile(szTemp)) {
			if (saveWriter.HasFile(szPerm))
				saveWriter.RemoveHashEntry(szPerm);
			saveWriter.RenameFile(szTemp, szPerm);
		}
	}
	assert(!GetPermSaveNames(dwIndex, szPerm));
}

bool ReadHero(SaveReader &archive, PlayerPack *pPack)
{
	size_t read;

	auto buf = ReadArchive(archive, "hero", &read);
	if (buf == nullptr)
		return false;

	bool ret = false;
	if (read == sizeof(*pPack)) {
		memcpy(pPack, buf.get(), sizeof(*pPack));
		ret = true;
	}

	return ret;
}

void EncodeHero(SaveWriter &saveWriter, const PlayerPack *pack)
{
	size_t packedLen = codec_get_encoded_len(sizeof(*pack));
	std::unique_ptr<byte[]> packed { new byte[packedLen] };

	memcpy(packed.get(), pack, sizeof(*pack));
	codec_encode(packed.get(), sizeof(*pack), packedLen, pfile_get_password());
	saveWriter.WriteFile("hero", packed.get(), packedLen);
}

SaveWriter GetSaveWriter(uint32_t saveNum)
{
	return SaveWriter(GetSavePath(saveNum));
}

SaveWriter GetStashWriter()
{
	return SaveWriter(GetStashSavePath());
}

#ifndef DISABLE_DEMOMODE
void CopySaveFile(uint32_t saveNum, std::string targetPath)
{
	const std::string savePath = GetSavePath(saveNum);
	CopyFileOverwrite(savePath.c_str(), targetPath.c_str());
}
#endif

void Game2UiPlayer(const Player &player, _uiheroinfo *heroinfo, bool bHasSaveFile)
{
	CopyUtf8(heroinfo->name, player._pName, sizeof(heroinfo->name));
	heroinfo->level = player._pLevel;
	heroinfo->heroclass = player._pClass;
	heroinfo->strength = player._pStrength;
	heroinfo->magic = player._pMagic;
	heroinfo->dexterity = player._pDexterity;
	heroinfo->vitality = player._pVitality;
	heroinfo->hassaved = bHasSaveFile;
	heroinfo->herorank = player.pDiabloKillLevel;
	heroinfo->spawned = gbIsSpawn;
}

bool ArchiveContainsGame(SaveReader &hsArchive)
{
	if (gbIsMultiplayer)
		return false;

	auto gameData = ReadArchive(hsArchive, "game");
	if (gameData == nullptr)
		return false;

	uint32_t hdr = LoadLE32(gameData.get());

	return IsHeaderValid(hdr);
}

std::optional<SaveReader> CreateSaveReader(std::string &&path)
{
#ifdef UNPACKED_SAVES
	if (!FileExists(path))
		return std::nullopt;
	return SaveReader(std::move(path));
#else
	std::int32_t error;
	return MpqArchive::Open(path.c_str(), error);
#endif
}

#ifndef DISABLE_DEMOMODE
struct CompareInfo {
	std::unique_ptr<byte[]> &data;
	size_t currentPosition;
	size_t size;
	bool isTownLevel;
	bool dataExists;
};

struct CompareCounter {
	int reference;
	int actual;
	int max()
	{
		return std::max(reference, actual);
	}
	void checkIfDataExists(int count, CompareInfo &compareInfoReference, CompareInfo &compareInfoActual)
	{
		if (reference == count)
			compareInfoReference.dataExists = false;
		if (actual == count)
			compareInfoActual.dataExists = false;
	}
};

inline bool string_ends_with(string_view value, string_view suffix)
{
	if (suffix.size() > value.size())
		return false;
	return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

void CreateDetailDiffs(string_view prefix, string_view memoryMapFile, CompareInfo &compareInfoReference, CompareInfo &compareInfoActual, std::unordered_map<std::string, size_t> &foundDiffs)
{
	// Note: Detail diffs are currently only supported in unit tests
	std::string memoryMapFileAssetName = StrCat(paths::BasePath(), "/test/fixtures/memory_map/", memoryMapFile, ".txt");

	SDL_RWops *handle = SDL_RWFromFile(memoryMapFileAssetName.c_str(), "r");
	if (handle == nullptr) {
		app_fatal(StrCat("MemoryMapFile ", memoryMapFile, " is missing"));
		return;
	}

	size_t readBytes = SDL_RWsize(handle);
	std::unique_ptr<byte[]> memoryMapFileData { new byte[readBytes] };
	SDL_RWread(handle, memoryMapFileData.get(), readBytes, 1);
	const string_view buffer(reinterpret_cast<const char *>(memoryMapFileData.get()), readBytes);

	std::unordered_map<std::string, CompareCounter> counter;

	auto getCounter = [&](const std::string &counterAsString) {
		auto it = counter.find(counterAsString);
		if (it != counter.end())
			return it->second;
		int countFromMapFile = std::stoi(counterAsString);
		return CompareCounter { countFromMapFile, countFromMapFile };
	};
	auto addDiff = [&](const std::string &diffKey) {
		auto it = foundDiffs.find(diffKey);
		if (it == foundDiffs.end()) {
			foundDiffs.insert_or_assign(diffKey, 1);
		} else {
			foundDiffs.insert_or_assign(diffKey, it->second + 1);
		}
	};

	auto compareBytes = [&](size_t countBytes) {
		if (compareInfoReference.dataExists && compareInfoReference.currentPosition + countBytes > compareInfoReference.size)
			app_fatal(StrCat("Comparsion failed. Too less bytes in reference to compare. Location: ", prefix));
		if (compareInfoActual.dataExists && compareInfoActual.currentPosition + countBytes > compareInfoActual.size)
			app_fatal(StrCat("Comparsion failed. Too less bytes in actual to compare. Location: ", prefix));
		bool result = true;
		if (compareInfoReference.dataExists && compareInfoActual.dataExists)
			result = memcmp(compareInfoReference.data.get() + compareInfoReference.currentPosition, compareInfoActual.data.get() + compareInfoActual.currentPosition, countBytes) == 0;
		if (compareInfoReference.dataExists)
			compareInfoReference.currentPosition += countBytes;
		if (compareInfoActual.dataExists)
			compareInfoActual.currentPosition += countBytes;
		return result;
	};

	auto read32BitInt = [&](CompareInfo &compareInfo, bool useLE) {
		int32_t value = 0;
		if (!compareInfo.dataExists)
			return value;
		if (compareInfo.currentPosition + sizeof(value) > compareInfo.size)
			app_fatal("read32BitInt failed. Too less bytes to read.");
		memcpy(&value, compareInfo.data.get() + compareInfo.currentPosition, sizeof(value));
		if (useLE)
			value = SDL_SwapLE32(value);
		else
			value = SDL_SwapBE32(value);
		return value;
	};

	for (string_view line : SplitByChar(buffer, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty())
			continue;
		const auto tokens = SplitByChar(line, ' ');
		auto it = tokens.begin();
		const auto end = tokens.end();
		if (it == end)
			continue;

		string_view command = *it;

		bool dataExistsReference = compareInfoReference.dataExists;
		bool dataExistsActual = compareInfoActual.dataExists;

		if (string_ends_with(command, "_HF")) {
			if (!gbIsHellfire)
				continue;
			command.remove_suffix(3);
		}
		if (string_ends_with(command, "_DA")) {
			if (gbIsHellfire)
				continue;
			command.remove_suffix(3);
		}
		if (string_ends_with(command, "_DL")) {
			if (compareInfoReference.isTownLevel && compareInfoActual.isTownLevel)
				continue;
			if (compareInfoReference.isTownLevel)
				compareInfoReference.dataExists = false;
			if (compareInfoActual.isTownLevel)
				compareInfoActual.dataExists = false;
			command.remove_suffix(3);
		}
		if (command == "R" || command == "LT" || command == "LC" || command == "LC_LE") {
			const auto bitsAsString = std::string(*++it);
			const auto comment = std::string(*++it);
			size_t bytes = static_cast<size_t>(std::stoi(bitsAsString) / 8);

			if (command == "LT") {
				int32_t valueReference = read32BitInt(compareInfoReference, false);
				int32_t valueActual = read32BitInt(compareInfoActual, false);
				assert(sizeof(valueReference) == bytes);
				compareInfoReference.isTownLevel = valueReference == 0;
				compareInfoActual.isTownLevel = valueActual == 0;
			}
			if (command == "LC" || command == "LC_LE") {
				int32_t valueReference = read32BitInt(compareInfoReference, command == "LC_LE");
				int32_t valueActual = read32BitInt(compareInfoActual, command == "LC_LE");
				assert(sizeof(valueReference) == bytes);
				counter.insert_or_assign(std::string(comment), CompareCounter { valueReference, valueActual });
			}

			if (!compareBytes(bytes)) {
				std::string diffKey = StrCat(prefix, ".", comment);
				addDiff(diffKey);
			}
		} else if (command == "M") {
			const auto countAsString = std::string(*++it);
			const auto bitsAsString = std::string(*++it);
			string_view comment = *++it;

			CompareCounter count = getCounter(countAsString);
			size_t bytes = static_cast<size_t>(std::stoi(bitsAsString) / 8);
			for (int i = 0; i < count.max(); i++) {
				count.checkIfDataExists(i, compareInfoReference, compareInfoActual);
				if (!compareBytes(bytes)) {
					std::string diffKey = StrCat(prefix, ".", comment);
					addDiff(diffKey);
				}
			}
		} else if (command == "C") {
			const auto countAsString = std::string(*++it);
			auto subMemoryMapFile = std::string(*++it);
			const auto comment = std::string(*++it);

			CompareCounter count = getCounter(countAsString);
			subMemoryMapFile.erase(std::remove(subMemoryMapFile.begin(), subMemoryMapFile.end(), '\r'), subMemoryMapFile.end());
			for (int i = 0; i < count.max(); i++) {
				count.checkIfDataExists(i, compareInfoReference, compareInfoActual);
				std::string subPrefix = StrCat(prefix, ".", comment);
				CreateDetailDiffs(subPrefix, subMemoryMapFile, compareInfoReference, compareInfoActual, foundDiffs);
			}
		}

		compareInfoReference.dataExists = dataExistsReference;
		compareInfoActual.dataExists = dataExistsActual;
	}
}

struct CompareTargets {
	std::string fileName;
	std::string memoryMapFileName;
	bool isTownLevel;
};

HeroCompareResult CompareSaves(const std::string &actualSavePath, const std::string &referenceSavePath, bool logDetails)
{
	std::vector<CompareTargets> possibleFileToCheck;
	possibleFileToCheck.push_back({ "hero", "hero", false });
	possibleFileToCheck.push_back({ "game", "game", false });
	possibleFileToCheck.push_back({ "additionalMissiles", "additionalMissiles", false });
	char szPerm[MaxMpqPathSize];
	for (int i = 0; GetPermSaveNames(i, szPerm); i++) {
		possibleFileToCheck.push_back({ std::string(szPerm), "level", i == 0 });
	}

	SaveReader actualArchive = *CreateSaveReader(std::string(actualSavePath));
	SaveReader referenceArchive = *CreateSaveReader(std::string(referenceSavePath));

	bool compareResult = true;
	std::string message;
	for (const auto &compareTarget : possibleFileToCheck) {
		size_t fileSizeActual = 0;
		auto fileDataActual = ReadArchive(actualArchive, compareTarget.fileName.c_str(), &fileSizeActual);
		size_t fileSizeReference = 0;
		auto fileDataReference = ReadArchive(referenceArchive, compareTarget.fileName.c_str(), &fileSizeReference);
		if (fileDataActual.get() == nullptr && fileDataReference.get() == nullptr) {
			continue;
		}
		if (fileSizeActual == fileSizeReference && memcmp(fileDataReference.get(), fileDataActual.get(), fileSizeActual) == 0)
			continue;
		compareResult = false;
		if (!message.empty())
			message.append("\n");
		if (fileSizeActual != fileSizeReference)
			StrAppend(message, "file \"", compareTarget.fileName, "\" is different size. Expected: ", fileSizeReference, " Actual: ", fileSizeActual);
		else
			StrAppend(message, "file \"", compareTarget.fileName, "\" has different content.");
		if (!logDetails)
			continue;
		std::unordered_map<std::string, size_t> foundDiffs;
		CompareInfo compareInfoReference = { fileDataReference, 0, fileSizeReference, compareTarget.isTownLevel, fileSizeReference != 0 };
		CompareInfo compareInfoActual = { fileDataActual, 0, fileSizeActual, compareTarget.isTownLevel, fileSizeActual != 0 };
		CreateDetailDiffs(compareTarget.fileName, compareTarget.memoryMapFileName, compareInfoReference, compareInfoActual, foundDiffs);
		if (compareInfoReference.currentPosition != fileSizeReference)
			app_fatal(StrCat("Comparsion failed. Uncompared bytes in reference. File: ", compareTarget.fileName));
		if (compareInfoActual.currentPosition != fileSizeActual)
			app_fatal(StrCat("Comparsion failed. Uncompared bytes in actual. File: ", compareTarget.fileName));
		for (auto entry : foundDiffs) {
			StrAppend(message, "\nDiff found in ", entry.first, " count: ", entry.second);
		}
	}
	return { compareResult ? HeroCompareResult::Same : HeroCompareResult::Difference, message };
}
#endif // !DISABLE_DEMOMODE

// [of] Every member name a save archive can hold, enumerated independently of
// the mode that happens to be loaded. GetSaveNames() splits 'l' (dungeon) from
// 's' (set level) at giNumberOfLevels -- 17 in Diablo, 25 in Hellfire -- so
// anything that has to be exhaustive silently misses members whenever that
// global disagrees with how the archive was written. Enumerating the widest
// range instead is a superset for both modes; absent names are simply skipped.
constexpr int MaxSaveLevelNames = 25;
static_assert(MaxSaveLevelNames <= NUMLEVELS,
    "level member names must index Player::_pLvlVisited/_pSLvlVisited directly");

template <typename Fn>
void ForEachSaveMemberName(Fn &&fn)
{
	for (const char *fixed : { "hero", "game", "heroitems", "hotkeys", "additionalMissiles" })
		fn(fixed);
	char name[MaxMpqPathSize];
	for (const char *prefix : { "perm", "temp" }) {
		for (const char suffix : { 'l', 's' }) {
			for (int i = 0; i < MaxSaveLevelNames; i++) {
				*fmt::format_to(name, "{}{}{:02d}", prefix, suffix, i) = '\0';
				fn(static_cast<const char *>(name));
			}
		}
	}
}

// [of] Empty a save slot: drop every member the archive could hold, not just
// the ones the mode currently loaded can name.
//
// Upstream deletes the save FILE, so what is left inside it never comes up.
// Here a save is a fixed 256 KB nonvolatile window that unlink() cannot
// remove, and BOTH spellings of a slot -- single_N.sv and single_N.hsv -- are
// bound to the SAME window (of_platform.c), so one archive is shared by the
// Diablo and the Hellfire runs of that slot. Clearing through GetFileName
// therefore only reached 0..giNumberOfLevels-1: deleting a Hellfire character
// while the in-game selector was on Diablo left perm/temp l/s 17..24 -- its
// Crypt and Nest members -- allocated, and the next character created in that
// slot inherited them as dead weight in a slot a finished save already fills
// to ~61%. A later mode switch made it worse than wasted space: ConvertLevels
// converts whatever member files exist, orphans included.
//
// Multiplayer archives only ever hold hero/heroitems/hotkeys, so sweeping the
// wider list is equivalent there.
void ClearSaveSlot(SaveWriter &saveWriter)
{
	ForEachSaveMemberName([&saveWriter](const char *name) { saveWriter.RemoveHashEntry(name); });
}

void pfile_write_hero(SaveWriter &saveWriter, bool writeGameData)
{
	if (writeGameData) {
#ifdef OPENFPGAOS
		// Before the writes, not after: the freed blocks are then available
		// to this very save, which matters most on the nearly-full slot that
		// makes reclaiming worth doing at all. It also drops any inherited
		// temp member for an unvisited level before RenameTempToPerm below
		// could promote it over this character's own copy.
		ReclaimUnreachableLevels(saveWriter, *MyPlayer);
#endif
		SaveGameData(saveWriter);
		RenameTempToPerm(saveWriter);
	}
	PlayerPack pkplr;
	Player &myPlayer = *MyPlayer;

	PackPlayer(pkplr, myPlayer);
	EncodeHero(saveWriter, &pkplr);
	if (!gbVanilla) {
		SaveHotkeys(saveWriter, myPlayer);
		SaveHeroItems(saveWriter, myPlayer);
	}
}

// [of] The save-layout migration below is MPQ-specific by nature: it exists to
// shed the archive's oversized hash/block tables and 4 KB sectors. Unpacked
// saves are plain files in a directory -- no tables, no sectors, nothing to
// migrate -- and their SaveWriter has no `recreate` constructor.
#ifndef UNPACKED_SAVES

// Reads just the archive header to classify its layout. Deliberately avoids
// opening an MpqWriter: that would rewrite the header and both tables on
// destruction, dirtying a nonvolatile slot every time the character-select
// screen looks at a save it does not need to touch.
bool SaveUsesLegacyLayout(const std::string &path)
{
	std::FILE *f = OpenFile(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	MpqFileHeader hdr;
	const size_t read = std::fread(&hdr, 1, sizeof(hdr), f);
	std::fclose(f);
	if (read != sizeof(hdr))
		return false;
	if (SDL_SwapLE32(hdr.signature) != MpqFileHeader::DiabloSignature)
		return false;
	// Any axis of the legacy layout qualifies: oversized tables OR the old
	// 4 KB sectors. Mirrors MpqWriter::IsLegacyLayout so the two definitions
	// of "needs migrating" cannot drift apart silently.
	return SDL_SwapLE32(hdr.hashEntriesCount) > MpqWriter::NewHashEntriesCount
	    || SDL_SwapLE32(hdr.blockEntriesCount) > MpqWriter::NewBlockEntriesCount
	    || SDL_SwapLE16(hdr.blockSizeFactor) < MpqWriter::NewBlockSizeFactor;
}

// One archive member held in memory across a layout migration.
struct StagedMember {
	std::string name;
	std::unique_ptr<byte[]> data;
	size_t size;
};

// Re-reads the rewritten archive and proves every staged member came back
// byte-for-byte. The migration discards the on-disk archive before writing, so
// this is the only thing that distinguishes "rewritten" from "destroyed".
bool VerifySaveMembers(const std::string &path, const std::vector<StagedMember> &staged)
{
	std::optional<SaveReader> archive = CreateSaveReader(std::string(path));
	if (!archive)
		return false;
	for (const StagedMember &m : staged) {
		if (!archive->HasFile(m.name.c_str()))
			return false;
		int32_t error = 0;
		size_t len = 0;
		std::unique_ptr<byte[]> got = archive->ReadFile(m.name.c_str(), len, error);
		if (got == nullptr || error != 0 || len != m.size)
			return false;
		if (std::memcmp(got.get(), m.data.get(), len) != 0)
			return false;
	}
	return true;
}

#endif // !UNPACKED_SAVES

void RemoveAllInvalidItems(Player &player)
{
	for (int i = 0; i < NUM_INVLOC; i++)
		RemoveInvalidItem(player.InvBody[i]);
	for (int i = 0; i < player._pNumInv; i++)
		RemoveInvalidItem(player.InvList[i]);
	for (int i = 0; i < MaxBeltItems; i++)
		RemoveInvalidItem(player.SpdList[i]);
	RemoveEmptyInventory(player);
}

} // namespace

#ifdef UNPACKED_SAVES
std::unique_ptr<byte[]> SaveReader::ReadFile(const char *filename, std::size_t &fileSize, int32_t &error)
{
	std::unique_ptr<byte[]> result;
	error = 0;
	const std::string path = dir_ + filename;
	uintmax_t size;
	if (!GetFileSize(path.c_str(), &size)) {
		error = 1;
		return nullptr;
	}
	fileSize = size;
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr) {
		error = 1;
		return nullptr;
	}
	result.reset(new byte[size]);
	if (std::fread(result.get(), size, 1, file) != 1) {
		std::fclose(file);
		error = 1;
		return nullptr;
	}
	std::fclose(file);
	return result;
}

bool SaveWriter::WriteFile(const char *filename, const byte *data, size_t size)
{
	const std::string path = dir_ + filename;
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr) {
		write_failed_ = true; // [of] surfaced via HadWriteFailure()
		return false;
	}
	if (std::fwrite(data, size, 1, file) != 1) {
		std::fclose(file);
		write_failed_ = true;
		return false;
	}
	std::fclose(file);
	return true;
}

void SaveWriter::RemoveHashEntries(bool (*fnGetName)(uint8_t, char *))
{
	char pszFileName[MaxMpqPathSize];

	for (uint8_t i = 0; fnGetName(i, pszFileName); i++) {
		RemoveHashEntry(pszFileName);
	}
}
#endif

std::optional<SaveReader> OpenSaveArchive(uint32_t saveNum)
{
	return CreateSaveReader(GetSavePath(saveNum));
}

std::optional<SaveReader> OpenStashArchive()
{
	return CreateSaveReader(GetStashSavePath());
}

std::unique_ptr<byte[]> ReadArchive(SaveReader &archive, const char *pszName, size_t *pdwLen)
{
	int32_t error;
	std::size_t length;

	std::unique_ptr<byte[]> result = archive.ReadFile(pszName, length, error);
	if (error != 0)
		return nullptr;

	std::size_t decodedLength = codec_decode(result.get(), length, pfile_get_password());
	if (decodedLength == 0)
		return nullptr;

	if (pdwLen != nullptr)
		*pdwLen = decodedLength;

	return result;
}

const char *pfile_get_password()
{
	if (gbIsSpawn)
		return gbIsMultiplayer ? PASSWORD_SPAWN_MULTI : PASSWORD_SPAWN_SINGLE;
	return gbIsMultiplayer ? PASSWORD_MULTI : PASSWORD_SINGLE;
}

// [of] Give back the space held by level members this character can never read.
//
// SaveLevel() sets the matching visited flag as it writes a member, and every
// LoadLevel() is gated on that same flag (diablo.cpp:2958 and 3003 for dungeon
// levels, 3053 for set levels), so for anything this character owns "member
// present" implies "flag set". A member whose flag is clear is unreachable in
// either game mode -- entering that level regenerates it -- so dropping it
// cannot lose state that would ever have been read back.
//
// This is the retroactive half of ClearSaveSlot(): a save that already
// inherited a deleted character's leftovers sheds them at its next full save
// instead of carrying them until the slot is recycled. Only worth the work
// where the slot is a fixed size, so pfile_write_hero calls it under
// OPENFPGAOS only; save_lifecycle_test drives it directly.
void ReclaimUnreachableLevels(SaveWriter &saveWriter, const Player &player)
{
	if (gbIsMultiplayer)
		return;
	char name[MaxMpqPathSize];
	int reclaimed = 0;
	for (int i = 0; i < MaxSaveLevelNames; i++) {
		for (const char suffix : { 'l', 's' }) {
			if (suffix == 'l' ? player._pLvlVisited[i] : player._pSLvlVisited[i])
				continue;
			for (const char *prefix : { "perm", "temp" }) {
				*fmt::format_to(name, "{}{}{:02d}", prefix, suffix, i) = '\0';
				if (!saveWriter.HasFile(name))
					continue;
				saveWriter.RemoveHashEntry(name);
				reclaimed++;
			}
		}
	}
	if (reclaimed > 0)
		LogInfo("pfile: reclaimed {} unreachable level member(s) from the save archive", reclaimed);
}

bool pfile_migrate_save_layout(uint32_t saveNum)
{
#ifdef UNPACKED_SAVES
	// Plain files in a directory: no archive tables or sectors to migrate.
	(void)saveNum;
	return false;
#else
	const std::string path = GetSavePath(saveNum);
	if (!FileExists(path.c_str()) || !SaveUsesLegacyLayout(path))
		return false;

	// --- stage every member ------------------------------------------------
	// Held decompressed, which is the expensive part (~3 MB for a finished
	// Diablo save, ~4.5 MB for a completionist Hellfire one). That is why this
	// runs from the character-select screen: no level graphics are resident, so
	// the heap is at its emptiest. Running it during a level load -- where the
	// monster-graphics spike already lives -- would be asking for the very OOM
	// this save-size work exists to avoid.
	std::vector<StagedMember> staged;
	bool readFailed = false;
	uint32_t liveMembers = 0;
	{
		std::optional<SaveReader> archive = CreateSaveReader(std::string(path));
		if (!archive)
			return false;
		int32_t countError = 0;
		liveMembers = archive->GetFileCount(countError);
		if (countError != 0)
			return false;
		ForEachSaveMemberName([&](const char *name) {
			if (readFailed || !archive->HasFile(name))
				return;
			int32_t error = 0;
			size_t len = 0;
			std::unique_ptr<byte[]> data = archive->ReadFile(name, len, error);
			if (data == nullptr || error != 0) {
				readFailed = true;
				return;
			}
			staged.push_back({ std::string(name), std::move(data), len });
		});
	}
	// Never touch the archive unless every member was recovered first. A
	// partial read here means a partial save after the rewrite.
	if (readFailed || staged.empty()) {
		LogError("pfile: refusing to migrate slot {} -- could not read all members", saveNum);
		return false;
	}
	// The archive itself knows how many live members it holds. If by-name
	// staging found fewer, some member is outside ForEachSaveMemberName's
	// list -- rewriting now would silently drop it, so leave the archive in
	// its legacy layout instead (fully playable, just bigger).
	if (staged.size() != liveMembers) {
		LogError("pfile: refusing to migrate slot {} -- staged {} of {} members", saveNum, staged.size(), liveMembers);
		return false;
	}

	// --- rewrite, then prove it round-tripped ------------------------------
	// The staged copy is deliberately kept alive across both attempts: it is
	// the only thing standing between a failed rewrite and a lost character.
	//
	// Atomicity, honestly: `recreate` discards the on-disk archive, so from
	// here until the writer's destructor lands the header there is a torn
	// window. On the Pocket that window is NOT the SD card -- writes go to
	// CRAM and the host only persists CRAM->SD on its own save-writeback
	// (menu exit / sleep), so a power cut mid-migration boots back into the
	// untouched legacy save and simply migrates again. The exposed case is an
	// app crash inside this function followed by a menu exit, which would
	// persist the torn state. There is no second slot to stage into and the
	// slot FS has no rename, so that window cannot be closed -- only kept
	// short, which is why everything is staged and verified around it.
	// "hero" is written first, so if a capacity abort ever cuts the loop
	// short, the destructor still writes consistent tables and the character
	// record itself survives.
	for (int attempt = 0; attempt < 2; attempt++) {
		{
			SaveWriter writer(path, /*recreate=*/true);
			for (const StagedMember &m : staged) {
				if (!writer.WriteFile(m.name.c_str(), m.data.get(), m.size))
					break;
			}
			if (writer.HadWriteFailure()) {
				LogError("pfile: slot {} migration write failed (attempt {})", saveNum, attempt + 1);
				continue;
			}
		}
		if (VerifySaveMembers(path, staged)) {
			LogVerbose("pfile: migrated slot {} to the compact layout ({} members)", saveNum, staged.size());
			return true;
		}
		LogError("pfile: slot {} failed verification after migration (attempt {})", saveNum, attempt + 1);
	}

	LogError("pfile: slot {} could not be migrated; save may need to be re-saved in game", saveNum);
	return false;
#endif // !UNPACKED_SAVES
}

void pfile_write_hero(bool writeGameData)
{
	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	pfile_write_hero(saveWriter, writeGameData);
	// [of] Historically every save failure (unwritable slot, capacity abort,
	// stream error) was swallowed into a LogError nobody sees on device --
	// "save looked fine, hero gone". Tell the player.
	if (saveWriter.HadWriteFailure()) {
		LogError("pfile_write_hero: save to slot {} failed", gSaveNumber);
		EventPlrMsg(_("Save failed!"), UiFlags::ColorRed);
	}
}

#ifndef DISABLE_DEMOMODE
void pfile_write_hero_demo(int demo)
{
	std::string savePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_reference_"));
	CopySaveFile(gSaveNumber, savePath);
	auto saveWriter = SaveWriter(savePath.c_str());
	pfile_write_hero(saveWriter, true);
}

HeroCompareResult pfile_compare_hero_demo(int demo, bool logDetails)
{
	std::string referenceSavePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_reference_"));

	if (!FileExists(referenceSavePath.c_str()))
		return { HeroCompareResult::ReferenceNotFound, {} };

	std::string actualSavePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_actual_"));
	{
		CopySaveFile(gSaveNumber, actualSavePath);
		SaveWriter saveWriter(actualSavePath.c_str());
		pfile_write_hero(saveWriter, true);
	}

	return CompareSaves(actualSavePath, referenceSavePath, logDetails);
}
#endif

void sfile_write_stash()
{
	if (!Stash.dirty)
		return;

	SaveWriter stashWriter = GetStashWriter();

	SaveStash(stashWriter);

	// [of] Only mark the stash clean if it actually reached the archive;
	// clearing the flag on a failed write silently dropped stash changes
	// (and on openfpgaOS the stash slot didn't even exist until now).
	if (!stashWriter.HadWriteFailure())
		Stash.dirty = false;
}

bool pfile_ui_set_hero_infos(bool (*uiAddHeroInfo)(_uiheroinfo *))
{
	memset(hero_names, 0, sizeof(hero_names));

	for (uint32_t i = 0; i < MAX_CHARACTERS; i++) {
		// [of] Character select is the one place with the whole heap free, so
		// it is where saves written by older builds get rewritten into the
		// compact layout. Returns immediately for slots that are empty or
		// already compact, so this costs nothing after the first visit. Any
		// failure BEFORE the rewrite starts (unreadable or unrecognized
		// member, count mismatch) leaves the save untouched; the rewrite
		// itself is verified and retried, and its narrow torn window is
		// documented at the function.
		pfile_migrate_save_layout(i);

		std::optional<SaveReader> archive = OpenSaveArchive(i);
		if (archive) {
			PlayerPack pkplr;
			if (ReadHero(*archive, &pkplr)) {
				_uiheroinfo uihero;
				uihero.saveNumber = i;
				strcpy(hero_names[i], pkplr.pName);
				bool hasSaveGame = ArchiveContainsGame(*archive);
				if (hasSaveGame)
					pkplr.bIsHellfire = gbIsHellfireSaveGame ? 1 : 0;

				Player &player = Players[0];

				UnPackPlayer(pkplr, player);
				LoadHeroItems(player);
				RemoveAllInvalidItems(player);
				CalcPlrInv(player, false);

				Game2UiPlayer(player, &uihero, hasSaveGame);
				uiAddHeroInfo(&uihero);
			}
		}
	}

	return true;
}

void pfile_ui_set_class_stats(unsigned int playerClass, _uidefaultstats *classStats)
{
	classStats->strength = PlayersData[playerClass].baseStr;
	classStats->magic = PlayersData[playerClass].baseMag;
	classStats->dexterity = PlayersData[playerClass].baseDex;
	classStats->vitality = PlayersData[playerClass].baseVit;
}

uint32_t pfile_ui_get_first_unused_save_num()
{
	uint32_t saveNum;
	for (saveNum = 0; saveNum < MAX_CHARACTERS; saveNum++) {
		if (hero_names[saveNum][0] == '\0')
			break;
	}
	return saveNum;
}

bool pfile_ui_save_create(_uiheroinfo *heroinfo)
{
	PlayerPack pkplr;

	uint32_t saveNum = heroinfo->saveNumber;
	if (saveNum >= MAX_CHARACTERS)
		return false;
	heroinfo->saveNumber = saveNum;

	giNumberOfLevels = gbIsHellfire ? 25 : 17;

	SaveWriter saveWriter = GetSaveWriter(saveNum);
	ClearSaveSlot(saveWriter);
	CopyUtf8(hero_names[saveNum], heroinfo->name, sizeof(hero_names[saveNum]));

	Player &player = Players[0];
	CreatePlayer(player, heroinfo->heroclass);
	CopyUtf8(player._pName, heroinfo->name, PlayerNameLength);
	PackPlayer(pkplr, player);
	EncodeHero(saveWriter, &pkplr);
	Game2UiPlayer(player, heroinfo, false);
	if (!gbVanilla) {
		SaveHotkeys(saveWriter, player);
		SaveHeroItems(saveWriter, player);
	}

	// [of] A create whose writes did not land is not a character. Without this
	// the menu listed the new hero out of memory for the rest of the session
	// and it was gone at the next boot: an unwritable slot (a saveNumber with
	// no nonvolatile window behind it) and an archive with no room left both
	// ended there silently. The caller turns false into "Unable to create
	// character." Roll the name back and empty the slot so a half-written hero
	// record cannot show up in the list either.
	if (saveWriter.HadWriteFailure()) {
		LogError("pfile: could not create a character in slot {}", saveNum);
		hero_names[saveNum][0] = '\0';
		ClearSaveSlot(saveWriter);
		return false;
	}

	return true;
}

bool pfile_delete_save(_uiheroinfo *heroInfo)
{
	uint32_t saveNum = heroInfo->saveNumber;
	if (saveNum < MAX_CHARACTERS) {
		hero_names[saveNum][0] = '\0';

#ifdef OPENFPGAOS
		// [of] On the openfpgaOS slot filesystem a save is a fixed,
		// preallocated CRAM window, not a real file: unlink/remove() is a
		// no-op there (like truncate()), so RemoveFile() below never clears
		// the slot. The "hero" record survives and pfile_ui_set_hero_infos
		// re-reads it on the next character-select rebuild, so the deleted
		// character reappears. Empty the MPQ directory in place instead:
		// drop every hash entry so ReadHero() finds nothing. The cleared
		// tables are written back to the slot when the writer closes and
		// persisted by the launcher's save-on-exit writeback (same path
		// normal saves use). ClearSaveSlot() sweeps both modes' member
		// names, so the space comes back whichever mode the deleted
		// character was played in -- see the comment there.
		{
			SaveWriter saveWriter = GetSaveWriter(saveNum);
			ClearSaveSlot(saveWriter);
		}
#endif

		RemoveFile(GetSavePath(saveNum).c_str());
	}
	return true;
}

void pfile_read_player_from_save(uint32_t saveNum, Player &player)
{
	PlayerPack pkplr;
	{
		std::optional<SaveReader> archive = OpenSaveArchive(saveNum);
		if (!archive)
			app_fatal(_("Unable to open archive"));
		if (!ReadHero(*archive, &pkplr))
			app_fatal(_("Unable to load character"));

		gbValidSaveFile = ArchiveContainsGame(*archive);
		if (gbValidSaveFile)
			pkplr.bIsHellfire = gbIsHellfireSaveGame ? 1 : 0;
	}

	UnPackPlayer(pkplr, player);
	LoadHeroItems(player);
	RemoveAllInvalidItems(player);
	CalcPlrInv(player, false);
}

void pfile_save_level()
{
	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	SaveLevel(saveWriter);
}

void pfile_convert_levels()
{
	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	ConvertLevels(saveWriter);
}

void pfile_remove_temp_files()
{
	if (gbIsMultiplayer)
		return;

	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	saveWriter.RemoveHashEntries(GetTempSaveNames);
}

void pfile_update(bool forceSave)
{
	static Uint32 prevTick;

	if (!gbIsMultiplayer)
		return;

	Uint32 tick = SDL_GetTicks();
	if (!forceSave && tick - prevTick <= 60000)
		return;

	prevTick = tick;
	pfile_write_hero();
	sfile_write_stash();
}

} // namespace devilution
