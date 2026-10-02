// Exercise the production monster loader and level cleanup with generated CL2
// assets. Only asset lookup, audio, and unrelated graphics subsystems are fake.
#include "diablo.h"
#include "engine/assets.hpp"
#include "engine/clx_sprite.hpp"
#include "engine/sound.h"
#include "levels/gendung.h"
#include "misdat.h"
#include "monster.h"
#include "player.h"
#include "utils/clx_decode.hpp"
#include "utils/cl2_to_clx.hpp"
#include "utils/endian_write.hpp"
#include <Aulib/Decoder.h>
#include <Aulib/Resampler.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

namespace {
size_t liveBytes;
size_t peakBytes;
struct alignas(std::max_align_t) Allocation { size_t size; };
std::array<std::vector<uint8_t>, 6> assets;
const std::array<std::vector<uint8_t>, 6> *activeAssets = &assets;
std::array<uint8_t, 256> translation;
constexpr char AnimationLetters[] = "nwahds";
constexpr unsigned Height = 64;
bool soundStopped;
bool streamStopped;

std::vector<uint8_t> MakeCl2(unsigned width, unsigned frames)
{
	// Eight directions, each containing literal rows with deterministic colors.
	std::vector<uint8_t> out(32);
	for (unsigned direction = 0; direction < 8; ++direction) {
		devilution::WriteLE32(&out[direction * 4], out.size());
		const size_t start = out.size();
		out.resize(start + (frames + 2) * 4);
		devilution::WriteLE32(&out[start], frames);
		for (unsigned frame = 0; frame < frames; ++frame) {
			devilution::WriteLE32(&out[start + (frame + 1) * 4], out.size() - start);
			const size_t header = out.size();
			out.resize(header + 10);
			devilution::WriteLE16(&out[header], 10);
			for (unsigned y = 0; y < Height; ++y) {
				for (unsigned x = 0; x < width;) {
					const unsigned n = std::min(65U, width - x);
					out.push_back(256 - n);
					for (unsigned i = 0; i < n; ++i, ++x)
						out.push_back((x + y + frame + direction) % 251);
				}
			}
		}
		devilution::WriteLE32(&out[start + (frames + 1) * 4], out.size() - start);
	}
	return out;
}

void CheckPixels(devilution::ClxSprite sprite, unsigned direction, unsigned frame)
{
	CHECK(sprite.height() == Height);
	const uint8_t *p = sprite.pixelData();
	const uint8_t *end = p + sprite.pixelDataSize();
	unsigned index = 0;
	while (p < end) {
		const uint8_t command = *p++;
		CHECK(devilution::IsClxOpaque(command));
		const bool fill = devilution::IsClxOpaqueFill(command);
		const unsigned count = fill ? devilution::GetClxOpaqueFillWidth(command) : devilution::GetClxOpaquePixelsWidth(command);
		for (unsigned i = 0; i < count; ++i, ++index) {
			const uint8_t value = fill ? *p : *p++;
			CHECK(value == (index % sprite.width() + index / sprite.width() + frame + direction) % 251);
		}
		if (fill) ++p;
	}
	CHECK(p == end);
	CHECK(index == sprite.width() * Height);
}
} // namespace

// Track allocations requested by the real loader/converter, including vector
// capacity and temporary buffers. No allocator-specific RSS heuristics.
void *operator new(size_t size)
{
	auto *allocation = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
	if (!allocation) throw std::bad_alloc();
	allocation->size = size;
	liveBytes += size;
	peakBytes = std::max(peakBytes, liveBytes);
	return allocation + 1;
}
void operator delete(void *p) noexcept
{
	if (!p) return;
	auto *allocation = static_cast<Allocation *>(p) - 1;
	liveBytes -= allocation->size;
	std::free(allocation);
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, size_t) noexcept { ::operator delete(p); }

namespace Aulib { Stream::~Stream() = default; }
namespace devilution {
// These paths aren't under test; no audio or missile buffers are created.
TSnd::~TSnd() = default;
MissileFileData MissileSpriteData[256];
void MissileFileData::LoadGFX() {}
void ClxApplyTrans(ClxSpriteList, const uint8_t *) {}
void ClxApplyTrans(ClxSpriteSheet, const uint8_t *) {}
void sound_stop() { soundStopped = true; }
void stream_stop() { streamStopped = true; }
void FreeMissileGFX() {}
void FreeObjectGFX() {}
void FreeTownerGFX() {}
void FreeStashGFX() {}
void DeactivateVirtualGamepad() {}
void FreeVirtualGamepadGFX() {}
std::unique_ptr<byte[]> pDungeonCels;
std::unique_ptr<MegaTile[]> pMegaTiles;
OptionalOwnedClxSpriteList pSpecialCels;

[[noreturn]] void app_fatal(string_view message)
{
	std::fprintf(stderr, "Unexpected fatal: %.*s\n", static_cast<int>(message.size()), message.data());
	std::exit(1);
}

AssetRef FindAsset(const char *path)
{
	AssetRef ref;
	const size_t length = std::strlen(path);
	if (std::strcmp(path + length - 4, ".trn") == 0) {
		ref.directHandle = SDL_RWFromConstMem(translation.data(), translation.size());
		return ref;
	}
	CHECK(std::strcmp(path + length - 4, ".cl2") == 0);
	const char *letter = std::strchr(AnimationLetters, path[length - 5]);
	CHECK(letter != nullptr);
	const auto &asset = (*activeAssets)[letter - AnimationLetters];
	CHECK(!asset.empty());
	ref.directHandle = SDL_RWFromConstMem(asset.data(), asset.size());
	return ref;
}
AssetHandle OpenAsset(AssetRef &&ref, bool)
{
	SDL_RWops *handle = ref.directHandle;
	ref.directHandle = nullptr;
	return AssetHandle(handle);
}
AssetHandle OpenAsset(const char *path, size_t &size, bool threadsafe)
{
	AssetRef ref = FindAsset(path);
	size = SDL_RWsize(ref.directHandle);
	return OpenAsset(std::move(ref), threadsafe);
}
AssetHandle OpenAsset(const char *path, bool threadsafe)
{
	size_t size;
	return OpenAsset(path, size, threadsafe);
}
// Fixture references always use directHandle, never an MPQ archive.
size_t MpqArchive::GetUnpackedFileSize(uint32_t, int32_t &) { std::abort(); }
} // namespace devilution

int main()
{
	using namespace devilution;
	bool peakFits = true;
	HeadlessMode = false;
	for (unsigned i = 0; i < translation.size(); ++i) translation[i] = i;
	const std::array<_monster_id, 4> roster { MT_GOLEM, MT_ADVOCATE, MT_RBLACK, MT_DIABLO };
	for (const auto type : roster) {
		const MonsterData &metadata = MonstersData[type];
		size_t maxAsset = 0;
		for (size_t i = 0; i < assets.size(); ++i) {
			assets[i] = metadata.frames[i] ? MakeCl2(metadata.width, metadata.frames[i]) : std::vector<uint8_t> {};
			maxAsset = std::max(maxAsset, assets[i].size());
		}
		const size_t before = liveBytes;
		for (int reload = 0; reload < 3; ++reload) {
			peakBytes = liveBytes;
			CMonster &monster = LevelMonsterTypes[0];
			monster.type = type;
			InitMonsterGFX(monster);
			const size_t resident = liveBytes - before;
			const size_t peak = peakBytes - before;
			// One retained roster plus at most two copies of ONE animation.
			peakFits &= peak < resident + 2 * maxAsset + 64 * 1024;
			for (size_t i = 0; i < assets.size(); ++i) {
				const auto &anim = monster.anims[i];
				if (metadata.frames[i] == 0 || (i == 5 && !metadata.hasSpecial)) {
					CHECK(!anim.sprites);
					continue;
				}
				CHECK(anim.sprites);
				CHECK(anim.frames == metadata.frames[i]);
				const auto sheet = anim.sprites->sheet();
				for (unsigned direction = 0; direction < 8; ++direction) {
					const auto list = sheet[direction];
					for (unsigned frame = 0; frame < static_cast<unsigned>(metadata.frames[i]); ++frame)
						CheckPixels(list[frame], direction, frame);
				}
			}
			if (reload == 0)
				std::printf("monster %d: resident=%zu KiB peak=%zu KiB\n", type, resident / 1024, peak / 1024);
			FreeMonsters();
			CHECK(liveBytes == before);
		}
	}

	HeadlessMode = true;
	const size_t beforeHeadless = liveBytes;
	InitMonsterGFX(LevelMonsterTypes[0]);
	CHECK(liveBytes == beforeHeadless);
	for (const auto &anim : LevelMonsterTypes[0].anims) CHECK(!anim.sprites);
	FreeMonsters();
	HeadlessMode = false;

	Players.resize(1);
	devilution::Player &player = Players[0];
	const auto cl2 = MakeCl2(32, 1);
	auto sprites = std::make_unique<uint8_t[]>(cl2.size());
	std::memcpy(sprites.get(), cl2.data(), cl2.size());
	player.AnimationData[0].sprites = Cl2ToClx(std::move(sprites), cl2.size(), PointerOrValue<uint16_t> { 32 }).sheet();
	player.AnimInfo.sprites = (*player.AnimationData[0].sprites)[0];
	player.previewCelSprite = (*player.AnimInfo.sprites)[0];
	FreeGameMem();
	CHECK(peakFits);
	CHECK(soundStopped && streamStopped);
	for (const auto &anim : player.AnimationData) CHECK(!anim.sprites);
	CHECK(!player.AnimInfo.sprites);
	CHECK(!player.previewCelSprite);

	// Retain the whole final-floor roster together, as the game does. Fixture
	// bytes are prepared before measuring so only loader allocations count.
	std::array<std::array<std::vector<uint8_t>, 6>, roster.size()> rosterAssets;
	size_t largestAnimation = 0;
	for (size_t type = 0; type < roster.size(); ++type) {
		const MonsterData &metadata = MonstersData[roster[type]];
		for (size_t anim = 0; anim < assets.size(); ++anim) {
			if (metadata.frames[anim] == 0) continue;
			rosterAssets[type][anim] = MakeCl2(metadata.width, metadata.frames[anim]);
			largestAnimation = std::max(largestAnimation, rosterAssets[type][anim].size());
		}
	}
	const size_t emptyLevelBytes = liveBytes;
	size_t firstResident = 0;
	for (int reload = 0; reload < 20; ++reload) {
		peakBytes = liveBytes;
		for (size_t type = 0; type < roster.size(); ++type) {
			activeAssets = &rosterAssets[type];
			LevelMonsterTypes[type].type = roster[type];
			InitMonsterGFX(LevelMonsterTypes[type]);
		}
		const size_t resident = liveBytes - emptyLevelBytes;
		CHECK(peakBytes - emptyLevelBytes < resident + 2 * largestAnimation + 64 * 1024);
		if (reload == 0) {
			firstResident = resident;
			std::printf("full final-floor roster: resident=%zu KiB peak=%zu KiB\n", resident / 1024, (peakBytes - emptyLevelBytes) / 1024);
		}
		CHECK(resident == firstResident);
		for (size_t type = 0; type < roster.size(); ++type) {
			for (const auto &anim : LevelMonsterTypes[type].anims) {
				if (!anim.sprites) continue;
				const auto sheet = anim.sprites->sheet();
				for (unsigned direction = 0; direction < 8; ++direction) {
					for (unsigned frame = 0; frame < static_cast<unsigned>(anim.frames); ++frame)
						CheckPixels(sheet[direction][frame], direction, frame);
				}
			}
		}
		auto playerData = std::make_unique<uint8_t[]>(cl2.size());
		std::memcpy(playerData.get(), cl2.data(), cl2.size());
		player.AnimationData[0].sprites = Cl2ToClx(std::move(playerData), cl2.size(), PointerOrValue<uint16_t> { 32 }).sheet();
		player.AnimInfo.sprites = (*player.AnimationData[0].sprites)[0];
		player.previewCelSprite = (*player.AnimInfo.sprites)[0];
		soundStopped = streamStopped = false;
		FreeGameMem(); // LoadGame calls this before it allocates the next level.
		CHECK(soundStopped && streamStopped);
		CHECK(!player.AnimInfo.sprites && !player.previewCelSprite);
		CHECK(liveBytes == emptyLevelBytes);
	}
	activeAssets = &assets;
	std::puts("PASS: final-floor pixels, bounded loading peak, 20 full-roster reloads, player graphics release");
}
