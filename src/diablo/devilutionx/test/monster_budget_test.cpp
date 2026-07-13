/**
 * @file monster_budget_test.cpp
 *
 * [of] Measures the per-level monster graphics budget.
 *
 * Added for openfpgaOS issue #4 (crash entering Diablo's lair on the Analogue
 * Pocket). Every level except 16 picks its monster roster through the loop at
 * the end of GetLevelMTypes(), which is hard-capped at `monstimgtot < 4000`
 * and additionally refuses any type whose `image` cost would cross that line.
 * Level 16 does not go through that loop at all: it adds Golem, Advocate,
 * Blood Knight and Diablo unconditionally and returns early, so its graphics
 * request is whatever those four happen to cost -- with no ceiling.
 *
 * On a desktop that is invisible. On a 52 MB heap that has been fragmented by
 * a full playthrough it is the single largest allocation burst in the game,
 * which is why the lair is the one transition that fails every time rather
 * than occasionally.
 *
 * These tests run headless, so InitMonsterGFX() skips the actual file loads
 * (see its `if (!HeadlessMode)` guard) and no game assets are required. The
 * cost is re-derived here from the resulting roster rather than read out of
 * monster.cpp's `monstimgtot`, which sits in an anonymous namespace; the sum
 * over LevelMonsterTypes is the same arithmetic AddMonsterType() performs.
 */
#include <gtest/gtest.h>

#include "diablo.h"
#include "engine/random.hpp"
#include "levels/gendung.h"
#include "monstdat.h"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "quests.h"

using namespace devilution;

namespace {

/** The ceiling GetLevelMTypes() enforces on every level that uses its loop. */
constexpr int GraphicsBudget = 4000;

/** Run the real monster-type selection for `level` and report what it cost. */
int BudgetForLevel(int level)
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

	InitLevelMonsters();
	SetRndSeed(0);
	GetLevelMTypes();

	int cost = 0;
	for (size_t i = 0; i < LevelMonsterTypeCount; i++)
		cost += MonstersData[LevelMonsterTypes[i].type].image;
	return cost;
}

TEST(MonsterBudget, EveryOrdinaryHellLevelRespectsTheBudget)
{
	// 13, 14 and 15 share level 16's tileset and are the closest comparison:
	// same DTYPE_HELL load path, but rostered through the capped loop.
	for (int level : { 13, 14, 15 }) {
		const int cost = BudgetForLevel(level);
		EXPECT_LE(cost, GraphicsBudget)
		    << "level " << level << " exceeded the graphics budget";
	}
}

TEST(MonsterBudget, LairExceedsTheBudgetEveryOtherLevelObeys)
{
	const int lair = BudgetForLevel(16);

	// Golem 386 + Advocate 2000 + Blood Knight 2120 + Diablo 2000.
	EXPECT_EQ(lair, 6506) << "level 16 roster changed; update the analysis in "
	                         "the file comment";
	EXPECT_GT(lair, GraphicsBudget)
	    << "level 16 no longer overruns the budget -- if this is intentional, "
	       "this test and the issue #4 analysis are stale";
}

// Not an assertion -- prints the table that motivated the change so a failure
// elsewhere has the numbers next to it.
TEST(MonsterBudget, PrintPerLevelTable)
{
	std::printf("  level | monster types | graphics cost | budget %d\n",
	    GraphicsBudget);
	for (int level = 1; level <= 16; level++) {
		const int cost = BudgetForLevel(level);
		std::printf("  %5d | %13zu | %13d | %s\n", level, LevelMonsterTypeCount,
		    cost, cost > GraphicsBudget ? "OVER" : "ok");
	}
}

} // namespace
