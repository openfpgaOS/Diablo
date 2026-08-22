/**
 * @file save_clear_test.cpp
 *
 * [of] A recycled save slot must come back EMPTY, whichever mode empties it.
 *
 * On openfpgaOS a save is a fixed 256 KB nonvolatile window that unlink()
 * cannot remove, and single_N.sv and single_N.hsv are bound to the SAME
 * window, so ONE archive serves both the Diablo and the Hellfire runs of a
 * slot. Emptying it used to enumerate members through GetFileName, which stops
 * at giNumberOfLevels -- 17 in Diablo, 25 in Hellfire -- so deleting a Hellfire
 * character while the in-game selector was on Diablo left its Crypt and Nest
 * members (perm/temp l/s 17..24) allocated, and the next character created in
 * that slot inherited them as dead weight in a slot a finished save already
 * fills to ~61%.
 *
 * Both halves of the fix are pinned here: the leftovers are gone, and the room
 * they held is genuinely reclaimed -- the recycled slot ends up the size of a
 * fresh one -- rather than merely unlinked.
 */
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "diablo.h"
#include "init.h"
#include "levels/gendung.h"
#include "loadsave.h"
#include "pfile.h"
#include "player.h"
#include "utils/file_util.h"
#include "utils/paths.h"
#include "utils/stdcompat/cstddef.hpp"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

// Level members only a Hellfire character can own. Diablo's giNumberOfLevels
// is 17, so these four are exactly what the old by-mode sweep could not name.
const char *const HellfireOnlyMembers[] = { "perml20", "perms20", "templ20", "temps20" };

// Incompressible filler: the archive compresses every member it stores, and a
// run of identical bytes would shrink to nothing and make the size assertion
// below vacuous.
std::vector<byte> NoisyPayload(size_t size)
{
	std::vector<byte> data(size);
	uint32_t state = 0x1234567;
	for (byte &b : data) {
		state = state * 1103515245 + 12345;
		b = static_cast<byte>(state >> 16);
	}
	return data;
}

uintmax_t SizeOf(const std::string &path)
{
	uintmax_t size = 0;
	EXPECT_TRUE(GetFileSize(path.c_str(), &size)) << path;
	return size;
}

TEST(SaveClear, RecycledSlotDropsOtherModeLeftovers)
{
#ifdef UNPACKED_SAVES
	GTEST_SKIP() << "unpacked saves are a directory of plain files: no archive "
	                "to leak space in";
#else
	paths::SetPrefPath(paths::BasePath());

	// Diablo mode is the half of the shared archive that could not name the
	// Hellfire-only members.
	gbVanilla = true;
	gbIsHellfire = false;
	gbIsHellfireSaveGame = false;
	gbIsMultiplayer = false;
	gbIsSpawn = false;
	leveltype = DTYPE_TOWN;
	giNumberOfLevels = 17;

	Players.resize(1);
	MyPlayerId = 0;
	// MyPlayer stays null across the creates, exactly as it is when the real
	// menu creates a character with no game running: CreatePlayer rolls the
	// starting inventory through AddItemToInvGrid, which broadcasts the item
	// only `if (&player == MyPlayer)` -- and with no net provider up that
	// broadcast segfaults in SNetSendMessage.
	MyPlayer = nullptr;

	const std::string recycled = paths::PrefPath() + "single_5.sv";
	const std::string fresh = paths::PrefPath() + "single_6.sv";
	RemoveFile(recycled.c_str());
	RemoveFile(fresh.c_str());

	// What a deleted Hellfire character leaves sitting in the slot.
	const std::vector<byte> payload = NoisyPayload(4096);
	{
		SaveWriter seed(recycled);
		for (const char *name : HellfireOnlyMembers)
			ASSERT_TRUE(seed.WriteFile(name, payload.data(), payload.size())) << name;
	}
	const uintmax_t seededSize = SizeOf(recycled);
	{
		std::optional<SaveReader> archive = OpenSaveArchive(5);
		ASSERT_TRUE(archive.has_value());
		for (const char *name : HellfireOnlyMembers)
			ASSERT_TRUE(archive->HasFile(name)) << name;
	}

	// Create one character over the leftovers and an identical one in a slot
	// that never had any, so the two archives differ only by what the clear
	// did or did not remove.
	for (uint32_t saveNum : { 5U, 6U }) {
		_uiheroinfo info {};
		info.heroclass = HeroClass::Warrior;
		info.saveNumber = saveNum;
		CopyUtf8(info.name, "Recycled", sizeof(info.name));
		ASSERT_TRUE(pfile_ui_save_create(&info)) << saveNum;
	}

	std::optional<SaveReader> archive = OpenSaveArchive(5);
	ASSERT_TRUE(archive.has_value());
	EXPECT_TRUE(archive->HasFile("hero"));
	for (const char *name : HellfireOnlyMembers)
		EXPECT_FALSE(archive->HasFile(name)) << name << " survived creating a character over it";

	// Removing a member frees its blocks back to the archive, so the recycled
	// slot must end up the size of the fresh one -- not that size plus the 16 KB
	// the leftovers held. The window is slack for the two heroes' item seeds,
	// which differ and compress differently.
	const uintmax_t recycledSize = SizeOf(recycled);
	const uintmax_t freshSize = SizeOf(fresh);
	EXPECT_LT(recycledSize, freshSize + 1024) << "leftovers were unlinked but their space was not reclaimed"
	                                          << " (seeded " << seededSize << ", fresh " << freshSize << ")";
	EXPECT_GT(recycledSize + 1024, freshSize);
#endif
}

} // namespace
} // namespace devilution
