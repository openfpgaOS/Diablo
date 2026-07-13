/**
 * @file make_lair_save_test.cpp
 *
 * [of] Generates a save that loads straight into Diablo's lair (level 16).
 *
 * Built for openfpgaOS issue #4. Reproducing that crash otherwise needs a full
 * fifteen-level playthrough on hardware; this writes a save whose stored
 * `currlevel` is 16, so loading it sends the game directly through the
 * level-16 load path -- the one that requests 32 MB of monster graphics
 * against a 4000-unit budget every other level respects.
 *
 * IMPORTANT caveat when interpreting a test with this save: it loads onto a
 * CLEAN heap, whereas the field crash happens after fifteen levels of
 * allocation churn. So it isolates the size of the level-16 spike from the
 * fragmentation that normally accompanies it:
 *   - crashes here  -> the spike alone is fatal; shrink the level-16 load.
 *   - survives here -> fragmentation is a needed co-factor; the fix has to
 *                      target allocator behaviour, not just the peak.
 * Either outcome narrows the fix, which is why it is worth generating.
 *
 * Usage (writes single_<slot>.sv, or .hsv when DEVILUTIONX_HELLFIRE=1):
 *
 *   DEVILUTIONX_MPQ_DIR=/path/to/mpqs \
 *   DEVILUTIONX_SAVE_DIR=/path/to/out \
 *   DEVILUTIONX_HELLFIRE=1 \
 *   DEVILUTIONX_SAVE_SLOT=0 \
 *     ./make_lair_save_test
 *
 * The MPQ dir needs lowercase `diabdat.mpq` plus `devilutionx.mpq`; the
 * Hellfire MPQs as well when generating a Hellfire save.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "diablo.h"
#include "engine/random.hpp"
#include "init.h"
#include "levels/gendung.h"
#include "loadsave.h"
#include "menu.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "pfile.h"
#include "portal.h"
#include "player.h"
#include "quests.h"
#include "utils/paths.h"

#include "DiabloUI/diabloui.h"

using namespace devilution;

namespace {

constexpr int LairLevel = 16;

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

TEST(MakeLairSave, WriteSaveThatLoadsIntoTheLair)
{
	const std::string mpqDir = EnvDir("DEVILUTIONX_MPQ_DIR");
	const std::string saveDir = EnvDir("DEVILUTIONX_SAVE_DIR");
	if (mpqDir.empty() || saveDir.empty())
		GTEST_SKIP() << "set DEVILUTIONX_MPQ_DIR and DEVILUTIONX_SAVE_DIR";

	const char *hf = std::getenv("DEVILUTIONX_HELLFIRE");
	const char *slotEnv = std::getenv("DEVILUTIONX_SAVE_SLOT");
	const uint32_t slot = slotEnv != nullptr
	    ? static_cast<uint32_t>(std::atoi(slotEnv))
	    : 0u;

	paths::SetBasePath(mpqDir);
	paths::SetAssetsPath(mpqDir);
	paths::SetPrefPath(saveDir);

	gbIsHellfire = hf != nullptr && hf[0] == '1';
	gbIsSpawn = false;
	gbIsMultiplayer = false;
	gbVanilla = false;
	leveltype = DTYPE_TOWN;

	LoadCoreArchives();
	LoadGameArchives();

	// LoadGameArchives() flips gbIsHellfire on by itself whenever hellfire.mpq
	// (plus hfmonk/hfmusic/hfvoice) are present, so the requested mode has to
	// be reasserted AFTER it -- setting it beforehand is silently overridden,
	// which quietly produces a .hsv when you asked for a .sv.
	gbIsHellfire = hf != nullptr && hf[0] == '1';
	giNumberOfLevels = gbIsHellfire ? 25 : 17;

	// The lair's tileset lives in the base game, but a Hellfire save must be
	// written with the Hellfire archives open or its level tables differ.
	ASSERT_TRUE(HeadlessMode) << "must run headless; the generator does no rendering";

	Players.resize(1);
	MyPlayer = &Players[0];
	MyPlayerId = 0;

	// --- create the character (writes the hero record into the save) ------
	// MyPlayer stays null across this call, deliberately. CreatePlayer rolls
	// the starting inventory through AddItemToInvGrid, which broadcasts each
	// placement -- but only `if (&player == MyPlayer)`. That is why the real
	// menu can create characters with no game running, and why pointing
	// MyPlayer at the new character here segfaults in SNetSendMessage when no
	// net provider exists yet.
	MyPlayer = nullptr;
	_uiheroinfo heroinfo = {};
	heroinfo.saveNumber = slot;
	heroinfo.heroclass = HeroClass::Warrior;
	std::snprintf(heroinfo.name, sizeof(heroinfo.name), "LairTest");
	ASSERT_TRUE(pfile_ui_save_create(&heroinfo)) << "could not create the hero record";

	// --- bring up the game exactly as StartGame() does --------------------
	// The loopback provider is not optional even in single player: the level
	// load sends messages through it. InitSingle also re-reads the hero from
	// gSaveNumber, which is why the save has to exist first -- a missing one is
	// a hard app_fatal("Unable to open archive").
	gSaveNumber = slot;
	gbLoadGame = false;
	gbValidSaveFile = false;
	sgGameInitInfo.nTickRate = 20;
	sgGameInitInfo.fullQuests = 1;
	ASSERT_TRUE(NetInit(/*bSinglePlayer=*/true)) << "loopback net init failed";
	ASSERT_NE(MyPlayer, nullptr);

	InitLevels();
	InitQuests();
	InitPortals();
	InitDungMsgs(*MyPlayer);
	DeltaSyncJunk();
	giNumberOfLevels = gbIsHellfire ? 25 : 17;

	devilution::Player &player = *MyPlayer;

	// The lair only builds its quest layout when the Diablo quest is live.
	Quests[Q_DIABLO]._qactive = QUEST_ACTIVE;

	// --- make the character survivable and mark the descent --------------
	player._pLevel = 30;
	player._pMaxHPBase = player._pHPBase = 2000 << 6;
	player._pMaxManaBase = player._pManaBase = 500 << 6;
	for (int i = 0; i <= LairLevel; i++)
		player._pLvlVisited[i] = true;

	// --- build level 16 --------------------------------------------------
	setlevel = false;
	currlevel = static_cast<uint8_t>(LairLevel);
	leveltype = GetLevelType(currlevel);
	player.plrlevel = static_cast<uint8_t>(LairLevel);
	player.plrIsOnSetLevel = false;
	ASSERT_EQ(leveltype, DTYPE_HELL) << "level 16 should be a hell level";

	LoadGameLevel(/*firstflag=*/true, ENTRY_MAIN);

	// --- write it out ----------------------------------------------------
	gbValidSaveFile = true;
	SaveGame();
	NetClose();

	const std::string out = saveDir + "single_" + std::to_string(slot)
	    + (gbIsHellfire ? ".hsv" : ".sv");
	std::printf("\n  wrote %s\n", out.c_str());
	std::printf("  mode=%s  currlevel=%d  leveltype=%d  monster types=%zu\n",
	    gbIsHellfire ? "Hellfire" : "Diablo", static_cast<int>(currlevel),
	    static_cast<int>(leveltype), LevelMonsterTypeCount);

	// Prove the file exists and is a real MPQ, so a broken generator cannot
	// masquerade as success.
	std::FILE *f = std::fopen(out.c_str(), "rb");
	ASSERT_NE(f, nullptr) << "save file was not created at " << out;
	char magic[4] = {};
	ASSERT_EQ(std::fread(magic, 1, 4, f), 4u);
	std::fclose(f);
	EXPECT_EQ(std::memcmp(magic, "MPQ\x1A", 4), 0) << "output is not an MPQ archive";
}

} // namespace
