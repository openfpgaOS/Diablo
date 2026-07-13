/**
 * @file monster_gfx_bytes_test.cpp
 *
 * [of] Converts the per-level monster graphics budget into real bytes.
 *
 * monster_budget_test.cpp shows level 16 asking for 6506 "image" units against
 * a 4000 cap every other level obeys, but `image` is an abstract budget number
 * from the original game -- it does not say how much RAM the roster actually
 * costs. This test loads the real sprites and measures the heap delta, which is
 * what decides whether the level-16 spike can plausibly exhaust the Pocket's
 * ~52 MB heap (issue #4) or whether we should be looking at fragmentation
 * instead.
 *
 * Needs the retail DIABDAT.mpq, so it is skipped unless DEVILUTIONX_MPQ_DIR
 * points at a directory containing it:
 *
 *   DEVILUTIONX_MPQ_DIR=/path/to/mpqs ./monster_gfx_bytes_test
 */
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include <gtest/gtest.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "diablo.h"
#include "engine/random.hpp"
#include "init.h"
#include "levels/gendung.h"
#include "monstdat.h"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "quests.h"
#include "utils/paths.h"

using namespace devilution;

namespace {

/** Resident bytes for this process.
 *
 * Read from /proc rather than mallinfo2(): the latter reports zeroes on this
 * toolchain, and resident size is in any case the number that matters for a
 * target with no swap and no overcommit -- it counts the pages the sprites
 * actually occupy, including anything glibc served via mmap (sprite sheets sit
 * above the 128 KB mmap threshold, so they never appear in the sbrk arena). */
size_t ResidentBytes()
{
	std::FILE *f = std::fopen("/proc/self/statm", "r");
	if (f == nullptr)
		return 0;
	unsigned long total = 0;
	unsigned long resident = 0;
	const int n = std::fscanf(f, "%lu %lu", &total, &resident);
	std::fclose(f);
	if (n != 2)
		return 0;
	return static_cast<size_t>(resident) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

struct LevelCost {
	size_t types;
	int budgetUnits;
	size_t bytes;
};

LevelCost MeasureLevel(int level)
{
	Players.resize(1);
	MyPlayer = &Players[0];
	MyPlayer->pOriginalCathedral = true;
	MyPlayer->_pLevel = 30;

	sgGameInitInfo.fullQuests = 1;
	gbIsMultiplayer = false;
	gbIsHellfire = false;
	gbIsSpawn = false;
	InitQuests();

	setlevel = false;
	currlevel = static_cast<uint8_t>(level);
	leveltype = GetLevelType(currlevel);

	FreeMonsters();
	InitLevelMonsters();
	SetRndSeed(0);

	const size_t before = ResidentBytes();
	GetLevelMTypes();
	const size_t after = ResidentBytes();

	LevelCost cost {};
	cost.types = LevelMonsterTypeCount;
	for (size_t i = 0; i < LevelMonsterTypeCount; i++)
		cost.budgetUnits += MonstersData[LevelMonsterTypes[i].type].image;
	cost.bytes = (after > before) ? after - before : 0;

	FreeMonsters();
	return cost;
}

class MonsterGfxBytes : public ::testing::Test {
protected:
	void SetUp() override
	{
		const char *dir = std::getenv("DEVILUTIONX_MPQ_DIR");
		if (dir == nullptr)
			GTEST_SKIP() << "set DEVILUTIONX_MPQ_DIR to a directory holding "
			                "DIABDAT.mpq to run this test";

		std::string base = dir;
		if (base.back() != '/')
			base += '/';
		paths::SetBasePath(base);
		paths::SetAssetsPath(base);

		// Sprites only load when not headless -- that guard is exactly what
		// this test needs to defeat (see InitMonsterGFX).
		HeadlessMode = false;
		LoadGameArchives();
	}
};

TEST_F(MonsterGfxBytes, PerLevelSpriteFootprint)
{
	ASSERT_FALSE(HeadlessMode) << "sprites would not be loaded, so the "
	                             "measurement would be meaningless";
	std::printf("  level | types | budget units | sprite bytes |     MB\n");
	size_t worstOrdinary = 0;
	for (int level = 1; level <= 16; level++) {
		const LevelCost cost = MeasureLevel(level);
		std::printf("  %5d | %5zu | %12d | %12zu | %6.2f%s\n", level, cost.types,
		    cost.budgetUnits, cost.bytes, cost.bytes / (1024.0 * 1024.0),
		    level == 16 ? "   <-- lair" : "");
		if (level < 16)
			worstOrdinary = std::max(worstOrdinary, cost.bytes);
	}
	std::printf("  heaviest ordinary level: %.2f MB\n",
	    worstOrdinary / (1024.0 * 1024.0));
}

} // namespace
