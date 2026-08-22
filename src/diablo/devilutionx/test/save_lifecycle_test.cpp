/**
 * @file save_lifecycle_test.cpp
 *
 * [of] Proves the save-slot reclaim never touches a real save.
 *
 * ReclaimUnreachableLevels() deletes members out of a live save archive, so
 * "it should be safe" is not good enough. This drives a real character through
 * a real playthrough -- create, descend, save, descend, save, walk back up --
 * using the same call sequences interfac.cpp uses for a level change, so every
 * `perml##` member in the archive was written by the engine itself. Then it
 * runs the reclaim exactly where pfile_write_hero runs it on device and
 * demands that the archive come back BYTE-IDENTICAL: on a save whose members
 * all belong to the character, the sweep must be a no-op.
 *
 * It then checks the cases that make the sweep worth having, and the one that
 * would make it dangerous:
 *   - a slot carrying another character's leftovers loses exactly those;
 *   - a set-level member whose _pSLvlVisited flag is set survives (an l/s or
 *     off-by-one mix-up in the index pairing would eat it);
 *   - switching the in-game Diablo/Hellfire selector -- the mode mismatch that
 *     created the leak in the first place -- still removes nothing owned;
 *   - after all of that the game reloads and a previously visited level still
 *     comes back from the archive rather than being regenerated.
 *
 * Needs the game data (the levels have to actually build):
 *
 *   DEVILUTIONX_MPQ_DIR=/path/to/mpqs ./save_lifecycle_test
 *
 * with lowercase `diabdat.mpq` + `devilutionx.mpq` in it, as make_lair_save_test
 * documents. Skips when unset.
 */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "automap.h"
#include "diablo.h"
#include "init.h"
#include "levels/gendung.h"
#include "loadsave.h"
#include "menu.h"
#include "msg.h"
#include "multi.h"
#include "pfile.h"
#include "player.h"
#include "portal.h"
#include "quests.h"
#include "utils/file_util.h"
#include "utils/paths.h"
#include "utils/stdcompat/cstddef.hpp"

#include "DiabloUI/diabloui.h"

using namespace devilution;

namespace {

constexpr uint32_t Slot = 3;
constexpr int DeepestLevel = 3;

// Every name a save archive can hold, mirroring pfile.cpp's ForEachSaveMemberName.
std::vector<std::string> AllMemberNames()
{
	std::vector<std::string> names = { "hero", "game", "heroitems", "hotkeys", "additionalMissiles" };
	for (const char *prefix : { "perm", "temp" }) {
		for (const char suffix : { 'l', 's' }) {
			for (int i = 0; i < 25; i++) {
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%s%c%02d", prefix, suffix, i);
				names.emplace_back(buf);
			}
		}
	}
	return names;
}

// name -> raw member bytes, for every member the archive actually holds.
std::map<std::string, std::vector<byte>> SnapshotArchive(uint32_t saveNum)
{
	std::map<std::string, std::vector<byte>> out;
	std::optional<SaveReader> archive = OpenSaveArchive(saveNum);
	EXPECT_TRUE(archive.has_value());
	if (!archive)
		return out;
	for (const std::string &name : AllMemberNames()) {
		if (!archive->HasFile(name.c_str()))
			continue;
		int32_t error = 0;
		size_t len = 0;
		std::unique_ptr<byte[]> data = archive->ReadFile(name.c_str(), len, error);
		EXPECT_TRUE(data != nullptr && error == 0) << name;
		if (data == nullptr || error != 0)
			continue;
		out.emplace(name, std::vector<byte>(data.get(), data.get() + len));
	}
	return out;
}

// The device runs the sweep from pfile_write_hero, on the writer it is about to
// save through. Same function, same point in the sequence.
void RunReclaim()
{
	SaveWriter writer = SaveWriter(paths::PrefPath() + "single_" + std::to_string(Slot) + (gbIsHellfire ? ".hsv" : ".sv"));
	ReclaimUnreachableLevels(writer, *MyPlayer);
}

void DescendTo(uint8_t level)
{
	// interfac.cpp WM_DIABNEXTLVL, minus the progress bar.
	pfile_save_level();
	FreeGameMem();
	setlevel = false;
	MyPlayer->plrlevel = level;
	currlevel = level;
	leveltype = GetLevelType(currlevel);
	LoadGameLevel(false, ENTRY_MAIN);
}

void AscendTo(uint8_t level)
{
	// interfac.cpp WM_DIABPREVLVL.
	pfile_save_level();
	FreeGameMem();
	setlevel = false;
	MyPlayer->plrlevel = level;
	currlevel = level;
	leveltype = GetLevelType(currlevel);
	LoadGameLevel(false, ENTRY_PREV);
}

std::string EnvDir(const char *name)
{
	const char *v = std::getenv(name);
	if (v == nullptr)
		return {};
	std::string s = v;
	if (!s.empty() && s.back() != '/')
		s += '/';
	return s;
}

TEST(SaveLifecycle, ReclaimNeverTouchesARealSave)
{
	const std::string mpqDir = EnvDir("DEVILUTIONX_MPQ_DIR");
	if (mpqDir.empty())
		GTEST_SKIP() << "set DEVILUTIONX_MPQ_DIR (needs diabdat.mpq + devilutionx.mpq)";

	const std::string saveDir = paths::BasePath();
	paths::SetBasePath(mpqDir);
	paths::SetAssetsPath(mpqDir);
	paths::SetPrefPath(saveDir);

	gbIsSpawn = false;
	gbIsMultiplayer = false;
	gbVanilla = false;
	leveltype = DTYPE_TOWN;
	LoadCoreArchives();
	LoadGameArchives();
	// A relative DEVILUTIONX_MPQ_DIR resolves against whatever working
	// directory the runner picked (ctest's is not the build root), and without
	// the archives the level builds below walk off the end of empty data
	// instead of failing an assertion. Check the data is really there.
	if (!HaveDiabdat())
		GTEST_SKIP() << "no diabdat.mpq under DEVILUTIONX_MPQ_DIR=" << mpqDir
		             << " (use an absolute path)";

	// LoadGameArchives() sets gbIsHellfire itself when the expansion MPQs are
	// present, so the mode has to be reasserted after it.
	gbIsHellfire = false;
	gbIsHellfireSaveGame = false;
	giNumberOfLevels = 17;

	const std::string savePath = saveDir + "single_" + std::to_string(Slot) + ".sv";
	RemoveFile(savePath.c_str());

	Players.resize(1);
	MyPlayerId = 0;

	// --- create the character (MyPlayer null across the call, see
	// make_lair_save_test for why) --------------------------------------
	MyPlayer = nullptr;
	_uiheroinfo heroinfo = {};
	heroinfo.saveNumber = Slot;
	heroinfo.heroclass = HeroClass::Rogue;
	std::snprintf(heroinfo.name, sizeof(heroinfo.name), "Lifecycle");
	ASSERT_TRUE(pfile_ui_save_create(&heroinfo));

	// --- bring the game up the way StartGame() does ----------------------
	gSaveNumber = Slot;
	gbLoadGame = false;
	gbValidSaveFile = false;
	sgGameInitInfo.nTickRate = 20;
	sgGameInitInfo.fullQuests = 0;
	ASSERT_TRUE(NetInit(/*bSinglePlayer=*/true));
	ASSERT_NE(MyPlayer, nullptr);
	InitLevels();
	InitQuests();
	InitPortals();
	InitDungMsgs(*MyPlayer);
	DeltaSyncJunk();
	giNumberOfLevels = 17;

	devilution::Player &player = *MyPlayer;
	player._pLevel = 20;
	player._pMaxHPBase = player._pHPBase = 1000 << 6;

	// --- town, then descend, saving each level on the way out ------------
	setlevel = false;
	currlevel = 0;
	player.plrlevel = 0;
	leveltype = DTYPE_TOWN;
	LoadGameLevel(/*firstflag=*/true, ENTRY_MAIN);

	// A marker the level file carries: AutomapView is written into every
	// non-town level member and read straight back by LoadLevel, so it proves
	// later that the level came out of the archive instead of being rebuilt.
	constexpr Point MarkerTile { 5, 7 };
	for (uint8_t lvl = 1; lvl <= DeepestLevel; lvl++) {
		DescendTo(lvl);
		ASSERT_NE(leveltype, DTYPE_TOWN) << "level " << int(lvl);
		if (lvl == 1)
			AutomapView[MarkerTile.x][MarkerTile.y] = MAP_EXP_SHRINE;
	}

	gbValidSaveFile = true;
	SaveGame();

	// Every level LEFT behind must be in the archive and flagged visited. The
	// level still being stood on is not one of them: its state rides in the
	// "game" member until SaveLevel writes it on the way out, which is also
	// when the visited flag goes up -- hence `<` and not `<=`.
	{
		std::optional<SaveReader> archive = OpenSaveArchive(Slot);
		ASSERT_TRUE(archive.has_value());
		for (uint8_t lvl = 0; lvl < DeepestLevel; lvl++) {
			char name[32];
			std::snprintf(name, sizeof(name), "perml%02d", lvl);
			EXPECT_TRUE(player._pLvlVisited[lvl]) << "level " << int(lvl) << " should be flagged visited";
			EXPECT_TRUE(archive->HasFile(name)) << name << " missing after the playthrough";
		}
	}

	// ---- 1. on a save the character owns outright, the sweep is a no-op --
	const std::map<std::string, std::vector<byte>> beforeReclaim = SnapshotArchive(Slot);
	ASSERT_FALSE(beforeReclaim.empty());
	RunReclaim();
	EXPECT_EQ(SnapshotArchive(Slot), beforeReclaim)
	    << "the reclaim changed a save built entirely by a real playthrough";

	// ---- 2. a set-level member whose flag is set must survive ------------
	// Set levels index _pSLvlVisited by setlvlnum, dungeon levels index
	// _pLvlVisited by currlevel; swapping the two would delete live quest
	// levels, so pin the pairing explicitly.
	{
		const std::vector<byte> questLevel(2048, static_cast<byte>(0x5A));
		{
			SaveWriter writer(savePath);
			ASSERT_TRUE(writer.WriteFile("perms04", questLevel.data(), questLevel.size()));
		}
		player._pSLvlVisited[4] = true;
		RunReclaim();
		{
			std::optional<SaveReader> archive = OpenSaveArchive(Slot);
			ASSERT_TRUE(archive.has_value());
			EXPECT_TRUE(archive->HasFile("perms04")) << "a visited set level was deleted";
		}

		// ... and goes once the flag says it was never visited.
		player._pSLvlVisited[4] = false;
		RunReclaim();
		{
			std::optional<SaveReader> archive = OpenSaveArchive(Slot);
			ASSERT_TRUE(archive.has_value());
			EXPECT_FALSE(archive->HasFile("perms04"));
		}
	}

	// ---- 3. another character's leftovers, and only those, are dropped ---
	{
		const std::vector<byte> orphan(4096, static_cast<byte>(0xC3));
		{
			SaveWriter writer(savePath);
			for (const char *name : { "perml20", "templ21", "perms09", "temps22" })
				ASSERT_TRUE(writer.WriteFile(name, orphan.data(), orphan.size())) << name;
		}
		const std::map<std::string, std::vector<byte>> owned = beforeReclaim;
		RunReclaim();
		const std::map<std::string, std::vector<byte>> after = SnapshotArchive(Slot);
		for (const char *name : { "perml20", "templ21", "perms09", "temps22" })
			EXPECT_EQ(after.count(name), 0u) << name << " survived the reclaim";
		for (const auto &kv : owned) {
			ASSERT_EQ(after.count(kv.first), 1u) << kv.first << " was deleted from a real save";
			EXPECT_EQ(after.at(kv.first), kv.second) << kv.first << " was modified";
		}
	}

	// ---- 4. the mode selector flipping under the shared archive ----------
	// A Hellfire instance running in Diablo mode (or the reverse) is what made
	// the old clear miss members; the reclaim must still not touch anything the
	// character owns when the mode disagrees with how the save was written.
	{
		// Snapshot while the mode still names this file: on device both
		// spellings are one CRAM window, but on the host `.sv` and `.hsv` are
		// separate files, so the archive has to be addressed by path here.
		const std::map<std::string, std::vector<byte>> before = SnapshotArchive(Slot);
		ASSERT_FALSE(before.empty());
		gbIsHellfire = true;
		giNumberOfLevels = 25;
		{
			SaveWriter writer(savePath);
			ReclaimUnreachableLevels(writer, player);
		}
		gbIsHellfire = false;
		giNumberOfLevels = 17;
		EXPECT_EQ(SnapshotArchive(Slot), before) << "a mode switch made the reclaim eat owned members";
	}

	// ---- 5. the save still loads, and a visited level still comes back ---
	// Walk back up to level 1: LoadGameLevel only calls LoadLevel() when the
	// visited flag is set, so recovering the marker proves both that the flag
	// and the member survived and that the member is the one being read.
	std::memset(AutomapView, 0, sizeof(AutomapView));
	AscendTo(1);
	EXPECT_EQ(AutomapView[MarkerTile.x][MarkerTile.y], MAP_EXP_SHRINE)
	    << "level 1 was regenerated instead of loaded from the save";

	// ---- 6. and the whole thing still loads through the real load path ---
	// Four reclaims and a mode flip later, LoadGame() -- what the menu's "Load
	// Game" runs -- has to bring the character, the progress flags and the
	// level itself back out of this archive.
	SaveGame();
	{
		std::optional<SaveReader> archive = OpenSaveArchive(Slot);
		ASSERT_TRUE(archive.has_value());
		EXPECT_TRUE(archive->HasFile("game")) << "the save lost its game state";
	}

	std::memset(AutomapView, 0, sizeof(AutomapView));
	gbLoadGame = true;
	LoadGame(/*firstflag=*/true);
	EXPECT_STREQ(MyPlayer->_pName, "Lifecycle");
	EXPECT_EQ(MyPlayer->_pClass, HeroClass::Rogue);
	EXPECT_EQ(static_cast<int>(currlevel), 1) << "reloaded on the wrong level";
	for (uint8_t lvl = 0; lvl <= DeepestLevel; lvl++)
		EXPECT_TRUE(MyPlayer->_pLvlVisited[lvl]) << "level " << int(lvl) << " lost its visited flag";
	EXPECT_EQ(AutomapView[MarkerTile.x][MarkerTile.y], MAP_EXP_SHRINE)
	    << "the reloaded level 1 is not the one that was saved";

	NetClose();
}

} // namespace
