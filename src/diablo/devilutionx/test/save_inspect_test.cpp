/**
 * @file save_inspect_test.cpp
 *
 * [of] Read-only dump of a save's character progress.
 *
 * Added while chasing openfpgaOS issue #4 (crash entering Diablo's lair).
 * Reproducing that crash needs a character standing on level 15 with the lair
 * unlocked, and the only way to know whether a given save qualifies -- without
 * booting hardware and squinting at the character-select screen -- is to read
 * it. Prints the character, which dungeon level it is on, and exactly which
 * levels it has visited.
 *
 * Note the lair is level 16 in BOTH Diablo and Hellfire and loads the same
 * monster roster either way, so a Diablo-mode `.sv` reproduces the level-16
 * memory spike just as well as a Hellfire `.hsv`.
 *
 * Point DEVILUTIONX_SAVE_DIR at a directory holding the save. Copy the save
 * there first -- do not aim this at an SD card you care about. The test only
 * reads, but the save path is also where DevilutionX would write.
 *
 *   DEVILUTIONX_SAVE_DIR=/tmp/saves ./save_inspect_test
 */
#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "diablo.h"
#include "init.h"
#include "loadsave.h"
#include "pfile.h"
#include "player.h"
#include "utils/paths.h"

using namespace devilution;

namespace {

TEST(SaveInspect, DumpProgress)
{
	const char *dir = std::getenv("DEVILUTIONX_SAVE_DIR");
	if (dir == nullptr)
		GTEST_SKIP() << "set DEVILUTIONX_SAVE_DIR to a directory holding the "
		                "save file(s) to inspect";

	std::string base = dir;
	if (base.back() != '/')
		base += '/';
	paths::SetPrefPath(base);

	// Unpacking a save touches item/spell data that lives in the MPQs, so the
	// archives have to be open or every slot reads back as an empty default
	// (which looks exactly like a fresh level-1 character -- misleading).
	const char *mpq = std::getenv("DEVILUTIONX_MPQ_DIR");
	ASSERT_NE(mpq, nullptr) << "set DEVILUTIONX_MPQ_DIR too (needs diabdat.mpq "
	                           "+ devilutionx.mpq), or saves read back empty";
	std::string mpqDir = mpq;
	if (mpqDir.back() != '/')
		mpqDir += '/';
	paths::SetBasePath(mpqDir);
	paths::SetAssetsPath(mpqDir);
	LoadCoreArchives();
	LoadGameArchives();

	const char *hellfire = std::getenv("DEVILUTIONX_HELLFIRE");
	gbIsHellfire = hellfire != nullptr && hellfire[0] == '1';
	gbIsMultiplayer = false;
	gbIsSpawn = false;
	giNumberOfLevels = gbIsHellfire ? 25 : 17;

	std::printf("  save dir: %s   mode: %s\n", base.c_str(),
	    gbIsHellfire ? "Hellfire (.hsv)" : "Diablo (.sv)");

	Players.resize(1);
	MyPlayer = &Players[0];

	for (uint32_t slot = 0; slot < 10; slot++) {
		// Probe for the file first: pfile_read_player_from_save() calls
		// app_fatal("Unable to open archive") on a missing slot, which would
		// kill the process before reaching the slots that do exist.
		const std::string path = base + "single_" + std::to_string(slot)
		    + (gbIsHellfire ? ".hsv" : ".sv");
		std::FILE *probe = std::fopen(path.c_str(), "rb");
		if (probe == nullptr)
			continue;
		std::fclose(probe);

		devilution::Player &player = Players[0];
		player = {};
		pfile_read_player_from_save(slot, player);

		if (player._pName[0] == '\0')
			continue;

		std::printf("\n  slot %u: \"%s\"  clvl %d  currently on dungeon level %d\n",
		    slot, player._pName, static_cast<int>(player._pLevel),
		    static_cast<int>(player.plrlevel));

		// _pLvlVisited is deliberately NOT reported: it is not part of the
		// hero record (see PlayerPack in pack.h, which carries only plrlevel
		// and pLevel) but of the game-state portion of the save, so reading
		// it here yields all-false regardless of how far the character got.
		// Judge reachability from the two fields that are genuinely stored.
		std::printf("    -> %s\n",
		    player.plrlevel >= 15
		        ? "USABLE: at/near the lair, load goes straight there"
		        : "NOT usable for the level-16 repro (character is not deep enough)");
	}
}

} // namespace
