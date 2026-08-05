/**
 * @file pfile.h
 *
 * Interface of the save game encoding functionality.
 */
#pragma once

#include <cstdint>

#include "DiabloUI/diabloui.h"
#include "player.h"

#ifdef UNPACKED_SAVES
#include "utils/file_util.h"
#else
#include "mpq/mpq_reader.hpp"
#include "mpq/mpq_writer.hpp"
#endif

namespace devilution {

#define MAX_CHARACTERS 99

extern bool gbValidSaveFile;

#ifdef UNPACKED_SAVES
struct SaveReader {
	explicit SaveReader(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	const std::string &dir() const
	{
		return dir_;
	}

	std::unique_ptr<byte[]> ReadFile(const char *filename, std::size_t &fileSize, int32_t &error);

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

private:
	std::string dir_;
};

struct SaveWriter {
	explicit SaveWriter(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	bool WriteFile(const char *filename, const byte *data, size_t size);

	// [of] Same contract as MpqWriter::HadWriteFailure: true once any
	// WriteFile since construction has failed. pfile_write_hero and
	// sfile_write_stash consult it to surface "Save failed!" and to avoid
	// clearing dirty flags; without it this configuration does not compile.
	bool HadWriteFailure() const
	{
		return write_failed_;
	}

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

	void RenameFile(const char *from, const char *to)
	{
		::devilution::RenameFile((dir_ + from).c_str(), (dir_ + to).c_str());
	}

	void RemoveHashEntry(const char *path)
	{
		RemoveFile((dir_ + path).c_str());
	}

	void RemoveHashEntries(bool (*fnGetName)(uint8_t, char *));

private:
	std::string dir_;
	bool write_failed_ = false;
};

#else
using SaveReader = MpqArchive;
using SaveWriter = MpqWriter;
#endif

/**
 * @brief Comparsion result of pfile_compare_hero_demo
 */
struct HeroCompareResult {
	enum Status : uint8_t {
		ReferenceNotFound,
		Same,
		Difference,
	};
	Status status;
	std::string message;
};

std::optional<SaveReader> OpenSaveArchive(uint32_t saveNum);
std::optional<SaveReader> OpenStashArchive();
const char *pfile_get_password();
std::unique_ptr<byte[]> ReadArchive(SaveReader &archive, const char *pszName, size_t *pdwLen = nullptr);
void pfile_write_hero(bool writeGameData = false);

#ifndef DISABLE_DEMOMODE
/**
 * @brief Save a reference game-state (save game) for the demo recording
 * @param demo that is recorded
 */
void pfile_write_hero_demo(int demo);
/**
 * @brief Compares the actual game-state (savegame) with a reference game-state (save game from demo recording)
 * @param demo for the comparsion
 * @param logDetails in case of a difference log details
 * @return The comparsion result.
 */
HeroCompareResult pfile_compare_hero_demo(int demo, bool logDetails);
#endif

void sfile_write_stash();

/**
 * @brief [of] Rewrites a save written by an older build into the compact
 * archive layout: 256/512-entry hash/block tables instead of 2048/2048, and
 * 64 KB sectors instead of 4 KB. On the fixed 256 KB nonvolatile slot that
 * takes a finished 16-level Hellfire save from 90% of the slot to 61%.
 *
 * A no-op unless the slot exists and actually uses the legacy layout (or the
 * build uses unpacked directory saves, which have nothing to migrate). Every
 * member is staged in memory first and the member count is cross-checked
 * against the archive's own, so an unrecognized member refuses migration
 * rather than being dropped; the rewrite is then verified byte-for-byte and
 * retried once. Failures before the rewrite leave the archive untouched.
 * Call only where the heap is quiet -- it holds the whole save decompressed
 * while it works.
 *
 * @return true if the save was migrated.
 */
bool pfile_migrate_save_layout(uint32_t saveNum);

bool pfile_ui_set_hero_infos(bool (*uiAddHeroInfo)(_uiheroinfo *));
void pfile_ui_set_class_stats(unsigned int playerClass, _uidefaultstats *classStats);
uint32_t pfile_ui_get_first_unused_save_num();
bool pfile_ui_save_create(_uiheroinfo *heroinfo);
bool pfile_delete_save(_uiheroinfo *heroInfo);
void pfile_read_player_from_save(uint32_t saveNum, Player &player);
void pfile_save_level();
void pfile_convert_levels();
void pfile_remove_temp_files();
std::unique_ptr<byte[]> pfile_read(const char *pszName, size_t *pdwLen);
void pfile_update(bool forceSave);

} // namespace devilution
